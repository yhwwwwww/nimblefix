#include <nimblefix/xml_schema.hpp>

#include <libxml/parser.h>
#include <libxml/tree.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <deque>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace nimblefix {

struct XmlSchema::Storage {
    std::deque<std::string> strings{};
    std::vector<EnumValueDescriptor> enum_values{};
    std::vector<FieldDescriptor> fields{};
    std::vector<std::vector<MessageMemberDescriptor>> group_members{};
    std::vector<std::vector<MemberTagEntry>> group_members_by_tag{};
    std::vector<std::vector<std::uint32_t>> group_required_members{};
    std::vector<std::vector<std::uint32_t>> group_group_tags{};
    std::vector<std::vector<MessageMemberDescriptor>> component_members{};
    std::vector<std::vector<MemberTagEntry>> component_members_by_tag{};
    std::vector<std::vector<std::uint32_t>> component_group_tags{};
    std::vector<std::vector<MessageMemberDescriptor>> message_members{};
    std::vector<std::vector<MemberTagEntry>> message_members_by_tag{};
    std::vector<std::vector<std::uint32_t>> message_required_members{};
    std::vector<std::vector<std::uint32_t>> message_group_tags{};
    std::vector<GroupDescriptor> groups{};
    std::vector<ComponentDescriptor> components{};
    std::vector<MessageDescriptor> messages{};
    std::array<std::int16_t, 128> single_char_message_index{};
    std::vector<LengthDataPair> pairs{};
    SchemaDescriptor descriptor{};

    [[nodiscard]] std::string_view keep(std::string value) {
        strings.push_back(std::move(value));
        return strings.back();
    }
};

namespace {

constexpr std::string_view codegen_version = "codegen-3";
constexpr std::array<std::uint32_t, 11> managed_tags{{
    tag::begin_string, tag::body_length, tag::check_sum,
    tag::msg_seq_num, tag::msg_type, tag::poss_dup_flag,
    tag::sender_comp_id, tag::sending_time, tag::target_comp_id,
    tag::orig_sending_time, tag::last_msg_seq_num_processed,
}};
constexpr std::size_t max_expanded_members_per_scope = 100'000;

struct Location {
    std::string source{};
    std::size_t line{1};
    std::size_t column{1};
};

struct XmlFailure {
    Status status{ErrorCode::invalid_value};
    Location location{};
    std::string message{};
};

struct FieldDef {
    std::uint32_t tag{};
    std::string name{};
    std::string type{};
    std::vector<std::pair<std::string, std::string>> enums{};
    Location location{};
};

struct MemberDef {
    enum class Kind : std::uint8_t { field, group, component };
    Kind kind{Kind::field};
    std::string name{};
    bool required{};
    std::vector<MemberDef> members{};
    Location location{};
};

struct ComponentDef {
    std::string name{};
    std::vector<MemberDef> members{};
    Location location{};
};

struct MessageDef {
    std::string name{};
    std::string msg_type{};
    std::string category{};
    std::vector<MemberDef> members{};
    Location location{};
};

struct Document {
    std::string source{};
    Location location{};
    std::map<std::string, std::string> identity{};
    std::vector<FieldDef> fields{};
    std::vector<ComponentDef> components{};
    std::vector<MessageDef> messages{};
    bool has_sections{};
};

struct FieldTypeInfo {
    FieldClass field_class{};
    WireType wire_type{};
};

[[nodiscard]] bool equal_member(const MemberDef& left, const MemberDef& right) {
    if (left.kind != right.kind || left.name != right.name || left.required != right.required ||
        left.members.size() != right.members.size()) return false;
    for (std::size_t i = 0; i < left.members.size(); ++i)
        if (!equal_member(left.members[i], right.members[i])) return false;
    return true;
}

[[nodiscard]] bool equal_members(const std::vector<MemberDef>& left,
    const std::vector<MemberDef>& right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i)
        if (!equal_member(left[i], right[i])) return false;
    return true;
}

[[nodiscard]] bool equal_field(const FieldDef& left, const FieldDef& right) {
    return left.tag == right.tag && left.name == right.name && left.type == right.type &&
        left.enums == right.enums;
}

