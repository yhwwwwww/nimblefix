#include "fastfix/codec/raw_passthrough.h"
#include "fastfix/codec/simd_scan.h"

#include <array>
#include <charconv>
#include <cstring>
#include <limits>

namespace fastfix::codec {

namespace {

inline constexpr std::size_t kBodyLengthPlaceholderWidth = 7U;

template <typename Integer>
inline constexpr std::size_t kIntBufSize =
    static_cast<std::size_t>(std::numeric_limits<Integer>::digits10) + 3U;

auto ParseUint32(std::string_view text) -> std::uint32_t {
    std::uint32_t value = 0;
    std::from_chars(text.data(), text.data() + text.size(), value);
    return value;
}

// Returns the string_view for a field value within the raw bytes,
// given the value_offset and value_length.
auto ValueView(std::span<const std::byte> data, std::size_t offset, std::size_t len) -> std::string_view {
    return std::string_view(reinterpret_cast<const char*>(data.data() + offset), len);
}

// Lightweight field scanner: finds tag=value\x01 and returns tag, value offset/len, next pos.
struct ScannedField {
    std::uint32_t tag{0};
    std::size_t value_offset{0};
    std::size_t value_length{0};
    std::size_t next_pos{0};
};

auto ScanField(std::span<const std::byte> data, std::size_t pos, std::byte delim) -> ScannedField {
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

// Session-layer header tags — used to determine where raw_body begins.
// Matches CompiledMessageDecoder::is_header_tag() plus framing tags.
auto IsSessionHeaderTag(std::uint32_t tag) -> bool {
    switch (tag) {
        case 8U: case 9U: case 10U:           // frame structure
        case 34U: case 35U: case 43U:         // SeqNum, MsgType, PossDupFlag
        case 49U: case 52U: case 56U:         // Sender, SendingTime, Target
        case 97U: case 122U: case 1137U:      // PossResend, OrigSendingTime, DefaultApplVerID
            return true;
        default:
            return false;
    }
}

}  // namespace

auto DecodeRawPassThrough(
    std::span<const std::byte> data,
    char delimiter,
    bool verify_checksum) -> base::Result<RawPassThroughView> {
    const auto delim = static_cast<std::byte>(static_cast<unsigned char>(
        delimiter == '\0' ? kFixSoh : delimiter));

    RawPassThroughView view;
    view.raw_message = data;

    // Field 0: must be tag 8 (BeginString)
    auto f = ScanField(data, 0, delim);
    if (f.tag != 8U) {
        return base::Status::FormatError("FIX frame must begin with tag 8");
    }
    view.begin_string = ValueView(data, f.value_offset, f.value_length);

    // Field 1: must be tag 9 (BodyLength)
    f = ScanField(data, f.next_pos, delim);
    if (f.tag != 9U) {
        return base::Status::FormatError("FIX frame must have tag 9 after tag 8");
    }
    const auto declared_body_length = ParseUint32(ValueView(data, f.value_offset, f.value_length));
    const auto body_start_offset = f.next_pos;

    // Track offset of body region (after last session header field, before tag 10).
    // We define body_begin as the position right after the last header field we've seen,
    // and body_end as the start of tag 10.
    std::size_t last_header_end = f.next_pos;
    std::size_t checksum_field_start = 0;
    bool saw_checksum = false;

    std::size_t pos = f.next_pos;
    while (pos < data.size()) {
        f = ScanField(data, pos, delim);
        if (f.tag == 0 && f.next_pos == 0) {
            return base::Status::FormatError("malformed FIX field");
        }
        const auto val = ValueView(data, f.value_offset, f.value_length);

        if (f.tag == 10U) {
            checksum_field_start = pos;
            saw_checksum = true;

            if (verify_checksum) {
                std::uint32_t actual_sum = 0;
                for (std::size_t i = 0; i < checksum_field_start; ++i) {
                    actual_sum += std::to_integer<unsigned char>(data[i]);
                }
                actual_sum %= 256U;
                const auto expected = ParseUint32(val);
                if (actual_sum != expected) {
                    return base::Status::FormatError("CheckSum mismatch");
                }
            }
            pos = f.next_pos;
            break;
        }

        switch (f.tag) {
            case 35U:
                view.msg_type = val;
                last_header_end = f.next_pos;
                break;
            case 34U:
                view.msg_seq_num = ParseUint32(val);
                last_header_end = f.next_pos;
                break;
            case 49U:
                view.sender_comp_id = val;
                last_header_end = f.next_pos;
                break;
            case 56U:
                view.target_comp_id = val;
                last_header_end = f.next_pos;
                break;
            case 52U:
                view.sending_time = val;
                last_header_end = f.next_pos;
                break;
            default:
                if (IsSessionHeaderTag(f.tag)) {
                    last_header_end = f.next_pos;
                }
                break;
        }
        pos = f.next_pos;
    }

    if (!saw_checksum) {
        return base::Status::FormatError("FIX frame missing CheckSum (tag 10)");
    }
    if (pos != data.size()) {
        return base::Status::FormatError("FIX frame has trailing data after CheckSum");
    }

    // Validate BodyLength
    const auto actual_body_length = checksum_field_start - body_start_offset;
    if (declared_body_length != actual_body_length) {
        return base::Status::FormatError("BodyLength mismatch");
    }

    // raw_body is everything between last_header_end and checksum_field_start.
    // This contains all application-level fields, unparsed.
    view.raw_body = data.subspan(last_header_end, checksum_field_start - last_header_end);
    view.valid = true;
    return view;
}

auto EncodeForwarded(
    const RawPassThroughView& inbound,
    const ForwardingOptions& options,
    EncodeBuffer* buffer) -> base::Status {
    if (buffer == nullptr) {
        return base::Status::InvalidArgument("encode buffer is null");
    }
    if (!inbound.valid) {
        return base::Status::InvalidArgument("inbound view is not valid");
    }

    const char soh = kFixSoh;
    auto& out = buffer->storage;
    out.clear();

    // Reserve a reasonable capacity to avoid reallocations.
    out.reserve(inbound.raw_message.size() + 128);

    // 1. Write 8=<begin_string>SOH
    const auto begin_string = options.begin_string.empty()
        ? inbound.begin_string
        : options.begin_string;
    out.append("8=");
    out.append(begin_string);
    out.push_back(soh);

    // 2. Write 9= + placeholder + SOH
    out.append("9=");
    const auto body_length_offset = out.size();
    out.append(kBodyLengthPlaceholderWidth, '0');
    out.push_back(soh);
    const auto body_start = out.size();

    // 3. Write 35=<msg_type>SOH
    out.append("35=");
    out.append(inbound.msg_type);
    out.push_back(soh);

    // 4. Write 49=<sender>SOH
    out.append("49=");
    out.append(options.sender_comp_id);
    out.push_back(soh);

    // 5. Write 56=<target>SOH
    out.append("56=");
    out.append(options.target_comp_id);
    out.push_back(soh);

    // 6. Write 34=<seq_num>SOH
    {
        std::array<char, kIntBufSize<std::uint32_t>> buf{};
        auto [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), options.msg_seq_num);
        if (ec != std::errc()) {
            return base::Status::FormatError("failed to format MsgSeqNum");
        }
        out.append("34=");
        out.append(buf.data(), static_cast<std::size_t>(ptr - buf.data()));
        out.push_back(soh);
    }

