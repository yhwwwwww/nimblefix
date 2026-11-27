#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <type_traits>

#include <nimblefix/codec.hpp>

namespace nimblefix {

enum class FieldClass : std::uint8_t {
    text,
    character,
    boolean,
    signed_integer,
    unsigned_integer,
    decimal,
    utc_timestamp,
    utc_date,
    utc_time,
    local_market_date,
    month_year,
    day_of_month,
    binary_data,
    tag_number,
    multiple_value_string,
    multiple_char_value,
    multiple_string_value,
};

enum class MemberKind : std::uint8_t { field, group };

struct EnumValueDescriptor {
    std::uint32_t tag{};
    std::string_view value{};
    std::string_view description{};
};

struct FieldDescriptor {
    std::uint32_t tag{};
    std::string_view name{};
    std::string_view type{};
    FieldClass field_class{FieldClass::text};
    WireType wire_type{WireType::string};
    std::span<const EnumValueDescriptor> enum_values{};
    std::uint32_t length_tag{};
    std::uint32_t data_tag{};
};

struct MessageMemberDescriptor {
    MemberKind kind{MemberKind::field};
    std::uint32_t field_index{};
    std::uint32_t group_index{};
    bool required{};
};

struct MemberTagEntry {
    std::uint32_t tag{};
    std::uint32_t member_index{};
};

struct GroupDescriptor {
    std::string_view name{};
    std::uint32_t count_tag{};
    std::uint32_t delimiter_tag{};
    std::span<const MessageMemberDescriptor> members{};
    std::span<const MemberTagEntry> members_by_tag{};
    std::span<const std::uint32_t> required_members{};
    std::span<const std::uint32_t> group_count_tags{};
    std::uint64_t group_count_tag_bloom{};
    bool has_group_index{};
};

struct ComponentDescriptor {
    std::string_view name{};
    std::span<const MessageMemberDescriptor> members{};
    std::span<const MemberTagEntry> members_by_tag{};
    std::span<const std::uint32_t> group_count_tags{};
    std::uint64_t group_count_tag_bloom{};
    bool has_group_index{};
    std::uint16_t required_session_mask{0xFFFFu};
};

struct MessageDescriptor {
    std::string_view name{};
    std::string_view msg_type{};
    std::string_view category{};
    std::span<const MessageMemberDescriptor> members{};
    std::span<const MemberTagEntry> members_by_tag{};
    std::span<const std::uint32_t> required_members{};
    std::span<const std::uint32_t> group_count_tags{};
    std::uint64_t group_count_tag_bloom{};
    bool has_group_index{};

    [[nodiscard]] constexpr std::uint32_t member_tag(
        const MessageMemberDescriptor& member,
        std::span<const FieldDescriptor> fields,
        std::span<const GroupDescriptor> groups) const noexcept {
        if (member.kind == MemberKind::field) {
            return member.field_index < fields.size() ? fields[member.field_index].tag : 0;
        }
        return member.group_index < groups.size() ? groups[member.group_index].count_tag : 0;
    }
};

enum class ValidationLevel : std::uint8_t {
    structure,
    required,
    strict,
};

enum class UnexpectedFieldPolicy : std::uint8_t {
    reject = 0,
    ignore_with_metric = 1,
    passthrough = 2,
};

using FastValidateFn = Status (*)(std::string_view msg_type,
                                  std::size_t msg_type_offset,
                                  std::span<const FieldView> fields,
                                  ValidationLevel level,
                                  detail::ParsedFieldIndex* out_index) noexcept;

struct SchemaDescriptor {
    std::string_view type{};
    std::string_view version{};
    std::string_view fingerprint{};
    std::span<const FieldDescriptor> fields{};
    std::span<const GroupDescriptor> groups{};
    std::span<const ComponentDescriptor> components{};
    std::span<const MessageDescriptor> messages{};
    std::int32_t header_component_index{-1};
    std::int32_t trailer_component_index{-1};
    std::span<const std::int16_t> single_char_message_index{};
    FastValidateFn fast_validate{nullptr};

    // Dictionary application version and session transport are distinct for FIXT.
    [[nodiscard]] constexpr bool matches_session(FixVersion transport,
        std::string_view appl_ver_id = {}) const noexcept {
        if (type == "FIXT")
            return transport == FixVersion::fixt11 && (version == "1.1" || version == "1.1.0");
        if (type != "FIX") return false;
        const auto base = [this](std::string_view plain, std::string_view zero) {
            return version == plain || version == zero;
        };
        if (transport == FixVersion::fixt11) {
            return (base("4.2", "4.2.0") && appl_ver_id == appl_ver_id::fix42) ||
                (base("4.3", "4.3.0") && appl_ver_id == appl_ver_id::fix43) ||
                (base("4.4", "4.4.0") && appl_ver_id == appl_ver_id::fix44) ||
                (base("5.0", "5.0.0") && appl_ver_id == appl_ver_id::fix50) ||
                (version == "5.0.1" && appl_ver_id == appl_ver_id::fix50sp1) ||
                (version == "5.0.2" && appl_ver_id == appl_ver_id::fix50sp2);
        }
        return (transport == FixVersion::fix42 && base("4.2", "4.2.0")) ||
            (transport == FixVersion::fix43 && base("4.3", "4.3.0")) ||
            (transport == FixVersion::fix44 && base("4.4", "4.4.0"));
    }

    [[nodiscard]] constexpr const FieldDescriptor* find_field(std::uint32_t tag) const noexcept {
        std::size_t first = 0;
        std::size_t last = fields.size();
        while (first < last) {
            const std::size_t middle = first + (last - first) / 2;
            if (fields[middle].tag < tag) first = middle + 1;
            else last = middle;
        }
        return first < fields.size() && fields[first].tag == tag ? &fields[first] : nullptr;
    }

    [[nodiscard]] constexpr const FieldDescriptor* find_field(std::string_view name) const noexcept {
        for (const auto& field : fields) if (field.name == name) return &field;
        return nullptr;
    }

    [[nodiscard]] constexpr const MessageDescriptor* find_message(std::string_view msg_type) const noexcept {
        if (msg_type.size() == 1 && single_char_message_index.size() == 128) {
            const auto code = static_cast<unsigned char>(msg_type[0]);
            if (code < 128) {
                const auto idx = single_char_message_index[code];
                return idx >= 0 && static_cast<std::size_t>(idx) < messages.size() ? &messages[static_cast<std::size_t>(idx)] : nullptr;
            }
        }
        for (const auto& message : messages) if (message.msg_type == msg_type) return &message;
        return nullptr;
    }

    [[nodiscard]] constexpr const MessageDescriptor* find_message_by_name(std::string_view name) const noexcept {
        for (const auto& message : messages) if (message.name == name) return &message;
        return nullptr;
    }
};

template <class Schema, std::uint32_t Tag>
struct SchemaField;

template <FieldClass Class>
struct FieldValueTypeImpl { using type = std::string_view; };

template <>
struct FieldValueTypeImpl<FieldClass::character> { using type = char; };
template <>
struct FieldValueTypeImpl<FieldClass::boolean> { using type = bool; };
template <>
struct FieldValueTypeImpl<FieldClass::signed_integer> { using type = std::int64_t; };
template <>
struct FieldValueTypeImpl<FieldClass::unsigned_integer> { using type = std::uint64_t; };
template <>
struct FieldValueTypeImpl<FieldClass::tag_number> { using type = std::uint64_t; };
template <>
struct FieldValueTypeImpl<FieldClass::day_of_month> { using type = std::uint64_t; };
template <>
struct FieldValueTypeImpl<FieldClass::decimal> { using type = Decimal; };

template <class Schema, std::uint32_t Tag>
using FieldValueType = typename FieldValueTypeImpl<SchemaField<Schema, Tag>::field_class>::type;

namespace detail {

inline constexpr std::uint32_t no_field_tag = 0;
inline constexpr std::size_t max_schema_group_depth = 64;

[[nodiscard]] inline const FieldDescriptor* descriptor_field(
    const SchemaDescriptor& schema, const MessageMemberDescriptor& member) noexcept {
    if (member.kind != MemberKind::field || member.field_index >= schema.fields.size()) return nullptr;
    return &schema.fields[member.field_index];
}

[[nodiscard]] inline const GroupDescriptor* descriptor_group(
    const SchemaDescriptor& schema, const MessageMemberDescriptor& member) noexcept {
    if (member.kind != MemberKind::group || member.group_index >= schema.groups.size()) return nullptr;
    return &schema.groups[member.group_index];
}

[[nodiscard]] inline std::uint32_t member_tag(
    const SchemaDescriptor& schema, const MessageMemberDescriptor& member) noexcept {
    if (const auto* field = descriptor_field(schema, member)) return field->tag;
    if (const auto* group = descriptor_group(schema, member)) return group->count_tag;
    return no_field_tag;
}

[[nodiscard]] inline bool is_session_or_frame_tag(std::uint32_t t) noexcept {
    return t == tag::begin_string || t == tag::body_length || t == tag::check_sum ||
        t == tag::msg_seq_num || t == tag::msg_type || t == tag::poss_dup_flag ||
        t == tag::sender_comp_id || t == tag::sending_time || t == tag::target_comp_id ||
        t == tag::orig_sending_time || t == tag::last_msg_seq_num_processed ||
        t == tag::appl_ver_id;
}

[[nodiscard]] inline bool parse_unsigned(std::string_view text, std::uint64_t& value) noexcept {
    if (text.empty()) return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

[[nodiscard]] inline bool parse_signed(std::string_view text, std::int64_t& value) noexcept {
    if (text.empty()) return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

[[nodiscard]] inline bool valid_date(std::string_view value) noexcept {
    if (value.size() != 8) return false;
    for (const char c : value) if (c < '0' || c > '9') return false;
    const unsigned month = static_cast<unsigned>((value[4] - '0') * 10 + value[5] - '0');
    const unsigned day = static_cast<unsigned>((value[6] - '0') * 10 + value[7] - '0');
    const unsigned year = static_cast<unsigned>((value[0] - '0') * 1000 + (value[1] - '0') * 100 +
        (value[2] - '0') * 10 + value[3] - '0');
    constexpr unsigned month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return false;
    unsigned max_day = month_days[month - 1];
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) ++max_day;
    return day >= 1 && day <= max_day;
}

[[nodiscard]] inline bool valid_time(std::string_view value) noexcept {
    if (value.size() != 8 && value.size() != 12 && value.size() != 15 && value.size() != 18) return false;
    for (std::size_t i = 0; i < 8; ++i) {
        if (i == 2 || i == 5) {
            if (value[i] != ':') return false;
        } else if (value[i] < '0' || value[i] > '9') return false;
    }
    const auto number = [&](std::size_t at) {
        return static_cast<unsigned>((value[at] - '0') * 10 + value[at + 1] - '0');
    };
    if (number(0) > 23 || number(3) > 59 || number(6) > 60) return false;
    if (value.size() > 8) {
        if (value[8] != '.') return false;
        for (std::size_t i = 9; i < value.size(); ++i) if (value[i] < '0' || value[i] > '9') return false;
    }
    return true;
}

[[nodiscard]] inline bool valid_month_year(std::string_view value) noexcept {
    if (value.size() != 6 && value.size() != 8) return false;
    for (std::size_t i = 0; i < 6; ++i)
        if (value[i] < '0' || value[i] > '9') return false;
    const auto month = static_cast<unsigned>((value[4] - '0') * 10 + value[5] - '0');
    if (month < 1 || month > 12) return false;
    if (value.size() == 6) return true;
    if (value[6] == 'w') return value[7] >= '1' && value[7] <= '5';
    if (value[6] < '0' || value[6] > '9' || value[7] < '0' || value[7] > '9') return false;
    const auto day = static_cast<unsigned>((value[6] - '0') * 10 + value[7] - '0');
    const auto year = static_cast<unsigned>((value[0] - '0') * 1000 + (value[1] - '0') * 100 +
        (value[2] - '0') * 10 + (value[3] - '0'));
    constexpr unsigned month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    unsigned max_day = month_days[month - 1];
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) ++max_day;
    return day >= 1 && day <= max_day;
}

[[nodiscard]] inline bool valid_mvs_value(std::string_view value, bool single_char) noexcept {
    if (!detail::is_valid_text(value)) return false;
    for (std::size_t first = 0; first < value.size();) {
        const auto separator = value.find(' ', first);
        const auto last = separator == std::string_view::npos ? value.size() : separator;
        if (last == first || (single_char && last - first != 1)) return false;
        first = separator == std::string_view::npos ? value.size() : separator + 1;
        if (first == value.size() && separator != std::string_view::npos) return false;
    }
    return true;
}

using TagPredicateFn = bool (*)(std::uint32_t tag) noexcept;

struct FastAncestorStack {
    static constexpr std::size_t max_depth = max_schema_group_depth + 2;
    TagPredicateFn predicates[max_depth];
    std::size_t count{0};
    TagPredicateFn trailer_pred{nullptr};