[[nodiscard]] bool valid_identifier(std::string_view value) noexcept {
    if (value.empty() || !((value[0] >= 'A' && value[0] <= 'Z') ||
        (value[0] >= 'a' && value[0] <= 'z'))) return false;
    for (const unsigned char ch : value) {
        if (!(ch == '_' || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9'))) return false;
    }
    if (value.find("__") != std::string_view::npos) return false;
    constexpr std::array<std::string_view, 97> keywords{{
        "alignas", "alignof", "and", "and_eq", "asm", "atomic_cancel", "atomic_commit",
        "atomic_noexcept", "auto", "bitand", "bitor", "bool", "break", "case", "catch", "char",
        "char8_t", "char16_t", "char32_t", "class", "compl", "concept", "const", "consteval",
        "constexpr", "constinit", "const_cast", "continue", "co_await", "co_return", "co_yield",
        "decltype", "default", "delete", "do", "double", "dynamic_cast", "else", "enum", "explicit",
        "export", "extern", "false", "float", "for", "friend", "goto", "if", "inline", "int",
        "long", "mutable", "namespace", "new", "noexcept", "not", "not_eq", "nullptr", "operator",
        "or", "or_eq", "private", "protected", "public", "reflexpr", "register", "reinterpret_cast",
        "requires", "return", "short", "signed", "sizeof", "static", "static_assert", "static_cast",
        "struct", "switch", "synchronized", "template", "this", "thread_local", "throw", "true",
        "try", "typedef", "typeid", "typename", "union", "unsigned", "using", "virtual", "void",
        "volatile", "wchar_t", "while", "xor", "xor_eq"
    }};
    return std::find(keywords.begin(), keywords.end(), value) == keywords.end();
}

[[nodiscard]] bool managed_tag(std::uint32_t tag) noexcept {
    return std::find(managed_tags.begin(), managed_tags.end(), tag) != managed_tags.end();
}

[[nodiscard]] FieldTypeInfo field_type_info(std::string_view type) noexcept {
    if (type == "STRING" || type == "COUNTRY" || type == "CURRENCY" || type == "EXCHANGE" ||
        type == "LANGUAGE" || type == "LOCALMKTTIME" || type == "TZTIMEONLY" ||
        type == "TZTIMESTAMP" || type == "XID" || type == "XIDREF")
        return {FieldClass::text, WireType::string};
    if (type == "MULTIPLEVALUESTRING") return {FieldClass::multiple_value_string, WireType::string};
    if (type == "MULTIPLECHARVALUE") return {FieldClass::multiple_char_value, WireType::string};
    if (type == "MULTIPLESTRINGVALUE") return {FieldClass::multiple_string_value, WireType::string};
    if (type == "CHAR") return {FieldClass::character, WireType::character};
    if (type == "BOOLEAN") return {FieldClass::boolean, WireType::character};
    if (type == "INT") return {FieldClass::signed_integer, WireType::integer};
    if (type == "LENGTH" || type == "NUMINGROUP" || type == "SEQNUM")
        return {FieldClass::unsigned_integer, WireType::integer};
    if (type == "TAGNUM") return {FieldClass::tag_number, WireType::integer};
    if (type == "DAYOFMONTH") return {FieldClass::day_of_month, WireType::integer};
    if (type == "AMT" || type == "FLOAT" || type == "PERCENTAGE" || type == "PRICE" ||
        type == "PRICEOFFSET" || type == "QTY") return {FieldClass::decimal, WireType::decimal};
    if (type == "UTCTIMESTAMP") return {FieldClass::utc_timestamp, WireType::utc_timestamp};
    if (type == "UTCDATE" || type == "UTCDATEONLY") return {FieldClass::utc_date, WireType::string};
    if (type == "LOCALMKTDATE") return {FieldClass::local_market_date, WireType::string};
    if (type == "UTCTIMEONLY") return {FieldClass::utc_time, WireType::string};
    if (type == "MONTHYEAR") return {FieldClass::month_year, WireType::string};
    if (type == "DATA" || type == "XMLDATA") return {FieldClass::binary_data, WireType::binary_data};
    return {};
}

[[nodiscard]] Location node_location(xmlNodePtr node, const std::string& source) {
    const auto line = xmlGetLineNo(node);
    return {source, line > 0 ? static_cast<std::size_t>(line) : 1, 1};
}

[[nodiscard]] std::string node_name(xmlNodePtr node) {
    return node == nullptr || node->name == nullptr
        ? std::string{} : reinterpret_cast<const char*>(node->name);
}

[[nodiscard]] bool has_element_children(xmlNodePtr node) noexcept {
    for (auto child = node == nullptr ? nullptr : node->children; child != nullptr; child = child->next)
        if (child->type == XML_ELEMENT_NODE) return true;
    return false;
}

[[nodiscard]] std::string property(xmlNodePtr node, const char* name) {
    struct XmlFreeDeleter { void operator()(xmlChar* ptr) const noexcept { xmlFree(ptr); } };
    std::unique_ptr<xmlChar, XmlFreeDeleter> raw(
        xmlGetProp(node, reinterpret_cast<const xmlChar*>(name)));
    if (!raw) return {};
    return std::string(reinterpret_cast<const char*>(raw.get()));
}

[[nodiscard]] std::map<std::string, std::string> read_attributes(xmlNodePtr node,
    std::initializer_list<std::string_view> allowed, std::initializer_list<std::string_view> required,
    const std::string& source) {
    const auto location = node_location(node, source);
    if (node->ns != nullptr) throw XmlFailure{{ErrorCode::invalid_value}, location, "XML namespaces are not supported"};
    std::set<std::string_view> allowed_set(allowed.begin(), allowed.end());
    std::map<std::string, std::string> result;
    for (auto attr = node->properties; attr != nullptr; attr = attr->next) {
        if (attr->ns != nullptr) throw XmlFailure{{ErrorCode::invalid_value}, location, "XML namespaces are not supported"};
        const std::string name = reinterpret_cast<const char*>(attr->name);
        if (!allowed_set.contains(name))
            throw XmlFailure{{ErrorCode::invalid_value}, location, "unexpected attribute '" + name + "'"};
        result.emplace(name, property(node, name.c_str()));
    }
    for (const auto name : required) {
        if (!result.contains(std::string(name)))
            throw XmlFailure{{ErrorCode::invalid_value}, location, "missing required attribute '" + std::string(name) + "'"};
    }
    return result;
}

[[nodiscard]] std::vector<xmlNodePtr> element_children(xmlNodePtr parent, const std::string& source) {
    std::vector<xmlNodePtr> result;
    for (auto child = parent == nullptr ? nullptr : parent->children; child != nullptr; child = child->next) {
        if (child->type == XML_ELEMENT_NODE) {
            if (child->ns != nullptr)
                throw XmlFailure{{ErrorCode::invalid_value}, node_location(child, source), "XML namespaces are not supported"};
            result.push_back(child);
        } else if (child->type == XML_TEXT_NODE || child->type == XML_CDATA_SECTION_NODE) {
            if (child->content != nullptr) {
                const auto* text = reinterpret_cast<const char*>(child->content);
                for (; *text != '\0'; ++text)
                    if (!std::isspace(static_cast<unsigned char>(*text)))
                        throw XmlFailure{{ErrorCode::invalid_value}, node_location(parent, source),
                            "unexpected text inside <" + node_name(parent) + ">"};
            }
        } else if (child->type == XML_ENTITY_REF_NODE || child->type == XML_ENTITY_NODE) {
            throw XmlFailure{{ErrorCode::invalid_value}, node_location(parent, source), "XML entities are not allowed"};
        }
    }
    return result;
}

void validate_tree_bounds(xmlNodePtr node, const std::string& source, const XmlSchemaLimits& limits,
    std::size_t depth, std::size_t& items) {
    for (auto current = node; current != nullptr; current = current->next) {
        if (current->type != XML_ELEMENT_NODE) continue;
        if (current->ns != nullptr)
            throw XmlFailure{{ErrorCode::invalid_value}, node_location(current, source), "XML namespaces are not supported"};
        ++items;
        if (items > limits.max_xml_items)
            throw XmlFailure{{ErrorCode::capacity_exceeded}, node_location(current, source), "XML item count limit exceeded"};
        if (depth > limits.max_xml_depth)
            throw XmlFailure{{ErrorCode::capacity_exceeded}, node_location(current, source), "XML nesting depth limit exceeded"};
        validate_tree_bounds(current->children, source, limits, depth + 1, items);
    }
}

[[nodiscard]] std::vector<MemberDef> parse_members(xmlNodePtr parent, const std::string& source,
    std::size_t depth, const XmlSchemaLimits& limits) {
    if (depth > limits.max_xml_depth)
        throw XmlFailure{{ErrorCode::capacity_exceeded}, node_location(parent, source), "XML nesting depth limit exceeded"};
    std::vector<MemberDef> result;
    std::map<std::pair<MemberDef::Kind, std::string>, bool> seen;
    for (auto child : element_children(parent, source)) {
        const auto kind_name = node_name(child);
        MemberDef::Kind kind;
        if (kind_name == "field") kind = MemberDef::Kind::field;
        else if (kind_name == "group") kind = MemberDef::Kind::group;
        else if (kind_name == "component") kind = MemberDef::Kind::component;
        else throw XmlFailure{{ErrorCode::invalid_value}, node_location(child, source),
            "unsupported <" + kind_name + "> structure under <" + node_name(parent) + ">"};
        const auto attrs = read_attributes(child, {"name", "required"}, {"name"}, source);
        MemberDef member;
        member.kind = kind;
        member.name = attrs.at("name");
        member.location = node_location(child, source);
        const auto required = attrs.contains("required") ? attrs.at("required") : "N";
        if (required != "Y" && required != "N")
            throw XmlFailure{{ErrorCode::invalid_value}, member.location, "required must be 'Y' or 'N'"};
        member.required = required == "Y";
        if (!valid_identifier(member.name))
            throw XmlFailure{{ErrorCode::invalid_value}, member.location, "invalid field/component/group name"};
        if (kind == MemberDef::Kind::group)
            member.members = parse_members(child, source, depth + 1, limits);
        else if (has_element_children(child))
            throw XmlFailure{{ErrorCode::invalid_value}, node_location(child, source),
                "unexpected nested content inside <" + kind_name + ">"};
        const auto [previous, inserted] = seen.emplace(std::make_pair(kind, member.name), member.required);
        if (!inserted) {
            // Some official legacy dictionaries repeat an identical field ref.
            // Normalize only exact field duplicates; keep conflicting or
            // structural duplicates explicit errors.
            if (kind == MemberDef::Kind::field && previous->second == member.required) continue;
            throw XmlFailure{{ErrorCode::invalid_value}, member.location, "duplicate member reference '" + member.name + "'"};
        }
        result.push_back(std::move(member));
    }
    return result;
}

[[nodiscard]] Document parse_document(const std::string& source, const std::string& input,
    const XmlSchemaLimits& limits) {
    static std::once_flag init_flag;
    std::call_once(init_flag, [] { xmlInitParser(); });
    xmlDocPtr raw_doc = xmlReadMemory(input.data(), static_cast<int>(input.size()), source.c_str(), nullptr,
        XML_PARSE_NONET | XML_PARSE_NOBLANKS | XML_PARSE_COMPACT | XML_PARSE_NOERROR | XML_PARSE_NOWARNING);
    if (raw_doc == nullptr) {
        throw XmlFailure{{ErrorCode::invalid_value}, {source, 1, 1}, "malformed XML document"};
    }
    std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> doc(raw_doc, &xmlFreeDoc);
    if (doc->intSubset != nullptr || doc->extSubset != nullptr)
        throw XmlFailure{{ErrorCode::invalid_value}, {source, 1, 1}, "DOCTYPE, entities, and external XML resources are not allowed"};
    xmlNodePtr root = xmlDocGetRootElement(doc.get());
    if (root == nullptr || node_name(root) != "fix")
        throw XmlFailure{{ErrorCode::invalid_value}, root == nullptr ? Location{source, 1, 1} : node_location(root, source),
            "expected <fix> root element"};
    std::size_t item_count = 0;
    validate_tree_bounds(root, source, limits, 1, item_count);

    Document result;
    result.source = source;
    result.location = node_location(root, source);
    result.identity = read_attributes(root, {"type", "major", "minor", "servicepack"}, {}, source);
    if (result.identity.contains("type") && result.identity.at("type") != "FIX" &&
        result.identity.at("type") != "FIXT")
        throw XmlFailure{{ErrorCode::invalid_value}, result.location, "only FIX and FIXT XML dictionaries are supported"};
    for (const auto key : {"major", "minor", "servicepack"}) {
        if (!result.identity.contains(key)) continue;
        const auto& value = result.identity.at(key);
        if (value.empty() || !std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= '0' && c <= '9'; }))
            throw XmlFailure{{ErrorCode::invalid_value}, result.location, "invalid FIX schema version attribute"};
    }

    xmlNodePtr field_section = nullptr;
    xmlNodePtr component_section = nullptr;
    xmlNodePtr message_section = nullptr;
    xmlNodePtr header_section = nullptr;
    xmlNodePtr trailer_section = nullptr;
    for (auto child : element_children(root, source)) {
        const auto name = node_name(child);
        xmlNodePtr* target = name == "fields" ? &field_section : name == "components" ? &component_section :
            name == "messages" ? &message_section : name == "header" ? &header_section :
            name == "trailer" ? &trailer_section : nullptr;
        if (target == nullptr)
            throw XmlFailure{{ErrorCode::invalid_value}, node_location(child, source), "unsupported FIX XML section <" + name + ">"};
        if (*target != nullptr)
            throw XmlFailure{{ErrorCode::invalid_value}, node_location(child, source), "duplicate FIX XML section <" + name + ">"};
        *target = child;
    }
    result.has_sections = field_section != nullptr || component_section != nullptr ||
        message_section != nullptr || header_section != nullptr || trailer_section != nullptr;
    if (!result.has_sections)
        throw XmlFailure{{ErrorCode::invalid_value}, result.location, "document must contain a FIX XML section"};

    if (field_section != nullptr) {
        (void)read_attributes(field_section, {}, {}, source);
        for (auto node : element_children(field_section, source)) {
            if (node_name(node) != "field")
                throw XmlFailure{{ErrorCode::invalid_value}, node_location(node, source), "expected <field> under <fields>"};
            const auto attrs = read_attributes(node, {"number", "name", "type"}, {"number", "name", "type"}, source);
            std::uint32_t tag{};
            const auto& number = attrs.at("number");
            const auto parsed = std::from_chars(number.data(), number.data() + number.size(), tag);
            if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size() || tag == 0 || tag > 999'999'999)
                throw XmlFailure{{ErrorCode::invalid_value}, node_location(node, source), "field number must be in 1..999999999"};
            const auto& name = attrs.at("name");
            const auto& type = attrs.at("type");
            if (!valid_identifier(name))
                throw XmlFailure{{ErrorCode::invalid_value}, node_location(node, source), "invalid field name"};
            if (field_type_info(type).wire_type == WireType::unspecified)
                throw XmlFailure{{ErrorCode::unsupported}, node_location(node, source), "unsupported FIX field type '" + type + "'"};
            FieldDef field{tag, name, type, {}, node_location(node, source)};
            std::set<std::string> seen_values;
            for (auto value_node : element_children(node, source)) {
                if (node_name(value_node) != "value")
                    throw XmlFailure{{ErrorCode::invalid_value}, node_location(value_node, source), "unexpected nested field definition"};
                const auto value_attrs = read_attributes(value_node, {"enum", "description"}, {"enum"}, source);
                const auto enum_value = value_attrs.at("enum");
                if (enum_value.empty() || enum_value.find('\x01') != std::string::npos ||
                    enum_value.find('\n') != std::string::npos || enum_value.find('\r') != std::string::npos ||
                    !seen_values.insert(enum_value).second)
                    throw XmlFailure{{ErrorCode::invalid_value}, node_location(value_node, source), "invalid or duplicate enum value"};
                if (has_element_children(value_node))
                    throw XmlFailure{{ErrorCode::invalid_value}, node_location(value_node, source), "enum value cannot contain child elements"};
                field.enums.emplace_back(enum_value,
                    value_attrs.contains("description") ? value_attrs.at("description") : std::string{});
            }
            std::sort(field.enums.begin(), field.enums.end());
            result.fields.push_back(std::move(field));
        }
    }

    if (component_section != nullptr) {
        (void)read_attributes(component_section, {}, {}, source);
        for (auto node : element_children(component_section, source)) {
            if (node_name(node) != "component")
                throw XmlFailure{{ErrorCode::invalid_value}, node_location(node, source), "expected <component> under <components>"};
            const auto attrs = read_attributes(node, {"name"}, {"name"}, source);
            if (!valid_identifier(attrs.at("name")))
                throw XmlFailure{{ErrorCode::invalid_value}, node_location(node, source), "invalid component name"};
            result.components.push_back({attrs.at("name"), parse_members(node, source, 1, limits), node_location(node, source)});
        }
    }
    if (header_section != nullptr) {
        (void)read_attributes(header_section, {}, {}, source);
        result.components.push_back({"Header", parse_members(header_section, source, 1, limits), node_location(header_section, source)});
    }
    if (trailer_section != nullptr) {
        (void)read_attributes(trailer_section, {}, {}, source);
        result.components.push_back({"Trailer", parse_members(trailer_section, source, 1, limits), node_location(trailer_section, source)});
    }
    if (message_section != nullptr) {
        (void)read_attributes(message_section, {}, {}, source);
        for (auto node : element_children(message_section, source)) {
            if (node_name(node) != "message")
                throw XmlFailure{{ErrorCode::invalid_value}, node_location(node, source), "expected <message> under <messages>"};
            const auto attrs = read_attributes(node, {"name", "msgtype", "msgcat"}, {"name", "msgtype"}, source);
            const auto& name = attrs.at("name");
            const auto& msg_type = attrs.at("msgtype");
            if (!valid_identifier(name) || msg_type.empty() || msg_type.size() > 16 ||
                !std::all_of(msg_type.begin(), msg_type.end(), [](unsigned char c) { return c >= 0x21 && c <= 0x7e; }))
                throw XmlFailure{{ErrorCode::invalid_value}, node_location(node, source), "invalid message name or MsgType"};
            const auto category = attrs.contains("msgcat") ? attrs.at("msgcat") : std::string{};
            if (!category.empty() && category != "admin" && category != "app")
                throw XmlFailure{{ErrorCode::invalid_value}, node_location(node, source), "msgcat must be 'admin' or 'app'"};
            result.messages.push_back({name, msg_type, category, parse_members(node, source, 1, limits), node_location(node, source)});
        }
    }
    return result;
}

