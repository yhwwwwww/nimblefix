#include "nimblefix/codec.hpp"

#include <array>
#include <cstdint>
#include <limits>

namespace nimblefix {
namespace {

constexpr std::array<LengthDataPair, 16> fix44_pairs{{
    {90, 91},   // SecureDataLen / SecureData
    {93, 89},   // SignatureLength / Signature
    {95, 96},   // RawDataLength / RawData
    {212, 213}, // XmlDataLen / XmlData
    {348, 349}, // EncodedIssuerLen / EncodedIssuer
    {350, 351}, // EncodedSecurityDescLen / EncodedSecurityDesc
    {352, 353}, // EncodedListExecInstLen / EncodedListExecInst
    {354, 355}, // EncodedTextLen / EncodedText
    {356, 357}, // EncodedSubjectLen / EncodedSubject
    {358, 359}, // EncodedHeadlineLen / EncodedHeadline
    {360, 361}, // EncodedAllocTextLen / EncodedAllocText
    {362, 363}, // EncodedUnderlyingIssuerLen / EncodedUnderlyingIssuer
    {364, 365}, // EncodedUnderlyingSecurityDescLen / EncodedUnderlyingSecurityDesc
    {445, 446}, // EncodedListStatusTextLen / EncodedListStatusText
    {618, 619}, // EncodedLegIssuerLen / EncodedLegIssuer
    {621, 622}, // EncodedLegSecurityDescLen / EncodedLegSecurityDesc
}};

constexpr bool is_decimal_digit(char c) noexcept { return c >= '0' && c <= '9'; }

bool is_valid_plain_value(std::string_view value) noexcept {
    // TagValue has no representation for an empty ordinary field. Binary bytes
    // bypass this check only after a known Length/Data pair supplies a boundary.
    return !value.empty() && detail::is_valid_text(value);
}

Status error(ErrorCode code, std::uint32_t tag, std::size_t offset) noexcept {
    return {code, tag <= detail::maximum_fix_tag ? static_cast<int>(tag) : 0, offset};
}

Status validate_hints(WireHints hints) noexcept {
    if (hints.length_data_pairs.size() > max_wire_hint_pairs)
        return {ErrorCode::capacity_exceeded, 0, max_wire_hint_pairs};

    const auto same_or_conflicting = [](LengthDataPair a, LengthDataPair b) noexcept {
        return a.length_tag == b.length_tag || a.length_tag == b.data_tag ||
               a.data_tag == b.length_tag || a.data_tag == b.data_tag;
    };

    for (std::size_t i = 0; i < hints.length_data_pairs.size(); ++i) {
        const auto pair = hints.length_data_pairs[i];
        if (pair.length_tag == 0 || pair.data_tag == 0 ||
            pair.length_tag > detail::maximum_fix_tag ||
            pair.data_tag > detail::maximum_fix_tag ||
            pair.length_tag == pair.data_tag ||
            detail::is_managed_tag(pair.length_tag) || detail::is_managed_tag(pair.data_tag))
            return error(ErrorCode::invalid_value, pair.length_tag, i);

        for (std::size_t j = i + 1; j < hints.length_data_pairs.size(); ++j) {
            if (same_or_conflicting(pair, hints.length_data_pairs[j]))
                return error(ErrorCode::invalid_value, pair.length_tag, i);
        }
        for (const auto builtin : fix44_pairs) {
            if (same_or_conflicting(pair, builtin))
                return error(ErrorCode::invalid_value, pair.length_tag, i);
        }
    }
    return Status::success();
}

bool find_length_pair(std::uint32_t length_tag,
                      WireHints hints,
                      LengthDataPair& found) noexcept {
    for (const auto pair : fix44_pairs) {
        if (pair.length_tag == length_tag) {
            found = pair;
            return true;
        }
    }
    for (const auto pair : hints.length_data_pairs) {
        if (pair.length_tag == length_tag) {
            found = pair;
            return true;
        }
    }
    return false;
}

bool is_data_tag(std::uint32_t tag, WireHints hints) noexcept {
    for (const auto pair : fix44_pairs) {
        if (pair.data_tag == tag) return true;
    }
    for (const auto pair : hints.length_data_pairs) {
        if (pair.data_tag == tag) return true;
    }
    return false;
}

// Scan a numeric tag up to a hard boundary. input may end before that boundary,
// which means the caller may provide another chunk later.
Status scan_tag(std::string_view input,
                std::size_t start,
                std::size_t hard_end,
                std::uint32_t& tag,
                std::size_t& equals_at) noexcept {
    const auto scan_end = input.size() < hard_end ? input.size() : hard_end;
    if (start >= scan_end) {
        return input.size() < hard_end
            ? Status{ErrorCode::need_more, 0, input.size()}
            : Status{ErrorCode::invalid_tag, 0, start};
    }

    std::uint64_t value = 0;
    std::size_t cursor = start;
    for (; cursor < scan_end; ++cursor) {
        const char c = input[cursor];
        if (c == '=') {
            if (cursor == start || value == 0 || value > detail::maximum_fix_tag)
                return {ErrorCode::invalid_tag, 0, start};
            tag = static_cast<std::uint32_t>(value);
            equals_at = cursor;
            return Status::success();
        }
        if (!is_decimal_digit(c)) return {ErrorCode::invalid_tag, 0, cursor};
        if (cursor == start && c == '0') return {ErrorCode::invalid_tag, 0, start};
        const auto digit = static_cast<unsigned>(c - '0');
        if (value > (detail::maximum_fix_tag - digit) / 10)
            return {ErrorCode::invalid_tag, 0, start};
        value = value * 10 + digit;
    }

    return input.size() < hard_end
        ? Status{ErrorCode::need_more, 0, input.size()}
        : Status{ErrorCode::invalid_tag, 0, start};
}

// Parse a nonempty unsigned decimal field. overflow is always rejected before
// multiplication, so arbitrary input cannot wrap the result.
Status parse_unsigned(std::string_view value,
                      std::size_t max_value,
                      std::size_t offset,
                      std::uint32_t tag,
                      ErrorCode code,
                      std::size_t& result) noexcept {
    if (value.empty()) return error(code, tag, offset);
    std::size_t parsed = 0;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (!is_decimal_digit(c)) return error(code, tag, offset + i);
        const auto digit = static_cast<std::size_t>(c - '0');
        if (parsed > (max_value - digit) / 10) return error(code, tag, offset + i);
        parsed = parsed * 10 + digit;
    }
    result = parsed;
    return Status::success();
}

Status fixed_begin_string(std::string_view input,
                          std::size_t value_start,
                          std::size_t& delimiter_at) noexcept {
    constexpr std::string_view expected = "FIX.4.4";
    std::size_t cursor = value_start;
    for (std::size_t i = 0; i < expected.size(); ++i, ++cursor) {
        if (cursor >= input.size()) return {ErrorCode::need_more, 8, input.size()};
        if (input[cursor] == soh) return {ErrorCode::invalid_header, 8, cursor};
        if (input[cursor] != expected[i]) return {ErrorCode::invalid_header, 8, cursor};
    }
    if (cursor >= input.size()) return {ErrorCode::need_more, 8, input.size()};
    if (input[cursor] != soh) return {ErrorCode::invalid_header, 8, cursor};
    delimiter_at = cursor;
    return Status::success();
}

// A BodyLength is read until SOH. If the buffer ends on digits, the value may
// still continue in the next read and is therefore incomplete rather than bad.
Status read_body_length(std::string_view input,
                        std::size_t value_start,
                        std::size_t max_frame_bytes,
                        std::size_t& delimiter_at,
                        std::size_t& body_length) noexcept {
    std::size_t cursor = value_start;
    std::size_t parsed = 0;
    for (; cursor < input.size() && cursor < max_frame_bytes; ++cursor) {
        const char c = input[cursor];
        if (c == soh) break;
        if (!is_decimal_digit(c))
            return {ErrorCode::invalid_body_length, 9, cursor};
        const auto digit = static_cast<std::size_t>(c - '0');
        if (parsed > (std::numeric_limits<std::size_t>::max() - digit) / 10)
            return {ErrorCode::invalid_body_length, 9, cursor};
        parsed = parsed * 10 + digit;
    }
    if (cursor == input.size() || cursor == max_frame_bytes) {
        if (cursor == max_frame_bytes)
            return {ErrorCode::too_large, 9, cursor};
        return {ErrorCode::need_more, 9, input.size()};
    }
    delimiter_at = cursor;
    body_length = parsed;
    return Status::success();
}

Status scan_value_delimiter(std::string_view input,
                            std::size_t value_start,
                            std::size_t hard_end,
                            std::uint32_t tag,
                            ErrorCode boundary_error,
                            std::size_t& delimiter_at) noexcept {
    const std::size_t scan_end = input.size() < hard_end ? input.size() : hard_end;
    for (std::size_t cursor = value_start; cursor < scan_end; ++cursor) {
        if (input[cursor] == soh) {
            delimiter_at = cursor;
            return Status::success();
        }
    }
    if (input.size() < hard_end) return {ErrorCode::need_more, static_cast<int>(tag), input.size()};
    return error(boundary_error, tag, hard_end);
}

Status read_body_tag(std::string_view input,
                     std::size_t start,
                     std::size_t checksum_offset,
                     std::uint32_t& tag,
                     std::size_t& equals_at) noexcept {
    return scan_tag(input, start, checksum_offset, tag, equals_at);
}

} // namespace