    [[nodiscard]] bool contains(std::uint32_t tag) const noexcept {
        for (std::size_t i = count; i > 0; --i) {
            if (predicates[i - 1](tag)) return true;
        }
        return trailer_pred != nullptr && trailer_pred(tag);
    }
};

[[nodiscard]] inline Status check_char(const FieldView& f) noexcept {
    if (f.value.size() == 1 && static_cast<unsigned char>(f.value[0]) >= 0x20U && f.value[0] != 0x7f)
        return Status::success();
    return {ErrorCode::invalid_value, static_cast<int>(f.tag), f.offset};
}
[[nodiscard]] inline Status check_bool(const FieldView& f) noexcept {
    if (f.value.size() == 1 && (f.value[0] == 'Y' || f.value[0] == 'N')) return Status::success();
    return {ErrorCode::invalid_value, static_cast<int>(f.tag), f.offset};
}
[[nodiscard]] inline Status check_i64(const FieldView& f) noexcept {
    std::int64_t v{};
    if (parse_signed(f.value, v)) return Status::success();
    return {ErrorCode::invalid_value, static_cast<int>(f.tag), f.offset};
}
[[nodiscard]] inline Status check_u64(const FieldView& f) noexcept {
    std::uint64_t v{};
    if (parse_unsigned(f.value, v)) return Status::success();
    return {ErrorCode::invalid_value, static_cast<int>(f.tag), f.offset};
}
[[nodiscard]] inline Status check_day_of_month(const FieldView& f) noexcept {
    std::uint64_t v{};
    if (parse_unsigned(f.value, v) && v >= 1U && v <= 31U) return Status::success();
    return {ErrorCode::invalid_value, static_cast<int>(f.tag), f.offset};
}
[[nodiscard]] inline Status check_tag_num(const FieldView& f) noexcept {
    std::uint64_t v{};
    if (!f.value.empty() && f.value[0] != '0' && parse_unsigned(f.value, v) &&
        v > 0U && v <= maximum_fix_tag) return Status::success();
    return {ErrorCode::invalid_value, static_cast<int>(f.tag), f.offset};
}
[[nodiscard]] inline Status check_decimal(const FieldView& f) noexcept {
    if (is_valid_decimal_fast(f.value)) return Status::success();
    return {ErrorCode::invalid_value, static_cast<int>(f.tag), f.offset};
}
[[nodiscard]] inline Status check_utc_ts(const FieldView& f) noexcept {
    if (is_valid_utc_timestamp(f.value)) return Status::success();
    return {ErrorCode::invalid_timestamp, static_cast<int>(f.tag), f.offset};
}
[[nodiscard]] inline Status check_date(const FieldView& f) noexcept {
    if (valid_date(f.value)) return Status::success();
    return {ErrorCode::invalid_value, static_cast<int>(f.tag), f.offset};
}
[[nodiscard]] inline Status check_time(const FieldView& f) noexcept {
    if (valid_time(f.value)) return Status::success();
    return {ErrorCode::invalid_value, static_cast<int>(f.tag), f.offset};
}
[[nodiscard]] inline Status check_month_year(const FieldView& f) noexcept {
    if (valid_month_year(f.value)) return Status::success();
    return {ErrorCode::invalid_value, static_cast<int>(f.tag), f.offset};
}
[[nodiscard]] inline Status check_binary(const FieldView& f) noexcept {
    if (!f.value.empty()) return Status::success();
    return {ErrorCode::invalid_value, static_cast<int>(f.tag), f.offset};
}
[[nodiscard]] inline Status check_text(const FieldView& f) noexcept {
    if (is_valid_text(f.value)) return Status::success();
    return {ErrorCode::invalid_value, static_cast<int>(f.tag), f.offset};
}
[[nodiscard]] inline Status check_mvs(const FieldView& f, bool single_char) noexcept {
    if (valid_mvs_value(f.value, single_char)) return Status::success();
    return {ErrorCode::invalid_value, static_cast<int>(f.tag), f.offset};
}
[[nodiscard]] inline Status check_len_data_pair(std::span<const FieldView> fields,
    std::uint32_t len_tag, int len_idx, int data_idx) noexcept {
    if (data_idx >= 0) {
        if (len_idx < 0) return {ErrorCode::missing_field, static_cast<int>(len_tag)};
        std::uint64_t len{};
        if (!parse_unsigned(fields[static_cast<std::size_t>(len_idx)].value, len) ||
            len == 0 || len != fields[static_cast<std::size_t>(data_idx)].value.size()) {
            return {ErrorCode::invalid_value, static_cast<int>(len_tag),
                fields[static_cast<std::size_t>(len_idx)].offset};
        }
    } else if (len_idx >= 0) {
        return {ErrorCode::invalid_value, static_cast<int>(len_tag),
            fields[static_cast<std::size_t>(len_idx)].offset};
    }
    return Status::success();
}

[[nodiscard]] inline bool valid_field_value(const FieldDescriptor& field, std::string_view value) noexcept {
    switch (field.field_class) {
    case FieldClass::character: return value.size() == 1 && static_cast<unsigned char>(value[0]) >= 0x20 && value[0] != 0x7f;
    case FieldClass::boolean: return value == "Y" || value == "N";
    case FieldClass::signed_integer: {
        std::int64_t parsed{};
        return parse_signed(value, parsed);
    }
    case FieldClass::unsigned_integer:
    case FieldClass::day_of_month: {
        std::uint64_t parsed{};
        if (!parse_unsigned(value, parsed)) return false;
        return field.field_class != FieldClass::day_of_month || (parsed >= 1 && parsed <= 31);
    }
    case FieldClass::tag_number: {
        if (value.empty() || value[0] == '0') return false;
        std::uint64_t parsed{};
        return parse_unsigned(value, parsed) && parsed > 0 && parsed <= maximum_fix_tag;
    }
    case FieldClass::decimal: return detail::is_valid_decimal_fast(value);
    case FieldClass::utc_timestamp: return detail::is_valid_utc_timestamp(value);
    case FieldClass::utc_date:
    case FieldClass::local_market_date: return valid_date(value);
    case FieldClass::utc_time: return valid_time(value);
    case FieldClass::month_year: return valid_month_year(value);
    case FieldClass::binary_data: return !value.empty();
    case FieldClass::text: return detail::is_valid_text(value);
    case FieldClass::multiple_value_string:
    case FieldClass::multiple_char_value:
    case FieldClass::multiple_string_value:
        return valid_mvs_value(value, field.field_class == FieldClass::multiple_char_value);
    }
    return false;
}

[[nodiscard]] inline bool enum_contains(const FieldDescriptor& field, std::string_view value) noexcept {
    if (value.size() == 1) {
        const char ch = value[0];
        for (const auto& entry : field.enum_values) {
            if (entry.value.size() == 1 && entry.value[0] == ch) return true;
        }
        return false;
    }
    for (const auto& entry : field.enum_values) if (entry.value == value) return true;
    return false;
}

[[nodiscard]] inline bool enum_values_accept(const FieldDescriptor& field, std::string_view value) noexcept {
    if (field.enum_values.empty()) return true;
    if (field.field_class != FieldClass::multiple_value_string &&
        field.field_class != FieldClass::multiple_char_value &&
        field.field_class != FieldClass::multiple_string_value) return enum_contains(field, value);
    for (std::size_t first = 0; first < value.size();) {
        const auto separator = value.find(' ', first);
        const auto last = separator == std::string_view::npos ? value.size() : separator;
        if (!enum_contains(field, value.substr(first, last - first))) return false;
        first = separator == std::string_view::npos ? value.size() : separator + 1;
    }
    return true;
}

struct MemberLevel {
    std::span<const MessageMemberDescriptor> members{};
    std::span<const MemberTagEntry> members_by_tag{};
};

[[nodiscard]] inline const MessageMemberDescriptor* find_member_in_level(
    const SchemaDescriptor& schema, const MemberLevel& level,
    std::uint32_t tag, std::size_t* out_index = nullptr) noexcept {
    if (!level.members_by_tag.empty()) {
        std::size_t first = 0;
        std::size_t last = level.members_by_tag.size();
        while (first < last) {
            const std::size_t mid = first + (last - first) / 2;
            if (level.members_by_tag[mid].tag < tag) first = mid + 1;
            else last = mid;
        }
        if (first < level.members_by_tag.size() && level.members_by_tag[first].tag == tag) {
            const auto idx = level.members_by_tag[first].member_index;
            if (idx < level.members.size()) {
                if (out_index != nullptr) *out_index = idx;
                return &level.members[idx];
            }
        }
        return nullptr;
    }
    for (std::size_t i = 0; i < level.members.size(); ++i) {
        if (member_tag(schema, level.members[i]) == tag) {
            if (out_index != nullptr) *out_index = i;
            return &level.members[i];
        }
    }
    return nullptr;
}

[[nodiscard]] inline bool member_contains_tag(const SchemaDescriptor& schema,
    const MemberLevel& level, std::uint32_t tag) noexcept {
    return find_member_in_level(schema, level, tag) != nullptr;
}

[[nodiscard]] inline const ComponentDescriptor* find_component(
    const SchemaDescriptor& schema, std::string_view name) noexcept {
    for (const auto& component : schema.components) if (component.name == name) return &component;
    return nullptr;
}

[[nodiscard]] inline const ComponentDescriptor* find_header_component(const SchemaDescriptor& schema) noexcept {
    if (schema.header_component_index >= 0 &&
        static_cast<std::size_t>(schema.header_component_index) < schema.components.size()) {
        return &schema.components[static_cast<std::size_t>(schema.header_component_index)];
    }
    if (schema.single_char_message_index.size() == 128) return nullptr;
    return find_component(schema, "Header");
}

[[nodiscard]] inline const ComponentDescriptor* find_trailer_component(const SchemaDescriptor& schema) noexcept {
    if (schema.trailer_component_index >= 0 &&
        static_cast<std::size_t>(schema.trailer_component_index) < schema.components.size()) {
        return &schema.components[static_cast<std::size_t>(schema.trailer_component_index)];
    }
    if (schema.single_char_message_index.size() == 128) return nullptr;
    return find_component(schema, "Trailer");
}

struct AncestorStack {
    // Only entries [0, count) are accessed; avoid zero-initializing 2 KB per message.
    MemberLevel levels[max_schema_group_depth + 2];
    std::size_t count{0};
    const ComponentDescriptor* trailer{nullptr};