    // 7. Write 52=<sending_time>SOH
    out.append("52=");
    out.append(options.sending_time);
    out.push_back(soh);

    // 8. Optional: 115=<on_behalf_of_comp_id>SOH
    if (!options.on_behalf_of_comp_id.empty()) {
        out.append("115=");
        out.append(options.on_behalf_of_comp_id);
        out.push_back(soh);
    }

    // 9. Optional: 128=<deliver_to_comp_id>SOH
    if (!options.deliver_to_comp_id.empty()) {
        out.append("128=");
        out.append(options.deliver_to_comp_id);
        out.push_back(soh);
    }

    // 10. Splice raw body bytes
    if (!inbound.raw_body.empty()) {
        const auto* body_ptr = reinterpret_cast<const char*>(inbound.raw_body.data());
        out.append(body_ptr, inbound.raw_body.size());
    }

    // 11. Fill in BodyLength
    const auto body_length = static_cast<std::uint32_t>(out.size() - body_start);
    {
        std::array<char, kIntBufSize<std::uint32_t>> buf{};
        auto [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), body_length);
        if (ec != std::errc()) {
            return base::Status::FormatError("failed to format BodyLength");
        }
        const auto digits = static_cast<std::size_t>(ptr - buf.data());
        if (digits > kBodyLengthPlaceholderWidth) {
            return base::Status::FormatError("BodyLength exceeds placeholder width");
        }
        out.replace(body_length_offset, kBodyLengthPlaceholderWidth, buf.data(), digits);
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
    out.append("10=");
    out.append(cksum.data(), 3);
    out.push_back(soh);