[[nodiscard]] std::string location_text(const Location& location) {
    return location.source + ":" + std::to_string(location.line) + ":" + std::to_string(location.column);
}

[[maybe_unused, nodiscard]] bool is_fix_field_type(std::string_view type) noexcept {
    return field_type_info(type).wire_type != WireType::unspecified;
}

[[nodiscard]] std::string member_key(const MemberDef& member) {
    std::string key = member.kind == MemberDef::Kind::field ? "f" :
        member.kind == MemberDef::Kind::group ? "g" : "c";
    key += member.name;
    key += member.required ? 'Y' : 'N';
    if (member.kind == MemberDef::Kind::group) {
        key += '[';
        for (const auto& child : member.members) {
            key += member_key(child);
            key += ';';
        }
        key += ']';
    }
    return key;
}

[[nodiscard]] bool identical_member_definitions(const std::vector<MemberDef>& a,
    const std::vector<MemberDef>& b) {
    return equal_members(a, b);
}

struct NormalizedSchema {
    std::map<std::string, std::string> identity{};
    std::map<std::string, FieldDef> fields{};
    std::map<std::string, ComponentDef> components{};
    std::map<std::string, MessageDef> messages{};
    std::map<std::string, std::string> component_by_source{};
    std::map<std::string, std::vector<MemberDef>> expanded_components{};
    std::map<std::string, std::vector<MemberDef>> expanded_messages{};
    std::vector<FieldDef> sorted_fields{};
    std::vector<ComponentDef> sorted_components{};
    std::vector<MessageDef> sorted_messages{};
};