    AncestorStack() noexcept = default;
    AncestorStack(const AncestorStack& other) noexcept : count(other.count), trailer(other.trailer) {
        for (std::size_t i = 0; i < count; ++i) levels[i] = other.levels[i];
    }
    AncestorStack& operator=(const AncestorStack& other) noexcept {
        count = other.count;
        trailer = other.trailer;
        for (std::size_t i = 0; i < count; ++i) levels[i] = other.levels[i];
        return *this;
    }

    [[nodiscard]] bool contains_tag(const SchemaDescriptor& schema, std::uint32_t tag) const noexcept {
        for (std::size_t i = count; i > 0; --i) {
            if (member_contains_tag(schema, levels[i - 1], tag)) return true;
        }
        if (trailer != nullptr) {
            const MemberLevel trailer_level{trailer->members, trailer->members_by_tag};
            if (member_contains_tag(schema, trailer_level, tag)) return true;
        }
        return false;
    }
};

struct GroupScan {
    Status status{};
    std::size_t end{};
};

[[nodiscard]] inline GroupScan scan_group_entry(const SchemaDescriptor& schema,
    std::span<const FieldView> wire_fields, const AncestorStack& ancestors,
    const GroupDescriptor& descriptor, std::size_t entry_start, std::size_t depth) noexcept;

[[nodiscard]] inline GroupScan skip_group(const SchemaDescriptor& schema,
    std::span<const FieldView> wire_fields, const AncestorStack& ancestors,
    const MemberLevel& parent_level, std::size_t count_index, std::size_t depth) noexcept {
    if (depth > max_schema_group_depth || count_index >= wire_fields.size())
        return {{ErrorCode::invalid_value, 0, count_index}, count_index};
    const auto* group_member = find_member_in_level(schema, parent_level, wire_fields[count_index].tag);
    if (group_member == nullptr || group_member->kind != MemberKind::group)
        return {{ErrorCode::invalid_value,
            static_cast<int>(wire_fields[count_index].tag), wire_fields[count_index].offset}, count_index};
    const auto* descriptor = descriptor_group(schema, *group_member);
    std::uint64_t count{};
    if (descriptor == nullptr || !parse_unsigned(wire_fields[count_index].value, count) ||
        count > wire_fields.size() - count_index - 1) {
        return {{ErrorCode::invalid_value, static_cast<int>(wire_fields[count_index].tag),
            wire_fields[count_index].offset}, count_index};
    }
    AncestorStack child_ancestors = ancestors;
    if (child_ancestors.count < max_schema_group_depth + 2)
        child_ancestors.levels[child_ancestors.count++] = parent_level;
    std::size_t cursor = count_index + 1;
    for (std::size_t entry_index = 0; entry_index < count; ++entry_index) {
        const auto entry = scan_group_entry(schema, wire_fields, child_ancestors,
            *descriptor, cursor, depth + 1);
        if (!entry.status.ok()) return entry;
        cursor = entry.end;
    }
    return {Status::success(), cursor};
}

[[nodiscard]] inline GroupScan scan_group_entry(const SchemaDescriptor& schema,
    std::span<const FieldView> wire_fields, const AncestorStack& ancestors,
    const GroupDescriptor& descriptor, std::size_t entry_start, std::size_t depth) noexcept {
    if (depth > max_schema_group_depth || entry_start >= wire_fields.size() ||
        wire_fields[entry_start].tag != descriptor.delimiter_tag) {
        const auto offset = entry_start < wire_fields.size() ? wire_fields[entry_start].offset : 0;
        return {{ErrorCode::invalid_value, static_cast<int>(descriptor.delimiter_tag), offset}, entry_start};
    }
    const MemberLevel group_level{descriptor.members, descriptor.members_by_tag};
    std::size_t cursor = entry_start + 1;
    if (!descriptor.members.empty() && descriptor.members.front().kind == MemberKind::group) {
        const auto delimiter_group = skip_group(schema, wire_fields, ancestors, group_level, entry_start, depth + 1);
        if (!delimiter_group.status.ok()) return delimiter_group;
        cursor = delimiter_group.end;
    }
    while (cursor < wire_fields.size()) {
        const auto tag = wire_fields[cursor].tag;
        if (tag == descriptor.delimiter_tag) break;
        if (const auto* member = find_member_in_level(schema, group_level, tag)) {
            if (member->kind == MemberKind::group) {
                const auto nested = skip_group(schema, wire_fields, ancestors, group_level, cursor, depth + 1);
                if (!nested.status.ok()) return nested;
                cursor = nested.end;
            } else {
                ++cursor;
            }
            continue;
        }
        if (tag == tag::check_sum || ancestors.contains_tag(schema, tag)) break;
        // Unknown extension fields stay within this entry until a delimiter or
        // a known field owned by any ancestor level or Trailer starts.
        ++cursor;
    }
    return {Status::success(), cursor};
}

struct LevelLookup {
    Status status{};
    std::size_t index{};
    bool found{};
};

[[nodiscard]] constexpr std::uint64_t group_tag_bloom_bit(std::uint32_t tag) noexcept {
    return std::uint64_t{1} << (tag & 63U);
}

// Most levels contain only a few groups. Prefer precomputed group_count_tags +
// 64-bit bloom filter (O(1) construction, 1-instruction scalar rejection) and
// fall back to scanning members only for hand-crafted descriptors without an index.
class LevelGroupLookup {
public:
    LevelGroupLookup(const SchemaDescriptor& schema,
        std::span<const MessageMemberDescriptor> members,
        const ComponentDescriptor* header = nullptr) noexcept
        : schema_(schema), members_(members), header_(header) {
        add_groups(members);
        if (header != nullptr) add_groups(header->members);
    }

    LevelGroupLookup(const SchemaDescriptor& schema,
        std::span<const MessageMemberDescriptor> members,
        std::span<const std::uint32_t> level_group_tags,
        std::uint64_t level_group_bloom,
        bool has_level_group_index,
        const ComponentDescriptor* header = nullptr) noexcept
        : schema_(schema), members_(members), header_(header) {
        if (has_level_group_index && (header == nullptr || header->has_group_index)) {
            indexed_ = true;
            level_tags_ = level_group_tags;
            header_tags_ = header != nullptr ? header->group_count_tags : std::span<const std::uint32_t>{};
            bloom_ = level_group_bloom | (header != nullptr ? header->group_count_tag_bloom : 0ULL);
            return;
        }
        add_groups(members);
        if (header != nullptr) add_groups(header->members);
    }

