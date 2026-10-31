#pragma once

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
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

struct GroupDescriptor {
    std::string_view name{};
    std::uint32_t count_tag{};
    std::uint32_t delimiter_tag{};
    std::span<const MessageMemberDescriptor> members{};
};

struct ComponentDescriptor {
    std::string_view name{};
    std::span<const MessageMemberDescriptor> members{};
};

struct MessageDescriptor {
    std::string_view name{};
    std::string_view msg_type{};
    std::string_view category{};
    std::span<const MessageMemberDescriptor> members{};

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

struct SchemaDescriptor {
    std::string_view type{};
    std::string_view version{};
    std::string_view fingerprint{};
    std::span<const FieldDescriptor> fields{};
    std::span<const GroupDescriptor> groups{};
    std::span<const ComponentDescriptor> components{};
    std::span<const MessageDescriptor> messages{};

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
            return (base("4.2", "4.2.0") && appl_ver_id == "4") ||
                (base("4.3", "4.3.0") && appl_ver_id == "5") ||
                (base("4.4", "4.4.0") && appl_ver_id == "6") ||
                (base("5.0", "5.0.0") && appl_ver_id == "7") ||
                (version == "5.0.1" && appl_ver_id == "8") ||
                (version == "5.0.2" && appl_ver_id == "9");
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
        for (const auto& message : messages) if (message.msg_type == msg_type) return &message;
        return nullptr;
    }

    [[nodiscard]] constexpr const MessageDescriptor* find_message_by_name(std::string_view name) const noexcept {
        for (const auto& message : messages) if (message.name == name) return &message;
        return nullptr;
    }
};

enum class ValidationLevel : std::uint8_t {
    structure,
    required,
    strict,
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

[[nodiscard]] inline bool is_session_or_frame_tag(std::uint32_t tag) noexcept {
    return tag == 8 || tag == 9 || tag == 10 || tag == 34 || tag == 35 ||
        tag == 43 || tag == 49 || tag == 52 || tag == 56 || tag == 122 || tag == 369 || tag == 1128;
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
    unsigned month = static_cast<unsigned>((value[4] - '0') * 10 + value[5] - '0');
    unsigned day = static_cast<unsigned>((value[6] - '0') * 10 + value[7] - '0');
    unsigned year = static_cast<unsigned>((value[0] - '0') * 1000 + (value[1] - '0') * 100 +
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
        std::uint64_t parsed{};
        return parse_unsigned(value, parsed) && parsed > 0 && parsed <= maximum_fix_tag;
    }
    case FieldClass::decimal: return Decimal::parse(value).ok();
    case FieldClass::utc_timestamp: return detail::is_valid_utc_timestamp(value);
    case FieldClass::utc_date:
    case FieldClass::local_market_date: return valid_date(value);
    case FieldClass::utc_time: return valid_time(value);
    case FieldClass::month_year: return valid_month_year(value);
    case FieldClass::binary_data: return true;
    case FieldClass::text:
    case FieldClass::multiple_value_string:
    case FieldClass::multiple_char_value:
    case FieldClass::multiple_string_value: {
        if (value.empty() || value.data() == nullptr || value.find('\x01') != std::string_view::npos ||
            value.find('\0') != std::string_view::npos) return false;
        if (field.field_class == FieldClass::text) return true;
        for (std::size_t first = 0; first < value.size();) {
            const auto separator = value.find(' ', first);
            const auto last = separator == std::string_view::npos ? value.size() : separator;
            if (last == first || (field.field_class == FieldClass::multiple_char_value && last - first != 1))
                return false;
            first = separator == std::string_view::npos ? value.size() : separator + 1;
            if (first == value.size() && separator != std::string_view::npos) return false;
        }
        return true;
    }
    }
    return false;
}

