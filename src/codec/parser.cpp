#include "nimblefix/codec.hpp"

#include <array>
#include <cstring>
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

Status error(ErrorCode code, std::uint32_t tag, std::size_t offset) noexcept {
    return {code, tag <= detail::maximum_fix_tag ? static_cast<int>(tag) : 0, offset};
}

constexpr std::size_t builtin_mask_words = 10; // covers tags 0..639

constexpr std::array<std::uint64_t, builtin_mask_words> make_builtin_len_mask() noexcept {
    std::array<std::uint64_t, builtin_mask_words> m{};
    for (const auto pair : fix44_pairs) {
        m[pair.length_tag >> 6U] |= (std::uint64_t{1} << (pair.length_tag & 63U));
    }
    return m;
}

constexpr std::array<std::uint64_t, builtin_mask_words> make_builtin_data_mask() noexcept {
    std::array<std::uint64_t, builtin_mask_words> m{};
    for (const auto pair : fix44_pairs) {
        m[pair.data_tag >> 6U] |= (std::uint64_t{1} << (pair.data_tag & 63U));
    }
    return m;
}

constexpr auto builtin_len_mask = make_builtin_len_mask();
constexpr auto builtin_data_mask = make_builtin_data_mask();

[[nodiscard]] constexpr bool is_builtin_len_tag(std::uint32_t tag) noexcept {
    return tag < builtin_mask_words * 64U &&
           (builtin_len_mask[tag >> 6U] & (std::uint64_t{1} << (tag & 63U))) != 0;
}

[[nodiscard]] constexpr bool is_builtin_data_tag(std::uint32_t tag) noexcept {
    return tag < builtin_mask_words * 64U &&
           (builtin_data_mask[tag >> 6U] & (std::uint64_t{1} << (tag & 63U))) != 0;
}

[[nodiscard]] constexpr bool is_builtin_pair(LengthDataPair p) noexcept {
    if (!is_builtin_len_tag(p.length_tag)) return false;
    for (const auto b : fix44_pairs) {
        if (b.length_tag == p.length_tag && b.data_tag == p.data_tag) return true;
    }
    return false;
}

Status validate_hints(WireHints hints) noexcept {
    if (hints.length_data_pairs.empty()) return Status::success();
    if (hints.length_data_pairs.size() > max_wire_hint_pairs)
        return {ErrorCode::capacity_exceeded, 0, max_wire_hint_pairs};

    // Fast path: standard FIX schemas pass only built-in fix44_pairs (O(N) bitmask instead of O(N^2)).
    std::uint32_t seen_builtin = 0;
    bool all_builtin = true;
    for (std::size_t i = 0; i < hints.length_data_pairs.size(); ++i) {
        const auto pair = hints.length_data_pairs[i];
        int match = -1;
        if (is_builtin_len_tag(pair.length_tag)) {
            for (std::size_t k = 0; k < fix44_pairs.size(); ++k) {
                if (fix44_pairs[k].length_tag == pair.length_tag &&
                    fix44_pairs[k].data_tag == pair.data_tag) {
                    match = static_cast<int>(k);
                    break;
                }
            }
        }
        if (match < 0) {
            all_builtin = false;
            break;
        }
        const std::uint32_t bit = std::uint32_t{1} << static_cast<unsigned>(match);
        if ((seen_builtin & bit) != 0) return error(ErrorCode::invalid_value, pair.length_tag, i);
        seen_builtin |= bit;
    }
    if (all_builtin) return Status::success();

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
            if (pair.length_tag == builtin.length_tag && pair.data_tag == builtin.data_tag) continue;
            if (same_or_conflicting(pair, builtin))
                return error(ErrorCode::invalid_value, pair.length_tag, i);
        }
    }
    return Status::success();
}

