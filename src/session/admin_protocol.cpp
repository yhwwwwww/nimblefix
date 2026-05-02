#include "nimblefix/session/admin_protocol.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <optional>
#include <string_view>

#include "nimblefix/advanced/message_builder.h"
#include "nimblefix/advanced/typed_message_view.h"
#include "nimblefix/codec/compiled_decoder.h"
#include "nimblefix/codec/fast_int_format.h"
#include "nimblefix/codec/fix_tags.h"
#include "nimblefix/codec/raw_passthrough.h"
#include "nimblefix/codec/simd_scan.h"
#include "nimblefix/profile/normalized_dictionary.h"
#include "nimblefix/store/session_store.h"

namespace nimble::session {

namespace {

using namespace nimble::codec::tags;

constexpr std::uint64_t kNanosPerSecond = 1'000'000'000ULL;
constexpr std::uint64_t kTestRequestGraceDivisor = 5U;
constexpr std::uint32_t kSessionRejectInvalidTagNumber = 0U;
constexpr std::uint32_t kSessionRejectRequiredTagMissing = 1U;
constexpr std::uint32_t kSessionRejectTagNotDefinedForMessage = 2U;
constexpr std::uint32_t kSessionRejectTagSpecifiedWithoutAValue = 4U;
constexpr std::uint32_t kSessionRejectValueIncorrect = 5U;
constexpr std::uint32_t kSessionRejectIncorrectDataFormatForValue = 6U;
constexpr std::uint32_t kSessionRejectDecryptionProblem = 7U;
constexpr std::uint32_t kSessionRejectCompIdProblem = 9U;
constexpr std::uint32_t kSessionRejectSendingTimeAccuracyProblem = 10U;
constexpr std::uint32_t kSessionRejectInvalidMsgType = 11U;
constexpr std::uint32_t kSessionRejectTagAppearsMoreThanOnce = 13U;
constexpr std::uint32_t kSessionRejectTagSpecifiedOutOfRequiredOrder = 14U;
constexpr std::uint32_t kSessionRejectRepeatingGroupFieldsOutOfRequiredOrder = 15U;
constexpr std::uint32_t kSessionRejectIncorrectNumInGroupCount = 16U;
constexpr std::uint32_t kBusinessRejectReasonTag = 380U;
constexpr std::uint32_t kBusinessRejectUnsupportedMessageType = 3U;
constexpr std::uint32_t kBusinessRejectApplicationNotAvailable = 4U;
constexpr std::uint32_t kBusinessRejectConditionallyRequiredFieldMissing = 5U;
constexpr std::size_t kInitialEncodeBufferBytes = 1024U;

struct ApplicationBusinessReject
{
  std::uint32_t reason{ 0U };
  std::string text;
};

auto
IsAdminMessage(std::string_view msg_type) -> bool
{
  return msg_type == "0" || msg_type == "1" || msg_type == "2" || msg_type == "3" || msg_type == "4" ||
         msg_type == "5" || msg_type == "A";
}

auto
ShouldIgnoreInboundDecodeFailure(const base::Status& status) -> bool
{
  return status.code() == base::ErrorCode::kFormatError;
}

auto
MessageRecordFlagValue(store::MessageRecordFlags flag) -> std::uint16_t
{
  return static_cast<std::uint16_t>(flag);
}

auto
HasBoolean(const message::MessageView& view, std::uint32_t tag) -> bool
{
  const auto value = view.get_boolean(tag);
  return value.has_value() && value.value();
}

auto
GetInt(const message::MessageView& view, std::uint32_t tag, std::int64_t fallback) -> std::int64_t
{
  const auto value = view.get_int(tag);
  return value.has_value() ? value.value() : fallback;
}

auto
GetStringView(const message::MessageView& view, std::uint32_t tag) -> std::string_view
{
  const auto value = view.get_string(tag);
  return value.value_or(std::string_view{});
}

auto
ParseDecimal(std::string_view text, std::size_t offset, std::size_t width, unsigned* value) -> bool
{
  if (offset + width > text.size()) {
    return false;
  }

  unsigned parsed = 0U;
  for (std::size_t index = 0; index < width; ++index) {
    const auto ch = text[offset + index];
    if (ch < '0' || ch > '9') {
      return false;
    }
    parsed = parsed * 10U + static_cast<unsigned>(ch - '0');
  }

  *value = parsed;
  return true;
}

auto
ParseFixUtcTimestampNs(std::string_view text) -> std::optional<std::uint64_t>
{
  if (text.size() < 17U || text[8] != '-' || text[11] != ':' || text[14] != ':') {
    return std::nullopt;
  }

  unsigned year = 0U;
  unsigned month = 0U;
  unsigned day = 0U;
  unsigned hour = 0U;
  unsigned minute = 0U;
  unsigned second = 0U;
  if (!ParseDecimal(text, 0U, 4U, &year) || !ParseDecimal(text, 4U, 2U, &month) || !ParseDecimal(text, 6U, 2U, &day) ||
      !ParseDecimal(text, 9U, 2U, &hour) || !ParseDecimal(text, 12U, 2U, &minute) ||
      !ParseDecimal(text, 15U, 2U, &second)) {
    return std::nullopt;
  }

  if (hour > 23U || minute > 59U || second > 60U) {
    return std::nullopt;
  }

  std::uint32_t fractional_ns = 0U;
  if (text.size() > 17U) {
    if (text[17] != '.') {
      return std::nullopt;
    }
    const auto fractional = text.substr(18U);
    if (fractional.empty() || fractional.size() > 9U) {
      return std::nullopt;
    }

    unsigned parsed_fraction = 0U;
    for (const auto ch : fractional) {
      if (ch < '0' || ch > '9') {
        return std::nullopt;
      }
      parsed_fraction = parsed_fraction * 10U + static_cast<unsigned>(ch - '0');
    }
    for (std::size_t index = fractional.size(); index < 9U; ++index) {
      parsed_fraction *= 10U;
    }
    fractional_ns = parsed_fraction;
  }

  using namespace std::chrono;
  const auto ymd =
    year_month_day{ std::chrono::year(static_cast<int>(year)), std::chrono::month(month), std::chrono::day(day) };
  if (!ymd.ok()) {
    return std::nullopt;
  }

  const auto time_point =
    sys_days{ ymd } + hours{ hour } + minutes{ minute } + seconds{ second } + nanoseconds{ fractional_ns };
  const auto epoch_ns = duration_cast<nanoseconds>(time_point.time_since_epoch()).count();
  if (epoch_ns < 0) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(epoch_ns);
}

auto
SendingTimeOutsideThreshold(std::string_view sending_time, std::uint64_t timestamp_ns, std::uint32_t threshold_seconds)
  -> bool
{
  if (threshold_seconds == 0U || sending_time.empty()) {
    return false;
  }

  const auto parsed_sending_time_ns = ParseFixUtcTimestampNs(sending_time);
  if (!parsed_sending_time_ns.has_value()) {
    return false;
  }

  const auto threshold_ns = static_cast<std::uint64_t>(threshold_seconds) * kNanosPerSecond;
  const auto earlier = std::min(parsed_sending_time_ns.value(), timestamp_ns);
  const auto later = std::max(parsed_sending_time_ns.value(), timestamp_ns);
  return later - earlier > threshold_ns;
}

auto
ResolveApplicationBusinessRejectReason(const AdminProtocolConfig& config,
                                       const profile::NormalizedDictionaryView& dictionary,
                                       const codec::DecodedMessageView& decoded) -> std::optional<std::uint32_t>
{
  if (IsAdminMessage(decoded.header.msg_type)) {
    return std::nullopt;
  }

  if (dictionary.find_message(decoded.header.msg_type) == nullptr) {
    return std::nullopt;
  }

  if (!config.application_messages_available) {
    return kBusinessRejectApplicationNotAvailable;
  }

  if (config.supported_app_msg_types.empty()) {
    return std::nullopt;
  }

  const auto supported =
    std::find(config.supported_app_msg_types.begin(), config.supported_app_msg_types.end(), decoded.header.msg_type);
  if (supported != config.supported_app_msg_types.end()) {
    return std::nullopt;
  }
  return kBusinessRejectUnsupportedMessageType;
}

auto
ResolveConditionalApplicationBusinessReject(const codec::DecodedMessageView& decoded)
  -> std::optional<ApplicationBusinessReject>
{
  const auto view = decoded.message.view();
  if (decoded.header.msg_type == "D") {
    const auto ord_type = view.get_char(kOrdType);
    if (ord_type.has_value() && ord_type.value() == '2' && !view.has_field(kPrice)) {
      return ApplicationBusinessReject{
        .reason = kBusinessRejectConditionallyRequiredFieldMissing,
        .text = "NewOrderSingle limit orders require Price",
      };
    }
  }
  return std::nullopt;
}

auto
RejectUnsupportedSessionEncryption(const message::MessageView& view,
                                   std::uint32_t* ref_tag_id,
                                   std::uint32_t* reject_reason,
                                   std::string* text) -> bool
{
  if (!view.has_field(kSecureDataLen) && !view.has_field(kSecureData)) {
    return true;
  }

  *ref_tag_id = view.has_field(kSecureData) ? kSecureData : kSecureDataLen;
  *reject_reason = kSessionRejectDecryptionProblem;
  *text = "session-layer SecureData is not supported by this runtime";
  return false;
}

auto
ParseSequenceResetNewSeq(const message::MessageView& view,
                         std::uint32_t* new_seq_num,
                         std::uint32_t* reject_reason,
                         std::string* text) -> bool
{
  const auto value = view.get_int(kNewSeqNo);
  if (!value.has_value() || value.value() <= 0) {
    *reject_reason = kSessionRejectRequiredTagMissing;
    *text = "SequenceReset requires NewSeqNo";
    return false;
  }

  *new_seq_num = static_cast<std::uint32_t>(value.value());
  return true;
}

auto
BuildAdminMessage(std::string_view msg_type) -> message::MessageBuilder
{
  message::MessageBuilder builder{ std::string(msg_type) };
  builder.set_string(kMsgType, std::string(msg_type));
  return builder;
}

auto
NotifyValidationWarning(std::uint64_t session_id,
                        const codec::ValidationIssue& issue,
                        std::string_view msg_type,
                        ValidationCallback* callback) -> void
{
  if (callback == nullptr) {
    return;
  }
  callback->OnValidationWarning(session_id, issue, msg_type);
}

auto
ShouldRejectValidationIssue(const ValidationPolicy& policy,
                            const codec::ValidationIssue& issue,
                            std::uint64_t session_id,
                            std::string_view msg_type,
                            std::string_view issue_value,
                            ValidationCallback* callback) -> bool
{
  switch (issue.kind) {
    case codec::ValidationIssueKind::kUnknownField:
    case codec::ValidationIssueKind::kFieldNotAllowed:
      switch (policy.unknown_field_action) {
        case UnknownFieldAction::kIgnore:
          NotifyValidationWarning(session_id, issue, msg_type, callback);
          return false;
        case UnknownFieldAction::kLogAndProcess:
          if (callback != nullptr && !callback->OnUnknownField(session_id, issue.tag, issue_value, msg_type)) {
            return true;
          }
          NotifyValidationWarning(session_id, issue, msg_type, callback);
          return false;
        case UnknownFieldAction::kReject:
        default:
          if (policy.reject_unknown_fields) {
            return true;
          }
          NotifyValidationWarning(session_id, issue, msg_type, callback);
          return false;
      }
    case codec::ValidationIssueKind::kTagSpecifiedWithoutAValue:
      if (policy.reject_tag_without_value) {
        return true;
      }
      NotifyValidationWarning(session_id, issue, msg_type, callback);
      return false;
    case codec::ValidationIssueKind::kIncorrectDataFormatForValue:
      switch (policy.malformed_field_action) {
        case MalformedFieldAction::kIgnore:
          NotifyValidationWarning(session_id, issue, msg_type, callback);
          return false;
        case MalformedFieldAction::kLog:
          if (callback != nullptr &&
              !callback->OnMalformedField(session_id, issue.tag, issue_value, msg_type, issue.text)) {
            return true;
          }
          NotifyValidationWarning(session_id, issue, msg_type, callback);
          return false;
        case MalformedFieldAction::kReject:
        default:
          if (policy.reject_incorrect_data_format) {
            return true;
          }
          NotifyValidationWarning(session_id, issue, msg_type, callback);
          return false;
      }
    case codec::ValidationIssueKind::kTagSpecifiedOutOfRequiredOrder:
      if (policy.reject_fields_out_of_order) {
        return true;
      }
      NotifyValidationWarning(session_id, issue, msg_type, callback);
      return false;
    case codec::ValidationIssueKind::kDuplicateField:
      if (policy.reject_duplicate_fields) {
        return true;
      }
      NotifyValidationWarning(session_id, issue, msg_type, callback);
      return false;
    case codec::ValidationIssueKind::kRepeatingGroupFieldsOutOfRequiredOrder:
      if (policy.reject_fields_out_of_order) {
        return true;
      }
      NotifyValidationWarning(session_id, issue, msg_type, callback);
      return false;
    case codec::ValidationIssueKind::kIncorrectNumInGroupCount:
      if (policy.reject_invalid_group_structure) {
        return true;
      }
      NotifyValidationWarning(session_id, issue, msg_type, callback);
      return false;
    case codec::ValidationIssueKind::kEnumValueNotAllowed:
      if (!policy.validate_enum_values) {
        NotifyValidationWarning(session_id, issue, msg_type, callback);
        return false;
      }
      // Route enum violations through malformed_field_action when not kReject.
      switch (policy.malformed_field_action) {
        case MalformedFieldAction::kIgnore:
          NotifyValidationWarning(session_id, issue, msg_type, callback);
          return false;
        case MalformedFieldAction::kLog:
          if (callback != nullptr &&
              !callback->OnMalformedField(session_id, issue.tag, issue_value, msg_type, issue.text)) {
            return true;
          }
          NotifyValidationWarning(session_id, issue, msg_type, callback);
          return false;
        case MalformedFieldAction::kReject:
        default:
          return true;
      }
    default:
      return false;
  }
}

auto
FindFieldValue(message::MessageView view, std::uint32_t tag) -> std::optional<std::string_view>
{
  for (std::size_t index = 0U; index < view.field_count(); ++index) {
    const auto field = view.field_at(index);
    if (field.has_value() && field->tag == tag) {
      return field->string_value;
    }
  }

  for (std::size_t group_index = 0U; group_index < view.group_count(); ++group_index) {
    const auto group = view.group_at(group_index);
    if (!group.has_value()) {
      continue;
    }
    for (const auto entry : *group) {
      auto value = FindFieldValue(entry, tag);
      if (value.has_value()) {
        return value;
      }
    }
  }
  return std::nullopt;
}

auto
FieldValueAllowedByEnum(const profile::NormalizedDictionaryView& dictionary,
                        const profile::FieldDefRecord& field_def,
                        std::string_view value) -> bool
{
  if (field_def.enum_count == 0U || value.empty()) {
    return true;
  }

  const auto enum_values = dictionary.enum_values();
  const auto begin = static_cast<std::size_t>(field_def.enum_offset);
  const auto count = static_cast<std::size_t>(field_def.enum_count);
  if (begin >= enum_values.size()) {
    return true;
  }

  const auto end = std::min(begin + count, enum_values.size());
  for (std::size_t index = begin; index < end; ++index) {
    const auto allowed = dictionary.string_at(enum_values[index].value_offset);
    if (allowed.has_value() && *allowed == value) {
      return true;
    }
  }
  return false;
}

auto
FindEnumValidationIssueInView(const profile::NormalizedDictionaryView& dictionary, message::MessageView view)
  -> std::optional<codec::ValidationIssue>
{
  for (std::size_t index = 0U; index < view.field_count(); ++index) {
    const auto field = view.field_at(index);
    if (!field.has_value()) {
      continue;
    }
    const auto* field_def = dictionary.find_field(field->tag);
    if (field_def == nullptr || FieldValueAllowedByEnum(dictionary, *field_def, field->string_value)) {
      continue;
    }
    return codec::ValidationIssue{
      .kind = codec::ValidationIssueKind::kEnumValueNotAllowed,
      .tag = field->tag,
      .text = "field " + std::to_string(field->tag) + " has an enum value not present in the bound dictionary",
    };
  }

  for (std::size_t group_index = 0U; group_index < view.group_count(); ++group_index) {
    const auto group = view.group_at(group_index);
    if (!group.has_value()) {
      continue;
    }
    for (const auto entry : *group) {
      auto issue = FindEnumValidationIssueInView(dictionary, entry);
      if (issue.has_value()) {
        return issue;
      }
    }
  }
  return std::nullopt;
}

auto
FindEnumValidationIssue(const ValidationPolicy& policy,
                        ValidationCallback* callback,
                        const profile::NormalizedDictionaryView& dictionary,
                        const codec::DecodedMessageView& decoded) -> std::optional<codec::ValidationIssue>
{
  if (!policy.validate_enum_values && callback == nullptr) {
    return std::nullopt;
  }
  return FindEnumValidationIssueInView(dictionary, decoded.message.view());
}

auto
ValidationIssueRejectReason(const codec::ValidationIssue& issue) -> std::uint32_t
{
  switch (issue.kind) {
    case codec::ValidationIssueKind::kFieldNotAllowed:
      return kSessionRejectTagNotDefinedForMessage;
    case codec::ValidationIssueKind::kUnknownField:
      return kSessionRejectInvalidTagNumber;
    case codec::ValidationIssueKind::kTagSpecifiedWithoutAValue:
      return kSessionRejectTagSpecifiedWithoutAValue;
    case codec::ValidationIssueKind::kIncorrectDataFormatForValue:
      return kSessionRejectIncorrectDataFormatForValue;
    case codec::ValidationIssueKind::kDuplicateField:
      return kSessionRejectTagAppearsMoreThanOnce;
    case codec::ValidationIssueKind::kTagSpecifiedOutOfRequiredOrder:
      return kSessionRejectTagSpecifiedOutOfRequiredOrder;
    case codec::ValidationIssueKind::kRepeatingGroupFieldsOutOfRequiredOrder:
      return kSessionRejectRepeatingGroupFieldsOutOfRequiredOrder;
    case codec::ValidationIssueKind::kIncorrectNumInGroupCount:
      return kSessionRejectIncorrectNumInGroupCount;
    case codec::ValidationIssueKind::kEnumValueNotAllowed:
      return kSessionRejectValueIncorrect;
    default:
      return 0U;
  }
}

auto
IsPreActivationState(SessionState state) -> bool
{
  return state == SessionState::kConnected || state == SessionState::kPendingLogon;
}

// Scan the header prefix of an encoded FIX frame to find where the
// application body begins (the byte offset after the last session-level
// header field's SOH).  This mirrors the logic in DecodeRawPassThrough
// but stops as soon as the first non-header field is encountered, so it
// only touches the short header prefix rather than the full frame.
auto
ComputeBodyStartOffset(std::span<const std::byte> frame) -> std::uint32_t
{
  constexpr auto kSoh = static_cast<std::byte>('\x01');
  constexpr auto kEquals = static_cast<std::byte>('=');

  const auto* data = frame.data();
  const auto size = frame.size();
  std::size_t pos = 0;
  std::size_t last_header_end = 0;

  while (pos < size) {
    // Find '='
    std::size_t eq = pos;
    while (eq < size && data[eq] != kEquals) {
      ++eq;
    }
    if (eq >= size || eq == pos) {
      break;
    }

    // Parse tag number
    std::uint32_t tag = 0;
    for (std::size_t i = pos; i < eq; ++i) {
      const auto digit = static_cast<unsigned int>(static_cast<unsigned char>(data[i]) - '0');
      if (digit > 9U) {
        return static_cast<std::uint32_t>(last_header_end);
      }
      tag = tag * 10U + digit;
    }

    // Find SOH after value
    std::size_t soh = eq + 1;
    while (soh < size && data[soh] != kSoh) {
      ++soh;
    }
    if (soh >= size) {
      break;
    }

    const auto field_end = soh + 1;

    if (nimble::codec::tags::IsAggregateSessionEnvelopeTag(tag)) {
      last_header_end = field_end;
    } else {
      // First non-header tag — body starts at the current position
      return static_cast<std::uint32_t>(last_header_end);
    }

    pos = field_end;
  }

  return static_cast<std::uint32_t>(last_header_end);
}

auto
IsActiveAdminState(SessionState state) -> bool
{
  return state == SessionState::kActive || state == SessionState::kAwaitingLogout ||
         state == SessionState::kResendProcessing;
}

auto
ApplySessionSendEnvelope(codec::EncodeOptions* options, SessionSendEnvelopeView envelope) -> void
{
  if (options == nullptr) {
    return;
  }
  options->sender_sub_id = std::string(envelope.sender_sub_id);
  options->target_sub_id = std::string(envelope.target_sub_id);
}

auto
AdminPhaseViolationText(SessionState state, std::string_view msg_type) -> std::optional<std::string>
{
  if (!IsAdminMessage(msg_type)) {
    return std::nullopt;
  }

  if (IsPreActivationState(state) && msg_type != "A" && msg_type != "5") {
    return std::string("received ") + std::string(msg_type) + " before Logon completed";
  }

  if (msg_type == "A" && IsActiveAdminState(state)) {
    return std::string("received unexpected Logon after session activation");
  }

  return std::nullopt;
}

auto
EncodePreEncodedApplicationToBuffer(EncodedApplicationMessageView message,
                                    const codec::EncodeOptions& options,
                                    codec::EncodeBuffer* buffer) -> base::Status
{
  if (buffer == nullptr) {
    return base::Status::InvalidArgument("encode buffer is null");
  }
  if (!message.valid()) {
    return base::Status::InvalidArgument("encoded application message is invalid");
  }

  const char delimiter = options.delimiter == '\0' ? codec::kFixSoh : options.delimiter;
  auto& out = buffer->storage;
  out.clear();
  out.reserve(kInitialEncodeBufferBytes + message.body.size());

  const auto begin_string =
    options.begin_string.empty() ? std::string_view("FIX.4.4") : std::string_view(options.begin_string);

  out.append(kBeginStringPrefix);
  out.append(begin_string);
  out.push_back(delimiter);
  out.append(kBodyLengthPrefix);

  constexpr std::size_t kBodyLengthPlaceholderWidth = 7U;
  const auto body_length_offset = out.size();
  out.append(kBodyLengthPlaceholderWidth, '0');
  out.push_back(delimiter);
  const auto body_start = out.size();

  auto append_string_field = [&](std::string_view prefix, std::string_view value) {
    out.append(prefix);
    out.append(value);
    out.push_back(delimiter);
  };
  auto append_uint_field = [&](std::string_view prefix, std::uint32_t value) {
    char buf[10];
    const auto len = codec::FormatUint32(buf, value);
    out.append(prefix);
    out.append(buf, len);
    out.push_back(delimiter);
  };

  append_string_field(kMsgTypePrefix, message.msg_type);
  append_uint_field(kMsgSeqNumPrefix, options.msg_seq_num == 0U ? 1U : options.msg_seq_num);
  if (!options.sender_comp_id.empty()) {
    append_string_field(kSenderCompIDPrefix, options.sender_comp_id);
  }
  if (!options.sender_sub_id.empty()) {
    append_string_field(kSenderSubIDPrefix, options.sender_sub_id);
  }
  if (!options.target_comp_id.empty()) {
    append_string_field(kTargetCompIDPrefix, options.target_comp_id);
  }
  if (!options.target_sub_id.empty()) {
    append_string_field(kTargetSubIDPrefix, options.target_sub_id);
  }

  codec::UtcTimestampBuffer timestamp_buffer;
  const auto sending_time = options.sending_time.empty()
                              ? codec::CurrentUtcTimestamp(&timestamp_buffer, options.timestamp_resolution)
                              : options.sending_time;
  append_string_field(kSendingTimePrefix, sending_time);

  if (message.msg_type == "A" && !options.default_appl_ver_id.empty()) {
    append_string_field(kDefaultApplVerIDPrefix, options.default_appl_ver_id);
  }
  if (options.poss_dup) {
    out.append(kPossDupFlagYesField);
    out.push_back(delimiter);
  }
  if (options.poss_resend) {
    out.append(kPossResendYesField);
    out.push_back(delimiter);
  }
  if (!options.orig_sending_time.empty()) {
    append_string_field(kOrigSendingTimePrefix, options.orig_sending_time);
  }
  if (!options.on_behalf_of_comp_id.empty()) {
    append_string_field(kOnBehalfOfCompIDPrefix, options.on_behalf_of_comp_id);
  }
  if (!options.deliver_to_comp_id.empty()) {
    append_string_field(kDeliverToCompIDPrefix, options.deliver_to_comp_id);
  }
  if (!message.body.empty()) {
    out.append(reinterpret_cast<const char*>(message.body.data()), message.body.size());
  }

  const auto body_length = static_cast<std::uint32_t>(out.size() - body_start);
  {
    char buf[10];
    const auto len = codec::FormatUint32(buf, body_length);
    if (len > kBodyLengthPlaceholderWidth) {
      return base::Status::FormatError("encoded body length exceeds BodyLength placeholder width");
    }
    out.replace(body_length_offset, kBodyLengthPlaceholderWidth, buf, len);
  }

  const auto checksum = codec::ComputeChecksumSIMD(out.data(), out.size()) % 256U;
  out.append(kCheckSumPrefix);
  std::array<char, 3> checksum_digits{};
  checksum_digits[0] = static_cast<char>('0' + ((checksum / 100U) % 10U));
  checksum_digits[1] = static_cast<char>('0' + ((checksum / 10U) % 10U));
  checksum_digits[2] = static_cast<char>('0' + (checksum % 10U));
  out.append(checksum_digits.data(), checksum_digits.size());
  out.push_back(delimiter);

  return base::Status::Ok();
}

} // namespace

struct AdminProtocol::Impl
{
  AdminProtocolConfig config_{};
  const profile::NormalizedDictionaryView* dictionary_{ nullptr };
  store::SessionStore* store_{ nullptr };
  std::optional<SessionCore> session_{};
  std::string outstanding_test_request_id_;
  bool logout_sent_{ false };
  std::uint64_t test_request_sent_ns_{ 0 };
  std::uint64_t logout_sent_ns_{ 0 };
  std::optional<base::Status> initialization_error_;
  codec::PrecompiledTemplateTable encode_templates_;
  codec::CompiledDecoderTable decode_table_;
  codec::DecodedMessageView inbound_decode_scratch_;
  codec::EncodeBuffer encode_buffer_;
  store::MessageRecordViewRange replay_range_buffer_;
  std::array<std::shared_ptr<ProtocolFrameList>, kReplayFrameBufferPoolSize> replay_frame_buffers_{};
  std::size_t replay_frame_buffer_cursor_{ 0U };
  std::vector<std::vector<std::byte>> deferred_gap_frames_;
};

AdminProtocol::AdminProtocol(AdminProtocolConfig config,
                             const profile::NormalizedDictionaryView& dictionary,
                             store::SessionStore* store)
  : impl_(std::make_unique<Impl>())
{
  impl_->config_ = std::move(config);
  impl_->dictionary_ = &dictionary;
  impl_->store_ = store;
  impl_->session_.emplace(impl_->config_.session);
  impl_->session_->SetWarmupCount(impl_->config_.warmup_message_count);

  // If transport_profile was left at default but begin_string was set, derive
  // the profile from begin_string so callers that only set begin_string still
  // get correct transport semantics.
  if (impl_->config_.transport_profile.begin_string != impl_->config_.begin_string &&
      !impl_->config_.begin_string.empty()) {
    impl_->config_.transport_profile = TransportSessionProfile::FromBeginString(impl_->config_.begin_string);
  }
  impl_->session_->set_transport_profile(&impl_->config_.transport_profile);
  impl_->encode_buffer_.storage.reserve(kInitialEncodeBufferBytes);
  for (auto& replay_frames : impl_->replay_frame_buffers_) {
    replay_frames = std::make_shared<ProtocolFrameList>();
  }

  auto table = codec::PrecompiledTemplateTable::Build(*impl_->dictionary_,
                                                      codec::EncodeTemplateConfig{
                                                        .begin_string = impl_->config_.begin_string,
                                                        .sender_comp_id = impl_->config_.sender_comp_id,
                                                        .target_comp_id = impl_->config_.target_comp_id,
                                                        .default_appl_ver_id = impl_->config_.default_appl_ver_id,
                                                      });
  if (table.ok()) {
    impl_->encode_templates_ = std::move(table).value();
  }

  impl_->decode_table_ = codec::CompiledDecoderTable::Build(*impl_->dictionary_);

  if (impl_->store_ == nullptr) {
    return;
  }

  auto recovery = impl_->store_->LoadRecoveryState(impl_->session_->session_id());
  if (!recovery.ok()) {
    if (recovery.status().code() != base::ErrorCode::kNotFound) {
      impl_->initialization_error_ = recovery.status();
    }
    return;
  }

  auto status = impl_->session_->RestoreSequenceState(recovery.value().next_in_seq, recovery.value().next_out_seq);
  if (!status.ok()) {
    impl_->initialization_error_ = status;
  }
}

AdminProtocol::~AdminProtocol() = default;

AdminProtocol::AdminProtocol(AdminProtocol&& other) noexcept = default;

auto
AdminProtocol::operator=(AdminProtocol&& other) noexcept -> AdminProtocol& = default;

auto
AdminProtocol::session() const -> const SessionCore&
{
  return *impl_->session_;
}

auto
AdminProtocol::mutable_session() -> SessionCore&
{
  return *impl_->session_;
}

#define config_ impl_->config_
#define dictionary_ (*impl_->dictionary_)
#define store_ impl_->store_
#define session_ (*impl_->session_)
#define outstanding_test_request_id_ impl_->outstanding_test_request_id_
#define logout_sent_ impl_->logout_sent_
#define test_request_sent_ns_ impl_->test_request_sent_ns_
#define logout_sent_ns_ impl_->logout_sent_ns_
#define initialization_error_ impl_->initialization_error_
#define encode_templates_ impl_->encode_templates_
#define decode_table_ impl_->decode_table_
#define inbound_decode_scratch_ impl_->inbound_decode_scratch_
#define encode_buffer_ impl_->encode_buffer_
#define replay_range_buffer_ impl_->replay_range_buffer_
#define replay_frame_buffers_ impl_->replay_frame_buffers_
#define replay_frame_buffer_cursor_ impl_->replay_frame_buffer_cursor_
#define deferred_gap_frames_ impl_->deferred_gap_frames_

auto
AdminProtocol::EnsureInitialized() const -> base::Status
{
  if (initialization_error_.has_value()) {
    return *initialization_error_;
  }
  return base::Status::Ok();
}

auto
AdminProtocol::ResolveEncodeTemplate(std::string_view msg_type) -> const codec::FrameEncodeTemplate*
{
  if (msg_type.empty()) {
    return nullptr;
  }
  return encode_templates_.find(msg_type);
}

auto
AdminProtocol::PersistRecoveryState() -> base::Status
{
  if (store_ == nullptr) {
    return base::Status::Ok();
  }

  const auto snapshot = session_.Snapshot();
  return store_->SaveRecoveryState(store::SessionRecoveryState{
    .session_id = snapshot.session_id,
    .next_in_seq = snapshot.next_in_seq,
    .next_out_seq = snapshot.next_out_seq,
    .last_inbound_ns = snapshot.last_inbound_ns,
    .last_outbound_ns = snapshot.last_outbound_ns,
    .active = snapshot.state != SessionState::kDisconnected,
  });
}

auto
AdminProtocol::RefreshSessionStateFromStore() -> base::Status
{
  if (store_ == nullptr) {
    return base::Status::Ok();
  }

  auto status = store_->Refresh();
  if (!status.ok()) {
    return status;
  }

  auto recovery = store_->LoadRecoveryState(session_.session_id());
  if (!recovery.ok()) {
    if (recovery.status().code() == base::ErrorCode::kNotFound) {
      return session_.RestoreSequenceState(1U, 1U);
    }
    return recovery.status();
  }

  return session_.RestoreSequenceState(recovery.value().next_in_seq, recovery.value().next_out_seq);
}

auto
AdminProtocol::ResetSessionState(std::uint32_t next_in_seq, std::uint32_t next_out_seq, bool reset_store)
  -> base::Status
{
  if (reset_store && store_ != nullptr) {
    auto status = store_->ResetSession(session_.session_id());
    if (!status.ok()) {
      return status;
    }
  }

  auto status = session_.RestoreSequenceState(next_in_seq, next_out_seq);
  if (!status.ok()) {
    return status;
  }

  return PersistRecoveryState();
}

auto
AdminProtocol::ReplayCounterpartyExpectedRange(std::uint32_t counterparty_next_expected,
                                               std::uint32_t pre_logon_next_out,
                                               std::uint64_t timestamp_ns,
                                               ProtocolEvent* event) -> base::Status
{
  if (event == nullptr || counterparty_next_expected >= pre_logon_next_out) {
    return base::Status::Ok();
  }

  ProtocolFrameList replay_frames;
  auto status = ReplayOutbound(counterparty_next_expected, pre_logon_next_out - 1U, timestamp_ns, &replay_frames);
  if (!status.ok()) {
    return status;
  }
  for (auto& frame : replay_frames) {
    event->outbound_frames.push_back(std::move(frame));
  }
  return base::Status::Ok();
}

auto
AdminProtocol::ValidateCompIds(const codec::DecodedMessageView& decoded,
                               std::uint32_t* ref_tag_id,
                               std::uint32_t* reject_reason,
                               std::string* text,
                               bool* disconnect) const -> bool
{
  const bool is_logon = decoded.header.msg_type == "A";
  if (!config_.begin_string.empty() && decoded.header.begin_string != config_.begin_string) {
    *ref_tag_id = kBeginString;
    *reject_reason = kSessionRejectValueIncorrect;
    *text = "unexpected BeginString on inbound frame";
    *disconnect = true;
    return false;
  }
  if (config_.validation_policy.enforce_comp_ids && !config_.target_comp_id.empty() &&
      decoded.header.sender_comp_id != config_.target_comp_id) {
    *ref_tag_id = kSenderCompID;
    *reject_reason = kSessionRejectCompIdProblem;
    *text = "unexpected SenderCompID on inbound frame";
    *disconnect = true;
    return false;
  }
  if (config_.validation_policy.enforce_comp_ids && !config_.sender_comp_id.empty() &&
      decoded.header.target_comp_id != config_.sender_comp_id) {
    *ref_tag_id = kTargetCompID;
    *reject_reason = kSessionRejectCompIdProblem;
    *text = "unexpected TargetCompID on inbound frame";
    *disconnect = true;
    return false;
  }
  if (is_logon && config_.validation_policy.require_default_appl_ver_id_on_logon &&
      config_.transport_profile.requires_default_appl_ver_id && decoded.header.default_appl_ver_id.empty()) {
    *ref_tag_id = kDefaultApplVerID;
    *reject_reason = kSessionRejectRequiredTagMissing;
    *text = "FIXT.1.1 logon requires DefaultApplVerID";
    *disconnect = true;
    return false;
  }
  if (is_logon && config_.validation_policy.require_default_appl_ver_id_on_logon &&
      !config_.default_appl_ver_id.empty() && decoded.header.default_appl_ver_id != config_.default_appl_ver_id) {
    *ref_tag_id = kDefaultApplVerID;
    *reject_reason = kSessionRejectValueIncorrect;
    *text = "unexpected DefaultApplVerID on inbound frame";
    *disconnect = true;
    return false;
  }
  return true;
}

auto
AdminProtocol::ValidatePossDup(const codec::DecodedMessageView& decoded) const -> base::Status
{
  if (!config_.validation_policy.require_orig_sending_time_on_poss_dup) {
    return base::Status::Ok();
  }
  if (!decoded.header.poss_dup) {
    return base::Status::Ok();
  }
  if (!decoded.header.orig_sending_time.empty()) {
    return base::Status::Ok();
  }
  return base::Status::InvalidArgument("PossDupFlag requires OrigSendingTime");
}

auto
AdminProtocol::ValidateAdministrativeMessage(const codec::DecodedMessageView& decoded,
                                             std::uint32_t* ref_tag_id,
                                             std::uint32_t* reject_reason,
                                             std::string* text,
                                             bool* disconnect) const -> bool
{
  if (!IsAdminMessage(decoded.header.msg_type)) {
    return true;
  }

  *disconnect = false;

  const auto view = decoded.message.view();
  if (!RejectUnsupportedSessionEncryption(view, ref_tag_id, reject_reason, text)) {
    return false;
  }

  auto* validation_callback = config_.validation_callback.get();
  if (decoded.validation_issue.present() &&
      ShouldRejectValidationIssue(config_.validation_policy,
                                  decoded.validation_issue,
                                  config_.session.session_id,
                                  decoded.header.msg_type,
                                  FindFieldValue(view, decoded.validation_issue.tag).value_or(std::string_view{}),
                                  validation_callback)) {
    *ref_tag_id = decoded.validation_issue.tag;
    *reject_reason = ValidationIssueRejectReason(decoded.validation_issue);
    *text = decoded.validation_issue.text;
    *disconnect = decoded.header.msg_type == "A";
    return false;
  }

  auto enum_issue = FindEnumValidationIssue(config_.validation_policy, validation_callback, dictionary_, decoded);
  if (enum_issue.has_value() &&
      ShouldRejectValidationIssue(config_.validation_policy,
                                  *enum_issue,
                                  config_.session.session_id,
                                  decoded.header.msg_type,
                                  FindFieldValue(view, enum_issue->tag).value_or(std::string_view{}),
                                  validation_callback)) {
    *ref_tag_id = enum_issue->tag;
    *reject_reason = ValidationIssueRejectReason(*enum_issue);
    *text = enum_issue->text;
    *disconnect = decoded.header.msg_type == "A";
    return false;
  }

  if (decoded.header.msg_type == "A") {
    if (!view.has_field(kEncryptMethod)) {
      *ref_tag_id = kEncryptMethod;
      *reject_reason = kSessionRejectRequiredTagMissing;
      *text = "Logon requires EncryptMethod";
      *disconnect = true;
      return false;
    }

    const auto encrypt_method = view.get_int(kEncryptMethod);
    if (!encrypt_method.has_value() || encrypt_method.value() != 0) {
      *ref_tag_id = kEncryptMethod;
      *reject_reason = kSessionRejectValueIncorrect;
      *text = "Logon EncryptMethod must be 0";
      *disconnect = true;
      return false;
    }

    if (!view.has_field(kHeartBtInt)) {
      *ref_tag_id = kHeartBtInt;
      *reject_reason = kSessionRejectRequiredTagMissing;
      *text = "Logon requires HeartBtInt";
      *disconnect = true;
      return false;
    }

    const auto heartbeat_interval = view.get_int(kHeartBtInt);
    if (!heartbeat_interval.has_value() || heartbeat_interval.value() <= 0) {
      *ref_tag_id = kHeartBtInt;
      *reject_reason = kSessionRejectValueIncorrect;
      *text = "Logon HeartBtInt must be positive";
      *disconnect = true;
      return false;
    }
  }

  if (decoded.header.msg_type == "1" && !view.has_field(kTestReqID)) {
    *ref_tag_id = kTestReqID;
    *reject_reason = kSessionRejectRequiredTagMissing;
    *text = "TestRequest requires TestReqID";
    return false;
  }

  if (decoded.header.msg_type == "2") {
    if (!view.has_field(kBeginSeqNo)) {
      *ref_tag_id = kBeginSeqNo;
      *reject_reason = kSessionRejectRequiredTagMissing;
      *text = "ResendRequest requires BeginSeqNo";
      return false;
    }
    if (!view.has_field(kEndSeqNo)) {
      *ref_tag_id = kEndSeqNo;
      *reject_reason = kSessionRejectRequiredTagMissing;
      *text = "ResendRequest requires EndSeqNo";
      return false;
    }

    const auto begin_seq = view.get_int(kBeginSeqNo);
    const auto end_seq = view.get_int(kEndSeqNo);
    if (!begin_seq.has_value() || begin_seq.value() <= 0) {
      *ref_tag_id = kBeginSeqNo;
      *reject_reason = kSessionRejectValueIncorrect;
      *text = "ResendRequest BeginSeqNo must be positive";
      return false;
    }
    if (!end_seq.has_value() || end_seq.value() < 0) {
      *ref_tag_id = kEndSeqNo;
      *reject_reason = kSessionRejectValueIncorrect;
      *text = "ResendRequest EndSeqNo must be zero or positive";
      return false;
    }
    if (end_seq.value() != 0 && begin_seq.value() > end_seq.value()) {
      *ref_tag_id = kEndSeqNo;
      *reject_reason = kSessionRejectValueIncorrect;
      *text = "ResendRequest BeginSeqNo must be less than or equal to EndSeqNo";
      return false;
    }
  }

  return true;
}

auto
AdminProtocol::ValidateApplicationMessage(const codec::DecodedMessageView& decoded,
                                          std::uint32_t* ref_tag_id,
                                          std::uint32_t* reject_reason,
                                          std::string* text) const -> bool
{
  if (IsAdminMessage(decoded.header.msg_type)) {
    return true;
  }

  const auto view = decoded.message.view();
  if (!RejectUnsupportedSessionEncryption(view, ref_tag_id, reject_reason, text)) {
    return false;
  }

  const auto* message_def = dictionary_.find_message(decoded.header.msg_type);
  if (message_def == nullptr) {
    if (!config_.validation_policy.require_known_app_message_type) {
      return true;
    }
    *ref_tag_id = kMsgType;
    *reject_reason = kSessionRejectInvalidMsgType;
    *text = "application message type is not present in the bound dictionary";
    return false;
  }

  auto* validation_callback = config_.validation_callback.get();
  if (decoded.validation_issue.present() &&
      ShouldRejectValidationIssue(config_.validation_policy,
                                  decoded.validation_issue,
                                  config_.session.session_id,
                                  decoded.header.msg_type,
                                  FindFieldValue(view, decoded.validation_issue.tag).value_or(std::string_view{}),
                                  validation_callback)) {
    *ref_tag_id = decoded.validation_issue.tag;
    *reject_reason = ValidationIssueRejectReason(decoded.validation_issue);
    *text = decoded.validation_issue.text;
    return false;
  }

  auto enum_issue = FindEnumValidationIssue(config_.validation_policy, validation_callback, dictionary_, decoded);
  if (enum_issue.has_value() &&
      ShouldRejectValidationIssue(config_.validation_policy,
                                  *enum_issue,
                                  config_.session.session_id,
                                  decoded.header.msg_type,
                                  FindFieldValue(view, enum_issue->tag).value_or(std::string_view{}),
                                  validation_callback)) {
    *ref_tag_id = enum_issue->tag;
    *reject_reason = ValidationIssueRejectReason(*enum_issue);
    *text = enum_issue->text;
    return false;
  }

  if (!config_.validation_policy.require_required_fields_on_app_messages) {
    return true;
  }

  auto typed = message::TypedMessageView::FromParts(dictionary_, decoded.message.view(), message_def);

  std::uint32_t missing_tag = 0U;
  auto status = typed.validate_required_fields(&missing_tag);
  if (status.ok()) {
    return true;
  }

  *ref_tag_id = missing_tag;
  *reject_reason = kSessionRejectRequiredTagMissing;
  *text = status.message();
  return false;
}

auto
AdminProtocol::ReserveReplayStorage(std::size_t frame_count) -> void
{
  replay_range_buffer_.records.reserve(frame_count);
  for (auto& replay_frames : replay_frame_buffers_) {
    if (!replay_frames) {
      continue;
    }
    replay_frames->reserve(frame_count);
  }
}

auto
AdminProtocol::EncodeFrame(const message::Message& message,
                           bool admin,
                           std::uint64_t timestamp_ns,
                           bool persist,
                           bool poss_dup,
                           bool allocate_seq,
                           std::uint16_t extra_record_flags,
                           std::uint32_t seq_override,
                           std::string_view orig_sending_time,
                           SessionSendEnvelopeView envelope) -> base::Result<EncodedFrame>
{
  return EncodeFrame(message.view(),
                     admin,
                     timestamp_ns,
                     persist,
                     poss_dup,
                     allocate_seq,
                     extra_record_flags,
                     seq_override,
                     std::move(orig_sending_time),
                     envelope);
}

auto
AdminProtocol::EncodeFrame(message::MessageView message,
                           bool admin,
                           std::uint64_t timestamp_ns,
                           bool persist,
                           bool poss_dup,
                           bool allocate_seq,
                           std::uint16_t extra_record_flags,
                           std::uint32_t seq_override,
                           std::string_view orig_sending_time,
                           SessionSendEnvelopeView envelope) -> base::Result<EncodedFrame>
{
  encode_buffer_.clear();

  std::uint32_t seq_num = seq_override;
  if (allocate_seq) {
    auto allocated = session_.AllocateOutboundSeq();
    if (!allocated.ok()) {
      return allocated.status();
    }
    seq_num = allocated.value();
  }

  codec::EncodeOptions options;
  options.begin_string = config_.begin_string;
  options.sender_comp_id = config_.sender_comp_id;
  options.target_comp_id = config_.target_comp_id;
  options.default_appl_ver_id = config_.default_appl_ver_id;
  options.orig_sending_time = orig_sending_time;
  options.timestamp_resolution = config_.timestamp_resolution;
  options.msg_seq_num = seq_num;
  options.poss_dup = poss_dup;
  ApplySessionSendEnvelope(&options, envelope);

  const auto msg_type = message.msg_type();
  auto encoded_status = [&]() -> base::Status {
    if (const auto* template_encoder = ResolveEncodeTemplate(msg_type); template_encoder != nullptr) {
      auto templated = template_encoder->EncodeToBuffer(message, options, &encode_buffer_);
      if (templated.ok()) {
        return templated;
      }
    }
    return codec::EncodeFixMessageToBuffer(message, dictionary_, options, &encode_buffer_);
  }();
  if (!encoded_status.ok()) {
    return encoded_status;
  }

  return FinalizeEncodedFrame(msg_type, admin, timestamp_ns, persist, poss_dup, extra_record_flags, seq_num);
}

auto
AdminProtocol::EncodeFrame(EncodedApplicationMessageView message,
                           bool admin,
                           std::uint64_t timestamp_ns,
                           bool persist,
                           bool poss_dup,
                           bool allocate_seq,
                           std::uint16_t extra_record_flags,
                           std::uint32_t seq_override,
                           std::string_view orig_sending_time,
                           SessionSendEnvelopeView envelope) -> base::Result<EncodedFrame>
{
  encode_buffer_.clear();

  std::uint32_t seq_num = seq_override;
  if (allocate_seq) {
    auto allocated = session_.AllocateOutboundSeq();
    if (!allocated.ok()) {
      return allocated.status();
    }
    seq_num = allocated.value();
  }

  codec::EncodeOptions options;
  options.begin_string = config_.begin_string;
  options.sender_comp_id = config_.sender_comp_id;
  options.target_comp_id = config_.target_comp_id;
  options.default_appl_ver_id = config_.default_appl_ver_id;
  options.orig_sending_time = orig_sending_time;
  options.timestamp_resolution = config_.timestamp_resolution;
  options.msg_seq_num = seq_num;
  options.poss_dup = poss_dup;
  ApplySessionSendEnvelope(&options, envelope);

  auto encoded_status = EncodePreEncodedApplicationToBuffer(message, options, &encode_buffer_);
  if (!encoded_status.ok()) {
    return encoded_status;
  }

  return FinalizeEncodedFrame(message.msg_type, admin, timestamp_ns, persist, poss_dup, extra_record_flags, seq_num);
}

auto
AdminProtocol::FinalizeEncodedFrame(std::string_view msg_type,
                                    bool admin,
                                    std::uint64_t timestamp_ns,
                                    bool persist,
                                    bool poss_dup,
                                    std::uint16_t extra_record_flags,
                                    std::uint32_t seq_num) -> base::Result<EncodedFrame>
{

  EncodedFrame encoded_frame;
  encoded_frame.bytes.assign(encode_buffer_.bytes());
  encoded_frame.msg_type = std::string(msg_type);
  encoded_frame.admin = admin;

  auto status = session_.RecordOutboundActivity(timestamp_ns);
  if (!status.ok()) {
    return status;
  }

  if (persist && store_ != nullptr) {
    std::uint16_t flags = extra_record_flags;
    if (admin) {
      flags |= MessageRecordFlagValue(store::MessageRecordFlags::kAdmin);
    }
    if (poss_dup) {
      flags |= MessageRecordFlagValue(store::MessageRecordFlags::kPossDup);
    }

    const auto body_offset = ComputeBodyStartOffset(encoded_frame.bytes.view());

    const auto snapshot = session_.Snapshot();
    status = store_->SaveOutboundViewAndRecoveryState(
      store::MessageRecordView{
        .session_id = session_.session_id(),
        .seq_num = seq_num,
        .timestamp_ns = timestamp_ns,
        .flags = flags,
        .payload = encoded_frame.bytes.view(),
        .body_start_offset = body_offset,
      },
      store::SessionRecoveryState{
        .session_id = snapshot.session_id,
        .next_in_seq = snapshot.next_in_seq,
        .next_out_seq = snapshot.next_out_seq,
        .last_inbound_ns = snapshot.last_inbound_ns,
        .last_outbound_ns = snapshot.last_outbound_ns,
        .active = snapshot.state != SessionState::kDisconnected,
      });
    if (!status.ok()) {
      return status;
    }
  } else {
    status = PersistRecoveryState();
    if (!status.ok()) {
      return status;
    }
  }

  return encoded_frame;
}

auto
AdminProtocol::BuildLogonFrame(std::uint64_t timestamp_ns, bool reset_seq_num) -> base::Result<EncodedFrame>
{
  auto builder = BuildAdminMessage("A");
  builder.set_int(kEncryptMethod, 0)
    .set_int(kHeartBtInt, static_cast<std::int64_t>(config_.heartbeat_interval_seconds));
  if (reset_seq_num) {
    builder.set_boolean(kResetSeqNumFlag, true);
  }
  if (config_.send_next_expected_msg_seq_num && config_.transport_profile.supports_next_expected_msg_seq_num) {
    const auto snapshot = session_.Snapshot();
    builder.set_int(kNextExpectedMsgSeqNum, static_cast<std::int64_t>(snapshot.next_in_seq));
  }
  return EncodeFrame(std::move(builder).build(), true, timestamp_ns, true, false, true, 0U);
}

auto
AdminProtocol::BuildHeartbeatFrame(std::uint64_t timestamp_ns, std::string_view test_request_id)
  -> base::Result<EncodedFrame>
{
  auto builder = BuildAdminMessage("0");
  if (!test_request_id.empty()) {
    builder.set_string(kTestReqID, std::string(test_request_id));
  }
  return EncodeFrame(std::move(builder).build(), true, timestamp_ns, true, false, true, 0U);
}

auto
AdminProtocol::BuildTestRequestFrame(std::uint64_t timestamp_ns, std::string_view test_request_id)
  -> base::Result<EncodedFrame>
{
  auto builder = BuildAdminMessage("1");
  builder.set_string(kTestReqID, std::string(test_request_id));
  return EncodeFrame(std::move(builder).build(), true, timestamp_ns, true, false, true, 0U);
}

auto
AdminProtocol::BuildResendRequestFrame(std::uint32_t begin_seq, std::uint32_t end_seq, std::uint64_t timestamp_ns)
  -> base::Result<EncodedFrame>
{
  auto builder = BuildAdminMessage("2");
  builder.set_int(kBeginSeqNo, static_cast<std::int64_t>(begin_seq))
    .set_int(kEndSeqNo, static_cast<std::int64_t>(end_seq));
  return EncodeFrame(std::move(builder).build(), true, timestamp_ns, true, false, true, 0U);
}

auto
AdminProtocol::BuildGapFillFrame(std::uint32_t begin_seq, std::uint32_t new_seq_num, std::uint64_t timestamp_ns)
  -> base::Result<EncodedFrame>
{
  auto builder = BuildAdminMessage("4");
  builder.set_boolean(kGapFillFlag, true).set_int(kNewSeqNo, static_cast<std::int64_t>(new_seq_num));
  builder.set_boolean(kPossDupFlag, true);
  return EncodeFrame(std::move(builder).build(),
                     true,
                     timestamp_ns,
                     false,
                     true,
                     false,
                     MessageRecordFlagValue(store::MessageRecordFlags::kGapFill),
                     begin_seq);
}

auto
AdminProtocol::BuildRejectFrame(std::uint32_t ref_seq_num,
                                std::string_view ref_msg_type,
                                std::uint32_t ref_tag_id,
                                std::uint32_t reject_reason,
                                std::string text,
                                std::uint64_t timestamp_ns) -> base::Result<EncodedFrame>
{
  auto builder = BuildAdminMessage("3");
  builder.set_int(kRefSeqNum, static_cast<std::int64_t>(ref_seq_num));
  builder.set_string(kRefMsgType, std::string(ref_msg_type));
  if (ref_tag_id != 0U) {
    builder.set_int(kRefTagID, static_cast<std::int64_t>(ref_tag_id));
  }
  builder.set_int(kRejectReason, static_cast<std::int64_t>(reject_reason));
  if (!text.empty()) {
    builder.set_string(kText, std::move(text));
  }
  return EncodeFrame(std::move(builder).build(), true, timestamp_ns, true, false, true, 0U);
}

auto
AdminProtocol::RejectInbound(const codec::DecodedMessageView& decoded,
                             std::uint32_t ref_tag_id,
                             std::uint32_t reject_reason,
                             std::string_view text,
                             std::uint64_t timestamp_ns,
                             bool disconnect) -> base::Result<ProtocolEvent>
{
  ProtocolEvent event;
  if (reject_reason == kSessionRejectInvalidMsgType) {
    event.warnings.push_back(std::string(text));
  } else {
    event.errors.push_back(std::string(text));
  }
  if (disconnect && decoded.header.msg_type == "A") {
    auto logout = BeginLogout(std::string(text), timestamp_ns);
    if (!logout.ok()) {
      return logout.status();
    }
    event.outbound_frames.push_back(std::move(logout).value());
    event.disconnect = true;
    return event;
  }

  auto reject = BuildRejectFrame(decoded.header.msg_seq_num,
                                 decoded.message.view().msg_type(),
                                 ref_tag_id,
                                 reject_reason,
                                 std::string(text),
                                 timestamp_ns);
  if (!reject.ok()) {
    return reject.status();
  }
  event.outbound_frames.push_back(std::move(reject).value());
  event.disconnect = disconnect;
  return event;
}

auto
AdminProtocol::ReplayOutbound(std::uint32_t begin_seq,
                              std::uint32_t end_seq,
                              std::uint64_t timestamp_ns,
                              ProtocolFrameList* frames) -> base::Status
{
  if (frames == nullptr) {
    return base::Status::InvalidArgument("replay frame list is null");
  }

  frames->clear();
  if (store_ == nullptr) {
    return base::Status::Ok();
  }

  const auto snapshot = session_.Snapshot();
  const auto bounded_end = (end_seq == 0U || end_seq >= snapshot.next_out_seq) ? snapshot.next_out_seq - 1U : end_seq;
  if (bounded_end < begin_seq || bounded_end == 0U) {
    return base::Status::Ok();
  }

  frames->reserve(static_cast<std::size_t>(bounded_end - begin_seq + 1U));

  auto status = store_->LoadOutboundRangeViews(session_.session_id(), begin_seq, bounded_end, &replay_range_buffer_);
  if (!status.ok()) {
    return status;
  }

  // Pre-build replay options — only seq_num and orig_sending_time change per
  // message.
  codec::UtcTimestampBuffer ts_buf;
  const auto sending_time = codec::CurrentUtcTimestamp(&ts_buf, config_.timestamp_resolution);

  codec::ReplayOptions replay_opts;
  replay_opts.sender_comp_id = config_.sender_comp_id;
  replay_opts.target_comp_id = config_.target_comp_id;
  replay_opts.begin_string = config_.begin_string;
  replay_opts.default_appl_ver_id = config_.default_appl_ver_id;
  replay_opts.sending_time = sending_time;

  std::uint32_t seq = begin_seq;
  std::size_t record_index = 0U;
  const auto& records = replay_range_buffer_.records;
  while (seq <= bounded_end) {
    const store::MessageRecordView* record = nullptr;
    if (record_index < records.size() && records[record_index].seq_num == seq) {
      record = &records[record_index];
    }
    const bool replayable = record != nullptr && !record->is_admin();
    if (!replayable) {
      const auto gap_begin = seq;
      while (seq <= bounded_end) {
        if (record_index < records.size() && records[record_index].seq_num == seq) {
          if (!records[record_index].is_admin()) {
            break;
          }
          ++record_index;
        }
        ++seq;
      }
      auto gap_fill = BuildGapFillFrame(gap_begin, seq, timestamp_ns);
      if (!gap_fill.ok()) {
        return gap_fill.status();
      }
      frames->push_back(std::move(gap_fill).value());
      continue;
    }

    // Lightweight header-only scan — skip checksum verification since we
    // encoded these bytes.
    auto parsed = record->body_start_offset == 0U
                    ? codec::DecodeRawPassThrough(record->payload, codec::kFixSoh, false)
                    : codec::DecodeRawPassThrough(record->payload, record->body_start_offset, codec::kFixSoh, false);
    if (!parsed.ok()) {
      return parsed.status();
    }

    replay_opts.msg_seq_num = record->seq_num;
    replay_opts.orig_sending_time = parsed.value().sending_time;

    EncodedFrame frame;
    status = codec::EncodeReplayInto(parsed.value(), replay_opts, &frame.bytes);
    if (!status.ok()) {
      return status;
    }

    frame.msg_type = std::string(parsed.value().msg_type);
    frame.admin = false;
    frames->push_back(std::move(frame));

    ++record_index;
    ++seq;
  }

  return base::Status::Ok();
}

auto
AdminProtocol::AcquireReplayFrameBuffer() -> std::shared_ptr<ProtocolFrameList>
{
  for (std::size_t attempt = 0U; attempt < replay_frame_buffers_.size(); ++attempt) {
    const auto index = (replay_frame_buffer_cursor_ + attempt) % replay_frame_buffers_.size();
    auto& replay_frames = replay_frame_buffers_[index];
    if (!replay_frames || replay_frames.use_count() != 1U) {
      continue;
    }

    replay_frame_buffer_cursor_ = index;
    replay_frames->clear();
    return replay_frames;
  }

  return {};
}

auto
AdminProtocol::OnTransportConnected(std::uint64_t timestamp_ns) -> base::Result<ProtocolEvent>
{
  ProtocolEvent event;

  auto status = EnsureInitialized();
  if (!status.ok()) {
    return status;
  }

  if (config_.refresh_on_logon) {
    status = RefreshSessionStateFromStore();
    if (!status.ok()) {
      return status;
    }
  }

  status = session_.OnTransportConnected();
  if (!status.ok()) {
    return status;
  }
  status = PersistRecoveryState();
  if (!status.ok()) {
    return status;
  }

  if (!config_.session.is_initiator) {
    return event;
  }

  if (config_.reset_seq_num_on_logon) {
    status = ResetSessionState(1U, 1U, true);
    if (!status.ok()) {
      return status;
    }
  }

  status = session_.BeginLogon();
  if (!status.ok()) {
    return status;
  }

  auto logon = BuildLogonFrame(timestamp_ns, config_.reset_seq_num_on_logon);
  if (!logon.ok()) {
    return logon.status();
  }
  event.outbound_frames.push_back(std::move(logon).value());
  return event;
}

auto
AdminProtocol::OnTransportClosed() -> base::Status
{
  auto status = EnsureInitialized();
  if (!status.ok()) {
    return status;
  }

  const auto state_before_close = session_.state();
  const bool reset_for_logout = config_.reset_seq_num_on_logout && state_before_close == SessionState::kAwaitingLogout;
  const bool reset_for_disconnect =
    config_.reset_seq_num_on_disconnect && state_before_close != SessionState::kAwaitingLogout;

  outstanding_test_request_id_.clear();
  logout_sent_ = false;
  test_request_sent_ns_ = 0U;
  logout_sent_ns_ = 0U;
  deferred_gap_frames_.clear();
  status = session_.OnTransportClosed();
  if (!status.ok()) {
    return status;
  }

  if (reset_for_logout || reset_for_disconnect) {
    return ResetSessionState(1U, 1U, true);
  }
  return PersistRecoveryState();
}

auto
AdminProtocol::OnInbound(std::span<const std::byte> frame, std::uint64_t timestamp_ns) -> base::Result<ProtocolEvent>
{
  auto decode_status = codec::DecodeFixMessageView(frame,
                                                   dictionary_,
                                                   decode_table_,
                                                   &inbound_decode_scratch_,
                                                   codec::kFixSoh,
                                                   config_.validation_policy.verify_checksum);
  if (!decode_status.ok()) {
    if (!ShouldIgnoreInboundDecodeFailure(decode_status)) {
      return decode_status;
    }
    auto status = EnsureInitialized();
    if (!status.ok()) {
      return status;
    }
    // Official session warning cases treat malformed inbound frames as ignored warnings.
    status = session_.RecordInboundActivity(timestamp_ns);
    if (!status.ok()) {
      return status;
    }
    status = PersistRecoveryState();
    if (!status.ok()) {
      return status;
    }
    ProtocolEvent event;
    if (decode_status.message().empty()) {
      event.warnings.push_back("malformed inbound frame ignored");
    } else {
      event.warnings.push_back(std::string(decode_status.message()));
    }
    return event;
  }
  auto event = OnInbound(inbound_decode_scratch_, timestamp_ns);
  if (!event.ok()) {
    return event.status();
  }
  if (event.value().application_messages.size() == 1U) {
    auto& message = event.value().application_messages.front();
    if (message.valid() && !message.owns_storage()) {
      event.value().AdoptParsedApplicationMessage(std::move(inbound_decode_scratch_.message),
                                                  inbound_decode_scratch_.raw);
    }
  } else {
    event.value().MaterializeApplicationMessages();
  }
  auto drain_status = DrainDeferredGapFrames(timestamp_ns, &event.value());
  if (!drain_status.ok()) {
    return drain_status;
  }
  return std::move(event).value();
}

auto
AdminProtocol::OnInbound(std::vector<std::byte>&& frame, std::uint64_t timestamp_ns) -> base::Result<ProtocolEvent>
{
  auto decode_status = codec::DecodeFixMessageView(std::span<const std::byte>(frame.data(), frame.size()),
                                                   dictionary_,
                                                   decode_table_,
                                                   &inbound_decode_scratch_,
                                                   codec::kFixSoh,
                                                   config_.validation_policy.verify_checksum);
  if (!decode_status.ok()) {
    if (!ShouldIgnoreInboundDecodeFailure(decode_status)) {
      return decode_status;
    }
    auto status = EnsureInitialized();
    if (!status.ok()) {
      return status;
    }
    // Official session warning cases treat malformed inbound frames as ignored warnings.
    status = session_.RecordInboundActivity(timestamp_ns);
    if (!status.ok()) {
      return status;
    }
    status = PersistRecoveryState();
    if (!status.ok()) {
      return status;
    }
    ProtocolEvent event;
    if (decode_status.message().empty()) {
      event.warnings.push_back("malformed inbound frame ignored");
    } else {
      event.warnings.push_back(std::string(decode_status.message()));
    }
    return event;
  }
  auto event = OnInbound(inbound_decode_scratch_, timestamp_ns);
  if (!event.ok()) {
    return event.status();
  }
  if (event.value().application_messages.size() == 1U) {
    auto& message = event.value().application_messages.front();
    if (message.valid() && !message.owns_storage()) {
      event.value().AdoptParsedApplicationMessage(std::move(inbound_decode_scratch_.message), std::move(frame));
    }
  } else {
    event.value().MaterializeApplicationMessages();
  }
  auto drain_status = DrainDeferredGapFrames(timestamp_ns, &event.value());
  if (!drain_status.ok()) {
    return drain_status;
  }
  return std::move(event).value();
}

auto
AdminProtocol::OnInbound(const codec::DecodedMessageView& decoded, std::uint64_t timestamp_ns)
  -> base::Result<ProtocolEvent>
{
  ProtocolEvent event;

  auto status = EnsureInitialized();
  if (!status.ok()) {
    return status;
  }

  const auto view = decoded.message.view();
  const auto msg_type = view.msg_type();
  const bool is_logon = msg_type == "A";
  const bool is_sequence_reset = msg_type == "4";
  const bool inbound_gap_fill = is_sequence_reset && HasBoolean(view, kGapFillFlag);
  const bool inbound_logon_reset = is_logon && HasBoolean(view, kResetSeqNumFlag);
  const bool acceptor_config_logon_reset =
    is_logon && !config_.session.is_initiator && config_.reset_seq_num_on_logon && !inbound_logon_reset;
  auto snapshot_before = session_.Snapshot();

  std::uint32_t ref_tag_id = 0U;
  std::uint32_t reject_reason = 0U;
  std::string reject_text;
  bool disconnect = false;
  const auto consume_expected_inbound_before_reject = [&]() -> base::Status {
    const auto state = session_.state();
    if (state != SessionState::kActive && state != SessionState::kAwaitingLogout &&
        state != SessionState::kResendProcessing) {
      return base::Status::Ok();
    }
    if (decoded.header.msg_seq_num != snapshot_before.next_in_seq) {
      return base::Status::Ok();
    }

    auto observe_status = session_.ObserveInboundSeq(decoded.header.msg_seq_num);
    if (!observe_status.ok()) {
      return observe_status;
    }
    if (session_.ConsumeResendCompleted()) {
      outstanding_test_request_id_.clear();
    }

    observe_status = session_.RecordInboundActivity(timestamp_ns);
    if (!observe_status.ok()) {
      return observe_status;
    }
    return PersistRecoveryState();
  };
  const auto reject_then_logout =
    [&](std::uint32_t ref_tag_id, std::uint32_t reject_reason, std::string text) -> base::Result<ProtocolEvent> {
    auto reject_event = RejectInbound(decoded, ref_tag_id, reject_reason, text, timestamp_ns, false);
    if (!reject_event.ok()) {
      return reject_event.status();
    }
    auto logout = BeginLogout(text, timestamp_ns);
    if (!logout.ok()) {
      return logout.status();
    }
    reject_event.value().outbound_frames.push_back(std::move(logout).value());
    reject_event.value().disconnect = true;
    return reject_event;
  };
  if (!ValidateCompIds(decoded, &ref_tag_id, &reject_reason, &reject_text, &disconnect)) {
    status = consume_expected_inbound_before_reject();
    if (!status.ok()) {
      return status;
    }
    if (disconnect && reject_reason == kSessionRejectCompIdProblem) {
      if (snapshot_before.state == SessionState::kConnected && is_logon &&
          decoded.header.msg_seq_num == snapshot_before.next_in_seq) {
        status = session_.ObserveInboundSeq(decoded.header.msg_seq_num);
        if (!status.ok()) {
          return status;
        }
        status = session_.RecordInboundActivity(timestamp_ns);
        if (!status.ok()) {
          return status;
        }
        status = PersistRecoveryState();
        if (!status.ok()) {
          return status;
        }
      }
      return reject_then_logout(ref_tag_id, reject_reason, std::move(reject_text));
    }
    return RejectInbound(decoded, ref_tag_id, reject_reason, std::move(reject_text), timestamp_ns, disconnect);
  }

  status = ValidatePossDup(decoded);
  if (!status.ok()) {
    auto consume_status = consume_expected_inbound_before_reject();
    if (!consume_status.ok()) {
      return consume_status;
    }
    return RejectInbound(decoded,
                         kOrigSendingTime,
                         kSessionRejectRequiredTagMissing,
                         status.message(),
                         timestamp_ns,
                         decoded.header.msg_type == "A");
  }
  if (decoded.header.poss_dup && !decoded.header.orig_sending_time.empty() && !decoded.header.sending_time.empty() &&
      decoded.header.orig_sending_time > decoded.header.sending_time) {
    auto consume_status = consume_expected_inbound_before_reject();
    if (!consume_status.ok()) {
      return consume_status;
    }
    return RejectInbound(decoded,
                         kSendingTime,
                         kSessionRejectSendingTimeAccuracyProblem,
                         "OrigSendingTime must not be later than SendingTime",
                         timestamp_ns,
                         decoded.header.msg_type == "A");
  }
  if (SendingTimeOutsideThreshold(decoded.header.sending_time, timestamp_ns, config_.sending_time_threshold_seconds)) {
    auto consume_status = consume_expected_inbound_before_reject();
    if (!consume_status.ok()) {
      return consume_status;
    }
    return reject_then_logout(
      kSendingTime, kSessionRejectSendingTimeAccuracyProblem, "SendingTime is outside the configured tolerance");
  }

  if (is_logon && config_.refresh_on_logon) {
    status = RefreshSessionStateFromStore();
    if (!status.ok()) {
      return status;
    }
  }

  if (inbound_logon_reset || acceptor_config_logon_reset) {
    const auto next_out_seq = config_.session.is_initiator ? snapshot_before.next_out_seq : 1U;
    status = ResetSessionState(1U, next_out_seq, !config_.session.is_initiator);
    if (!status.ok()) {
      return status;
    }
    snapshot_before = session_.Snapshot();
  }

  const auto phase_violation_text = AdminPhaseViolationText(snapshot_before.state, msg_type);
  const auto send_phase_violation_logout = [&](std::string text) -> base::Result<ProtocolEvent> {
    ProtocolEvent phase_event;
    phase_event.errors.push_back(text);
    if (!config_.session.is_initiator && snapshot_before.state == SessionState::kConnected && msg_type != "A") {
      phase_event.disconnect = true;
      return phase_event;
    }
    auto logout = BeginLogout(std::move(text), timestamp_ns);
    if (!logout.ok()) {
      return logout.status();
    }
    phase_event.outbound_frames.push_back(std::move(logout).value());
    phase_event.disconnect = true;
    return phase_event;
  };

  const auto record_inbound_liveness = [&]() -> base::Status {
    auto activity_status = session_.RecordInboundActivity(timestamp_ns);
    if (!activity_status.ok()) {
      return activity_status;
    }

    if (store_ != nullptr) {
      std::uint16_t inbound_flags = 0U;
      if (IsAdminMessage(decoded.header.msg_type)) {
        inbound_flags |= MessageRecordFlagValue(store::MessageRecordFlags::kAdmin);
      }
      if (decoded.header.poss_dup) {
        inbound_flags |= MessageRecordFlagValue(store::MessageRecordFlags::kPossDup);
      }
      const auto snapshot = session_.Snapshot();
      return store_->SaveInboundViewAndRecoveryState(
        store::MessageRecordView{
          .session_id = session_.session_id(),
          .seq_num = decoded.header.msg_seq_num,
          .timestamp_ns = timestamp_ns,
          .flags = inbound_flags,
          .payload = decoded.raw,
        },
        store::SessionRecoveryState{
          .session_id = snapshot.session_id,
          .next_in_seq = snapshot.next_in_seq,
          .next_out_seq = snapshot.next_out_seq,
          .last_inbound_ns = snapshot.last_inbound_ns,
          .last_outbound_ns = snapshot.last_outbound_ns,
          .active = snapshot.state != SessionState::kDisconnected,
        });
    }

    return PersistRecoveryState();
  };

  if (phase_violation_text.has_value() && !config_.session.is_initiator &&
      snapshot_before.state == SessionState::kConnected && msg_type != "A") {
    status = session_.RecordInboundActivity(timestamp_ns);
    if (!status.ok()) {
      return status;
    }
    status = PersistRecoveryState();
    if (!status.ok()) {
      return status;
    }
    event.errors.push_back(*phase_violation_text);
    event.disconnect = true;
    return event;
  }

  if (decoded.header.msg_seq_num < snapshot_before.next_in_seq) {
    if (is_sequence_reset) {
      std::uint32_t new_seq_num = 0U;
      if (!ParseSequenceResetNewSeq(view, &new_seq_num, &reject_reason, &reject_text)) {
        return RejectInbound(decoded, kNewSeqNo, reject_reason, std::move(reject_text), timestamp_ns, false);
      }

      if (inbound_gap_fill && !decoded.header.poss_dup) {
        auto logout = BeginLogout("MsgSeqNum too low, expecting " + std::to_string(snapshot_before.next_in_seq) +
                                    " but received " + std::to_string(decoded.header.msg_seq_num),
                                  timestamp_ns);
        if (!logout.ok()) {
          return logout.status();
        }
        event.errors.push_back("MsgSeqNum too low, expecting " + std::to_string(snapshot_before.next_in_seq) +
                               " but received " + std::to_string(decoded.header.msg_seq_num));
        event.outbound_frames.push_back(std::move(logout).value());
        event.disconnect = true;
        return event;
      }

      if (new_seq_num < snapshot_before.next_in_seq) {
        return RejectInbound(decoded,
                             kNewSeqNo,
                             kSessionRejectValueIncorrect,
                             "SequenceReset NewSeqNo must not move inbound sequence backwards",
                             timestamp_ns,
                             false);
      }

      if (new_seq_num > snapshot_before.next_in_seq) {
        status = session_.AdvanceInboundExpectedSeq(new_seq_num);
        if (!status.ok()) {
          return status;
        }
        if (session_.ConsumeResendCompleted()) {
          outstanding_test_request_id_.clear();
        }
      }

      status = record_inbound_liveness();
      if (!status.ok()) {
        return status;
      }
      return event;
    }

    if (decoded.header.poss_dup) {
      status = record_inbound_liveness();
      if (!status.ok()) {
        return status;
      }
      return event;
    }
    if (!config_.validation_policy.reject_on_stale_msg_seq_num) {
      status = record_inbound_liveness();
      if (!status.ok()) {
        return status;
      }
      return event;
    }
    auto logout = BeginLogout("received stale inbound FIX sequence number", timestamp_ns);
    if (!logout.ok()) {
      return logout.status();
    }
    event.errors.push_back("received stale inbound FIX sequence number");
    event.outbound_frames.push_back(std::move(logout).value());
    event.disconnect = true;
    return event;
  }

  if (is_sequence_reset && !inbound_gap_fill) {
    std::uint32_t new_seq_num = 0U;
    if (!ParseSequenceResetNewSeq(view, &new_seq_num, &reject_reason, &reject_text)) {
      return RejectInbound(decoded, kNewSeqNo, reject_reason, std::move(reject_text), timestamp_ns, false);
    }

    if (new_seq_num < snapshot_before.next_in_seq) {
      return RejectInbound(decoded,
                           kNewSeqNo,
                           kSessionRejectValueIncorrect,
                           "SequenceReset NewSeqNo must not move inbound sequence backwards",
                           timestamp_ns,
                           false);
    }

    if (new_seq_num > snapshot_before.next_in_seq) {
      status = session_.AdvanceInboundExpectedSeq(new_seq_num);
      if (!status.ok()) {
        return status;
      }
      if (session_.ConsumeResendCompleted()) {
        outstanding_test_request_id_.clear();
      }
    }

    status = record_inbound_liveness();
    if (!status.ok()) {
      return status;
    }

    if (new_seq_num == snapshot_before.next_in_seq) {
      event.warnings.push_back("SequenceReset NewSeqNo equals expected inbound sequence");
    }

    if (phase_violation_text.has_value()) {
      return send_phase_violation_logout(*phase_violation_text);
    }

    ref_tag_id = 0U;
    reject_reason = 0U;
    reject_text.clear();
    disconnect = false;
    if (!ValidateAdministrativeMessage(decoded, &ref_tag_id, &reject_reason, &reject_text, &disconnect)) {
      return RejectInbound(decoded, ref_tag_id, reject_reason, std::move(reject_text), timestamp_ns, disconnect);
    }

    return event;
  }

  status = session_.ObserveInboundSeq(decoded.header.msg_seq_num);
  if (session_.ConsumeResendCompleted()) {
    outstanding_test_request_id_.clear();
  }
  if (!status.ok()) {
    if (decoded.header.msg_seq_num > snapshot_before.next_in_seq && phase_violation_text.has_value()) {
      status = session_.RecordInboundActivity(timestamp_ns);
      if (!status.ok()) {
        return status;
      }
      return send_phase_violation_logout(*phase_violation_text);
    }
    if (decoded.header.msg_seq_num > snapshot_before.next_in_seq && session_.pending_resend().has_value()) {
      status = session_.RecordInboundActivity(timestamp_ns);
      if (!status.ok()) {
        return status;
      }
      const auto& pending = *session_.pending_resend();
      auto resend = BuildResendRequestFrame(pending.begin_seq, pending.end_seq, timestamp_ns);
      if (!resend.ok()) {
        return resend.status();
      }
      event.outbound_frames.push_back(std::move(resend).value());
      // Queue the gap-triggering message for deferred replay after the gap
      // is fully filled.  The message has seq > expected and cannot be
      // processed yet, but will be lost if not saved here.
      deferred_gap_frames_.emplace_back(decoded.raw.begin(), decoded.raw.end());
      return event;
    }
    return status;
  }

  status = record_inbound_liveness();
  if (!status.ok()) {
    return status;
  }

  if (phase_violation_text.has_value()) {
    return send_phase_violation_logout(*phase_violation_text);
  }

  ref_tag_id = 0U;
  reject_reason = 0U;
  reject_text.clear();
  disconnect = false;
  if (!ValidateAdministrativeMessage(decoded, &ref_tag_id, &reject_reason, &reject_text, &disconnect)) {
    return RejectInbound(decoded, ref_tag_id, reject_reason, std::move(reject_text), timestamp_ns, disconnect);
  }

  if (msg_type == "A") {
    const auto inbound_next_expected = view.get_int(kNextExpectedMsgSeqNum);
    const bool inbound_reset = HasBoolean(view, kResetSeqNumFlag);
    if (inbound_reset) {
      const auto snapshot = session_.Snapshot();
      const auto next_out = config_.session.is_initiator ? snapshot.next_out_seq : 1U;
      status = ResetSessionState(decoded.header.msg_seq_num + 1U, next_out, false);
      if (!status.ok()) {
        return status;
      }
    }

    const auto pre_logon_next_out = session_.Snapshot().next_out_seq;
    if (inbound_next_expected.has_value() && inbound_next_expected.value() <= 0) {
      auto logout = BeginLogout("received invalid NextExpectedMsgSeqNum on Logon", timestamp_ns);
      if (!logout.ok()) {
        return logout.status();
      }
      event.outbound_frames.push_back(std::move(logout).value());
      event.disconnect = true;
      return event;
    }
    if (inbound_next_expected.has_value() &&
        static_cast<std::uint64_t>(inbound_next_expected.value()) > static_cast<std::uint64_t>(pre_logon_next_out)) {
      auto logout = BeginLogout("counterparty requested unsent outbound sequence range", timestamp_ns);
      if (!logout.ok()) {
        return logout.status();
      }
      event.outbound_frames.push_back(std::move(logout).value());
      event.disconnect = true;
      return event;
    }

    if (!config_.session.is_initiator) {
      auto response = BuildLogonFrame(timestamp_ns, config_.reset_seq_num_on_logon || inbound_reset);
      if (!response.ok()) {
        return response.status();
      }
      event.outbound_frames.push_back(std::move(response).value());
    }

    status = session_.OnLogonAccepted();
    if (!status.ok()) {
      return status;
    }
    status = PersistRecoveryState();
    if (!status.ok()) {
      return status;
    }
    event.session_active = true;
    if (inbound_next_expected.has_value()) {
      status = ReplayCounterpartyExpectedRange(
        static_cast<std::uint32_t>(inbound_next_expected.value()), pre_logon_next_out, timestamp_ns, &event);
      if (!status.ok()) {
        return status;
      }
    }
    return event;
  }

  if (msg_type == "0") {
    const auto test_request_id = GetStringView(view, kTestReqID);
    if (!test_request_id.empty() && test_request_id == outstanding_test_request_id_) {
      outstanding_test_request_id_.clear();
      test_request_sent_ns_ = 0U;
    }
    return event;
  }

  if (msg_type == "1") {
    auto response = BuildHeartbeatFrame(timestamp_ns, GetStringView(view, kTestReqID));
    if (!response.ok()) {
      return response.status();
    }
    event.outbound_frames.push_back(std::move(response).value());
    return event;
  }

  if (msg_type == "2") {
    const auto begin_seq = static_cast<std::uint32_t>(std::max<std::int64_t>(1, GetInt(view, kBeginSeqNo, 1)));
    const auto end_seq = static_cast<std::uint32_t>(std::max<std::int64_t>(0, GetInt(view, kEndSeqNo, 0)));
    if (auto replay_frames = AcquireReplayFrameBuffer()) {
      status = ReplayOutbound(begin_seq, end_seq, timestamp_ns, replay_frames.get());
      if (!status.ok()) {
        return status;
      }
      event.outbound_frames.borrow(std::move(replay_frames));
      return event;
    }

    ProtocolFrameList replay_frames;
    status = ReplayOutbound(begin_seq, end_seq, timestamp_ns, &replay_frames);
    if (!status.ok()) {
      return status;
    }
    event.outbound_frames = std::move(replay_frames);
    return event;
  }

  if (msg_type == "4") {
    std::uint32_t new_seq_num = 0U;
    if (!ParseSequenceResetNewSeq(view, &new_seq_num, &reject_reason, &reject_text)) {
      return RejectInbound(decoded, kNewSeqNo, reject_reason, std::move(reject_text), timestamp_ns, false);
    }

    const auto snapshot = session_.Snapshot();
    if (new_seq_num < snapshot.next_in_seq) {
      return RejectInbound(decoded,
                           kNewSeqNo,
                           kSessionRejectValueIncorrect,
                           "SequenceReset NewSeqNo must not move inbound sequence backwards",
                           timestamp_ns,
                           false);
    }

    status = session_.AdvanceInboundExpectedSeq(new_seq_num);
    if (!status.ok()) {
      return status;
    }
    status = PersistRecoveryState();
    if (!status.ok()) {
      return status;
    }
    return event;
  }

  if (msg_type == "5") {
    if (session_.state() != SessionState::kAwaitingLogout) {
      auto response = BeginLogout({}, timestamp_ns);
      if (!response.ok()) {
        return response.status();
      }
      event.outbound_frames.push_back(std::move(response).value());
    }
    event.disconnect = true;
    return event;
  }

  if (msg_type == "3") {
    event.session_reject = true;
    event.application_messages.push_back(message::MessageRef::Borrow(decoded.message.view()));
    return event;
  }

  if (session_.state() != SessionState::kActive && session_.state() != SessionState::kAwaitingLogout &&
      session_.state() != SessionState::kResendProcessing) {
    return base::Status::InvalidArgument("application message received before session activation");
  }

  ref_tag_id = 0U;
  reject_reason = 0U;
  reject_text.clear();
  const auto build_business_reject = [&](std::uint32_t business_reject_reason,
                                         std::string text) -> base::Result<ProtocolEvent> {
    ProtocolEvent business_reject_event;
    auto builder = BuildAdminMessage("j");
    builder.set_int(kRefSeqNum, static_cast<std::int64_t>(decoded.header.msg_seq_num));
    builder.set_string(kRefMsgType, std::string(decoded.message.view().msg_type()));
    builder.set_int(kBusinessRejectReasonTag, static_cast<std::int64_t>(business_reject_reason));
    if (!text.empty()) {
      builder.set_string(kText, std::move(text));
    }
    auto frame = EncodeFrame(std::move(builder).build(), true, timestamp_ns, true, false, true, 0U);
    if (!frame.ok()) {
      return frame.status();
    }
    business_reject_event.outbound_frames.push_back(std::move(frame).value());
    if (business_reject_reason == kBusinessRejectUnsupportedMessageType ||
        business_reject_reason == kBusinessRejectApplicationNotAvailable) {
      business_reject_event.warnings.push_back(text);
    } else {
      business_reject_event.errors.push_back(text);
    }
    return business_reject_event;
  };
  if (const auto business_reject_reason = ResolveApplicationBusinessRejectReason(config_, dictionary_, decoded);
      business_reject_reason.has_value()) {
    const auto text = business_reject_reason.value() == kBusinessRejectApplicationNotAvailable
                        ? std::string("application handling is not available for this session")
                        : std::string("application message type is not supported for this session");
    return build_business_reject(business_reject_reason.value(), text);
  }
  if (const auto conditional_business_reject = ResolveConditionalApplicationBusinessReject(decoded);
      conditional_business_reject.has_value()) {
    return build_business_reject(conditional_business_reject->reason, conditional_business_reject->text);
  }
  if (!ValidateApplicationMessage(decoded, &ref_tag_id, &reject_reason, &reject_text)) {
    return RejectInbound(decoded, ref_tag_id, reject_reason, std::move(reject_text), timestamp_ns, false);
  }

  event.application_messages.push_back(message::MessageRef::Borrow(decoded.message.view()));
  if (HasBoolean(view, kPossResend)) {
    event.poss_resend = true;
  }
  return event;
}

auto
AdminProtocol::OnTimer(std::uint64_t timestamp_ns) -> base::Result<ProtocolEvent>
{
  ProtocolEvent event;

  auto status = EnsureInitialized();
  if (!status.ok()) {
    return status;
  }

  const auto snapshot = session_.Snapshot();
  if (snapshot.state != SessionState::kActive && snapshot.state != SessionState::kAwaitingLogout &&
      snapshot.state != SessionState::kResendProcessing && snapshot.state != SessionState::kPendingLogon) {
    return event;
  }

  const auto interval_ns = std::max<std::uint64_t>(1U, config_.heartbeat_interval_seconds) * kNanosPerSecond;

  // PendingLogon timeout: disconnect if logon handshake not completed within
  // 2*interval.
  if (snapshot.state == SessionState::kPendingLogon && snapshot.last_outbound_ns != 0U &&
      timestamp_ns > snapshot.last_outbound_ns + (interval_ns * 2U)) {
    event.disconnect = true;
  }
  if (snapshot.state == SessionState::kPendingLogon) {
    return event;
  }

  // Logout timeout: disconnect if counterparty does not complete logout within
  // one interval.
  if (snapshot.state == SessionState::kAwaitingLogout && logout_sent_ns_ != 0U &&
      timestamp_ns > logout_sent_ns_ + interval_ns) {
    event.warnings.push_back("Logout acknowledgement timeout");
    event.disconnect = true;
    return event;
  }

  // TestRequest timeout: disconnect if no response within one interval of
  // sending TestRequest.
  if (!outstanding_test_request_id_.empty() && test_request_sent_ns_ != 0U &&
      timestamp_ns > test_request_sent_ns_ + interval_ns) {
    event.disconnect = true;
    return event;
  }

  const auto test_request_threshold_ns = interval_ns + (interval_ns / kTestRequestGraceDivisor);

  // Send TestRequest after HeartBtInt plus the official 20% grace period of
  // inbound silence (only if not already waiting for one).
  if (snapshot.last_inbound_ns != 0U && timestamp_ns > snapshot.last_inbound_ns + test_request_threshold_ns &&
      outstanding_test_request_id_.empty()) {
    outstanding_test_request_id_ = std::to_string(timestamp_ns);
    test_request_sent_ns_ = timestamp_ns;
    auto test_request = BuildTestRequestFrame(timestamp_ns, outstanding_test_request_id_);
    if (!test_request.ok()) {
      return test_request.status();
    }
    event.outbound_frames.push_back(std::move(test_request).value());
    return event;
  }

  // Regular heartbeat when no outbound activity for one interval.
  if (snapshot.last_outbound_ns == 0U || timestamp_ns > snapshot.last_outbound_ns + interval_ns) {
    auto heartbeat = BuildHeartbeatFrame(timestamp_ns, {});
    if (!heartbeat.ok()) {
      return heartbeat.status();
    }
    event.outbound_frames.push_back(std::move(heartbeat).value());
  }

  return event;
}

auto
AdminProtocol::NextTimerDeadline(std::uint64_t timestamp_ns) const -> std::optional<std::uint64_t>
{
  auto status = EnsureInitialized();
  if (!status.ok()) {
    return std::nullopt;
  }

  const auto snapshot = session_.Snapshot();
  if (snapshot.state != SessionState::kActive && snapshot.state != SessionState::kAwaitingLogout &&
      snapshot.state != SessionState::kResendProcessing && snapshot.state != SessionState::kPendingLogon) {
    return std::nullopt;
  }

  const auto interval_ns = std::max<std::uint64_t>(1U, config_.heartbeat_interval_seconds) * kNanosPerSecond;
  std::optional<std::uint64_t> deadline;
  const auto update_deadline = [&](std::uint64_t candidate_ns) {
    if (!deadline.has_value() || candidate_ns < *deadline) {
      deadline = candidate_ns;
    }
  };

  if (snapshot.state == SessionState::kPendingLogon && snapshot.last_outbound_ns != 0U) {
    update_deadline(snapshot.last_outbound_ns + (interval_ns * 2U));
  }
  if (snapshot.state == SessionState::kPendingLogon) {
    if (deadline.has_value() && *deadline < timestamp_ns) {
      deadline = timestamp_ns;
    }
    return deadline;
  }

  if (snapshot.state == SessionState::kAwaitingLogout && logout_sent_ns_ != 0U) {
    update_deadline(logout_sent_ns_ + interval_ns);
  }

  if (!outstanding_test_request_id_.empty() && test_request_sent_ns_ != 0U) {
    update_deadline(test_request_sent_ns_ + interval_ns);
  } else if (snapshot.last_inbound_ns != 0U) {
    const auto test_request_threshold_ns = interval_ns + (interval_ns / kTestRequestGraceDivisor);
    update_deadline(snapshot.last_inbound_ns + test_request_threshold_ns);
  }

  if (snapshot.last_outbound_ns == 0U) {
    update_deadline(timestamp_ns);
  } else {
    update_deadline(snapshot.last_outbound_ns + interval_ns);
  }

  if (deadline.has_value() && *deadline < timestamp_ns) {
    deadline = timestamp_ns;
  }
  return deadline;
}

auto
AdminProtocol::SendApplication(const message::Message& message,
                               std::uint64_t timestamp_ns,
                               SessionSendEnvelopeView envelope) -> base::Result<EncodedFrame>
{
  auto status = EnsureInitialized();
  if (!status.ok()) {
    return status;
  }

  if (session_.state() != SessionState::kActive && session_.state() != SessionState::kAwaitingLogout) {
    return base::Status::InvalidArgument("cannot send application payload on an inactive FIX session");
  }
  return EncodeFrame(message, false, timestamp_ns, true, false, true, 0U, 0U, {}, envelope);
}

auto
AdminProtocol::SendApplication(message::MessageView message,
                               std::uint64_t timestamp_ns,
                               SessionSendEnvelopeView envelope) -> base::Result<EncodedFrame>
{
  auto status = EnsureInitialized();
  if (!status.ok()) {
    return status;
  }

  if (session_.state() != SessionState::kActive && session_.state() != SessionState::kAwaitingLogout) {
    return base::Status::InvalidArgument("cannot send application payload on an inactive FIX session");
  }
  return EncodeFrame(message, false, timestamp_ns, true, false, true, 0U, 0U, {}, envelope);
}

auto
AdminProtocol::SendApplication(const message::MessageRef& message,
                               std::uint64_t timestamp_ns,
                               SessionSendEnvelopeView envelope) -> base::Result<EncodedFrame>
{
  return SendApplication(message.view(), timestamp_ns, envelope);
}

auto
AdminProtocol::SendEncodedApplication(const EncodedApplicationMessage& message,
                                      std::uint64_t timestamp_ns,
                                      SessionSendEnvelopeView envelope) -> base::Result<EncodedFrame>
{
  return SendEncodedApplication(message.view(), timestamp_ns, envelope);
}

auto
AdminProtocol::SendEncodedApplication(EncodedApplicationMessageView message,
                                      std::uint64_t timestamp_ns,
                                      SessionSendEnvelopeView envelope) -> base::Result<EncodedFrame>
{
  auto status = EnsureInitialized();
  if (!status.ok()) {
    return status;
  }

  if (session_.state() != SessionState::kActive && session_.state() != SessionState::kAwaitingLogout) {
    return base::Status::InvalidArgument("cannot send application payload on an inactive FIX session");
  }
  return EncodeFrame(message, false, timestamp_ns, true, false, true, 0U, 0U, {}, envelope);
}

auto
AdminProtocol::SendEncodedApplication(const EncodedApplicationMessageRef& message,
                                      std::uint64_t timestamp_ns,
                                      SessionSendEnvelopeView envelope) -> base::Result<EncodedFrame>
{
  return SendEncodedApplication(message.view(), timestamp_ns, envelope);
}

auto
AdminProtocol::BeginLogout(std::string text, std::uint64_t timestamp_ns) -> base::Result<EncodedFrame>
{
  auto status = EnsureInitialized();
  if (!status.ok()) {
    return status;
  }

  if (logout_sent_) {
    return base::Status::InvalidArgument("logout already sent");
  }

  if (session_.state() == SessionState::kActive || session_.state() == SessionState::kResendProcessing) {
    status = session_.BeginLogout();
    if (!status.ok()) {
      return status;
    }
  }

  auto builder = BuildAdminMessage("5");
  if (!text.empty()) {
    builder.set_string(kText, std::move(text));
  }
  logout_sent_ = true;
  logout_sent_ns_ = timestamp_ns;
  return EncodeFrame(std::move(builder).build(), true, timestamp_ns, true, false, true, 0U);
}

auto
AdminProtocol::DrainDeferredGapFrames(std::uint64_t timestamp_ns, ProtocolEvent* event) -> base::Status
{
  // Iterative drain: we process deferred frames one at a time instead of
  // recursing through OnInbound(span, ...).  OnInbound(DecodedMessageView)
  // may trigger further resends that re-queue into deferred_gap_frames_, but
  // the loop guard (!session_.pending_resend()) prevents unbounded iteration:
  // a new resend request stops the drain until the resend completes.
  std::size_t index = 0;
  while (index < deferred_gap_frames_.size() && !session_.pending_resend().has_value()) {
    // Move the frame out; leave a moved-from entry that we skip on cleanup.
    auto frame = std::move(deferred_gap_frames_[index]);
    ++index;

    codec::DecodedMessageView decoded;
    auto decode_status = codec::DecodeFixMessageView(std::span<const std::byte>(frame),
                                                     dictionary_,
                                                     decode_table_,
                                                     &decoded,
                                                     codec::kFixSoh,
                                                     config_.validation_policy.verify_checksum);
    if (!decode_status.ok()) {
      continue; // corrupt frame — discard silently
    }
    if (decoded.header.msg_seq_num < session_.Snapshot().next_in_seq) {
      continue; // already consumed via the normal resend stream
    }

    auto result = OnInbound(decoded, timestamp_ns);
    if (!result.ok()) {
      deferred_gap_frames_.erase(deferred_gap_frames_.begin(),
                                 deferred_gap_frames_.begin() + static_cast<std::ptrdiff_t>(index));
      return result.status();
    }
    auto& inner = result.value();
    if (inner.application_messages.size() == 1U) {
      auto& message = inner.application_messages.front();
      if (message.valid() && !message.owns_storage()) {
        inner.AdoptParsedApplicationMessage(std::move(decoded.message), std::move(frame));
      }
    } else {
      inner.MaterializeApplicationMessages();
    }
    for (auto& f : inner.outbound_frames) {
      event->outbound_frames.push_back(std::move(f));
    }
    for (auto& m : inner.application_messages) {
      event->application_messages.push_back(std::move(m));
    }
    if (inner.disconnect) {
      event->disconnect = true;
      break;
    }
  }
  // Bulk-erase all consumed entries in one O(n) pass.
  if (index > 0) {
    deferred_gap_frames_.erase(deferred_gap_frames_.begin(),
                               deferred_gap_frames_.begin() + static_cast<std::ptrdiff_t>(index));
  }
  return base::Status::Ok();
}

#undef config_
#undef dictionary_
#undef store_
#undef session_
#undef outstanding_test_request_id_
#undef logout_sent_
#undef test_request_sent_ns_
#undef logout_sent_ns_
#undef initialization_error_
#undef encode_templates_
#undef decode_table_
#undef inbound_decode_scratch_
#undef encode_buffer_
#undef replay_range_buffer_
#undef replay_frame_buffers_
#undef replay_frame_buffer_cursor_
#undef deferred_gap_frames_

} // namespace nimble::session