[[nodiscard]] inline bool enum_contains(const FieldDescriptor& field, std::string_view value) noexcept {
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

[[nodiscard]] inline bool member_contains_tag(const SchemaDescriptor& schema,
    std::span<const MessageMemberDescriptor> members, std::uint32_t tag) noexcept {
    for (const auto& member : members) if (member_tag(schema, member) == tag) return true;
    return false;
}

[[nodiscard]] inline const ComponentDescriptor* find_component(
    const SchemaDescriptor& schema, std::string_view name) noexcept {
    for (const auto& component : schema.components) if (component.name == name) return &component;
    return nullptr;
}

struct GroupScan {
    Status status{};
    std::size_t end{};
};

[[nodiscard]] inline GroupScan scan_group_entry(const SchemaDescriptor& schema,
    std::span<const FieldView> wire_fields, std::span<const MessageMemberDescriptor> parent_members,
    const GroupDescriptor& descriptor, std::size_t entry_start, std::size_t depth) noexcept;

[[nodiscard]] inline GroupScan skip_group(const SchemaDescriptor& schema,
    std::span<const FieldView> wire_fields, std::span<const MessageMemberDescriptor> parent_members,
    std::size_t count_index, std::size_t depth) noexcept {
    if (depth > max_schema_group_depth || count_index >= wire_fields.size())
        return {{ErrorCode::invalid_value, 0, count_index}, count_index};
    const auto group_member = std::find_if(parent_members.begin(), parent_members.end(), [&](const auto& member) {
        const auto* group = descriptor_group(schema, member);
        return group != nullptr && group->count_tag == wire_fields[count_index].tag;
    });
    if (group_member == parent_members.end()) return {{ErrorCode::invalid_value,
        static_cast<int>(wire_fields[count_index].tag), wire_fields[count_index].offset}, count_index};
    const auto* descriptor = descriptor_group(schema, *group_member);
    std::uint64_t count{};
    if (descriptor == nullptr || !parse_unsigned(wire_fields[count_index].value, count) ||
        count > wire_fields.size() - count_index - 1) {
        return {{ErrorCode::invalid_value, static_cast<int>(wire_fields[count_index].tag),
            wire_fields[count_index].offset}, count_index};
    }
    std::size_t cursor = count_index + 1;
    for (std::size_t entry_index = 0; entry_index < count; ++entry_index) {
        const auto entry = scan_group_entry(schema, wire_fields, parent_members,
            *descriptor, cursor, depth + 1);
        if (!entry.status.ok()) return entry;
        cursor = entry.end;
    }
    return {Status::success(), cursor};
}

[[nodiscard]] inline GroupScan scan_group_entry(const SchemaDescriptor& schema,
    std::span<const FieldView> wire_fields, std::span<const MessageMemberDescriptor> parent_members,
    const GroupDescriptor& descriptor, std::size_t entry_start, std::size_t depth) noexcept {
    if (depth > max_schema_group_depth || entry_start >= wire_fields.size() ||
        wire_fields[entry_start].tag != descriptor.delimiter_tag) {
        const auto offset = entry_start < wire_fields.size() ? wire_fields[entry_start].offset : 0;
        return {{ErrorCode::invalid_value, static_cast<int>(descriptor.delimiter_tag), offset}, entry_start};
    }
    std::size_t cursor = entry_start + 1;
    if (!descriptor.members.empty() && descriptor.members.front().kind == MemberKind::group) {
        const auto delimiter_group = skip_group(schema, wire_fields, descriptor.members, entry_start, depth + 1);
        if (!delimiter_group.status.ok()) return delimiter_group;
        cursor = delimiter_group.end;
    }
    while (cursor < wire_fields.size()) {
        if (wire_fields[cursor].tag == descriptor.delimiter_tag) break;
        bool consumed_member = false;
        for (const auto& member : descriptor.members) {
            if (member_tag(schema, member) != wire_fields[cursor].tag) continue;
            if (member.kind == MemberKind::group) {
                const auto nested = skip_group(schema, wire_fields, descriptor.members, cursor, depth + 1);
                if (!nested.status.ok()) return nested;
                cursor = nested.end;
            } else {
                ++cursor;
            }
            consumed_member = true;
            break;
        }
        if (consumed_member) continue;
        if (member_contains_tag(schema, parent_members, wire_fields[cursor].tag)) break;
        // Unknown extension fields stay within this entry until a delimiter or
        // a known field owned by the parent message starts the next level.
        ++cursor;
    }
    return {Status::success(), cursor};
}

struct LevelLookup {
    Status status{};
    std::size_t index{};
    bool found{};
};

[[nodiscard]] inline LevelLookup find_level_field(const SchemaDescriptor& schema,
    std::span<const MessageMemberDescriptor> members, std::span<const FieldView> fields,
    std::uint32_t tag) noexcept {
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
        const auto group_member = std::find_if(members.begin(), members.end(), [&](const auto& member) {
            const auto* group = descriptor_group(schema, member);
            return group != nullptr && group->count_tag == current_tag;
        });
        if (group_member != members.end()) {
            const auto scan = skip_group(schema, fields, members, cursor, 0);
            if (!scan.status.ok()) { result.status = scan.status; return result; }
            cursor = scan.end;
            continue;
        }
        ++cursor;
    }
    if (!result.found) result.status = {ErrorCode::missing_field, static_cast<int>(tag)};
    return result;
}

