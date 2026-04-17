#include "fastfix/codec/raw_passthrough.h"
#include "fastfix/codec/fast_int_format.h"
#include "fastfix/codec/fix_tags.h"
#include "fastfix/codec/simd_scan.h"

#include <array>
#include <charconv>
#include <cstring>
#include <limits>

namespace fastfix::codec {

namespace {

using namespace fastfix::codec::tags;

inline constexpr std::size_t kBodyLengthPlaceholderWidth = 7U;

template<typename Integer>
inline constexpr std::size_t kIntBufSize = static_cast<std::size_t>(std::numeric_limits<Integer>::digits10) + 3U;

auto
ParseUint32(std::string_view text) -> std::uint32_t
{
  std::uint32_t value = 0;
  auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc() || ptr != text.data() + text.size()) {
    return 0;
  }
  return value;
}

auto
ParseBoolean(std::string_view text) -> bool
{
  return text == "Y";
}

// Returns the string_view for a field value within the raw bytes,
// given the value_offset and value_length.
auto
ValueView(std::span<const std::byte> data, std::size_t offset, std::size_t len) -> std::string_view
{
  return std::string_view(reinterpret_cast<const char*>(data.data() + offset), len);
}

// Lightweight field scanner: finds tag=value\x01 and returns tag, value
// offset/len, next pos.
struct ScannedField
{
  std::uint32_t tag{ 0 };
  std::size_t value_offset{ 0 };
  std::size_t value_length{ 0 };
  std::size_t next_pos{ 0 };
};

auto
ScanField(std::span<const std::byte> data, std::size_t pos, std::byte delim) -> ScannedField
{
  const auto eq_byte = static_cast<std::byte>('=');
  if (pos >= data.size()) {
    return {};
  }
  const auto remaining = data.size() - pos;
  const auto* soh = FindByte(data.data() + pos, remaining, delim);
  const auto soh_idx = static_cast<std::size_t>(soh - data.data());
  if (soh_idx >= data.size() || soh_idx == pos) {
    return {};
  }
  const auto field_len = soh_idx - pos;
  const auto* eq = FindByte(data.data() + pos, field_len, eq_byte);
  const auto eq_off = static_cast<std::size_t>(eq - (data.data() + pos));
  if (eq_off >= field_len || eq_off == 0) {
    return {};
  }
  const auto* tag_chars = reinterpret_cast<const char*>(data.data() + pos);
  std::uint32_t tag = 0;
  auto [ptr, ec] = std::from_chars(tag_chars, tag_chars + eq_off, tag);
  if (ec != std::errc() || ptr != tag_chars + eq_off) {
    return {};
  }
  return ScannedField{
    .tag = tag,
    .value_offset = pos + eq_off + 1,
    .value_length = field_len - eq_off - 1,
    .next_pos = soh_idx + 1,
  };
}

// Aggregate session/routing header tags plus framing tags — used to determine
// where raw_body begins.
auto
IsSessionHeaderTag(std::uint32_t tag) -> bool
{
  return IsAggregateSessionEnvelopeTag(tag);
}

void
AppendStringField(std::string& out, std::string_view prefix, std::string_view value, char delimiter)
{
  out.append(prefix);
  out.append(value);
  out.push_back(delimiter);
}

void
AppendCountField(std::string& out, std::string_view prefix, std::uint32_t value, char delimiter)
{
  char buf[10];
  const auto len = FormatUint32(buf, value);
  out.append(prefix);
  out.append(buf, len);
  out.push_back(delimiter);
}

void
AppendOptionalFlagField(std::string& out, std::string_view prefix, bool enabled, char delimiter)
{
  if (!enabled) {
    return;
  }
  out.append(prefix);
  out.push_back('Y');
  out.push_back(delimiter);
}

auto
ResolveReplayString(std::string_view preferred, std::string_view fallback) -> std::string_view
{
  return preferred.empty() ? fallback : preferred;
}

auto
ResolveReplayDefaultApplVerID(const RawPassThroughView& stored, const ReplayOptions& options) -> std::string_view
{
  if (!options.default_appl_ver_id.empty()) {
    return options.default_appl_ver_id;
  }
  if (stored.msg_type == "A") {
    return stored.default_appl_ver_id;
  }
  return {};
}

auto
CountStringFieldBytes(std::string_view prefix, std::string_view value) -> std::size_t
{
  return prefix.size() + value.size() + 1U;
}

auto
CountLiteralFieldBytes(std::string_view literal) -> std::size_t
{
  return literal.size() + 1U;
}

auto
CountUintFieldBytes(std::string_view prefix, std::uint32_t value) -> std::size_t
{
  char digits[10];
  return prefix.size() + FormatUint32(digits, value) + 1U;
}

auto
DecodeRawPassThroughImpl(std::span<const std::byte> data,
                         const std::uint32_t* known_body_start_offset,
                         char delimiter,
                         bool verify_checksum) -> base::Result<RawPassThroughView>
{
  const auto delim = static_cast<std::byte>(static_cast<unsigned char>(delimiter == '\0' ? kFixSoh : delimiter));

  RawPassThroughView view;
  view.raw_message = data;

  auto f = ScanField(data, 0, delim);
  if (f.tag != kBeginString) {
    return base::Status::FormatError("FIX frame must begin with tag 8");
  }
  view.begin_string = ValueView(data, f.value_offset, f.value_length);

  f = ScanField(data, f.next_pos, delim);
  if (f.tag != kBodyLength) {
    return base::Status::FormatError("FIX frame must have tag 9 after tag 8");
  }
  const auto declared_body_length = ParseUint32(ValueView(data, f.value_offset, f.value_length));
  const auto session_prefix_end = f.next_pos;

  if (data.size() < 7) {
    return base::Status::FormatError("FIX frame too short for CheckSum");
  }
  if (static_cast<std::byte>('1') != data[data.size() - 7] || static_cast<std::byte>('0') != data[data.size() - 6] ||
      static_cast<std::byte>('=') != data[data.size() - 5] || delim != data[data.size() - 1]) {
    return base::Status::FormatError("FIX frame missing CheckSum (tag 10)");
  }
  const auto checksum_field_start = data.size() - 7;

  if (verify_checksum) {
    std::uint32_t actual_sum = 0;
    for (std::size_t i = 0; i < checksum_field_start; ++i) {
      actual_sum += std::to_integer<unsigned char>(data[i]);
    }
    actual_sum %= 256U;
    const auto expected = ParseUint32(ValueView(data, data.size() - 4, 3));
    if (actual_sum != expected) {
      return base::Status::FormatError("CheckSum mismatch");
    }
  }

  const auto actual_body_length = checksum_field_start - session_prefix_end;
  if (declared_body_length != actual_body_length) {
    return base::Status::FormatError("BodyLength mismatch");
  }

  const auto scan_end =
    known_body_start_offset == nullptr ? checksum_field_start : static_cast<std::size_t>(*known_body_start_offset);
  if (scan_end < session_prefix_end || scan_end > checksum_field_start) {
    return base::Status::FormatError("stored body_start_offset is outside the FIX body");
  }

  std::size_t last_header_end = session_prefix_end;
  std::size_t pos = session_prefix_end;
  while (pos < scan_end) {
    f = ScanField(data, pos, delim);
    if (f.tag == 0 && f.next_pos == 0) {
      return base::Status::FormatError("malformed FIX field");
    }

    if (!IsSessionHeaderTag(f.tag)) {
      if (known_body_start_offset != nullptr) {
        return base::Status::FormatError("stored body_start_offset does not align with a FIX body boundary");
      }
      break;
    }

    const auto val = ValueView(data, f.value_offset, f.value_length);
    switch (f.tag) {
      case kMsgType:
        view.msg_type = val;
        break;
      case kMsgSeqNum:
        view.msg_seq_num = ParseUint32(val);
        break;
      case kSenderCompID:
        view.sender_comp_id = val;
        break;
      case kSenderSubID:
        view.sender_sub_id = val;
        break;
      case kTargetCompID:
        view.target_comp_id = val;
        break;
      case kTargetSubID:
        view.target_sub_id = val;
        break;
      case kOnBehalfOfCompID:
        view.on_behalf_of_comp_id = val;
        break;
      case kDeliverToCompID:
        view.deliver_to_comp_id = val;
        break;
      case kDefaultApplVerID:
        view.default_appl_ver_id = val;
        break;
      case kSendingTime:
        view.sending_time = val;
        break;
      case kOrigSendingTime:
        view.orig_sending_time = val;
        break;
      case kPossDupFlag:
        view.poss_dup = ParseBoolean(val);
        break;
      case kPossResend:
        view.poss_resend = ParseBoolean(val);
        break;
      default:
        break;
    }

    last_header_end = f.next_pos;
    pos = f.next_pos;
  }

  const auto body_start_offset =
    known_body_start_offset == nullptr ? static_cast<std::uint32_t>(last_header_end) : *known_body_start_offset;
  view.raw_body = data.subspan(body_start_offset, checksum_field_start - body_start_offset);
  view.body_start_offset = body_start_offset;
  view.valid = true;
  return view;
}

} // namespace