[[nodiscard]] std::vector<MemberDef> expand_members(const std::vector<MemberDef>& members,
    const std::map<std::string, ComponentDef>& components, bool inherited_required,
    std::vector<std::string>& stack, std::size_t& budget) {
    std::vector<MemberDef> output;
    for (const auto& member : members) {
        if (budget == 0)
            throw XmlFailure{{ErrorCode::capacity_exceeded}, member.location, "expanded component member limit exceeded"};
        --budget;
        const bool required = inherited_required && member.required;
        if (member.kind == MemberDef::Kind::component) {
            const auto found = components.find(member.name);
            if (found == components.end())
                throw XmlFailure{{ErrorCode::invalid_value}, member.location, "unknown component reference '" + member.name + "'"};
            if (std::find(stack.begin(), stack.end(), member.name) != stack.end())
                throw XmlFailure{{ErrorCode::invalid_value}, member.location, "recursive component reference '" + member.name + "'"};
            stack.push_back(member.name);
            auto expanded = expand_members(found->second.members, components, required, stack, budget);
            stack.pop_back();
            output.insert(output.end(), std::make_move_iterator(expanded.begin()), std::make_move_iterator(expanded.end()));
        } else if (member.kind == MemberDef::Kind::group) {
            MemberDef expanded = member;
            expanded.required = required;
            expanded.members = expand_members(member.members, components, true, stack, budget);
            output.push_back(std::move(expanded));
        } else {
            MemberDef expanded = member;
            expanded.required = required;
            output.push_back(std::move(expanded));
        }
    }
    return output;
}

void check_references(const std::vector<MemberDef>& members,
    const std::map<std::string, FieldDef>& fields,
    const std::map<std::string, ComponentDef>& components,
    std::vector<std::string>& component_stack, std::set<std::string>& verified_components,
    const std::string& context) {
    for (const auto& member : members) {
        if (member.kind == MemberDef::Kind::field) {
            if (!fields.contains(member.name))
                throw XmlFailure{{ErrorCode::invalid_value}, member.location, "unknown field reference '" + member.name + "' in " + context};
        } else if (member.kind == MemberDef::Kind::component) {
            const auto found = components.find(member.name);
            if (found == components.end())
                throw XmlFailure{{ErrorCode::invalid_value}, member.location, "unknown component reference '" + member.name + "' in " + context};
            if (std::find(component_stack.begin(), component_stack.end(), member.name) != component_stack.end())
                throw XmlFailure{{ErrorCode::invalid_value}, member.location, "recursive component reference '" + member.name + "'"};
            if (verified_components.contains(member.name)) continue;
            component_stack.push_back(member.name);
            check_references(found->second.members, fields, components, component_stack, verified_components, context);
            component_stack.pop_back();
            verified_components.insert(member.name);
        } else {
            const auto count = fields.find(member.name);
            if (count == fields.end())
                throw XmlFailure{{ErrorCode::invalid_value}, member.location, "unknown repeating group counter field '" + member.name + "'"};
            if (count->second.type != "NUMINGROUP" && count->second.type != "INT")
                throw XmlFailure{{ErrorCode::invalid_value}, member.location, "repeating group counter must have NUMINGROUP or legacy INT type"};
            check_references(member.members, fields, components, component_stack, verified_components, context);
        }
    }
}

void validate_scope(const std::vector<MemberDef>& members, const std::map<std::string, FieldDef>& fields,
    const std::string& context, bool allow_managed) {
    std::set<std::uint32_t> seen;
    for (const auto& member : members) {
        const auto found = fields.find(member.name);
        if (found == fields.end()) {
            if (member.kind == MemberDef::Kind::component) continue;
            throw XmlFailure{{ErrorCode::invalid_value}, member.location, "unknown field reference '" + member.name + "'"};
        }
        const auto tag = found->second.tag;
        if (!seen.insert(tag).second)
            throw XmlFailure{{ErrorCode::invalid_value}, member.location, "duplicate field tag in " + context};
        if (member.kind == MemberDef::Kind::field && !allow_managed && managed_tag(tag))
            throw XmlFailure{{ErrorCode::invalid_value}, member.location, "references engine-managed FIX tag " + std::to_string(tag)};
        if (member.kind == MemberDef::Kind::group) {
            if (member.members.empty() || (member.members.front().kind != MemberDef::Kind::field &&
                member.members.front().kind != MemberDef::Kind::group))
                throw XmlFailure{{ErrorCode::invalid_value}, member.location, "repeating group must begin with a field delimiter"};
            validate_scope(member.members, fields, "group " + member.name, allow_managed);
        }
    }
}