    return base::Status::Ok();
}

auto EncodeReplay(
    const RawPassThroughView& stored,
    const ReplayOptions& options,
    EncodeBuffer* buffer) -> base::Status {
    if (buffer == nullptr) {
        return base::Status::InvalidArgument("encode buffer is null");
    }
    if (!stored.valid) {
        return base::Status::InvalidArgument("stored view is not valid");
    }

    const char soh = kFixSoh;
    auto& out = buffer->storage;
    out.clear();

    out.reserve(stored.raw_message.size() + 64);

    // 1. 8=<begin_string>SOH
    const auto begin_string = options.begin_string.empty()
        ? stored.begin_string
        : options.begin_string;
    out.append("8=");
    out.append(begin_string);
    out.push_back(soh);

    // 2. 9=<placeholder>SOH
    out.append("9=");
    const auto body_length_offset = out.size();
    out.append(kBodyLengthPlaceholderWidth, '0');
    out.push_back(soh);
    const auto body_start = out.size();

    // 3. 35=<msg_type>SOH
    out.append("35=");
    out.append(stored.msg_type);
    out.push_back(soh);

    // 4. 49=<sender>SOH
    out.append("49=");
    out.append(options.sender_comp_id);
    out.push_back(soh);

    // 5. 56=<target>SOH
    out.append("56=");
    out.append(options.target_comp_id);
    out.push_back(soh);

    // 6. 34=<seq_num>SOH
    {
        std::array<char, kIntBufSize<std::uint32_t>> buf{};
        auto [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), options.msg_seq_num);
        if (ec != std::errc()) {
            return base::Status::FormatError("failed to format MsgSeqNum");
        }
        out.append("34=");
        out.append(buf.data(), static_cast<std::size_t>(ptr - buf.data()));
        out.push_back(soh);
    }

    // 7. 52=<sending_time>SOH
    out.append("52=");
    out.append(options.sending_time);
    out.push_back(soh);

    // 8. 43=Y SOH (always set for replay)
    out.append("43=Y");
    out.push_back(soh);

    // 9. 122=<orig_sending_time>SOH
    if (!options.orig_sending_time.empty()) {
        out.append("122=");
        out.append(options.orig_sending_time);
        out.push_back(soh);
    }

    // 10. Optional 1128=<default_appl_ver_id>SOH
    if (!options.default_appl_ver_id.empty()) {
        out.append("1128=");
        out.append(options.default_appl_ver_id);
        out.push_back(soh);
    }

    // 11. Splice raw body bytes unchanged
    if (!stored.raw_body.empty()) {
        const auto* body_ptr = reinterpret_cast<const char*>(stored.raw_body.data());
        out.append(body_ptr, stored.raw_body.size());
    }

    // 12. Backfill BodyLength
    const auto body_length = static_cast<std::uint32_t>(out.size() - body_start);
    {
        std::array<char, kIntBufSize<std::uint32_t>> buf{};
        auto [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), body_length);
        if (ec != std::errc()) {
            return base::Status::FormatError("failed to format BodyLength");
        }
        const auto digits = static_cast<std::size_t>(ptr - buf.data());
        if (digits > kBodyLengthPlaceholderWidth) {
            return base::Status::FormatError("BodyLength exceeds placeholder width");
        }
        out.replace(body_length_offset, kBodyLengthPlaceholderWidth, buf.data(), digits);
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
    out.append("10=");
    out.append(cksum.data(), 3);
    out.push_back(soh);

    return base::Status::Ok();
}

}  // namespace fastfix::codec