[[nodiscard]] inline Status validate_unknown_fields(const SchemaDescriptor& schema,
    std::span<const MessageMemberDescriptor> members, std::span<const FieldView> fields,
    ValidationLevel level, std::size_t depth, bool top_level) noexcept {
    if (level != ValidationLevel::strict) return Status::success();
    const auto* header = top_level ? find_component(schema, "Header") : nullptr;
    const auto* trailer = top_level ? find_component(schema, "Trailer") : nullptr;
    for (std::size_t cursor = 0; cursor < fields.size();) {
        const auto tag = fields[cursor].tag;
        const auto member = std::find_if(members.begin(), members.end(), [&](const auto& value) {
            return member_tag(schema, value) == tag;
        });
        if (member != members.end()) {
            if (member->kind == MemberKind::group) {
                const auto skipped = skip_group(schema, fields, members, cursor, depth);
                if (!skipped.status.ok()) return skipped.status;
                cursor = skipped.end;
            } else {
                ++cursor;
            }
            continue;
        }
        if (top_level && is_session_or_frame_tag(tag)) {
            ++cursor;
            continue;
        }
        bool allowed_header_field = false;
        for (const auto* component : {header, trailer}) {
            if (component == nullptr) continue;
            const auto component_member = std::find_if(component->members.begin(), component->members.end(),
                [&](const auto& value) { return member_tag(schema, value) == tag; });
            if (component_member == component->members.end()) continue;
            if (component_member->kind == MemberKind::group) {
                const auto skipped = skip_group(schema, fields, component->members, cursor, depth);
                if (!skipped.status.ok()) return skipped.status;
                cursor = skipped.end;
            } else {
                ++cursor;
            }
            allowed_header_field = true;
            break;
        }
        if (!allowed_header_field)
            return {ErrorCode::invalid_value, static_cast<int>(tag), fields[cursor].offset};
    }
    return Status::success();
}

[[nodiscard]] inline const FieldView* find_top_level_field(
    const SchemaDescriptor& schema, const MessageDescriptor& message,
    const FlatMessageView& flat, std::uint32_t tag, Status& status) noexcept {
    const auto fields = flat.fields();
    const auto result = find_level_field(schema, message.members, fields, tag);
    status = result.status;
    return result.found ? &fields[result.index] : nullptr;
}