[[nodiscard]] NormalizedSchema normalize(std::vector<Document> documents, const XmlSchemaLimits& limits) {
    if (documents.empty()) throw XmlFailure{{ErrorCode::invalid_value}, {}, "at least one XML input is required"};
    NormalizedSchema result;
    result.identity = documents.front().identity;
    for (const auto& document : documents) {
        if (document.identity != result.identity)
            throw XmlFailure{{ErrorCode::schema_mismatch}, document.location, "conflicting FIX schema identity/version across XML inputs"};
        for (const auto& field : document.fields) {
            auto by_name = result.fields.find(field.name);
            auto by_tag = std::find_if(result.fields.begin(), result.fields.end(), [&](const auto& item) {
                return item.second.tag == field.tag;
            });
            const FieldDef* previous = by_name != result.fields.end() ? &by_name->second :
                by_tag != result.fields.end() ? &by_tag->second : nullptr;
            if (previous != nullptr) {
                if (!equal_field(*previous, field))
                    throw XmlFailure{{ErrorCode::schema_mismatch}, field.location,
                        "conflicting field definition; previous definition at " + location_text(previous->location)};
            } else {
                result.fields.emplace(field.name, field);
            }
        }
        for (const auto& component : document.components) {
            const auto found = result.components.find(component.name);
            if (found != result.components.end()) {
                if (!identical_member_definitions(found->second.members, component.members))
                    throw XmlFailure{{ErrorCode::schema_mismatch}, component.location,
                        "conflicting component '" + component.name + "'; previous definition at " + location_text(found->second.location)};
            } else {
                result.components.emplace(component.name, component);
            }
        }
        for (const auto& message : document.messages) {
            const auto found_name = result.messages.find(message.name);
            const auto found_type = std::find_if(result.messages.begin(), result.messages.end(), [&](const auto& item) {
                return item.second.msg_type == message.msg_type;
            });
            const MessageDef* previous = found_name != result.messages.end() ? &found_name->second :
                found_type != result.messages.end() ? &found_type->second : nullptr;
            if (previous != nullptr) {
                if (previous->name != message.name || previous->msg_type != message.msg_type ||
                    previous->category != message.category || !equal_members(previous->members, message.members))
                    throw XmlFailure{{ErrorCode::schema_mismatch}, message.location,
                        "conflicting message definition; previous definition at " + location_text(previous->location)};
            } else {
                result.messages.emplace(message.name, message);
            }
        }
    }
    if (result.fields.size() > limits.max_fields || result.messages.size() > limits.max_messages ||
        result.components.size() > limits.max_components)
        throw XmlFailure{{ErrorCode::capacity_exceeded}, {}, "schema definition count exceeds configured limit"};
    if (result.messages.empty()) throw XmlFailure{{ErrorCode::invalid_value}, {}, "merged XML inputs contain no <message> definitions"};

    std::set<std::string> verified_components;
    for (const auto& [name, component] : result.components) {
        std::vector<std::string> stack{name};
        check_references(component.members, result.fields, result.components, stack, verified_components, "component '" + name + "'");
        std::size_t budget = max_expanded_members_per_scope;
        const auto expanded = expand_members(component.members, result.components, true, stack, budget);
        validate_scope(expanded, result.fields, "component '" + name + "'", name == "Header" || name == "Trailer");
        result.expanded_components.emplace(name, expanded);
    }
    for (const auto& [name, message] : result.messages) {
        std::vector<std::string> stack;
        check_references(message.members, result.fields, result.components, stack, verified_components, "message '" + name + "'");
        std::size_t budget = max_expanded_members_per_scope;
        const auto expanded = expand_members(message.members, result.components, true, stack, budget);
        validate_scope(expanded, result.fields, "message '" + name + "'", false);
        result.expanded_messages.emplace(name, expanded);
    }

    result.sorted_fields.reserve(result.fields.size());
    for (const auto& [name, field] : result.fields) { (void)name; result.sorted_fields.push_back(field); }
    std::sort(result.sorted_fields.begin(), result.sorted_fields.end(), [](const auto& a, const auto& b) { return a.tag < b.tag; });
    for (const auto& [name, component] : result.components) { (void)name; result.sorted_components.push_back(component); }
    for (const auto& [name, message] : result.messages) { (void)name; result.sorted_messages.push_back(message); }
    std::sort(result.sorted_messages.begin(), result.sorted_messages.end(), [](const auto& a, const auto& b) {
        return std::tie(a.name, a.msg_type) < std::tie(b.name, b.msg_type);
    });
    return result;
}

struct GroupUse {
    MemberDef member{};
    std::string key{};
};

[[nodiscard]] std::string group_key(const MemberDef& member) {
    std::string result = member.name + (member.required ? "Y{" : "N{");
    for (const auto& child : member.members) { result += member_key(child); result += ';'; }
    result += '}';
    return result;
}

void collect_groups(const std::vector<MemberDef>& members, std::map<std::string, MemberDef>& groups) {
    for (const auto& member : members) {
        if (member.kind != MemberDef::Kind::group) continue;
        groups.emplace(group_key(member), member);
        collect_groups(member.members, groups);
    }
}

[[maybe_unused, nodiscard]] std::string_view class_type_name(FieldClass value) noexcept {
    switch (value) {
    case FieldClass::text: return "text";
    case FieldClass::character: return "character";
    case FieldClass::boolean: return "boolean";
    case FieldClass::signed_integer: return "signed_integer";
    case FieldClass::unsigned_integer: return "unsigned_integer";
    case FieldClass::decimal: return "decimal";
    case FieldClass::utc_timestamp: return "utc_timestamp";
    case FieldClass::utc_date: return "utc_date";
    case FieldClass::utc_time: return "utc_time";
    case FieldClass::local_market_date: return "local_market_date";
    case FieldClass::month_year: return "month_year";
    case FieldClass::day_of_month: return "day_of_month";
    case FieldClass::binary_data: return "binary_data";
    case FieldClass::tag_number: return "tag_number";
    case FieldClass::multiple_value_string: return "multiple_value_string";
    case FieldClass::multiple_char_value: return "multiple_char_value";
    case FieldClass::multiple_string_value: return "multiple_string_value";
    }
    return "text";
}

class FingerprintBuilder {
public:
    FingerprintBuilder() { bytes_.append("nimblefix-schema-v3\0codegen-3\0", 30); }
    void number(std::size_t value) { bytes_ += std::to_string(value); bytes_.push_back(';'); }
    void text(std::string_view value) {
        bytes_ += std::to_string(value.size());
        bytes_.push_back(':');
        bytes_.append(value.data(), value.size());
    }
    void members(const std::vector<MemberDef>& values,
        const std::map<std::string, FieldDef>& fields) {
        number(values.size());
        for (const auto& member : values) {
            bytes_.push_back(member.kind == MemberDef::Kind::group ? 'g' : 'f');
            number(fields.at(member.name).tag);
            bytes_.push_back(member.required ? '1' : '0');
            if (member.kind == MemberDef::Kind::group) members(member.members, fields);
        }
    }
    [[nodiscard]] std::string finish();
private:
    std::string bytes_{};
};