auto
DecodeRawPassThrough(std::span<const std::byte> data, char delimiter, bool verify_checksum)
  -> base::Result<RawPassThroughView>
{
  return DecodeRawPassThroughImpl(data, nullptr, delimiter, verify_checksum);
}

auto
DecodeRawPassThrough(std::span<const std::byte> data,
                     std::uint32_t body_start_offset,
                     char delimiter,
                     bool verify_checksum) -> base::Result<RawPassThroughView>
{
  return DecodeRawPassThroughImpl(data, &body_start_offset, delimiter, verify_checksum);
}

auto
EncodeForwarded(const RawPassThroughView& inbound, const ForwardingOptions& options, EncodeBuffer* buffer)
  -> base::Status
{
  if (buffer == nullptr) {
    return base::Status::InvalidArgument("encode buffer is null");
  }
  if (!inbound.valid) {
    return base::Status::InvalidArgument("inbound view is not valid");
  }

  const char soh = options.delimiter == '\0' ? kFixSoh : options.delimiter;
  auto& out = buffer->storage;
  out.clear();

  // Reserve a reasonable capacity to avoid reallocations.
  out.reserve(inbound.raw_message.size() + 128);

  // 1. Write 8=<begin_string>SOH
  const auto begin_string = options.begin_string.empty() ? inbound.begin_string : options.begin_string;
  out.append(kBeginStringPrefix);
  out.append(begin_string);
  out.push_back(soh);

  // 2. Write 9= + placeholder + SOH
  out.append(kBodyLengthPrefix);
  const auto body_length_offset = out.size();
  out.append(kBodyLengthPlaceholderWidth, '0');
  out.push_back(soh);
  const auto body_start = out.size();

  // 3-12. Shared session/routing header fields.
  AppendStringField(out, kMsgTypePrefix, inbound.msg_type, soh);
  AppendCountField(out, kMsgSeqNumPrefix, options.msg_seq_num, soh);
  AppendStringField(out, kSenderCompIDPrefix, options.sender_comp_id, soh);
  if (!options.sender_sub_id.empty()) {
    AppendStringField(out, kSenderSubIDPrefix, options.sender_sub_id, soh);
  }
  AppendStringField(out, kTargetCompIDPrefix, options.target_comp_id, soh);
  if (!options.target_sub_id.empty()) {
    AppendStringField(out, kTargetSubIDPrefix, options.target_sub_id, soh);
  }
  AppendStringField(out, kSendingTimePrefix, options.sending_time, soh);
  AppendOptionalFlagField(out, std::string_view("43="), options.poss_dup, soh);
  AppendOptionalFlagField(out, kPossResendPrefix, options.poss_resend, soh);
  if (!options.orig_sending_time.empty()) {
    AppendStringField(out, kOrigSendingTimePrefix, options.orig_sending_time, soh);
  }
  if (!options.on_behalf_of_comp_id.empty()) {
    AppendStringField(out, kOnBehalfOfCompIDPrefix, options.on_behalf_of_comp_id, soh);
  }
  if (!options.deliver_to_comp_id.empty()) {
    AppendStringField(out, kDeliverToCompIDPrefix, options.deliver_to_comp_id, soh);
  }

  // 10. Splice raw body bytes
  if (!inbound.raw_body.empty()) {
    const auto* body_ptr = reinterpret_cast<const char*>(inbound.raw_body.data());
    out.append(body_ptr, inbound.raw_body.size());
  }

  // 11. Fill in BodyLength
  const auto body_length = static_cast<std::uint32_t>(out.size() - body_start);
  {
    char buf[10];
    const auto len = FormatUint32(buf, body_length);
    if (len > kBodyLengthPlaceholderWidth) {
      return base::Status::FormatError("BodyLength exceeds placeholder width");
    }
    out.replace(body_length_offset, kBodyLengthPlaceholderWidth, buf, len);
  }

  // 12. Compute and append 10=<checksum>SOH
  std::uint32_t checksum = 0;
  for (const auto ch : out) {
    checksum += static_cast<unsigned char>(ch);
  }
  checksum %= 256U;

  std::array<char, 3> cksum{};
  cksum[0] = static_cast<char>('0' + ((checksum / 100U) % 10U));
  cksum[1] = static_cast<char>('0' + ((checksum / 10U) % 10U));
  cksum[2] = static_cast<char>('0' + (checksum % 10U));
  out.append(kCheckSumPrefix);
  out.append(cksum.data(), 3);
  out.push_back(soh);

  return base::Status::Ok();
}