[[nodiscard]] inline Status validate_members(const SchemaDescriptor& schema,
    std::span<const MessageMemberDescriptor> members, std::span<const FieldView> fields,
    ValidationLevel level, std::size_t depth = 0) noexcept {
    if (depth > max_schema_group_depth) return {ErrorCode::invalid_value};
    for (std::size_t i = 0; i < members.size(); ++i) {
        const auto& member = members[i];
        const auto tag = member_tag(schema, member);
        if (member.kind == MemberKind::field) {
            const auto* descriptor = descriptor_field(schema, member);
            if (descriptor == nullptr) return {ErrorCode::invalid_value, static_cast<int>(tag)};
            const auto lookup = find_level_field(schema, members, fields, tag);
            if (!lookup.status.ok() && lookup.status.code != ErrorCode::missing_field) return lookup.status;
            const FieldView* found = lookup.found ? &fields[lookup.index] : nullptr;
            if (found == nullptr) {
                if (level != ValidationLevel::structure && member.required)
                    return {ErrorCode::required_field_missing, static_cast<int>(tag)};
                continue;
            }
            if (!valid_field_value(*descriptor, found->value))
                return {descriptor->field_class == FieldClass::utc_timestamp ? ErrorCode::invalid_timestamp : ErrorCode::invalid_value,
                    static_cast<int>(tag), found->offset};
            if (level == ValidationLevel::strict && !enum_values_accept(*descriptor, found->value))
                return {ErrorCode::invalid_value, static_cast<int>(tag), found->offset};
            if (descriptor->length_tag != 0) {
                const auto length_lookup = find_level_field(schema, members, fields, descriptor->length_tag);
                if (!length_lookup.status.ok()) return length_lookup.status;
                std::uint64_t length{};
                if (!parse_unsigned(fields[length_lookup.index].value, length) || length != found->value.size())
                    return {ErrorCode::invalid_value, static_cast<int>(descriptor->length_tag),
                        fields[length_lookup.index].offset};
            } else if (descriptor->data_tag != 0 && descriptor->field_class == FieldClass::unsigned_integer) {
                const auto data_lookup = find_level_field(schema, members, fields, descriptor->data_tag);
                std::uint64_t length{};
                if (!parse_unsigned(found->value, length))
                    return {ErrorCode::invalid_value, static_cast<int>(tag), found->offset};
                if (data_lookup.status.ok()) {
                    if (length != fields[data_lookup.index].value.size())
                        return {ErrorCode::invalid_value, static_cast<int>(tag), found->offset};
                } else if (data_lookup.status.code != ErrorCode::missing_field || length != 0) {
                    return data_lookup.status.code == ErrorCode::missing_field
                        ? Status{ErrorCode::invalid_value, static_cast<int>(tag), found->offset}
                        : data_lookup.status;
                }
            }
        } else {
            const auto* group = descriptor_group(schema, member);
            if (group == nullptr) return {ErrorCode::invalid_value, static_cast<int>(tag)};
            const auto lookup = find_level_field(schema, members, fields, group->count_tag);
            if (!lookup.status.ok() && lookup.status.code != ErrorCode::missing_field) return lookup.status;
            if (!lookup.found) {
                if (level != ValidationLevel::structure && member.required)
                    return {ErrorCode::required_field_missing, static_cast<int>(tag)};
                continue;
            }
            const std::size_t count_index = lookup.index;
            std::uint64_t count{};
            if (!parse_unsigned(fields[count_index].value, count) || count > fields.size() - count_index - 1)
                return {ErrorCode::invalid_value, static_cast<int>(tag), fields[count_index].offset};
            const auto group_scan = skip_group(schema, fields, members, count_index, depth);
            if (!group_scan.status.ok()) return group_scan.status;
            if (level != ValidationLevel::structure) {
                std::size_t cursor = count_index + 1;
                for (std::size_t entry = 0; entry < count; ++entry) {
                    const auto entry_start = cursor;
                    const auto entry_scan = scan_group_entry(schema, fields, members, *group, cursor, depth + 1);
                    if (!entry_scan.status.ok()) return entry_scan.status;
                    cursor = entry_scan.end;
                    const std::size_t entry_end = cursor;
                    const auto status = validate_members(schema, group->members,
                        fields.subspan(entry_start, entry_end - entry_start), level, depth + 1);
                    if (!status.ok()) return status;
                }
            }
        }
    }
    return validate_unknown_fields(schema, members, fields, level, depth, depth == 0);
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
        const auto result = Decimal::parse(text);
        if (!result.ok()) return Status{ErrorCode::invalid_decimal, static_cast<int>(tag)};
        return result.value();
    }
}

[[nodiscard]] inline const GroupDescriptor* find_group_descriptor(
    const SchemaDescriptor& schema, std::span<const MessageMemberDescriptor> members,
    std::uint32_t count_tag) noexcept {
    for (const auto& member : members) {
        const auto* group = descriptor_group(schema, member);
        if (group != nullptr && group->count_tag == count_tag) return group;
    }
    return nullptr;
}

} // namespace detail

[[nodiscard]] inline bool is_valid_schema_field_value(const FieldDescriptor& field,
    std::string_view value) noexcept {
    if (!detail::valid_field_value(field, value)) return false;
    return detail::enum_values_accept(field, value);
}

[[nodiscard]] inline Status validate_schema_message(const SchemaDescriptor& schema,
    const FlatMessageView& message, ValidationLevel level = ValidationLevel::strict) noexcept {
    const auto msg_type = message.find_unique(35);
    if (!msg_type.ok()) return msg_type.status();
    const auto* descriptor = schema.find_message(msg_type.value().value);
    if (descriptor == nullptr) return {ErrorCode::unsupported, 35, msg_type.value().offset};
    return detail::validate_members(schema, descriptor->members, message.fields(), level);
}

template <class Schema>
class GroupEntryView;

template <class Schema>
class GroupView {
public:
    [[nodiscard]] std::size_t size() const noexcept { return entry_count_; }
    [[nodiscard]] std::uint32_t count_tag() const noexcept { return descriptor_ == nullptr ? 0 : descriptor_->count_tag; }
    [[nodiscard]] std::uint32_t delimiter_tag() const noexcept { return descriptor_ == nullptr ? 0 : descriptor_->delimiter_tag; }
    [[nodiscard]] GroupEntryView<Schema> at(std::size_t index) const noexcept;