// Small self-contained SHA-256 implementation keeps the optional XML target
// independent from OpenSSL and matches the generator's canonical byte stream.
[[nodiscard]] std::string sha256(std::string_view input) {
    constexpr std::array<std::uint32_t, 64> constants{{
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    }};
    std::array<std::uint32_t, 8> hash{{0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}};
    std::vector<std::uint8_t> data(input.begin(), input.end());
    const std::uint64_t bit_count = static_cast<std::uint64_t>(data.size()) * 8;
    data.push_back(0x80);
    while ((data.size() % 64) != 56) data.push_back(0);
    for (int shift = 56; shift >= 0; shift -= 8) data.push_back(static_cast<std::uint8_t>(bit_count >> shift));
    const auto rotate = [](std::uint32_t value, unsigned bits) { return (value >> bits) | (value << (32 - bits)); };
    for (std::size_t block = 0; block < data.size(); block += 64) {
        std::array<std::uint32_t, 64> words{};
        for (std::size_t i = 0; i < 16; ++i) {
            const auto at = block + i * 4;
            words[i] = (std::uint32_t(data[at]) << 24) | (std::uint32_t(data[at + 1]) << 16) |
                (std::uint32_t(data[at + 2]) << 8) | std::uint32_t(data[at + 3]);
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const auto s0 = rotate(words[i - 15], 7) ^ rotate(words[i - 15], 18) ^ (words[i - 15] >> 3);
            const auto s1 = rotate(words[i - 2], 17) ^ rotate(words[i - 2], 19) ^ (words[i - 2] >> 10);
            words[i] = words[i - 16] + s0 + words[i - 7] + s1;
        }
        auto [a,b,c,d,e,f,g,h] = hash;
        for (std::size_t i = 0; i < 64; ++i) {
            const auto s1 = rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25);
            const auto choose = (e & f) ^ (~e & g);
            const auto t1 = h + s1 + choose + constants[i] + words[i];
            const auto s0 = rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22);
            const auto majority = (a & b) ^ (a & c) ^ (b & c);
            const auto t2 = s0 + majority;
            h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        hash[0]+=a; hash[1]+=b; hash[2]+=c; hash[3]+=d;
        hash[4]+=e; hash[5]+=f; hash[6]+=g; hash[7]+=h;
    }
    constexpr char hex[] = "0123456789abcdef";
    std::string output;
    output.reserve(64);
    for (const auto word : hash) for (int shift = 28; shift >= 0; shift -= 4)
        output.push_back(hex[(word >> shift) & 0xf]);
    return output;
}

std::string FingerprintBuilder::finish() { return sha256(bytes_); }

[[nodiscard]] std::string schema_fingerprint(const NormalizedSchema& schema) {
    FingerprintBuilder canonical;
    canonical.number(schema.identity.size());
    for (const auto& [key, value] : schema.identity) { canonical.text(key); canonical.text(value); }
    canonical.number(schema.sorted_fields.size());
    for (const auto& field : schema.sorted_fields) {
        canonical.number(field.tag);
        canonical.text(field.name);
        canonical.text(field.type);
        canonical.number(field.enums.size());
        for (const auto& [value, description] : field.enums) { canonical.text(value); canonical.text(description); }
    }
    canonical.number(schema.expanded_components.size());
    for (const auto& [name, members] : schema.expanded_components) {
        canonical.text(name);
        canonical.members(members, schema.fields);
    }
    canonical.number(schema.sorted_messages.size());
    for (const auto& message : schema.sorted_messages) {
        canonical.text(message.name);
        canonical.text(message.msg_type);
        canonical.text(message.category);
        canonical.members(schema.expanded_messages.at(message.name), schema.fields);
    }
    return canonical.finish();
}

[[nodiscard]] std::uint32_t pair_tag_for(const FieldDef& field,
    const std::map<std::string, FieldDef>& fields) {
    std::string compact_name = field.name;
    for (std::size_t position = 0;
         (position = compact_name.find("Security", position)) != std::string::npos;
         position += 3) {
        compact_name.replace(position, 8, "Sec");
    }
    if (field.type == "DATA" || field.type == "XMLDATA") {
        for (const auto& suffix : {std::string{"Length"}, std::string{"Len"}}) {
            for (const auto& base : {field.name, compact_name}) {
                const auto found = fields.find(base + suffix);
                if (found != fields.end() && found->second.type == "LENGTH") return found->second.tag;
            }
        }
        return 0;
    }
    if (field.type == "LENGTH" || field.type == "NUMINGROUP") {
        const auto suffix = field.name.ends_with("Length") ? std::size_t{6} :
            field.name.ends_with("Len") ? std::size_t{3} : std::size_t{0};
        if (suffix == 0) return 0;
        const auto name = field.name.substr(0, field.name.size() - suffix);
        std::string compact_base = name;
        for (std::size_t position = 0;
             (position = compact_base.find("Security", position)) != std::string::npos;
             position += 3) {
            compact_base.replace(position, 8, "Sec");
        }
        std::string expanded_base = name;
        for (std::size_t position = 0;
             (position = expanded_base.find("Sec", position)) != std::string::npos;
             position += 8) {
            if (expanded_base.compare(position, 8, "Security") == 0) continue;
            expanded_base.replace(position, 3, "Security");
        }
        for (const auto& candidate : {name, compact_base, expanded_base}) {
            const auto data = fields.find(candidate);
            if (data != fields.end() && (data->second.type == "DATA" || data->second.type == "XMLDATA"))
                return data->second.tag;
        }
    }
    return 0;
}