auto
EncodeReplay(const RawPassThroughView& stored, const ReplayOptions& options, EncodeBuffer* buffer) -> base::Status
{
  if (buffer == nullptr) {
    return base::Status::InvalidArgument("encode buffer is null");
  }
  if (!stored.valid) {
    return base::Status::InvalidArgument("stored view is not valid");
  }

  const char soh = options.delimiter == '\0' ? kFixSoh : options.delimiter;
  auto& out = buffer->storage;
  out.clear();

  out.reserve(stored.raw_message.size() + 64);

  // 1. 8=<begin_string>SOH
  const auto begin_string = options.begin_string.empty() ? stored.begin_string : options.begin_string;
  out.append(kBeginStringPrefix);
  out.append(begin_string);
  out.push_back(soh);

  // 2. 9=<placeholder>SOH
  out.append(kBodyLengthPrefix);
  const auto body_length_offset = out.size();
  out.append(kBodyLengthPlaceholderWidth, '0');
  out.push_back(soh);
  const auto body_start = out.size();

  const auto replay_default_appl_ver_id = ResolveReplayDefaultApplVerID(stored, options);
  const auto replay_sender_sub_id = ResolveReplayString(options.sender_sub_id, stored.sender_sub_id);
  const auto replay_target_sub_id = ResolveReplayString(options.target_sub_id, stored.target_sub_id);
  const auto replay_on_behalf_of = ResolveReplayString(options.on_behalf_of_comp_id, stored.on_behalf_of_comp_id);
  const auto replay_deliver_to = ResolveReplayString(options.deliver_to_comp_id, stored.deliver_to_comp_id);

  AppendStringField(out, kMsgTypePrefix, stored.msg_type, soh);
  AppendCountField(out, kMsgSeqNumPrefix, options.msg_seq_num, soh);
  AppendStringField(out, kSenderCompIDPrefix, options.sender_comp_id, soh);
  if (!replay_sender_sub_id.empty()) {
    AppendStringField(out, kSenderSubIDPrefix, replay_sender_sub_id, soh);
  }
  AppendStringField(out, kTargetCompIDPrefix, options.target_comp_id, soh);
  if (!replay_target_sub_id.empty()) {
    AppendStringField(out, kTargetSubIDPrefix, replay_target_sub_id, soh);
  }
  AppendStringField(out, kSendingTimePrefix, options.sending_time, soh);
  if (!replay_default_appl_ver_id.empty()) {
    AppendStringField(out, kDefaultApplVerIDPrefix, replay_default_appl_ver_id, soh);
  }
  AppendOptionalFlagField(out, std::string_view("43="), true, soh);
  AppendOptionalFlagField(out, kPossResendPrefix, options.poss_resend, soh);
  if (!options.orig_sending_time.empty()) {
    AppendStringField(out, kOrigSendingTimePrefix, options.orig_sending_time, soh);
  }
  if (!replay_on_behalf_of.empty()) {
    AppendStringField(out, kOnBehalfOfCompIDPrefix, replay_on_behalf_of, soh);
  }
  if (!replay_deliver_to.empty()) {
    AppendStringField(out, kDeliverToCompIDPrefix, replay_deliver_to, soh);
  }

  // 11. Splice raw body bytes unchanged
  if (!stored.raw_body.empty()) {
    const auto* body_ptr = reinterpret_cast<const char*>(stored.raw_body.data());
    out.append(body_ptr, stored.raw_body.size());
  }

  // 12. Backfill BodyLength
  const auto body_length = static_cast<std::uint32_t>(out.size() - body_start);
  {
    char buf[10];
    const auto len = FormatUint32(buf, body_length);
    if (len > kBodyLengthPlaceholderWidth) {
      return base::Status::FormatError("BodyLength exceeds placeholder width");
    }
    out.replace(body_length_offset, kBodyLengthPlaceholderWidth, buf, len);
  }

  // 13. Compute and append 10=<checksum>SOH
  std::uint32_t checksum = 0;
  for (const auto ch : out) {
    checksum += static_cast<unsigned char>(ch);
  }
  checksum %= 256U;

  std::array<char, 3> cksum{};
  cksum[0] = static_cast<char>('0' + ((checksum / 100U) % 10U));
  cksum[1] = static_cast<char>('0' + ((checksum / 10U) % 10U));
  cksum[2] = static_cast<char>('0' + (checksum % 10U));
  out.append(kCheckSumPrefix);
  out.append(cksum.data(), 3);
  out.push_back(soh);

  return base::Status::Ok();
}