Status inspect_frame(std::string_view input,
                     FrameView& output,
                     WireHints hints,
                     CodecLimits limits) noexcept {
    output = {};
    const Status hint_status = validate_hints(hints);
    if (!hint_status.ok()) return hint_status;
    constexpr std::size_t minimum_fix44_frame_bytes = 26;
    if (limits.max_frame_bytes < minimum_fix44_frame_bytes)
        return {ErrorCode::too_large, 0, 0};

    std::uint32_t tag = 0;
    std::size_t equals_at = 0;
    Status status = scan_tag(input, 0, std::numeric_limits<std::size_t>::max(), tag, equals_at);
    if (!status.ok()) return status;
    if (tag != 8) return {ErrorCode::invalid_header, static_cast<int>(tag), 0};
    std::size_t begin_delimiter = 0;
    status = fixed_begin_string(input, equals_at + 1, begin_delimiter);
    if (!status.ok()) return status;

    const std::size_t body_length_tag_start = begin_delimiter + 1;
    status = scan_tag(input,
                      body_length_tag_start,
                      std::numeric_limits<std::size_t>::max(),
                      tag,
                      equals_at);
    if (!status.ok()) return status;
    if (tag != 9) return {ErrorCode::invalid_header, static_cast<int>(tag), body_length_tag_start};

    std::size_t body_length_delimiter = 0;
    std::size_t body_length = 0;
    status = read_body_length(input,
                              equals_at + 1,
                              limits.max_frame_bytes,
                              body_length_delimiter,
                              body_length);
    if (!status.ok()) return status;
    const std::size_t body_start = body_length_delimiter + 1;

    constexpr std::size_t checksum_field_bytes = 7; // 10=ddd<SOH>
    if (body_start > limits.max_frame_bytes ||
        checksum_field_bytes > limits.max_frame_bytes - body_start ||
        body_length > limits.max_frame_bytes - body_start - checksum_field_bytes)
        return {ErrorCode::too_large, 9, body_length_tag_start};
    if (body_length > std::numeric_limits<std::size_t>::max() - body_start)
        return {ErrorCode::invalid_body_length, 9, body_length_tag_start};
    const std::size_t checksum_offset = body_start + body_length;
    const std::size_t frame_size = checksum_offset + checksum_field_bytes;

    // The first body field must be MsgType (35), and its delimiter and value
    // must fit inside the declared body.
    if (body_length < 5) return {ErrorCode::invalid_body_length, 9, body_length_tag_start};
    status = read_body_tag(input, body_start, checksum_offset, tag, equals_at);
    if (!status.ok()) return status;
    if (tag != 35) return {ErrorCode::invalid_header, static_cast<int>(tag), body_start};
    std::size_t msg_type_delimiter = 0;
    status = scan_value_delimiter(input,
                                  equals_at + 1,
                                  checksum_offset,
                                  35,
                                  ErrorCode::invalid_body_length,
                                  msg_type_delimiter);
    if (!status.ok()) return status;
    if (msg_type_delimiter == equals_at + 1)
        return {ErrorCode::invalid_header, 35, equals_at + 1};

    if (input.size() < checksum_offset)
        return {ErrorCode::need_more, 0, input.size()};
    if (input[checksum_offset - 1] != soh)
        return {ErrorCode::invalid_frame, 0, checksum_offset - 1};

    // Verify whatever part of the checksum field has arrived. A corrupt prefix
    // is already invalid even if the rest of the TCP read has not arrived.
    constexpr std::string_view checksum_prefix = "10=";
    const std::size_t available_checksum = input.size() > checksum_offset
        ? input.size() - checksum_offset : 0;
    const std::size_t prefix_bytes = available_checksum < checksum_prefix.size()
        ? available_checksum : checksum_prefix.size();
    for (std::size_t i = 0; i < prefix_bytes; ++i) {
        if (input[checksum_offset + i] != checksum_prefix[i])
            return {ErrorCode::invalid_checksum, 10, checksum_offset + i};
    }
    if (available_checksum > 3) {
        const std::size_t digits_available = available_checksum - 3 < 3
            ? available_checksum - 3 : 3;
        for (std::size_t i = 0; i < digits_available; ++i) {
            if (!is_decimal_digit(input[checksum_offset + 3 + i]))
                return {ErrorCode::invalid_checksum, 10, checksum_offset + 3 + i};
        }
    }
    if (available_checksum >= checksum_field_bytes && input[checksum_offset + 6] != soh)
        return {ErrorCode::invalid_checksum, 10, checksum_offset + 6};
    if (input.size() < frame_size) return {ErrorCode::need_more, 10, input.size()};

    const unsigned check_value = static_cast<unsigned>(input[checksum_offset + 3] - '0') * 100U +
                                 static_cast<unsigned>(input[checksum_offset + 4] - '0') * 10U +
                                 static_cast<unsigned>(input[checksum_offset + 5] - '0');
    if (check_value > 255U) return {ErrorCode::invalid_checksum, 10, checksum_offset + 3};
    unsigned sum = 0;
    for (std::size_t i = 0; i < checksum_offset; ++i)
        sum = (sum + static_cast<unsigned char>(input[i])) & 0xffU;
    if (sum != check_value) return {ErrorCode::invalid_checksum, 10, checksum_offset + 3};

    output.bytes = input.substr(0, frame_size);
    output.consumed = frame_size;
    return Status::success();
}