    [[nodiscard]] bool contains(std::uint32_t tag) const noexcept {
        if (indexed_) {
            if ((bloom_ & group_tag_bloom_bit(tag)) == 0) return false;
            for (const auto group_tag : level_tags_) {
                if (group_tag == tag) return true;
            }
            for (const auto group_tag : header_tags_) {
                if (group_tag == tag) return true;
            }
            return false;
        }
        if (overflow_) {
            const auto has_in = [this, tag](std::span<const MessageMemberDescriptor> list) noexcept {
                return std::any_of(list.begin(), list.end(), [&](const auto& member) {
                    const auto* group = descriptor_group(schema_, member);
                    return group != nullptr && group->count_tag == tag;
                });
            };
            return has_in(members_) || (header_ != nullptr && has_in(header_->members));
        }
        if ((bloom_ & group_tag_bloom_bit(tag)) == 0) return false;
        for (std::size_t i = 0; i < count_; ++i)
            if (groups_[i]->count_tag == tag) return true;
        return false;
    }

private:
    void add_groups(std::span<const MessageMemberDescriptor> list) noexcept {
        for (const auto& member : list) {
            const auto* group = descriptor_group(schema_, member);
            if (group == nullptr) continue;
            bloom_ |= group_tag_bloom_bit(group->count_tag);
            if (count_ == 16U) {
                overflow_ = true;
                return;
            }
            groups_[count_++] = group;
        }
    }