    [[nodiscard]] static Result<GroupView<Schema>> find(const SchemaDescriptor& schema,
        std::span<const MessageMemberDescriptor> parent_members,
        std::span<const FieldView> fields, std::uint32_t count_tag) noexcept {
        const auto* descriptor = detail::find_group_descriptor(schema, parent_members, count_tag);
        if (descriptor == nullptr) return Status{ErrorCode::unsupported, static_cast<int>(count_tag)};
        const auto lookup = detail::find_level_field(schema, parent_members, fields, count_tag);
        if (!lookup.status.ok()) return lookup.status;
        std::uint64_t count{};
        if (!detail::parse_unsigned(fields[lookup.index].value, count) || count > fields.size() - lookup.index - 1)
            return Status{ErrorCode::invalid_value, static_cast<int>(count_tag), fields[lookup.index].offset};
        const auto scan = detail::skip_group(schema, fields, parent_members, lookup.index, 0);
        if (!scan.status.ok()) return scan.status;
        return GroupView{schema, *descriptor, parent_members, fields, lookup.index,
            static_cast<std::size_t>(count)};
    }

private:
    friend class GroupEntryView<Schema>;
    GroupView(const SchemaDescriptor& schema, const GroupDescriptor& descriptor,
        std::span<const MessageMemberDescriptor> parent_members, std::span<const FieldView> fields,
        std::size_t count_index, std::size_t entry_count) noexcept
        : schema_(&schema), descriptor_(&descriptor), parent_members_(parent_members), fields_(fields),
          count_index_(count_index), entry_count_(entry_count) {}

    const SchemaDescriptor* schema_{};
    const GroupDescriptor* descriptor_{};
    std::span<const MessageMemberDescriptor> parent_members_{};
    std::span<const FieldView> fields_{};
    std::size_t count_index_{};
    std::size_t entry_count_{};
};

template <class Schema>
class GroupEntryView {
public:
    [[nodiscard]] Status status() const noexcept { return status_; }
    [[nodiscard]] const GroupDescriptor& descriptor() const noexcept { return *descriptor_; }

    template <std::uint32_t Tag>
    [[nodiscard]] Result<FieldValueType<Schema, Tag>> get() const noexcept {
        if (!status_.ok()) return status_;
        const auto lookup = detail::find_level_field(*schema_, descriptor_->members, fields_, Tag);
        if (!lookup.status.ok()) return lookup.status;
        using Value = FieldValueType<Schema, Tag>;
        return detail::parse_field_value<Value>(fields_[lookup.index].value, Tag);
    }

    template <std::uint32_t CountTag>
    [[nodiscard]] Result<GroupView<Schema>> group() const noexcept {
        if (!status_.ok()) return status_;
        return GroupView<Schema>::find(*schema_, descriptor_->members, fields_, CountTag);
    }

private:
    template <class> friend class TypedMessageView;
    template <class> friend class GroupView;

    GroupEntryView(const SchemaDescriptor& schema, const GroupDescriptor& descriptor,
        std::span<const FieldView> fields, Status status = Status::success()) noexcept
        : schema_(&schema), descriptor_(&descriptor), fields_(fields), status_(status) {}

    const SchemaDescriptor* schema_{};
    const GroupDescriptor* descriptor_{};
    std::span<const FieldView> fields_{};
    Status status_{};
};

template <class Schema>
inline GroupEntryView<Schema> GroupView<Schema>::at(std::size_t index) const noexcept {
    if (index >= entry_count_)
        return GroupEntryView<Schema>{*schema_, *descriptor_, {},
            Status{ErrorCode::missing_field, static_cast<int>(count_tag()), index}};
    std::size_t cursor = count_index_ + 1;
    for (std::size_t entry = 0; entry <= index; ++entry) {
        const auto start = cursor;
        const auto scan = detail::scan_group_entry(*schema_, fields_, parent_members_, *descriptor_, cursor, 0);
        if (!scan.status.ok()) return GroupEntryView<Schema>{*schema_, *descriptor_, {}, scan.status};
        cursor = scan.end;
        if (entry == index) return GroupEntryView<Schema>{*schema_, *descriptor_, fields_.subspan(start, cursor - start)};
    }
    return GroupEntryView<Schema>{*schema_, *descriptor_, {}, Status{ErrorCode::invalid_value}};
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
        return GroupView<Schema>::find(Schema::descriptor(), descriptor_->members, flat_.fields(), CountTag);
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

private:
    FlatMessageView flat_{};
    const MessageDescriptor* descriptor_{};
};

} // namespace nimblefix