[[nodiscard]] std::unique_ptr<XmlSchema::Storage> build_storage(const NormalizedSchema& schema,
    const XmlSchemaLimits& limits) {
    auto storage = std::make_unique<XmlSchema::Storage>();
    storage->strings.emplace_back(schema_fingerprint(schema));
    const auto fingerprint_view = std::string_view{storage->strings.back()};

    std::map<std::string, std::uint32_t> field_indexes;
    std::map<std::uint32_t, std::uint32_t> field_index_by_tag;
    std::size_t enum_count = 0;
    for (const auto& field : schema.sorted_fields) enum_count += field.enums.size();
    storage->enum_values.reserve(enum_count);
    for (const auto& field : schema.sorted_fields) {
        const auto start = storage->enum_values.size();
        for (const auto& [value, description] : field.enums)
            storage->enum_values.push_back({field.tag, storage->keep(value), storage->keep(description)});
        const auto info = field_type_info(field.type);
        std::uint32_t length_tag = 0;
        const bool is_data = field.type == "DATA" || field.type == "XMLDATA";
        std::uint32_t data_tag = is_data ? field.tag : 0;
        if (is_data) length_tag = pair_tag_for(field, schema.fields);
        else if (field.type == "LENGTH" || field.type == "NUMINGROUP") data_tag = pair_tag_for(field, schema.fields);
        if (is_data && length_tag == 0)
            throw XmlFailure{{ErrorCode::invalid_value}, field.location, "binary field has no matching LENGTH field"};
        const auto field_index = static_cast<std::uint32_t>(storage->fields.size());
        field_indexes.emplace(field.name, field_index);
        field_index_by_tag.emplace(field.tag, field_index);
        const std::span<const EnumValueDescriptor> enum_span = field.enums.empty()
            ? std::span<const EnumValueDescriptor>{}
            : std::span<const EnumValueDescriptor>{storage->enum_values.data() + start, field.enums.size()};
        storage->fields.push_back({field.tag, storage->keep(field.name), storage->keep(field.type),
            info.field_class, info.wire_type, enum_span, length_tag, data_tag});
    }

    std::map<std::string, MemberDef> group_definitions;
    for (const auto& [name, members] : schema.expanded_components) { (void)name; collect_groups(members, group_definitions); }
    for (const auto& [name, members] : schema.expanded_messages) { (void)name; collect_groups(members, group_definitions); }
    if (group_definitions.size() > limits.max_groups)
        throw XmlFailure{{ErrorCode::capacity_exceeded}, {}, "group count exceeds configured limit"};
    std::vector<std::string> group_keys;
    group_keys.reserve(group_definitions.size());
    for (const auto& [key, member] : group_definitions) { (void)member; group_keys.push_back(key); }
    std::map<std::string, std::uint32_t> group_indexes;
    for (std::size_t i = 0; i < group_keys.size(); ++i) group_indexes.emplace(group_keys[i], static_cast<std::uint32_t>(i));

    const auto make_members = [&](const std::vector<MemberDef>& members, auto&& self) -> std::vector<MessageMemberDescriptor> {
        std::vector<MessageMemberDescriptor> output;
        output.reserve(members.size());
        for (const auto& member : members) {
            if (member.kind == MemberDef::Kind::field) {
                output.push_back({MemberKind::field, field_indexes.at(member.name), 0, member.required});
            } else if (member.kind == MemberDef::Kind::group) {
                output.push_back({MemberKind::group, 0, group_indexes.at(group_key(member)), member.required});
            } else {
                (void)self;
                throw XmlFailure{{ErrorCode::invalid_value}, member.location, "component was not expanded"};
            }
        }
        return output;
    };

    const auto make_tag_index = [&](const std::vector<MessageMemberDescriptor>& members) {
        std::vector<MemberTagEntry> by_tag;
        by_tag.reserve(members.size());
        for (std::size_t idx = 0; idx < members.size(); ++idx) {
            const auto& m = members[idx];
            const auto tag = m.kind == MemberKind::field
                ? storage->fields[m.field_index].tag
                : schema.fields.at(group_definitions.at(group_keys[m.group_index]).name).tag;
            by_tag.push_back({tag, static_cast<std::uint32_t>(idx)});
        }
        std::sort(by_tag.begin(), by_tag.end(),
            [](const MemberTagEntry& a, const MemberTagEntry& b) { return a.tag < b.tag; });
        return by_tag;
    };
    const auto make_required = [](const std::vector<MessageMemberDescriptor>& members) {
        std::vector<std::uint32_t> required;
        for (std::size_t idx = 0; idx < members.size(); ++idx) {
            if (members[idx].required) required.push_back(static_cast<std::uint32_t>(idx));
        }
        return required;
    };
    std::vector<std::uint32_t> group_count_tag_by_index;
    group_count_tag_by_index.reserve(group_keys.size());
    for (const auto& key : group_keys) {
        group_count_tag_by_index.push_back(schema.fields.at(group_definitions.at(key).name).tag);
    }
    const auto make_group_tags = [&](const std::vector<MessageMemberDescriptor>& members) {
        std::vector<std::uint32_t> tags;
        std::uint64_t bloom = 0;
        for (const auto& member : members) {
            if (member.kind != MemberKind::group) continue;
            const auto tag = group_count_tag_by_index[member.group_index];
            tags.push_back(tag);
            bloom |= detail::group_tag_bloom_bit(tag);
        }
        return std::pair<std::vector<std::uint32_t>, std::uint64_t>{std::move(tags), bloom};
    };

    storage->group_members.reserve(group_keys.size());
    storage->group_members_by_tag.reserve(group_keys.size());
    storage->group_required_members.reserve(group_keys.size());
    storage->group_group_tags.reserve(group_keys.size());
    std::vector<std::uint64_t> group_blooms;
    group_blooms.reserve(group_keys.size());
    for (const auto& key : group_keys) {
        auto members = make_members(group_definitions.at(key).members, make_members);
        storage->group_members_by_tag.push_back(make_tag_index(members));
        storage->group_required_members.push_back(make_required(members));
        auto [gtags, gbloom] = make_group_tags(members);
        storage->group_group_tags.push_back(std::move(gtags));
        group_blooms.push_back(gbloom);
        storage->group_members.push_back(std::move(members));
    }
    storage->groups.reserve(group_keys.size());
    for (std::size_t i = 0; i < group_keys.size(); ++i) {
        const auto& group = group_definitions.at(group_keys[i]);
        if (group.members.empty() || (group.members.front().kind != MemberDef::Kind::field &&
            group.members.front().kind != MemberDef::Kind::group))
            throw XmlFailure{{ErrorCode::invalid_value}, group.location, "repeating group must begin with a field delimiter"};
        const auto counter = schema.fields.at(group.name).tag;
        const auto delimiter = schema.fields.at(group.members.front().name).tag;
        storage->groups.push_back({storage->keep(group.name), counter, delimiter,
            std::span<const MessageMemberDescriptor>{storage->group_members[i]},
            std::span<const MemberTagEntry>{storage->group_members_by_tag[i]},
            std::span<const std::uint32_t>{storage->group_required_members[i]},
            std::span<const std::uint32_t>{storage->group_group_tags[i]},
            group_blooms[i], true});
    }

    storage->component_members.reserve(schema.sorted_components.size());
    storage->component_members_by_tag.reserve(schema.sorted_components.size());
    storage->component_group_tags.reserve(schema.sorted_components.size());
    std::vector<std::uint64_t> component_blooms;
    component_blooms.reserve(schema.sorted_components.size());
    for (const auto& component : schema.sorted_components) {
        auto members = make_members(schema.expanded_components.at(component.name), make_members);
        storage->component_members_by_tag.push_back(make_tag_index(members));
        auto [gtags, gbloom] = make_group_tags(members);
        storage->component_group_tags.push_back(std::move(gtags));
        component_blooms.push_back(gbloom);
        storage->component_members.push_back(std::move(members));
    }
    storage->components.reserve(schema.sorted_components.size());
    std::int32_t header_comp_idx = -1;
    std::int32_t trailer_comp_idx = -1;
    auto session_req_mask_for = [&](const std::vector<MessageMemberDescriptor>& members) -> std::uint16_t {
        std::uint16_t mask = 0;
        for (const auto& m : members) {
            if (!m.required || m.kind != MemberKind::field || m.field_index >= storage->fields.size()) continue;
            switch (storage->fields[m.field_index].tag) {
            case 8u:    mask |= (1u << 0); break;
            case 9u:    mask |= (1u << 1); break;
            case 35u:   mask |= (1u << 2); break;
            case 49u:   mask |= (1u << 3); break;
            case 56u:   mask |= (1u << 4); break;
            case 34u:   mask |= (1u << 5); break;
            case 52u:   mask |= (1u << 6); break;
            case 43u:   mask |= (1u << 7); break;
            case 122u:  mask |= (1u << 8); break;
            case 369u:  mask |= (1u << 9); break;
            case 1128u: mask |= (1u << 10); break;
            case 10u:   mask |= (1u << 11); break;
            default: break;
            }
        }
        return mask;
    };
    for (std::size_t i = 0; i < schema.sorted_components.size(); ++i) {
        if (schema.sorted_components[i].name == "Header") header_comp_idx = static_cast<std::int32_t>(i);
        if (schema.sorted_components[i].name == "Trailer") trailer_comp_idx = static_cast<std::int32_t>(i);
        const std::uint16_t req_smask = session_req_mask_for(storage->component_members[i]);
        storage->components.push_back({storage->keep(schema.sorted_components[i].name),
            std::span<const MessageMemberDescriptor>{storage->component_members[i]},
            std::span<const MemberTagEntry>{storage->component_members_by_tag[i]},
            std::span<const std::uint32_t>{storage->component_group_tags[i]},
            component_blooms[i], true, req_smask});
    }

    storage->message_members.reserve(schema.sorted_messages.size());
    storage->message_members_by_tag.reserve(schema.sorted_messages.size());
    storage->message_required_members.reserve(schema.sorted_messages.size());
    storage->message_group_tags.reserve(schema.sorted_messages.size());
    std::vector<std::uint64_t> message_blooms;
    message_blooms.reserve(schema.sorted_messages.size());
    for (const auto& message : schema.sorted_messages) {
        auto members = make_members(schema.expanded_messages.at(message.name), make_members);
        storage->message_members_by_tag.push_back(make_tag_index(members));
        storage->message_required_members.push_back(make_required(members));
        auto [gtags, gbloom] = make_group_tags(members);
        storage->message_group_tags.push_back(std::move(gtags));
        message_blooms.push_back(gbloom);
        storage->message_members.push_back(std::move(members));
    }
    storage->messages.reserve(schema.sorted_messages.size());
    storage->single_char_message_index.fill(-1);
    for (std::size_t i = 0; i < schema.sorted_messages.size(); ++i) {
        const auto& message = schema.sorted_messages[i];
        if (message.msg_type.size() == 1) {
            const auto code = static_cast<unsigned char>(message.msg_type[0]);
            if (code < 128 && storage->single_char_message_index[code] == -1) {
                storage->single_char_message_index[code] = static_cast<std::int16_t>(i);
            }
        }
        storage->messages.push_back({storage->keep(message.name), storage->keep(message.msg_type),
            storage->keep(message.category), std::span<const MessageMemberDescriptor>{storage->message_members[i]},
            std::span<const MemberTagEntry>{storage->message_members_by_tag[i]},
            std::span<const std::uint32_t>{storage->message_required_members[i]},
            std::span<const std::uint32_t>{storage->message_group_tags[i]},
            message_blooms[i], true});
    }

    for (const auto& field : schema.sorted_fields) {
        if (field.type != "DATA" && field.type != "XMLDATA") continue;
        storage->pairs.push_back({pair_tag_for(field, schema.fields), field.tag});
    }
    if (storage->pairs.size() > max_wire_hint_pairs)
        throw XmlFailure{{ErrorCode::capacity_exceeded}, {}, "schema has more than the supported Length/Data pair limit"};
    const auto type = schema.identity.contains("type") ? schema.identity.at("type") : "FIX";
    std::string version;
    for (const auto key : {"major", "minor", "servicepack"}) {
        if (!schema.identity.contains(key)) continue;
        if (!version.empty()) version.push_back('.');
        version += schema.identity.at(key);
    }
    storage->descriptor = {storage->keep(type), storage->keep(version), fingerprint_view,
        std::span<const FieldDescriptor>{storage->fields}, std::span<const GroupDescriptor>{storage->groups},
        std::span<const ComponentDescriptor>{storage->components}, std::span<const MessageDescriptor>{storage->messages},
        header_comp_idx, trailer_comp_idx, std::span<const std::int16_t>{storage->single_char_message_index}};
    return storage;
}