    const SchemaDescriptor& schema_;
    std::span<const MessageMemberDescriptor> members_;
    const ComponentDescriptor* header_{};
    std::span<const std::uint32_t> level_tags_{};
    std::span<const std::uint32_t> header_tags_{};
    std::uint64_t bloom_{};
    bool indexed_{};
    const GroupDescriptor* groups_[16];
    std::size_t count_{};
    bool overflow_{};
};

[[nodiscard]] inline LevelLookup find_level_field(const SchemaDescriptor& schema,
    const MemberLevel& level, std::span<const FieldView> fields,
    std::uint32_t tag, const LevelGroupLookup& groups,
    const AncestorStack& ancestors, const ComponentDescriptor* header = nullptr) noexcept {
    LevelLookup result{};
    for (std::size_t cursor = 0; cursor < fields.size();) {
        const auto current_tag = fields[cursor].tag;
        if (current_tag == tag) {
            if (result.found) {
                result.status = {ErrorCode::duplicate_field, static_cast<int>(tag), fields[cursor].offset};
                return result;
            }
            result.index = cursor;
            result.found = true;
        }
        if (groups.contains(current_tag)) {
            const MemberLevel* owner = &level;
            MemberLevel header_level{};
            if (header != nullptr && find_member_in_level(schema, level, current_tag) == nullptr) {
                header_level = {header->members, header->members_by_tag};
                owner = &header_level;
            }
            const auto scan = skip_group(schema, fields, ancestors, *owner, cursor, 0);
            if (!scan.status.ok()) { result.status = scan.status; return result; }
            cursor = scan.end;
            continue;
        }
        ++cursor;
    }
    if (!result.found) result.status = {ErrorCode::missing_field, static_cast<int>(tag)};
    return result;
}

[[nodiscard]] inline LevelLookup find_level_field(const SchemaDescriptor& schema,
    std::span<const MessageMemberDescriptor> members, std::span<const FieldView> fields,
    std::uint32_t tag, std::span<const MemberTagEntry> members_by_tag = {},
    const AncestorStack* existing_ancestors = nullptr,
    const ComponentDescriptor* header = nullptr,
    std::span<const std::uint32_t> level_group_tags = {},
    std::uint64_t level_group_bloom = 0,
    bool has_level_group_index = false) noexcept {
    const MemberLevel level{members, members_by_tag};
    const LevelGroupLookup groups{schema, members, level_group_tags, level_group_bloom,
        has_level_group_index, header};
    AncestorStack default_ancestors{};
    if (existing_ancestors == nullptr) {
        default_ancestors.trailer = find_trailer_component(schema);
    }
    return find_level_field(schema, level, fields, tag, groups,
        existing_ancestors != nullptr ? *existing_ancestors : default_ancestors, header);
}

[[nodiscard]] inline const FieldView* find_top_level_field(
    const SchemaDescriptor& schema, const MessageDescriptor& message,
    const FlatMessageView& flat, std::uint32_t tag, Status& status) noexcept {
    const auto fields = flat.fields();
    const auto* header = find_header_component(schema);
    const auto result = find_level_field(schema, message.members, fields, tag,
        message.members_by_tag, nullptr, header,
        message.group_count_tags, message.group_count_tag_bloom, message.has_group_index);
    status = result.status;
    return result.found ? &fields[result.index] : nullptr;
}

// Single-pass wire-order validation: O(F log M + R) with zero heap allocations.
// Preserves structural error precedence (malformed group boundaries and
// duplicates fail before scalar or missing-required diagnostics).
[[nodiscard]] inline Status validate_members_fast(const SchemaDescriptor& schema,
    const MemberLevel& level, std::span<const std::uint32_t> required_members,
    std::span<const FieldView> fields, ValidationLevel validation_level,
    const AncestorStack& ancestors, std::size_t depth,
    UnexpectedFieldPolicy unexpected_policy = UnexpectedFieldPolicy::reject,
    std::uint64_t* unexpected_count = nullptr) noexcept {
    if (depth > max_schema_group_depth) return {ErrorCode::invalid_value};

    const bool top_level = (depth == 0);
    const auto* header = top_level ? find_header_component(schema) : nullptr;
    const auto* trailer = top_level ? ancestors.trailer : nullptr;
    const MemberLevel header_level = header != nullptr
        ? MemberLevel{header->members, header->members_by_tag} : MemberLevel{};
    const MemberLevel trailer_level = trailer != nullptr
        ? MemberLevel{trailer->members, trailer->members_by_tag} : MemberLevel{};

    // Track seen members without heap allocation (covers up to 2,048 members per level).
    constexpr std::size_t max_bitset_words = 32;
    constexpr std::size_t max_indexed_members = max_bitset_words * 64;
    std::uint64_t seen_bits[max_bitset_words];
    const bool use_bitset = level.members.size() <= max_indexed_members;
    if (use_bitset && !level.members.empty()) {
        const std::size_t bitset_words = (level.members.size() + 63U) >> 6U;
        std::memset(seen_bits, 0, bitset_words * sizeof(std::uint64_t));
    }

    struct SeenMember {
        std::uint32_t member_index;
        std::uint32_t wire_index;
        std::uint8_t owner; // 0 = level, 1 = header, 2 = trailer
    };
    constexpr std::size_t max_env_bitset_words = 4;
    std::uint64_t env_seen_bits[max_env_bitset_words]{};
    const std::size_t header_member_count = header != nullptr ? header->members.size() : 0;
    const std::size_t trailer_member_count = trailer != nullptr ? trailer->members.size() : 0;
    const bool use_env_bitset = (header_member_count + trailer_member_count) <= (max_env_bitset_words * 64);
    constexpr std::size_t stack_seen_cap = 256;
    SeenMember seen_list[stack_seen_cap];
    std::size_t seen_count = 0;
    bool seen_overflow = false;
    std::uint8_t section = 0; // 0 = header, 1 = body, 2 = trailer (top_level only)
    Status first_order_error{};
    Status first_unknown_error{};

    // Pass 1: Walk wire fields once to validate group structure, detect duplicates,
    // and collect seen member positions (including non-session Header/Trailer members).
    for (std::size_t cursor = 0; cursor < fields.size();) {
        const auto tag = fields[cursor].tag;
        if (top_level && is_session_or_frame_tag(tag)) {
            if (tag == tag::check_sum) {
                section = 2u;
            } else if (section != 0u && validation_level == ValidationLevel::strict && first_order_error.ok()) {
                first_order_error = {ErrorCode::invalid_value, static_cast<int>(tag), fields[cursor].offset};
            }
            ++cursor;
            continue;
        }
        std::size_t member_idx = 0;
        const auto* member = find_member_in_level(schema, level, tag, &member_idx);
        if (member != nullptr) {
            if (top_level) {
                if (section == 2u) {
                    if (validation_level == ValidationLevel::strict && first_order_error.ok()) {
                        first_order_error = {ErrorCode::invalid_value, static_cast<int>(tag), fields[cursor].offset};
                    }
                } else {
                    section = 1u;
                }
            }
            // Check duplicates.
            bool is_dup = false;
            if (use_bitset) {
                const auto mask = std::uint64_t{1} << (member_idx & 63U);
                if ((seen_bits[member_idx >> 6U] & mask) != 0) is_dup = true;
                else seen_bits[member_idx >> 6U] |= mask;
            } else {
                for (std::size_t s = 0; s < seen_count; ++s) {
                    if (seen_list[s].owner == 0 && seen_list[s].member_index == member_idx) { is_dup = true; break; }
                }
            }
            if (is_dup) {
                return {ErrorCode::duplicate_field, static_cast<int>(tag), fields[cursor].offset};
            }

            std::size_t next_cursor = cursor + 1;
            if (member->kind == MemberKind::group) {
                const auto scan = skip_group(schema, fields, ancestors, level, cursor, depth);
                if (!scan.status.ok()) return scan.status;
                next_cursor = scan.end;
            }
            if (seen_count < stack_seen_cap) {
                seen_list[seen_count++] = SeenMember{
                    static_cast<std::uint32_t>(member_idx),
                    static_cast<std::uint32_t>(cursor),
                    0};
            } else {
                seen_overflow = true;
            }
            cursor = next_cursor;
            continue;
        }

        bool allowed_envelope = false;
        if (header != nullptr) {
            std::size_t h_idx = 0;
            if (const auto* hm = find_member_in_level(schema, header_level, tag, &h_idx)) {
                if (section != 0u && validation_level == ValidationLevel::strict && first_order_error.ok()) {
                    first_order_error = {ErrorCode::invalid_value, static_cast<int>(tag), fields[cursor].offset};
                }
                bool is_dup = false;
                if (use_env_bitset) {
                    const auto mask = std::uint64_t{1} << (h_idx & 63U);
                    if ((env_seen_bits[h_idx >> 6U] & mask) != 0) is_dup = true;
                    else env_seen_bits[h_idx >> 6U] |= mask;
                } else {
                    for (std::size_t s = 0; s < seen_count; ++s) {
                        if (seen_list[s].owner == 1 && seen_list[s].member_index == h_idx) { is_dup = true; break; }
                    }
                }
                if (is_dup) {
                    return {ErrorCode::duplicate_field, static_cast<int>(tag), fields[cursor].offset};
                }
                std::size_t next_cursor = cursor + 1;
                if (hm->kind == MemberKind::group) {
                    const auto scan = skip_group(schema, fields, ancestors, header_level, cursor, depth);
                    if (!scan.status.ok()) return scan.status;
                    next_cursor = scan.end;
                }
                if (seen_count < stack_seen_cap) {
                    seen_list[seen_count++] = SeenMember{
                        static_cast<std::uint32_t>(h_idx),
                        static_cast<std::uint32_t>(cursor),
                        1};
                } else {
                    seen_overflow = true;
                }
                cursor = next_cursor;
                allowed_envelope = true;
            }
        }
        if (!allowed_envelope && trailer != nullptr) {
            std::size_t t_idx = 0;
            if (const auto* tm = find_member_in_level(schema, trailer_level, tag, &t_idx)) {
                section = 2u;
                const std::size_t env_bit = header_member_count + t_idx;
                bool is_dup = false;
                if (use_env_bitset) {
                    const auto mask = std::uint64_t{1} << (env_bit & 63U);
                    if ((env_seen_bits[env_bit >> 6U] & mask) != 0) is_dup = true;
                    else env_seen_bits[env_bit >> 6U] |= mask;
                } else {
                    for (std::size_t s = 0; s < seen_count; ++s) {
                        if (seen_list[s].owner == 2 && seen_list[s].member_index == t_idx) { is_dup = true; break; }
                    }
                }
                if (is_dup) {
                    return {ErrorCode::duplicate_field, static_cast<int>(tag), fields[cursor].offset};
                }
                std::size_t next_cursor = cursor + 1;
                if (tm->kind == MemberKind::group) {
                    const auto scan = skip_group(schema, fields, ancestors, trailer_level, cursor, depth);
                    if (!scan.status.ok()) return scan.status;
                    next_cursor = scan.end;
                }
                if (seen_count < stack_seen_cap) {
                    seen_list[seen_count++] = SeenMember{
                        static_cast<std::uint32_t>(t_idx),
                        static_cast<std::uint32_t>(cursor),
                        2};
                } else {
                    seen_overflow = true;
                }
                cursor = next_cursor;
                allowed_envelope = true;
            }
        }
        if (!allowed_envelope) {
            if (unexpected_policy == UnexpectedFieldPolicy::reject) {
                if (validation_level == ValidationLevel::strict && first_unknown_error.ok()) {
                    first_unknown_error = {ErrorCode::invalid_value, static_cast<int>(tag), fields[cursor].offset};
                }
            } else if (unexpected_count != nullptr) {
                ++(*unexpected_count);
            }
            ++cursor;
        }
    }

    // Helper to look up a seen wire index by tag (for Length/Data pair checks).
    const auto find_seen_wire_index = [&](std::uint32_t target_tag, std::uint8_t owner = 0) noexcept -> const FieldView* {
        if (!seen_overflow) {
            for (std::size_t s = 0; s < seen_count; ++s) {
                if (seen_list[s].owner != owner) continue;
                const auto& f = fields[seen_list[s].wire_index];
                if (f.tag == target_tag) return &f;
            }
            return nullptr;
        }
        const auto& target_level = owner == 1 ? header_level : (owner == 2 ? trailer_level : level);
        const auto lookup = find_level_field(schema, target_level.members, fields, target_tag,
            target_level.members_by_tag, &ancestors, owner == 0 ? header : nullptr);
        return lookup.found ? &fields[lookup.index] : nullptr;
    };

    // Pass 2: Validate scalar values, enums, Length/Data pairs, and nested groups
    // for all seen members (O(F) instead of O(M * F)).
    const auto validate_seen = [&](std::uint8_t owner, std::size_t member_idx, std::size_t wire_idx) noexcept -> Status {
        const auto& owner_level = owner == 1 ? header_level : (owner == 2 ? trailer_level : level);
        const auto& member = owner_level.members[member_idx];
        const auto& found = fields[wire_idx];
        const auto tag = found.tag;
        if (member.kind == MemberKind::field) {
            const auto* descriptor = descriptor_field(schema, member);
            if (descriptor == nullptr) return {ErrorCode::invalid_value, static_cast<int>(tag)};
            if (!valid_field_value(*descriptor, found.value)) {
                return {descriptor->field_class == FieldClass::utc_timestamp
                    ? ErrorCode::invalid_timestamp : ErrorCode::invalid_value,
                    static_cast<int>(tag), found.offset};
            }
            if (validation_level == ValidationLevel::strict && !enum_values_accept(*descriptor, found.value)) {
                return {ErrorCode::invalid_value, static_cast<int>(tag), found.offset};
            }
            if (descriptor->length_tag != 0) {
                const auto* len_field = find_seen_wire_index(descriptor->length_tag, owner);
                if (len_field == nullptr)
                    return {ErrorCode::missing_field, static_cast<int>(descriptor->length_tag)};
                std::uint64_t length{};
                if (!parse_unsigned(len_field->value, length) || length == 0 || length != found.value.size())
                    return {ErrorCode::invalid_value, static_cast<int>(descriptor->length_tag), len_field->offset};
            } else if (descriptor->data_tag != 0 && descriptor->field_class == FieldClass::unsigned_integer) {
                std::uint64_t length{};
                if (!parse_unsigned(found.value, length) || length == 0)
                    return {ErrorCode::invalid_value, static_cast<int>(tag), found.offset};
                const auto* data_field = find_seen_wire_index(descriptor->data_tag, owner);
                if (data_field == nullptr || length != data_field->value.size())
                    return {ErrorCode::invalid_value, static_cast<int>(tag), found.offset};
            }
            return Status::success();
        }

        const auto* group = descriptor_group(schema, member);
        if (group == nullptr) return {ErrorCode::invalid_value, static_cast<int>(tag)};
        std::uint64_t count{};
        if (!parse_unsigned(found.value, count) || count > fields.size() - wire_idx - 1)
            return {ErrorCode::invalid_value, static_cast<int>(tag), found.offset};
        if (validation_level != ValidationLevel::structure) {
            AncestorStack child_ancestors = ancestors;
            if (child_ancestors.count < max_schema_group_depth + 2)
                child_ancestors.levels[child_ancestors.count++] = owner_level;
            const MemberLevel group_level{group->members, group->members_by_tag};
            std::size_t cursor = wire_idx + 1;
            for (std::size_t entry = 0; entry < count; ++entry) {
                const auto entry_start = cursor;
                const auto entry_scan = scan_group_entry(schema, fields, child_ancestors, *group, cursor, depth + 1);
                if (!entry_scan.status.ok()) return entry_scan.status;
                cursor = entry_scan.end;
                const auto status = validate_members_fast(schema, group_level, group->required_members,
                    fields.subspan(entry_start, cursor - entry_start), validation_level,
                    child_ancestors, depth + 1, unexpected_policy, unexpected_count);
                if (!status.ok()) return status;
            }
        }
        return Status::success();
    };

    if (!seen_overflow) {
        for (std::size_t s = 0; s < seen_count; ++s) {
            if (const auto st = validate_seen(seen_list[s].owner, seen_list[s].member_index, seen_list[s].wire_index); !st.ok())
                return st;
        }
    } else {
        // Fallback for messages with >256 distinct present top-level fields.
        for (std::size_t cursor = 0; cursor < fields.size();) {
            const auto tag = fields[cursor].tag;
            if (top_level && is_session_or_frame_tag(tag)) {
                ++cursor;
                continue;
            }
            std::size_t member_idx = 0;
            if (const auto* member = find_member_in_level(schema, level, tag, &member_idx)) {
                if (const auto st = validate_seen(0, member_idx, cursor); !st.ok()) return st;
                if (member->kind == MemberKind::group) {
                    const auto scan = skip_group(schema, fields, ancestors, level, cursor, depth);
                    if (!scan.status.ok()) return scan.status;
                    cursor = scan.end;
                    continue;
                }
            } else if (header != nullptr) {
                if (const auto* hm = find_member_in_level(schema, header_level, tag, &member_idx)) {
                    if (const auto st = validate_seen(1, member_idx, cursor); !st.ok()) return st;
                    if (hm->kind == MemberKind::group) {
                        const auto scan = skip_group(schema, fields, ancestors, header_level, cursor, depth);
                        if (!scan.status.ok()) return scan.status;
                        cursor = scan.end;
                        continue;
                    }
                } else if (trailer != nullptr) {
                    if (const auto* tm = find_member_in_level(schema, trailer_level, tag, &member_idx)) {
                        if (const auto st = validate_seen(2, member_idx, cursor); !st.ok()) return st;
                        if (tm->kind == MemberKind::group) {
                            const auto scan = skip_group(schema, fields, ancestors, trailer_level, cursor, depth);
                            if (!scan.status.ok()) return scan.status;
                            cursor = scan.end;
                            continue;
                        }
                    }
                }
            } else if (trailer != nullptr) {
                if (const auto* tm = find_member_in_level(schema, trailer_level, tag, &member_idx)) {
                    if (const auto st = validate_seen(2, member_idx, cursor); !st.ok()) return st;
                    if (tm->kind == MemberKind::group) {
                        const auto scan = skip_group(schema, fields, ancestors, trailer_level, cursor, depth);
                        if (!scan.status.ok()) return scan.status;
                        cursor = scan.end;
                        continue;
                    }
                }
            }
            ++cursor;
        }
    }

    if (!first_order_error.ok()) return first_order_error;

    // Pass 3: Check required members (O(R) when required_members is populated),
    // including non-session required Header/Trailer fields and groups.
    if (validation_level != ValidationLevel::structure) {
        const auto is_member_seen = [&](std::size_t idx) noexcept -> bool {
            if (use_bitset) return (seen_bits[idx >> 6U] & (std::uint64_t{1} << (idx & 63U))) != 0;
            const auto tag = member_tag(schema, level.members[idx]);
            return find_seen_wire_index(tag) != nullptr;
        };
        if (!required_members.empty()) {
            for (const auto req_idx : required_members) {
                if (req_idx < level.members.size() && !is_member_seen(req_idx)) {
                    return {ErrorCode::required_field_missing,
                        static_cast<int>(member_tag(schema, level.members[req_idx]))};
                }
            }
        } else {
            for (std::size_t i = 0; i < level.members.size(); ++i) {
                if (level.members[i].required && !is_member_seen(i)) {
                    return {ErrorCode::required_field_missing,
                        static_cast<int>(member_tag(schema, level.members[i]))};
                }
            }
        }
        if (top_level && header != nullptr) {
            for (std::size_t i = 0; i < header->members.size(); ++i) {
                if (!header->members[i].required) continue;
                const auto tag = member_tag(schema, header->members[i]);
                if (is_session_or_frame_tag(tag)) continue;
                const bool present = use_env_bitset
                    ? ((env_seen_bits[i >> 6U] & (std::uint64_t{1} << (i & 63U))) != 0)
                    : (find_seen_wire_index(tag, 1) != nullptr);
                if (!present) {
                    std::size_t body_idx = 0;
                    if (find_member_in_level(schema, level, tag, &body_idx) == nullptr || !is_member_seen(body_idx)) {
                        return {ErrorCode::required_field_missing, static_cast<int>(tag)};
                    }
                }
            }
        }
        if (top_level && trailer != nullptr) {
            for (std::size_t i = 0; i < trailer->members.size(); ++i) {
                if (!trailer->members[i].required) continue;
                const auto tag = member_tag(schema, trailer->members[i]);
                if (is_session_or_frame_tag(tag)) continue;
                const std::size_t env_bit = header_member_count + i;
                const bool present = use_env_bitset
                    ? ((env_seen_bits[env_bit >> 6U] & (std::uint64_t{1} << (env_bit & 63U))) != 0)
                    : (find_seen_wire_index(tag, 2) != nullptr);
                if (!present) {
                    std::size_t body_idx = 0;
                    if (find_member_in_level(schema, level, tag, &body_idx) == nullptr || !is_member_seen(body_idx)) {
                        return {ErrorCode::required_field_missing, static_cast<int>(tag)};
                    }
                }
            }
        }
    }

    if (!first_unknown_error.ok()) return first_unknown_error;
    return Status::success();
}

template <class T>
[[nodiscard]] inline Result<T> parse_field_value(std::string_view text, std::uint32_t tag) noexcept {
    if constexpr (std::is_same_v<T, std::string_view>) {
        return text;
    } else if constexpr (std::is_same_v<T, char>) {
        if (text.size() != 1) return Status{ErrorCode::invalid_value, static_cast<int>(tag)};
        return text[0];
    } else if constexpr (std::is_same_v<T, bool>) {
        if (text == "Y") return true;
        if (text == "N") return false;
        return Status{ErrorCode::invalid_value, static_cast<int>(tag)};
    } else if constexpr (std::is_same_v<T, std::int64_t>) {
        std::int64_t value{};
        if (!parse_signed(text, value)) return Status{ErrorCode::invalid_value, static_cast<int>(tag)};
        return value;
    } else if constexpr (std::is_same_v<T, std::uint64_t>) {
        std::uint64_t value{};
        if (!parse_unsigned(text, value)) return Status{ErrorCode::invalid_value, static_cast<int>(tag)};
        return value;
    } else if constexpr (std::is_same_v<T, Decimal>) {
        const auto result = detail::parse_decimal_fast(text);
        if (!result.ok()) return Status{ErrorCode::invalid_decimal, static_cast<int>(tag)};
        return result.value();
    }
}

[[nodiscard]] inline const GroupDescriptor* find_group_descriptor(
    const SchemaDescriptor& schema, const MemberLevel& level,
    std::uint32_t count_tag) noexcept {
    const auto* member = find_member_in_level(schema, level, count_tag);
    return member != nullptr ? descriptor_group(schema, *member) : nullptr;
}

// Validate session/frame Header & Trailer tags (8, 9, 10, 34, 35, 43, 49, 52, 56, 122, 369, 1128)
// for standalone full-message validation (validate_schema_message). SessionDriver/DirectInitiator
// call validate_schema_message_for_type after validating session header fields themselves.
[[nodiscard]] inline Status validate_header_session_tags(const SchemaDescriptor& schema,
    const ComponentDescriptor& header, const ComponentDescriptor* trailer,
    std::span<const FieldView> fields, ValidationLevel level,
    UnexpectedFieldPolicy unexpected_policy = UnexpectedFieldPolicy::reject,
    std::uint64_t* unexpected_count = nullptr) noexcept {
    constexpr std::uint16_t kBit8    = 1u << 0;
    constexpr std::uint16_t kBit9    = 1u << 1;
    constexpr std::uint16_t kBit35   = 1u << 2;
    constexpr std::uint16_t kBit49   = 1u << 3;
    constexpr std::uint16_t kBit56   = 1u << 4;
    constexpr std::uint16_t kBit34   = 1u << 5;
    constexpr std::uint16_t kBit52   = 1u << 6;
    constexpr std::uint16_t kBit43   = 1u << 7;
    constexpr std::uint16_t kBit122  = 1u << 8;
    constexpr std::uint16_t kBit369  = 1u << 9;
    constexpr std::uint16_t kBit1128 = 1u << 10;
    constexpr std::uint16_t kBit10   = 1u << 11;

    constexpr std::uint64_t kLowSessionTagMask =
        (std::uint64_t{1} << tag::begin_string)  | (std::uint64_t{1} << tag::body_length)  |
        (std::uint64_t{1} << tag::check_sum)     | (std::uint64_t{1} << tag::msg_seq_num)  |
        (std::uint64_t{1} << tag::msg_type)      | (std::uint64_t{1} << tag::poss_dup_flag) |
        (std::uint64_t{1} << tag::sender_comp_id) | (std::uint64_t{1} << tag::sending_time) |
        (std::uint64_t{1} << tag::target_comp_id);

    std::uint16_t seen = 0;
    Status first_content_err{};
    Status first_unknown_err{};

    for (const auto& f : fields) {
        const auto tag = f.tag;
        if (tag < 64u) {
            if (((kLowSessionTagMask >> tag) & 1ULL) == 0) continue;
        } else if (tag != tag::orig_sending_time && tag != tag::last_msg_seq_num_processed &&
                   tag != tag::appl_ver_id) [[likely]] {
            continue;
        }
        std::uint16_t bit = 0;
        switch (tag) {
        case tag::begin_string:
            bit = kBit8;
            if (first_content_err.ok() && !detail::is_valid_text(f.value))
                first_content_err = {ErrorCode::invalid_value, tag::begin_string, f.offset};
            break;
        case tag::body_length: {
            bit = kBit9;
            std::uint64_t parsed{};
            if (first_content_err.ok() && !parse_unsigned(f.value, parsed))
                first_content_err = {ErrorCode::invalid_value, tag::body_length, f.offset};
            break;
        }
        case tag::msg_type:
            bit = kBit35;
            if (first_content_err.ok() && !detail::is_valid_text(f.value))
                first_content_err = {ErrorCode::invalid_value, tag::msg_type, f.offset};
            break;
        case tag::sender_comp_id:
            bit = kBit49;
            if (first_content_err.ok() && !detail::is_valid_text(f.value))
                first_content_err = {ErrorCode::invalid_value, tag::sender_comp_id, f.offset};
            break;
        case tag::target_comp_id:
            bit = kBit56;
            if (first_content_err.ok() && !detail::is_valid_text(f.value))
                first_content_err = {ErrorCode::invalid_value, tag::target_comp_id, f.offset};
            break;
        case tag::msg_seq_num: {
            bit = kBit34;
            std::uint64_t parsed{};
            if (first_content_err.ok() && (!parse_unsigned(f.value, parsed) || parsed == 0))
                first_content_err = {ErrorCode::invalid_value, tag::msg_seq_num, f.offset};
            break;
        }
        case tag::sending_time:
            bit = kBit52;
            if (first_content_err.ok() && !detail::is_valid_utc_timestamp(f.value))
                first_content_err = {ErrorCode::invalid_timestamp, tag::sending_time, f.offset};
            break;
        case tag::poss_dup_flag:
            bit = kBit43;
            if (first_content_err.ok() && f.value != "Y" && f.value != "N")
                first_content_err = {ErrorCode::invalid_value, tag::poss_dup_flag, f.offset};
            break;
        case tag::orig_sending_time:
            bit = kBit122;
            if (first_content_err.ok() && !detail::is_valid_utc_timestamp(f.value))
                first_content_err = {ErrorCode::invalid_timestamp, tag::orig_sending_time, f.offset};
            break;
        case tag::last_msg_seq_num_processed: {
            bit = kBit369;
            std::uint64_t parsed{};
            if (first_content_err.ok() && !parse_unsigned(f.value, parsed))
                first_content_err = {ErrorCode::invalid_value, tag::last_msg_seq_num_processed, f.offset};
            break;
        }
        case tag::appl_ver_id: {
            bit = kBit1128;
            const MemberLevel hlevel{header.members, header.members_by_tag};
            const auto* m = find_member_in_level(schema, hlevel, tag::appl_ver_id);
            if (m == nullptr) {
                if (unexpected_policy == UnexpectedFieldPolicy::reject) {
                    if (level == ValidationLevel::strict && first_unknown_err.ok())
                        first_unknown_err = {ErrorCode::invalid_value, tag::appl_ver_id, f.offset};
                } else if (unexpected_count != nullptr) {
                    ++(*unexpected_count);
                }
            } else if (first_content_err.ok()) {
                const auto* fd = descriptor_field(schema, *m);
                if (fd != nullptr) {
                    if (!valid_field_value(*fd, f.value))
                        first_content_err = {ErrorCode::invalid_value, tag::appl_ver_id, f.offset};
                    else if (level == ValidationLevel::strict && !enum_values_accept(*fd, f.value))
                        first_content_err = {ErrorCode::invalid_value, tag::appl_ver_id, f.offset};
                }
            }
            break;
        }
        case tag::check_sum:
            bit = kBit10;
            if (first_content_err.ok() && !detail::is_valid_text(f.value))
                first_content_err = {ErrorCode::invalid_value, tag::check_sum, f.offset};
            break;
        default:
            continue;
        }
        if ((seen & bit) != 0) {
            return {ErrorCode::duplicate_field, static_cast<int>(tag), f.offset};
        }
        seen |= bit;
    }

    if (!first_content_err.ok()) return first_content_err;

    if (level != ValidationLevel::structure) {
        const bool need_hdr_scan = header.required_session_mask == 0xFFFFu
            ? true
            : ((seen & header.required_session_mask) != header.required_session_mask);
        if (need_hdr_scan) {
            for (const auto& m : header.members) {
                if (!m.required || m.kind != MemberKind::field) continue;
                const auto tag = member_tag(schema, m);
                std::uint16_t bit = 0;
                switch (tag) {
                case tag::begin_string:               bit = kBit8; break;
                case tag::body_length:                bit = kBit9; break;
                case tag::msg_type:                   bit = kBit35; break;
                case tag::sender_comp_id:             bit = kBit49; break;
                case tag::target_comp_id:             bit = kBit56; break;
                case tag::msg_seq_num:                bit = kBit34; break;
                case tag::sending_time:               bit = kBit52; break;
                case tag::poss_dup_flag:              bit = kBit43; break;
                case tag::orig_sending_time:          bit = kBit122; break;
                case tag::last_msg_seq_num_processed: bit = kBit369; break;
                case tag::appl_ver_id:                bit = kBit1128; break;
                default: continue;
                }
                if ((seen & bit) == 0) {
                    return {ErrorCode::required_field_missing, static_cast<int>(tag)};
                }
            }
        }
        if (trailer != nullptr) {
            const bool need_trl_scan = trailer->required_session_mask == 0xFFFFu
                ? ((seen & kBit10) == 0)
                : ((seen & trailer->required_session_mask) != trailer->required_session_mask);
            if (need_trl_scan) {
                for (const auto& m : trailer->members) {
                    if (m.required && m.kind == MemberKind::field &&
                        member_tag(schema, m) == tag::check_sum) {
                        return {ErrorCode::required_field_missing, tag::check_sum};
                    }
                }
            }
        }
    }

    if (!first_unknown_err.ok()) return first_unknown_err;
    return Status::success();
}

} // namespace detail

[[nodiscard]] inline bool is_valid_schema_field_value(const FieldDescriptor& field,
    std::string_view value) noexcept {
    if (!detail::valid_field_value(field, value)) return false;
    return detail::enum_values_accept(field, value);
}

namespace detail {
[[gnu::noinline, gnu::cold]] [[nodiscard]] inline Status validate_schema_message_for_type_slow(
    const SchemaDescriptor& schema,
    std::string_view msg_type, std::size_t msg_type_offset,
    std::span<const FieldView> fields, ValidationLevel level,
    detail::ParsedFieldIndex* out_index,
    UnexpectedFieldPolicy unexpected_policy,
    std::uint64_t* unexpected_count) noexcept {
    if (out_index != nullptr) out_index->invalidate();
    const auto* descriptor = schema.find_message(msg_type);
    if (descriptor == nullptr) return {ErrorCode::unsupported, tag::msg_type, msg_type_offset};
    detail::AncestorStack ancestors;
    ancestors.trailer = detail::find_trailer_component(schema);
    const detail::MemberLevel root_level{descriptor->members, descriptor->members_by_tag};
    return detail::validate_members_fast(schema, root_level, descriptor->required_members,
        fields, level, ancestors, 0, unexpected_policy, unexpected_count);
}
} // namespace detail

[[nodiscard]] inline Status validate_schema_message_for_type(const SchemaDescriptor& schema,
    std::string_view msg_type, std::size_t msg_type_offset,
    std::span<const FieldView> fields, ValidationLevel level = ValidationLevel::strict,
    detail::ParsedFieldIndex* out_index = nullptr,
    UnexpectedFieldPolicy unexpected_policy = UnexpectedFieldPolicy::reject,
    std::uint64_t* unexpected_count = nullptr) noexcept {
    if (schema.fast_validate != nullptr) [[likely]] {
        const auto fast = schema.fast_validate(msg_type, msg_type_offset, fields, level, out_index);
        if (fast.ok() || unexpected_policy == UnexpectedFieldPolicy::reject ||
            fast.code != ErrorCode::invalid_value) [[likely]] {
            return fast;
        }
    }
    return detail::validate_schema_message_for_type_slow(
        schema, msg_type, msg_type_offset, fields, level, out_index, unexpected_policy, unexpected_count);
}

[[nodiscard]] inline Status validate_schema_message(const SchemaDescriptor& schema,
    const FlatMessageView& message, ValidationLevel level = ValidationLevel::strict,
    UnexpectedFieldPolicy unexpected_policy = UnexpectedFieldPolicy::reject,
    std::uint64_t* unexpected_count = nullptr) noexcept {
    const auto fields = message.fields();
    Status st{};
    if (fields.size() >= 3 && fields[2].tag == tag::msg_type) {
        st = validate_schema_message_for_type(schema, fields[2].value, fields[2].offset,
            fields, level, message.parsed_index(), unexpected_policy, unexpected_count);
    } else {
        const auto msg_type = message.find_unique(tag::msg_type);
        if (!msg_type.ok()) return msg_type.status();
        st = validate_schema_message_for_type(schema, msg_type.value().value, msg_type.value().offset,
            fields, level, message.parsed_index(), unexpected_policy, unexpected_count);
    }
    if (!st.ok()) return st;
    if (const auto* header = detail::find_header_component(schema); header != nullptr) {
        const auto hst = detail::validate_header_session_tags(
            schema, *header, detail::find_trailer_component(schema), fields, level,
            unexpected_policy, unexpected_count);
        if (!hst.ok()) {
            if (message.parsed_index() != nullptr) message.parsed_index()->invalidate();
            return hst;
        }
    }
    return Status::success();
}

template <class Schema>
class GroupEntryView;

template <class Schema>
class GroupView {
public:
    class Iterator {
    public:
        Iterator(const GroupView* view, std::size_t index, std::size_t cursor) noexcept
            : view_(view), index_(index), cursor_(cursor) {
            if (view_ != nullptr && index_ < view_->entry_count_) advance_scan();
        }
        [[nodiscard]] GroupEntryView<Schema> operator*() const noexcept;
        Iterator& operator++() noexcept {
            ++index_;
            cursor_ = next_cursor_;
            if (view_ != nullptr && index_ < view_->entry_count_) advance_scan();
            return *this;
        }
        [[nodiscard]] bool operator==(const Iterator& other) const noexcept {
            return index_ == other.index_;
        }
        [[nodiscard]] bool operator!=(const Iterator& other) const noexcept {
            return index_ != other.index_;
        }
    private:
        void advance_scan() noexcept {
            if (view_->entry_count_ == 1 && view_->group_end_ > view_->count_index_ + 1) {
                status_ = Status::success();
                next_cursor_ = view_->group_end_;
                return;
            }
            const auto scan = detail::scan_group_entry(*view_->schema_, view_->fields_,
                view_->ancestors_, *view_->descriptor_, cursor_, 0);
            status_ = scan.status;
            next_cursor_ = scan.end;
        }
        const GroupView* view_{};
        std::size_t index_{};
        std::size_t cursor_{};
        std::size_t next_cursor_{};
        Status status_{};
    };