Result<FieldView> FlatMessageView::find_unique(std::uint32_t tag) const noexcept {
    bool found = false;
    FieldView value{};
    for (const auto field : fields_) {
        if (field.tag != tag) continue;
        if (found) return error(ErrorCode::duplicate_field, tag, field.offset);
        value = field;
        found = true;
    }
    if (!found) return error(ErrorCode::missing_field, tag, 0);
    return value;
}

Status parse_flat(FrameView frame,
                  std::span<FieldView> scratch,
                  FlatMessageView& output,
                  WireHints hints,
                  CodecLimits limits) noexcept {
    output.frame_ = {};
    output.fields_ = {};
    if (frame.bytes.empty() || frame.consumed != frame.bytes.size())
        return {ErrorCode::invalid_frame, 0, 0};

    FrameView verified{};
    Status status = inspect_frame(frame.bytes, verified, hints, limits);
    if (!status.ok()) return status;
    if (verified.consumed != frame.bytes.size())
        return {ErrorCode::invalid_frame, 0, verified.consumed};

    const std::size_t checksum_offset = frame.bytes.size() - 7;
    const std::size_t first_header_delimiter = frame.bytes.find(soh);
    const std::size_t second_header_delimiter = frame.bytes.find(soh, first_header_delimiter + 1);
    const std::size_t body_start = second_header_delimiter + 1;
    std::size_t cursor = 0;
    std::size_t count = 0;
    auto add_field = [&](std::uint32_t field_tag,
                         std::size_t value_start,
                         std::size_t value_length,
                         std::size_t field_offset) noexcept -> Status {
        if (count == scratch.size())
            return error(ErrorCode::capacity_exceeded, field_tag, field_offset);
        scratch[count++] = FieldView{field_tag, frame.bytes.substr(value_start, value_length), field_offset};
        return Status::success();
    };

    while (cursor < frame.bytes.size()) {
        std::uint32_t current_tag = 0;
        std::size_t equals = 0;
        status = scan_tag(frame.bytes, cursor, frame.bytes.size(), current_tag, equals);
        if (!status.ok()) return status;
        const std::size_t value_start = equals + 1;
        if (cursor == checksum_offset) {
            if (current_tag != 10) return {ErrorCode::invalid_checksum, 10, cursor};
            status = add_field(current_tag, value_start, 3, cursor);
            if (!status.ok()) return status;
            cursor = frame.bytes.size();
            continue;
        }
        if (cursor > checksum_offset || current_tag == 10)
            return {ErrorCode::invalid_checksum, 10, cursor};
        if ((current_tag == 8 && cursor != 0) ||
            (current_tag == 9 && cursor != first_header_delimiter + 1) ||
            (current_tag == 35 && cursor != body_start))
            return {ErrorCode::invalid_header, static_cast<int>(current_tag), cursor};

        LengthDataPair pair{};
        if (find_length_pair(current_tag, hints, pair)) {
            std::size_t length_delimiter = 0;
            status = scan_value_delimiter(frame.bytes,
                                          value_start,
                                          checksum_offset,
                                          current_tag,
                                          ErrorCode::invalid_value,
                                          length_delimiter);
            if (!status.ok()) return status;
            std::size_t data_length = 0;
            status = parse_unsigned(frame.bytes.substr(value_start, length_delimiter - value_start),
                                    std::numeric_limits<std::size_t>::max(),
                                    value_start,
                                    current_tag,
                                    ErrorCode::invalid_value,
                                    data_length);
            if (!status.ok()) return status;
            status = add_field(current_tag,
                               value_start,
                               length_delimiter - value_start,
                               cursor);
            if (!status.ok()) return status;

            const std::size_t data_field_start = length_delimiter + 1;
            std::uint32_t data_tag = 0;
            std::size_t data_equals = 0;
            status = scan_tag(frame.bytes, data_field_start, checksum_offset, data_tag, data_equals);
            if (!status.ok()) return status;
            if (data_tag != pair.data_tag)
                return error(ErrorCode::invalid_value, current_tag, data_field_start);
            const std::size_t data_start = data_equals + 1;
            const std::size_t available = checksum_offset - data_start;
            if (data_length == 0 || data_length >= available)
                return error(ErrorCode::invalid_value, data_tag, data_start);
            const std::size_t data_delimiter = data_start + data_length;
            if (frame.bytes[data_delimiter] != soh)
                return error(ErrorCode::invalid_value, data_tag, data_delimiter);
            status = add_field(data_tag, data_start, data_length, data_field_start);
            if (!status.ok()) return status;
            cursor = data_delimiter + 1;
            continue;
        }
        if (is_data_tag(current_tag, hints))
            return error(ErrorCode::invalid_value, current_tag, cursor);

        // Unknown fields use the plain-text boundary. A custom binary field
        // cannot be inferred from SOH bytes; without a WireHint, callers can
        // retain FrameView::bytes as an opaque frame instead of flat parsing.
        std::size_t delimiter = 0;
        status = scan_value_delimiter(frame.bytes,
                                      value_start,
                                      checksum_offset,
                                      current_tag,
                                      ErrorCode::invalid_frame,
                                      delimiter);
        if (!status.ok()) return status;
        const std::string_view plain_value = frame.bytes.substr(value_start, delimiter - value_start);
        if (!is_valid_plain_value(plain_value))
            return error(ErrorCode::invalid_value, current_tag, value_start);
        status = add_field(current_tag, value_start, delimiter - value_start, cursor);
        if (!status.ok()) return status;
        cursor = delimiter + 1;
    }

    output.frame_ = frame.bytes;
    output.fields_ = std::span<const FieldView>(scratch.data(), count);
    return Status::success();
}

} // namespace nimblefix