// ComputeChecksumSIMD is now in simd_scan.h (fastfix::codec namespace).

auto
EncodeReplayInto(const RawPassThroughView& stored, const ReplayOptions& options, session::EncodedFrameBytes* out)
  -> base::Status
{
  if (out == nullptr) {
    return base::Status::InvalidArgument("output is null");
  }
  if (!stored.valid) {
    return base::Status::InvalidArgument("stored view is not valid");
  }

  const char soh = options.delimiter == '\0' ? kFixSoh : options.delimiter;
  const auto replay_default_appl_ver_id = ResolveReplayDefaultApplVerID(stored, options);
  const auto replay_sender_sub_id = ResolveReplayString(options.sender_sub_id, stored.sender_sub_id);
  const auto replay_target_sub_id = ResolveReplayString(options.target_sub_id, stored.target_sub_id);
  const auto replay_on_behalf_of = ResolveReplayString(options.on_behalf_of_comp_id, stored.on_behalf_of_comp_id);
  const auto replay_deliver_to = ResolveReplayString(options.deliver_to_comp_id, stored.deliver_to_comp_id);
  const auto begin_string = options.begin_string.empty() ? stored.begin_string : options.begin_string;
  const auto body_bytes = stored.raw_body.size();
  const auto body_length = static_cast<std::uint32_t>(
    CountStringFieldBytes(kMsgTypePrefix, stored.msg_type) +
    CountUintFieldBytes(kMsgSeqNumPrefix, options.msg_seq_num) +
    CountStringFieldBytes(kSenderCompIDPrefix, options.sender_comp_id) +
    (replay_sender_sub_id.empty() ? 0U : CountStringFieldBytes(kSenderSubIDPrefix, replay_sender_sub_id)) +
    CountStringFieldBytes(kTargetCompIDPrefix, options.target_comp_id) +
    (replay_target_sub_id.empty() ? 0U : CountStringFieldBytes(kTargetSubIDPrefix, replay_target_sub_id)) +
    CountStringFieldBytes(kSendingTimePrefix, options.sending_time) +
    (replay_default_appl_ver_id.empty() ? 0U
                                        : CountStringFieldBytes(kDefaultApplVerIDPrefix, replay_default_appl_ver_id)) +
    CountLiteralFieldBytes(std::string_view("43=Y")) +
    (options.poss_resend ? CountLiteralFieldBytes(std::string_view("97=Y")) : 0U) +
    (options.orig_sending_time.empty() ? 0U
                                       : CountStringFieldBytes(kOrigSendingTimePrefix, options.orig_sending_time)) +
    (replay_on_behalf_of.empty() ? 0U : CountStringFieldBytes(kOnBehalfOfCompIDPrefix, replay_on_behalf_of)) +
    (replay_deliver_to.empty() ? 0U : CountStringFieldBytes(kDeliverToCompIDPrefix, replay_deliver_to)) + body_bytes);
  const auto encoded_buffer_size =
    CountStringFieldBytes(kBeginStringPrefix, begin_string) + CountUintFieldBytes(kBodyLengthPrefix, body_length) +
    static_cast<std::size_t>(body_length - body_bytes) + (options.zero_copy_body ? 0U : body_bytes) + 7U;

  // Choose storage: inline or overflow
  char* buf;
  if (encoded_buffer_size <= session::kEncodedFrameInlineCapacity) {
    buf = reinterpret_cast<char*>(out->inline_storage.data());
    out->overflow_storage.clear();
  } else {
    out->inline_size = 0;
    out->overflow_storage.resize(encoded_buffer_size);
    buf = reinterpret_cast<char*>(out->overflow_storage.data());
  }

  // Incremental checksum — accumulate as we write each byte of the header.
  // FIX checksum = sum of ALL bytes before the 10= field (including 8=, 9=).
  std::uint32_t header_checksum = 0;
  std::size_t pos = 0;

  auto append_sv = [&](std::string_view sv) {
    std::memcpy(buf + pos, sv.data(), sv.size());
    for (std::size_t i = 0; i < sv.size(); ++i) {
      header_checksum += static_cast<unsigned char>(sv[i]);
    }
    pos += sv.size();
  };
  auto append_char = [&](char ch) {
    buf[pos++] = ch;
    header_checksum += static_cast<unsigned char>(ch);
  };

  // 1. 8=<begin_string>SOH
  append_sv(kBeginStringPrefix);
  append_sv(begin_string);
  append_char(soh);

  // 2. 9=<body_length>SOH
  append_sv(kBodyLengthPrefix);
  {
    char body_length_digits[10];
    const auto len = FormatUint32(body_length_digits, body_length);
    append_sv(std::string_view(body_length_digits, len));
  }
  append_char(soh);

  append_sv(kMsgTypePrefix);
  append_sv(stored.msg_type);
  append_char(soh);

  {
    char num_buf[10];
    const auto num_len = FormatUint32(num_buf, options.msg_seq_num);
    append_sv(kMsgSeqNumPrefix);
    append_sv(std::string_view(num_buf, num_len));
    append_char(soh);
  }

  append_sv(kSenderCompIDPrefix);
  append_sv(options.sender_comp_id);
  append_char(soh);
  if (!replay_sender_sub_id.empty()) {
    append_sv(kSenderSubIDPrefix);
    append_sv(replay_sender_sub_id);
    append_char(soh);
  }
  append_sv(kTargetCompIDPrefix);
  append_sv(options.target_comp_id);
  append_char(soh);
  if (!replay_target_sub_id.empty()) {
    append_sv(kTargetSubIDPrefix);
    append_sv(replay_target_sub_id);
    append_char(soh);
  }
  append_sv(kSendingTimePrefix);
  append_sv(options.sending_time);
  append_char(soh);
  if (!replay_default_appl_ver_id.empty()) {
    append_sv(kDefaultApplVerIDPrefix);
    append_sv(replay_default_appl_ver_id);
    append_char(soh);
  }
  append_sv(std::string_view("43=Y"));
  append_char(soh);
  if (options.poss_resend) {
    append_sv(std::string_view("97=Y"));
    append_char(soh);
  }
  if (!options.orig_sending_time.empty()) {
    append_sv(kOrigSendingTimePrefix);
    append_sv(options.orig_sending_time);
    append_char(soh);
  }
  if (!replay_on_behalf_of.empty()) {
    append_sv(kOnBehalfOfCompIDPrefix);
    append_sv(replay_on_behalf_of);
    append_char(soh);
  }
  if (!replay_deliver_to.empty()) {
    append_sv(kDeliverToCompIDPrefix);
    append_sv(replay_deliver_to);
    append_char(soh);
  }

  // 11. Body handling: zero-copy scatter-gather or inline copy
  std::uint32_t body_checksum = 0;
  const auto body_data_size = body_bytes;
  if (body_data_size > 0) {
    body_checksum = ComputeChecksumSIMD(reinterpret_cast<const char*>(stored.raw_body.data()), body_data_size);
  }

  std::size_t splice_offset = 0;
  if (options.zero_copy_body) {
    // Zero-copy path: body stays at its source address (e.g., mmap).
    // Record splice point; trailer written next in buf.
    splice_offset = pos;
  } else {
    // Inline path: copy body into buf.
    if (body_data_size > 0) {
      std::memcpy(buf + pos, stored.raw_body.data(), body_data_size);
      pos += body_data_size;
    }
  }

  // 12. Combine: header (incremental) + body (SIMD), no full-buffer rescan
  std::uint32_t checksum = (header_checksum + body_checksum) % 256U;

  std::memcpy(buf + pos, kCheckSumPrefix.data(), kCheckSumPrefix.size());
  pos += kCheckSumPrefix.size();
  buf[pos++] = static_cast<char>('0' + ((checksum / 100U) % 10U));
  buf[pos++] = static_cast<char>('0' + ((checksum / 10U) % 10U));
  buf[pos++] = static_cast<char>('0' + (checksum % 10U));
  buf[pos++] = soh;

  // Finalize
  if (encoded_buffer_size <= session::kEncodedFrameInlineCapacity) {
    out->inline_size = pos;
  } else {
    out->overflow_storage.resize(pos);
  }

  if (options.zero_copy_body && body_data_size > 0) {
    out->external_body = stored.raw_body;
    out->body_splice_offset = splice_offset;
  } else {
    out->external_body = {};
    out->body_splice_offset = 0;
  }

  return base::Status::Ok();
}

} // namespace fastfix::codec