    [[nodiscard]] std::size_t size() const noexcept { return entry_count_; }
    [[nodiscard]] bool empty() const noexcept { return entry_count_ == 0; }
    [[nodiscard]] std::uint32_t count_tag() const noexcept { return descriptor_ == nullptr ? 0 : descriptor_->count_tag; }
    [[nodiscard]] std::uint32_t delimiter_tag() const noexcept { return descriptor_ == nullptr ? 0 : descriptor_->delimiter_tag; }
    [[nodiscard]] GroupEntryView<Schema> at(std::size_t index) const noexcept;
    [[nodiscard]] Iterator begin() const noexcept { return Iterator{this, 0, count_index_ + 1}; }
    [[nodiscard]] Iterator end() const noexcept { return Iterator{this, entry_count_, fields_.size()}; }

    [[nodiscard]] static Result<GroupView<Schema>> find(const SchemaDescriptor& schema,
        std::span<const MessageMemberDescriptor> parent_members,
        std::span<const FieldView> fields, std::uint32_t count_tag,
        std::span<const MemberTagEntry> parent_by_tag = {},
        const detail::AncestorStack* existing_ancestors = nullptr,
        std::span<const std::uint32_t> parent_group_tags = {},
        std::uint64_t parent_group_bloom = 0,
        bool has_parent_group_index = false) noexcept {
        const detail::MemberLevel parent_level{parent_members, parent_by_tag};
        const auto* descriptor = detail::find_group_descriptor(schema, parent_level, count_tag);
        if (descriptor == nullptr) return Status{ErrorCode::unsupported, static_cast<int>(count_tag)};
        detail::AncestorStack ancestors;
        if (existing_ancestors != nullptr) {
            ancestors = *existing_ancestors;
        } else {
            ancestors.trailer = detail::find_trailer_component(schema);
        }
        const auto lookup = detail::find_level_field(schema, parent_members, fields, count_tag,
            parent_by_tag, &ancestors, nullptr,
            parent_group_tags, parent_group_bloom, has_parent_group_index);
        if (!lookup.status.ok()) return lookup.status;
        std::uint64_t count{};
        if (!detail::parse_unsigned(fields[lookup.index].value, count) || count > fields.size() - lookup.index - 1)
            return Status{ErrorCode::invalid_value, static_cast<int>(count_tag), fields[lookup.index].offset};
        const auto scan = detail::skip_group(schema, fields, ancestors, parent_level, lookup.index, 0);
        if (!scan.status.ok()) return scan.status;
        if (ancestors.count < detail::max_schema_group_depth + 2)
            ancestors.levels[ancestors.count++] = parent_level;
        return GroupView{schema, *descriptor, ancestors, fields, lookup.index,
            static_cast<std::size_t>(count), scan.end};
    }

private:
    template <class> friend class TypedMessageView;
    friend class GroupEntryView<Schema>;
    friend class Iterator;