bool find_length_pair_fast(std::uint32_t length_tag,
                           WireHints hints,
                           bool has_extra_hints,
                           std::uint64_t extra_len_bloom,
                           LengthDataPair& found) noexcept {
    if (is_builtin_len_tag(length_tag)) {
        for (const auto pair : fix44_pairs) {
            if (pair.length_tag == length_tag) {
                found = pair;
                return true;
            }
        }
    }
    if (has_extra_hints && (extra_len_bloom & (std::uint64_t{1} << (length_tag & 63U))) != 0) {
        for (const auto pair : hints.length_data_pairs) {
            if (pair.length_tag == length_tag) {
                found = pair;
                return true;
            }
        }
    }
    return false;
}

bool is_data_tag_fast(std::uint32_t tag,
                      WireHints hints,
                      bool has_extra_hints,
                      std::uint64_t extra_data_bloom) noexcept {
    if (is_builtin_data_tag(tag)) return true;
    if (has_extra_hints && (extra_data_bloom & (std::uint64_t{1} << (tag & 63U))) != 0) {
        for (const auto pair : hints.length_data_pairs) {
            if (pair.data_tag == tag) return true;
        }
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

Status read_begin_string(std::string_view input,
                         std::size_t value_start,
                         std::size_t max_frame_bytes,
                         std::size_t& delimiter_at,
                         FixVersion& version) noexcept {
    const auto scan_end = input.size() < max_frame_bytes ? input.size() : max_frame_bytes;
    for (std::size_t cursor = value_start; cursor < scan_end; ++cursor) {
        const char ch = input[cursor];
        if (ch == soh) {
            if (cursor == value_start) return {ErrorCode::invalid_header, 8, value_start};
            const auto parsed = parse_fix_version(input.substr(value_start, cursor - value_start));
            if (!parsed.ok()) return {ErrorCode::invalid_header, 8, value_start};
            version = parsed.value();
            delimiter_at = cursor;
            return Status::success();
        }
        if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7f)
            return {ErrorCode::invalid_header, 8, cursor};
    }
    if (input.size() < max_frame_bytes) return {ErrorCode::need_more, 8, input.size()};
    return {ErrorCode::too_large, 8, max_frame_bytes};
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
    if (value_start < scan_end) {
        if (const void* found = std::memchr(input.data() + value_start, soh, scan_end - value_start)) {
            delimiter_at = static_cast<std::size_t>(static_cast<const char*>(found) - input.data());
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
    FixVersion version = FixVersion::fix44;
    if (input.empty()) return {ErrorCode::need_more, 8, 0};
    if (input[0] != '8') return {ErrorCode::invalid_header, 0, 0};
    if (input.size() == 1) return {ErrorCode::need_more, 8, 1};
    if (input[1] != '=') return {ErrorCode::invalid_header, 0, 0};
    tag = 8;
    equals_at = 1;
    Status status{};
    std::size_t begin_delimiter = 0;
    status = read_begin_string(input, equals_at + 1, limits.max_frame_bytes, begin_delimiter, version);
    if (!status.ok()) return status;

    const std::size_t body_length_tag_start = begin_delimiter + 1;
    if (body_length_tag_start >= input.size()) return {ErrorCode::need_more, 9, input.size()};
    if (input[body_length_tag_start] != '9') return {ErrorCode::invalid_header, 0, body_length_tag_start};
    if (body_length_tag_start + 1 >= input.size()) return {ErrorCode::need_more, 9, input.size()};
    if (input[body_length_tag_start + 1] != '=') return {ErrorCode::invalid_header, 0, body_length_tag_start};
    tag = 9;
    equals_at = body_length_tag_start + 1;

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
    std::uint64_t sum = 0;
    const auto* raw = reinterpret_cast<const unsigned char*>(input.data());
    for (std::size_t i = 0; i < checksum_offset; ++i) sum += raw[i];
    if ((sum & 0xffU) != check_value) return {ErrorCode::invalid_checksum, 10, checksum_offset + 3};

    output.bytes = input.substr(0, frame_size);
    output.consumed = frame_size;
    output.version = version;
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

Status parse_flat_verified(FrameView frame,
                           std::span<FieldView> scratch,
                           FlatMessageView& output,
                           WireHints hints) noexcept {
    output.frame_ = {};
    output.fields_ = {};
    if (frame.bytes.size() < 26 || frame.consumed != frame.bytes.size())
        return {ErrorCode::invalid_frame, 0, 0};

    const std::size_t checksum_offset = frame.bytes.size() - 7;
    const std::size_t first_header_delimiter = frame.bytes.find(soh);
    if (first_header_delimiter == std::string_view::npos || first_header_delimiter + 1 >= checksum_offset)
        return {ErrorCode::invalid_header, 8, 0};
    const std::size_t second_header_delimiter = frame.bytes.find(soh, first_header_delimiter + 1);
    if (second_header_delimiter == std::string_view::npos || second_header_delimiter >= checksum_offset)
        return {ErrorCode::invalid_header, 9, first_header_delimiter + 1};
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

    const char* const raw_bytes = frame.bytes.data();
    const std::size_t frame_len = frame.bytes.size();
    bool has_extra_hints = false;
    std::uint64_t extra_len_bloom = 0;
    std::uint64_t extra_data_bloom = 0;
    for (const auto p : hints.length_data_pairs) {
        if (!is_builtin_pair(p)) {
            has_extra_hints = true;
            extra_len_bloom |= (std::uint64_t{1} << (p.length_tag & 63U));
            extra_data_bloom |= (std::uint64_t{1} << (p.data_tag & 63U));
        }
    }

    Status status{};
    while (cursor < frame_len) {
        std::uint32_t current_tag = 0;
        std::size_t equals = 0;
        status = scan_tag(frame.bytes, cursor, frame_len, current_tag, equals);
        if (!status.ok()) return status;
        const std::size_t value_start = equals + 1;
        if (cursor == checksum_offset) {
            if (current_tag != 10) return {ErrorCode::invalid_checksum, 10, cursor};
            status = add_field(current_tag, value_start, 3, cursor);
            if (!status.ok()) return status;
            cursor = frame_len;
            continue;
        }
        if (cursor > checksum_offset || current_tag == 10)
            return {ErrorCode::invalid_checksum, 10, cursor};
        if ((current_tag == 8 && cursor != 0) ||
            (current_tag == 9 && cursor != first_header_delimiter + 1) ||
            (current_tag == 35 && cursor != body_start))
            return {ErrorCode::invalid_header, static_cast<int>(current_tag), cursor};

        LengthDataPair pair{};
        if (find_length_pair_fast(current_tag, hints, has_extra_hints, extra_len_bloom, pair)) {
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
            if (raw_bytes[data_delimiter] != soh)
                return error(ErrorCode::invalid_value, data_tag, data_delimiter);
            status = add_field(data_tag, data_start, data_length, data_field_start);
            if (!status.ok()) return status;
            cursor = data_delimiter + 1;
            continue;
        }
        if (is_data_tag_fast(current_tag, hints, has_extra_hints, extra_data_bloom))
            return error(ErrorCode::invalid_value, current_tag, cursor);

        // Single-pass scan for SOH delimiter + ASCII printable text validation.
        std::size_t delimiter = value_start;
        while (delimiter < checksum_offset) {
            const unsigned char ch = static_cast<unsigned char>(raw_bytes[delimiter]);
            if (ch == static_cast<unsigned char>(soh)) break;
            if (ch < 0x20U || ch == 0x7fU)
                return error(ErrorCode::invalid_value, current_tag, value_start);
            ++delimiter;
        }
        if (delimiter == checksum_offset)
            return error(ErrorCode::invalid_frame, current_tag, checksum_offset);
        if (delimiter == value_start)
            return error(ErrorCode::invalid_value, current_tag, value_start);
        status = add_field(current_tag, value_start, delimiter - value_start, cursor);
        if (!status.ok()) return status;
        cursor = delimiter + 1;
    }

    output.frame_ = frame.bytes;
    output.fields_ = std::span<const FieldView>(scratch.data(), count);
    return Status::success();
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
    const Status status = inspect_frame(frame.bytes, verified, hints, limits);
    if (!status.ok()) return status;
    if (verified.consumed != frame.bytes.size())
        return {ErrorCode::invalid_frame, 0, verified.consumed};
    return parse_flat_verified(verified, scratch, output, hints);
}

} // namespace nimblefix