void set_diagnostic(XmlSchemaDiagnostic& diagnostic, const XmlFailure& failure) noexcept {
    try {
        diagnostic.status = failure.status;
        diagnostic.source = failure.location.source;
        diagnostic.message = failure.message;
        diagnostic.line = failure.location.line;
        diagnostic.column = failure.location.column;
    } catch (...) {
        diagnostic.status = {ErrorCode::capacity_exceeded};
        diagnostic.source.clear();
        diagnostic.message.clear();
        diagnostic.line = diagnostic.column = 0;
    }
}

} // namespace

XmlSchema::XmlSchema() noexcept = default;
XmlSchema::XmlSchema(std::unique_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}
XmlSchema::XmlSchema(XmlSchema&&) noexcept = default;
XmlSchema& XmlSchema::operator=(XmlSchema&&) noexcept = default;
XmlSchema::~XmlSchema() = default;

const SchemaDescriptor& XmlSchema::descriptor() const noexcept {
    static constexpr SchemaDescriptor empty{};
    return storage_ ? storage_->descriptor : empty;
}

WireHints XmlSchema::wire_hints() const noexcept {
    return storage_ ? WireHints{storage_->pairs} : WireHints{};
}

Status load_xml_schema(std::span<const std::string_view> paths, XmlSchema& output,
    XmlSchemaDiagnostic& diagnostic, XmlSchemaLimits limits) noexcept {
    diagnostic = {};
    if (paths.empty()) {
        const XmlFailure failure{{ErrorCode::invalid_value}, {}, "at least one XML input is required"};
        set_diagnostic(diagnostic, failure);
        return failure.status;
    }
    if (paths.size() > limits.max_input_files) {
        const XmlFailure failure{{ErrorCode::capacity_exceeded}, {}, "XML input file count limit exceeded"};
        set_diagnostic(diagnostic, failure);
        return failure.status;
    }
    try {
        std::vector<Document> documents;
        documents.reserve(paths.size());
        std::size_t total_bytes = 0;
        for (const auto path_view : paths) {
            const std::string path(path_view);
            std::ifstream stream(path, std::ios::binary);
            if (!stream) {
                const XmlFailure failure{{ErrorCode::io_error}, {path, 0, 0}, "cannot open XML input"};
                set_diagnostic(diagnostic, failure);
                return failure.status;
            }
            const auto max_read = limits.max_input_file_bytes == std::numeric_limits<std::size_t>::max()
                ? limits.max_input_file_bytes : limits.max_input_file_bytes + 1;
            std::string input;
            input.resize(max_read);
            stream.read(input.data(), static_cast<std::streamsize>(input.size()));
            if (stream.bad()) {
                const XmlFailure failure{{ErrorCode::io_error}, {path, 0, 0}, "I/O error reading XML input"};
                set_diagnostic(diagnostic, failure);
                return failure.status;
            }
            const auto read_count = static_cast<std::size_t>(stream.gcount());
            if (read_count > limits.max_input_file_bytes) {
                const XmlFailure failure{{ErrorCode::capacity_exceeded}, {path, 0, 0}, "XML file exceeds byte limit"};
                set_diagnostic(diagnostic, failure);
                return failure.status;
            }
            input.resize(read_count);
            if (input.size() > limits.max_total_input_bytes - std::min(total_bytes, limits.max_total_input_bytes)) {
                const XmlFailure failure{{ErrorCode::capacity_exceeded}, {path, 0, 0}, "combined XML inputs exceed byte limit"};
                set_diagnostic(diagnostic, failure);
                return failure.status;
            }
            total_bytes += input.size();
            if (input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
                const XmlFailure failure{{ErrorCode::capacity_exceeded}, {path, 0, 0}, "XML input is too large for the parser"};
                set_diagnostic(diagnostic, failure);
                return failure.status;
            }
            auto document = parse_document(path, input, limits);
            documents.push_back(std::move(document));
        }
        auto normalized = normalize(std::move(documents), limits);
        auto storage = build_storage(normalized, limits);
        output = XmlSchema{std::move(storage)};
        diagnostic = {};
        return Status::success();
    } catch (const XmlFailure& failure) {
        set_diagnostic(diagnostic, failure);
        return failure.status;
    } catch (...) {
        const XmlFailure failure{{ErrorCode::capacity_exceeded}, {}, "XML schema allocation or parsing limit exceeded"};
        set_diagnostic(diagnostic, failure);
        return failure.status;
    }
}

} // namespace nimblefix