    [[nodiscard]] static Result<GroupView<Schema>> from_indexed(const SchemaDescriptor& schema,
        const MessageDescriptor& parent_msg, const GroupDescriptor& descriptor,
        std::span<const FieldView> fields, std::size_t count_index, std::size_t group_end) noexcept {
        std::uint64_t count{};
        if (count_index >= fields.size() || !detail::parse_unsigned(fields[count_index].value, count) ||
            count > fields.size() - count_index - 1) {
            return Status{ErrorCode::invalid_value, static_cast<int>(descriptor.count_tag),
                count_index < fields.size() ? fields[count_index].offset : 0};
        }
        detail::AncestorStack ancestors;
        ancestors.trailer = detail::find_trailer_component(schema);
        ancestors.levels[0] = detail::MemberLevel{parent_msg.members, parent_msg.members_by_tag};
        ancestors.count = 1;
        return GroupView{schema, descriptor, ancestors, fields, count_index,
            static_cast<std::size_t>(count), group_end};
    }

    GroupView(const SchemaDescriptor& schema, const GroupDescriptor& descriptor,
        detail::AncestorStack ancestors, std::span<const FieldView> fields,
        std::size_t count_index, std::size_t entry_count, std::size_t group_end = 0) noexcept
        : schema_(&schema), descriptor_(&descriptor), ancestors_(ancestors), fields_(fields),
          count_index_(count_index), entry_count_(entry_count), group_end_(group_end) {}

    const SchemaDescriptor* schema_{};
    const GroupDescriptor* descriptor_{};
    detail::AncestorStack ancestors_{};
    std::span<const FieldView> fields_{};
    std::size_t count_index_{};
    std::size_t entry_count_{};
    std::size_t group_end_{};
};

template <class Schema>
class GroupEntryView {
public:
    [[nodiscard]] Status status() const noexcept { return status_; }
    [[nodiscard]] const GroupDescriptor& descriptor() const noexcept { return *descriptor_; }

    template <std::uint32_t Tag>
    [[nodiscard]] Result<FieldValueType<Schema, Tag>> get() const noexcept {
        if (!status_.ok()) return status_;
        using Value = FieldValueType<Schema, Tag>;
        if (descriptor_->has_group_index && descriptor_->group_count_tags.empty()) {
            const FieldView* found = nullptr;
            for (const auto& f : fields_) {
                if (f.tag == Tag) {
                    if (found != nullptr) {
                        return Status{ErrorCode::duplicate_field, static_cast<int>(Tag), f.offset};
                    }
                    found = &f;
                }
            }
            if (found == nullptr) return Status{ErrorCode::missing_field, static_cast<int>(Tag)};
            return detail::parse_field_value<Value>(found->value, Tag);
        }
        const auto lookup = detail::find_level_field(*schema_, descriptor_->members, fields_, Tag,
            descriptor_->members_by_tag, &ancestors_, nullptr,
            descriptor_->group_count_tags, descriptor_->group_count_tag_bloom, descriptor_->has_group_index);
        if (!lookup.status.ok()) return lookup.status;
        return detail::parse_field_value<Value>(fields_[lookup.index].value, Tag);
    }

    template <std::uint32_t CountTag>
    [[nodiscard]] Result<GroupView<Schema>> group() const noexcept {
        if (!status_.ok()) return status_;
        return GroupView<Schema>::find(*schema_, descriptor_->members, fields_, CountTag,
            descriptor_->members_by_tag, &ancestors_,
            descriptor_->group_count_tags, descriptor_->group_count_tag_bloom, descriptor_->has_group_index);
    }

private:
    template <class> friend class TypedMessageView;
    template <class> friend class GroupView;

    GroupEntryView(const SchemaDescriptor& schema, const GroupDescriptor& descriptor,
        detail::AncestorStack ancestors, std::span<const FieldView> fields,
        Status status = Status::success()) noexcept
        : schema_(&schema), descriptor_(&descriptor), ancestors_(ancestors), fields_(fields), status_(status) {}

    const SchemaDescriptor* schema_{};
    const GroupDescriptor* descriptor_{};
    detail::AncestorStack ancestors_{};
    std::span<const FieldView> fields_{};
    Status status_{};
};

template <class Schema>
inline GroupEntryView<Schema> GroupView<Schema>::Iterator::operator*() const noexcept {
    if (!status_.ok())
        return GroupEntryView<Schema>{*view_->schema_, *view_->descriptor_, view_->ancestors_, {}, status_};
    return GroupEntryView<Schema>{*view_->schema_, *view_->descriptor_, view_->ancestors_,
        view_->fields_.subspan(cursor_, next_cursor_ - cursor_)};
}

template <class Schema>
inline GroupEntryView<Schema> GroupView<Schema>::at(std::size_t index) const noexcept {
    if (index >= entry_count_)
        return GroupEntryView<Schema>{*schema_, *descriptor_, ancestors_, {},
            Status{ErrorCode::missing_field, static_cast<int>(count_tag()), index}};
    if (entry_count_ == 1 && group_end_ > count_index_ + 1) {
        return GroupEntryView<Schema>{*schema_, *descriptor_, ancestors_,
            fields_.subspan(count_index_ + 1, group_end_ - (count_index_ + 1))};
    }
    std::size_t cursor = count_index_ + 1;
    for (std::size_t entry = 0; entry <= index; ++entry) {
        const auto start = cursor;
        const auto scan = detail::scan_group_entry(*schema_, fields_, ancestors_, *descriptor_, cursor, 0);
        if (!scan.status.ok()) return GroupEntryView<Schema>{*schema_, *descriptor_, ancestors_, {}, scan.status};
        cursor = scan.end;
        if (entry == index) return GroupEntryView<Schema>{*schema_, *descriptor_, ancestors_, fields_.subspan(start, cursor - start)};
    }
    return GroupEntryView<Schema>{*schema_, *descriptor_, ancestors_, {}, Status{ErrorCode::invalid_value}};
}

template <class Schema>
class TypedMessageView {
public:
    TypedMessageView(const FlatMessageView& flat, const MessageDescriptor& descriptor) noexcept
        : flat_(flat), descriptor_(&descriptor) {}

    [[nodiscard]] const FlatMessageView& flat() const noexcept { return flat_; }
    [[nodiscard]] std::string_view msg_type() const noexcept { return descriptor_->msg_type; }
    [[nodiscard]] const MessageDescriptor& descriptor() const noexcept { return *descriptor_; }

    template <std::uint32_t CountTag>
    [[nodiscard]] Result<GroupView<Schema>> group() const noexcept {
        return GroupView<Schema>::find(Schema::descriptor(), descriptor_->members, flat_.fields(), CountTag,
            descriptor_->members_by_tag, nullptr,
            descriptor_->group_count_tags, descriptor_->group_count_tag_bloom, descriptor_->has_group_index);
    }

    template <std::uint32_t Tag>
    [[nodiscard]] Result<FieldValueType<Schema, Tag>> get() const noexcept {
        Status lookup_status{};
        const auto* field = detail::find_top_level_field(Schema::descriptor(), *descriptor_,
            flat_, Tag, lookup_status);
        if (field == nullptr) return lookup_status;
        using Value = FieldValueType<Schema, Tag>;
        return detail::parse_field_value<Value>(field->value, Tag);
    }

protected:
    template <std::uint32_t Tag, std::uint32_t MsgIdx, std::uint32_t MemberIdx>
    [[nodiscard]] Result<FieldValueType<Schema, Tag>> get_indexed() const noexcept {
        if constexpr (MemberIdx < detail::ParsedFieldIndex::max_members) {
            if (const auto* idx = flat_.parsed_index();
                idx != nullptr && idx->matches(&Schema::descriptor(), MsgIdx)) {
                const int wire_idx = idx->lookup(MemberIdx);
                if (wire_idx < 0) return Status{ErrorCode::missing_field, static_cast<int>(Tag)};
                const auto fields = flat_.fields();
                if (static_cast<std::size_t>(wire_idx) < fields.size()) {
                    using Value = FieldValueType<Schema, Tag>;
                    return detail::parse_field_value<Value>(fields[static_cast<std::size_t>(wire_idx)].value, Tag);
                }
            }
        }
        return get<Tag>();
    }

    template <std::uint32_t CountTag, std::uint32_t MsgIdx, std::uint32_t MemberIdx>
    [[nodiscard]] Result<GroupView<Schema>> group_indexed() const noexcept {
        if constexpr (MemberIdx < detail::ParsedFieldIndex::max_members) {
            if (const auto* idx = flat_.parsed_index();
                idx != nullptr && idx->matches(&Schema::descriptor(), MsgIdx)) {
                const int wire_idx = idx->lookup(MemberIdx);
                if (wire_idx < 0) return Status{ErrorCode::missing_field, static_cast<int>(CountTag)};
                const auto& desc = Schema::descriptor();
                const auto g_idx = descriptor_->members[MemberIdx].group_index;
                return GroupView<Schema>::from_indexed(desc, *descriptor_, desc.groups[g_idx],
                    flat_.fields(), static_cast<std::size_t>(wire_idx),
                    static_cast<std::size_t>(idx->group_end[MemberIdx]));
            }
        }
        return group<CountTag>();
    }

private:
    FlatMessageView flat_{};
    const MessageDescriptor* descriptor_{};
};

} // namespace nimblefix
