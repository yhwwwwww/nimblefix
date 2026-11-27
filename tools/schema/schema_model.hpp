#pragma once

#include "xml_parser.hpp"
#include "../../include/nimblefix/protocol.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace nimblefix::codegen {

inline const std::set<std::string_view>& admin_msg_types() {
    static const std::set<std::string_view> s{
        msg_type::heartbeat, msg_type::test_request, msg_type::resend_request,
        msg_type::reject, msg_type::sequence_reset, msg_type::logout, msg_type::logon,
    };
    return s;
}

inline const std::set<std::uint32_t>& managed_tags() {
    static const std::set<std::uint32_t> s{
        tag::begin_string, tag::body_length, tag::check_sum,
        tag::msg_seq_num, tag::msg_type, tag::poss_dup_flag,
        tag::sender_comp_id, tag::sending_time, tag::target_comp_id,
        tag::orig_sending_time, tag::last_msg_seq_num_processed,
    };
    return s;
}

inline const std::set<std::uint32_t>& session_or_frame_tags() {
    static const std::set<std::uint32_t> s{
        tag::begin_string, tag::body_length, tag::check_sum,
        tag::msg_seq_num, tag::msg_type, tag::poss_dup_flag,
        tag::sender_comp_id, tag::sending_time, tag::target_comp_id,
        tag::orig_sending_time, tag::last_msg_seq_num_processed,
        tag::appl_ver_id,
    };
    return s;
}

inline const std::set<std::pair<std::uint32_t, std::uint32_t>>& standard_length_data_pairs() {
    static const std::set<std::pair<std::uint32_t, std::uint32_t>> s{
        {tag::secure_data_len, tag::secure_data},
        {tag::signature_length, tag::signature},
        {tag::raw_data_length, tag::raw_data},
        {tag::xml_data_len, tag::xml_data},
        {tag::encoded_issuer_len, tag::encoded_issuer},
        {tag::encoded_security_desc_len, tag::encoded_security_desc},
        {tag::encoded_list_exec_inst_len, tag::encoded_list_exec_inst},
        {tag::encoded_text_len, tag::encoded_text},
        {tag::encoded_subject_len, tag::encoded_subject},
        {tag::encoded_headline_len, tag::encoded_headline},
        {tag::encoded_alloc_text_len, tag::encoded_alloc_text},
        {tag::encoded_underlying_issuer_len, tag::encoded_underlying_issuer},
        {tag::encoded_underlying_security_desc_len, tag::encoded_underlying_security_desc},
        {tag::encoded_list_status_text_len, tag::encoded_list_status_text},
        {tag::encoded_leg_issuer_len, tag::encoded_leg_issuer},
        {tag::encoded_leg_security_desc_len, tag::encoded_leg_security_desc},
    };
    return s;
}

inline const std::map<std::string, std::string>& fix_wire_types() {
    static const std::map<std::string, std::string> m{
        {"STRING", "string"}, {"COUNTRY", "string"}, {"CURRENCY", "string"}, {"EXCHANGE", "string"},
        {"MULTIPLEVALUESTRING", "string"}, {"MULTIPLECHARVALUE", "string"}, {"MULTIPLESTRINGVALUE", "string"},
        {"LANGUAGE", "string"}, {"LOCALMKTTIME", "string"}, {"TZTIMEONLY", "string"}, {"TZTIMESTAMP", "string"},
        {"XID", "string"}, {"XIDREF", "string"}, {"XMLDATA", "binary_data"},
        {"CHAR", "character"}, {"BOOLEAN", "character"},
        {"INT", "integer"}, {"LENGTH", "integer"}, {"NUMINGROUP", "integer"},
        {"SEQNUM", "integer"}, {"DAYOFMONTH", "integer"}, {"TAGNUM", "integer"},
        {"AMT", "decimal"}, {"FLOAT", "decimal"}, {"PERCENTAGE", "decimal"},
        {"PRICE", "decimal"}, {"PRICEOFFSET", "decimal"}, {"QTY", "decimal"},
        {"UTCTIMESTAMP", "utc_timestamp"}, {"UTCDATEONLY", "string"}, {"UTCDATE", "string"},
        {"LOCALMKTDATE", "string"}, {"UTCTIMEONLY", "string"}, {"MONTHYEAR", "string"},
        {"DATA", "binary_data"},
    };
    return m;
}

inline const std::map<std::string, std::string>& fix_field_classes() {
    static const std::map<std::string, std::string> m{
        {"STRING", "text"}, {"COUNTRY", "text"}, {"CURRENCY", "text"}, {"EXCHANGE", "text"},
        {"LANGUAGE", "text"}, {"LOCALMKTTIME", "text"}, {"TZTIMEONLY", "text"}, {"TZTIMESTAMP", "text"},
        {"XID", "text"}, {"XIDREF", "text"},
        {"MULTIPLEVALUESTRING", "multiple_value_string"},
        {"MULTIPLECHARVALUE", "multiple_char_value"},
        {"MULTIPLESTRINGVALUE", "multiple_string_value"},
        {"CHAR", "character"}, {"BOOLEAN", "boolean"},
        {"INT", "signed_integer"}, {"LENGTH", "unsigned_integer"}, {"NUMINGROUP", "unsigned_integer"},
        {"SEQNUM", "unsigned_integer"}, {"TAGNUM", "tag_number"}, {"DAYOFMONTH", "day_of_month"},
        {"AMT", "decimal"}, {"FLOAT", "decimal"}, {"PERCENTAGE", "decimal"},
        {"PRICE", "decimal"}, {"PRICEOFFSET", "decimal"}, {"QTY", "decimal"},
        {"UTCTIMESTAMP", "utc_timestamp"}, {"UTCDATE", "utc_date"}, {"UTCDATEONLY", "utc_date"},
        {"LOCALMKTDATE", "local_market_date"}, {"UTCTIMEONLY", "utc_time"}, {"MONTHYEAR", "month_year"},
        {"DATA", "binary_data"}, {"XMLDATA", "binary_data"},
    };
    return m;
}

inline const std::set<std::string_view>& cpp_keywords() {
    static const std::set<std::string_view> s{
        "alignas", "alignof", "and", "and_eq", "asm", "atomic_cancel", "atomic_commit", "atomic_noexcept",
        "auto", "bitand", "bitor", "bool", "break", "case", "catch", "char", "char8_t", "char16_t", "char32_t",
        "class", "compl", "concept", "const", "consteval", "constexpr", "constinit", "const_cast", "continue",
        "co_await", "co_return", "co_yield", "decltype", "default", "delete", "do", "double", "dynamic_cast",
        "else", "enum", "explicit", "export", "extern", "false", "float", "for", "friend", "goto", "if", "inline", "int",
        "long", "mutable", "namespace", "new", "noexcept", "not", "not_eq", "nullptr", "operator", "or", "or_eq",
        "private", "protected", "public", "register", "reinterpret_cast", "requires", "return", "short",
        "signed", "sizeof", "static", "static_assert", "static_cast", "struct", "switch", "synchronized",
        "template", "this", "thread_local", "throw", "true", "try", "typedef", "typeid", "typename", "union",
        "unsigned", "using", "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq",
    };
    return s;
}

inline const std::set<std::string_view>& builder_reserved_setters() {
    static const std::set<std::string_view> s{
        "make", "set", "status", "view", "reset", "header", "validate_required", "entry",
        "msg_type", "slot_count", "slot_storage_bytes", "wire_order", "encoding_plan",
        "physical_slot_map", "shape_type", "policy_type", "canonical_shape_type", "plan_type",
    };
    return s;
}

inline const std::set<std::string_view>& nimblefix_root_symbols() {
    static const std::set<std::string_view> s{
        "Decimal", "ErrorCode", "Field", "FieldKind", "FieldList", "FieldWireType",
        "Fields", "FixedMessage", "FixedShapeMessage", "FixedString", "FlatMessagePolicy",
        "MessageBuilder", "MessageLimits", "Result", "StaticMessage", "Status",
        "WireRank", "WireType", "appl_ver_id", "detail", "msg_type", "reject_reason",
        "soh", "tag",
    };
    return s;
}

struct FieldDef {
    std::uint32_t number{};
    std::string name;
    std::string field_type;
    std::vector<std::pair<std::string, std::string>> enum_values; // sorted
    Location location;

    [[nodiscard]] bool same_signature(const FieldDef& o) const {
        return number == o.number && name == o.name &&
               field_type == o.field_type && enum_values == o.enum_values;
    }
};

struct MemberDef {
    std::string field_name;
    bool required{};
    Location location;
    std::string kind{"field"}; // "field", "component", "group"
    std::vector<MemberDef> members;

    [[nodiscard]] bool same_signature(const MemberDef& o) const {
        if (kind != o.kind || field_name != o.field_name || required != o.required ||
            members.size() != o.members.size()) {
            return false;
        }
        for (std::size_t i = 0; i < members.size(); ++i) {
            if (!members[i].same_signature(o.members[i])) return false;
        }
        return true;
    }
};

struct MessageDef {
    std::string name;
    std::string msg_type;
    std::string category;
    std::vector<MemberDef> members;
    Location location;

    [[nodiscard]] bool same_signature(const MessageDef& o) const {
        if (name != o.name || msg_type != o.msg_type || category != o.category ||
            members.size() != o.members.size()) {
            return false;
        }
        for (std::size_t i = 0; i < members.size(); ++i) {
            if (!members[i].same_signature(o.members[i])) return false;
        }
        return true;
    }
};

struct ComponentDef {
    std::string name;
    std::vector<MemberDef> members;
    Location location;

    [[nodiscard]] bool same_signature(const ComponentDef& o) const {
        if (name != o.name || members.size() != o.members.size()) return false;
        for (std::size_t i = 0; i < members.size(); ++i) {
            if (!members[i].same_signature(o.members[i])) return false;
        }
        return true;
    }
};

struct ParsedDocument {
    std::string source;
    Location root_location;
    std::vector<std::pair<std::string, std::string>> identity;
    std::vector<FieldDef> fields;
    std::vector<MessageDef> messages;
    std::vector<ComponentDef> components;
};

struct Schema {
    std::vector<std::pair<std::string, std::string>> identity;
    std::map<std::string, FieldDef> fields_by_name;
    std::map<std::string, MessageDef> messages_by_name;
    std::map<std::string, ComponentDef> components_by_name;
    std::map<std::string, std::vector<MemberDef>> expanded_messages;
    std::map<std::string, std::vector<MemberDef>> expanded_components;
};

inline bool is_ascii_identifier(std::string_view s) noexcept {
    if (s.empty() || !std::isalpha(static_cast<unsigned char>(s[0]))) return false;
    for (char c : s) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
    }
    return true;
}

inline bool is_all_digits(std::string_view s) noexcept {
    if (s.empty()) return false;
    for (char c : s) if (c < '0' || c > '9') return false;
    return true;
}

inline bool is_upper_type_token(std::string_view s) noexcept {
    if (s.empty() || s[0] < 'A' || s[0] > 'Z') return false;
    for (char c : s) {
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

inline void validate_xml_name(std::string_view value, std::string_view what, const Location& loc) {
    if (!is_ascii_identifier(value) || cpp_keywords().contains(value) || value.find("__") != std::string_view::npos) {
        throw SchemaError(
            "invalid C++ " + std::string{what} + " identifier '" + std::string{value} +
            "'; expected a non-reserved ASCII identifier",
            loc);
    }
}

inline std::string snake_case(std::string_view name) {
    // Matches Python:
    // re.sub(r"([A-Z]+)([A-Z][a-z])", r"\1_\2", name)
    // re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", value)
    auto is_up = [](char c) noexcept { return c >= 'A' && c <= 'Z'; };
    auto is_lo = [](char c) noexcept { return c >= 'a' && c <= 'z'; };
    auto is_lo_or_dig = [](char c) noexcept {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    };
    std::string pass1;
    pass1.reserve(name.size() + 8);
    for (std::size_t i = 0; i < name.size(); ++i) {
        if (i + 2 < name.size() && is_up(name[i]) && is_up(name[i + 1]) && is_lo(name[i + 2])) {
            pass1.push_back(name[i]);
            pass1.push_back('_');
        } else {
            pass1.push_back(name[i]);
        }
    }
    std::string pass2;
    pass2.reserve(pass1.size() + 8);
    for (std::size_t i = 0; i < pass1.size(); ++i) {
        if (i + 1 < pass1.size() && is_lo_or_dig(pass1[i]) && is_up(pass1[i + 1])) {
            pass2.push_back(pass1[i]);
            pass2.push_back('_');
        } else {
            pass2.push_back(pass1[i]);
        }
    }
    for (char& c : pass2) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return pass2;
}

inline std::string read_bounded(const std::filesystem::path& path, std::size_t max_bytes, std::string_view label) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw SchemaError("cannot read " + std::string{label} + " " + path.string());
    }
    std::string data;
    char buf[16384];
    while (in) {
        in.read(buf, sizeof(buf));
        const auto got = static_cast<std::size_t>(in.gcount());
        if (got == 0) break;
        if (data.size() + got > max_bytes) {
            throw SchemaError(std::string{label} + " exceeds " + std::to_string(max_bytes) + " byte limit");
        }
        data.append(buf, got);
    }
    return data;
}

inline void check_attrs(
    const XmlNode& node,
    std::initializer_list<std::string_view> allowed,
    std::initializer_list<std::string_view> required = {}) {
    for (const auto& [k, _v] : node.attrs) {
        if (std::find(allowed.begin(), allowed.end(), std::string_view{k}) == allowed.end()) {
            throw SchemaError("unexpected attribute '" + k + "' on <" + node.name + ">", node.location);
        }
    }
    for (const auto req : required) {
        if (!node.attrs.contains(std::string{req})) {
            throw SchemaError("missing required attribute '" + std::string{req} + "' on <" + node.name + ">", node.location);
        }
    }
}

inline std::vector<const XmlNode*> check_children(const XmlNode& node, std::string_view expected) {
    std::vector<const XmlNode*> out;
    out.reserve(node.children.size());
    for (const auto& child : node.children) {
        if (child.name != expected) {
            std::string reason;
            if (child.name == "group") reason = "repeating groups are not supported in stage1 XML codegen";
            else if (child.name == "component") reason = "components are not supported in stage1 XML codegen";
            else reason = "unsupported <" + child.name + "> structure under <" + node.name + ">";
            throw SchemaError(reason, child.location);
        }
        out.push_back(&child);
    }
    return out;
}

inline void require_leaf(const XmlNode& node) {
    if (node.children.empty()) return;
    const auto& child = node.children.front();
    std::string reason;
    if (child.name == "group") reason = "repeating groups are not supported in stage1 XML codegen";
    else if (child.name == "component") reason = "components are not supported in stage1 XML codegen";
    else reason = "unsupported <" + child.name + "> structure under <" + node.name + ">";
    throw SchemaError(reason, child.location);
}

inline std::vector<MemberDef> parse_members(const XmlNode& parent) {
    std::vector<MemberDef> members;
    std::map<std::pair<std::string, std::string>, MemberDef> seen;
    for (const auto& node : parent.children) {
        if (node.name != "field" && node.name != "component" && node.name != "group") {
            throw SchemaError("unsupported <" + node.name + "> structure under <" + parent.name + ">", node.location);
        }
        check_attrs(node, {"name", "required"}, {"name"});
        const auto& attrs = node.attrs;
        const std::string& mname = attrs.at("name");
        validate_xml_name(mname, node.name + " reference", node.location);
        const std::string req_str = attrs.contains("required") ? attrs.at("required") : "N";
        if (req_str != "Y" && req_str != "N") {
            throw SchemaError("required must be 'Y' or 'N'", node.location);
        }
        const std::string kind = node.name;
        std::vector<MemberDef> children;
        if (kind == "group") {
            children = parse_members(node);
        } else if (!node.children.empty()) {
            const auto& ch = node.children.front();
            throw SchemaError("unexpected <" + ch.name + "> structure under <" + node.name + ">", ch.location);
        }
        MemberDef member{mname, req_str == "Y", node.location, kind, std::move(children)};
        const auto key = std::make_pair(kind, mname);
        if (const auto it = seen.find(key); it != seen.end()) {
            if (kind == "field" && it->second.required == member.required) continue;
            throw SchemaError("duplicate " + kind + " reference '" + mname + "' under <" + parent.name + ">", node.location);
        }
        seen.emplace(key, member);
        members.push_back(std::move(member));
    }
    return members;
}

inline ParsedDocument parse_document(std::string_view data, const std::string& source) {
    const XmlNode root = StrictXmlParser::parse(data, source);
    if (root.name != "fix") {
        throw SchemaError("expected <fix> root, found <" + root.name + ">", root.location);
    }
    check_attrs(root, {"type", "major", "minor", "servicepack"});
    const auto& root_attrs = root.attrs;
    const std::string root_type = root_attrs.contains("type") ? root_attrs.at("type") : "FIX";
    if (root_type != "FIX" && root_type != "FIXT") {
        throw SchemaError("only FIX and FIXT XML dictionaries are supported", root.location);
    }
    for (const char* k : {"major", "minor", "servicepack"}) {
        if (const auto it = root_attrs.find(k); it != root_attrs.end() && !is_all_digits(it->second)) {
            throw SchemaError("invalid FIX schema version attribute " + std::string{k} + "='" + it->second + "'", root.location);
        }
    }
    std::vector<std::pair<std::string, std::string>> identity(root_attrs.begin(), root_attrs.end());

    std::vector<const XmlNode*> field_sections, message_sections, component_sections, headers, trailers;
    for (const auto& part : root.children) {
        if (part.name == "fields") field_sections.push_back(&part);
        else if (part.name == "messages") message_sections.push_back(&part);
        else if (part.name == "components") component_sections.push_back(&part);
        else if (part.name == "header") headers.push_back(&part);
        else if (part.name == "trailer") trailers.push_back(&part);
        else throw SchemaError("unsupported <" + part.name + "> section", part.location);
    }
    if (field_sections.size() > 1)
        throw SchemaError("only one <fields> section is allowed per XML document", field_sections[1]->location);
    if (message_sections.size() > 1)
        throw SchemaError("only one <messages> section is allowed per XML document", message_sections[1]->location);
    if (component_sections.size() > 1)
        throw SchemaError("only one <components> section is allowed per XML document", component_sections[1]->location);
    if (headers.size() > 1 || trailers.size() > 1)
        throw SchemaError("only one <header> and <trailer> section is allowed per XML document", root.location);

    std::vector<FieldDef> fields;
    if (!field_sections.empty()) {
        const auto& section = *field_sections[0];
        check_attrs(section, {});
        for (const auto* node : check_children(section, "field")) {
            check_attrs(*node, {"number", "name", "type"}, {"number", "name", "type"});
            const auto& attrs = node->attrs;
            const std::string& num_s = attrs.at("number");
            if (!is_all_digits(num_s)) throw SchemaError("invalid field number '" + num_s + "'", node->location);
            std::uint64_t num64 = 0;
            const auto r = std::from_chars(num_s.data(), num_s.data() + num_s.size(), num64);
            if (r.ec != std::errc{} || r.ptr != num_s.data() + num_s.size())
                throw SchemaError("invalid field number '" + num_s + "'", node->location);
            if (num64 == 0 || num64 > kMaxFixTag)
                throw SchemaError("field number must be between 1 and " + std::to_string(kMaxFixTag), node->location);
            const std::string& fname = attrs.at("name");
            const std::string& ftype = attrs.at("type");
            validate_xml_name(fname, "field", node->location);
            if (!is_upper_type_token(ftype))
                throw SchemaError("invalid FIX field type '" + ftype + "'", node->location);
            if (!fix_wire_types().contains(ftype))
                throw SchemaError("unsupported FIX field type '" + ftype + "'", node->location);

            std::vector<std::pair<std::string, std::string>> enums;
            std::set<std::string> seen_enum;
            for (const auto* vnode : check_children(*node, "value")) {
                require_leaf(*vnode);
                check_attrs(*vnode, {"enum", "description"}, {"enum"});
                const auto& vattrs = vnode->attrs;
                const std::string& ev = vattrs.at("enum");
                if (ev.empty() || ev.find('\x01') != std::string::npos ||
                    ev.find('\n') != std::string::npos || ev.find('\r') != std::string::npos) {
                    throw SchemaError("enum value must be non-empty and contain no control delimiter", vnode->location);
                }
                if (!seen_enum.insert(ev).second) {
                    throw SchemaError("duplicate enum value '" + ev + "'", vnode->location);
                }
                enums.emplace_back(ev, vattrs.contains("description") ? vattrs.at("description") : "");
            }
            std::sort(enums.begin(), enums.end());
            fields.push_back(FieldDef{static_cast<std::uint32_t>(num64), fname, ftype, std::move(enums), node->location});
        }
    }

    std::vector<ComponentDef> components;
    if (!component_sections.empty()) {
        const auto& section = *component_sections[0];
        check_attrs(section, {});
        for (const auto* node : check_children(section, "component")) {
            check_attrs(*node, {"name"}, {"name"});
            const auto& attrs = node->attrs;
            validate_xml_name(attrs.at("name"), "component", node->location);
            components.push_back(ComponentDef{attrs.at("name"), parse_members(*node), node->location});
        }
    }
    if (!headers.empty()) {
        check_attrs(*headers[0], {});
        components.push_back(ComponentDef{"Header", parse_members(*headers[0]), headers[0]->location});
    }
    if (!trailers.empty()) {
        check_attrs(*trailers[0], {});
        components.push_back(ComponentDef{"Trailer", parse_members(*trailers[0]), trailers[0]->location});
    }

    std::vector<MessageDef> messages;
    if (!message_sections.empty()) {
        const auto& section = *message_sections[0];
        check_attrs(section, {});
        for (const auto* node : check_children(section, "message")) {
            check_attrs(*node, {"name", "msgtype", "msgcat"}, {"name", "msgtype"});
            const auto& attrs = node->attrs;
            validate_xml_name(attrs.at("name"), "message", node->location);
            const std::string& mt = attrs.at("msgtype");
            bool mt_ok = !mt.empty() && mt.size() <= 16;
            for (unsigned char c : mt) if (c < 0x21 || c > 0x7E) mt_ok = false;
            if (!mt_ok) throw SchemaError("msgtype must be 1 to 16 printable ASCII characters", node->location);
            const std::string cat = attrs.contains("msgcat") ? attrs.at("msgcat") : "";
            if (!cat.empty() && cat != "admin" && cat != "app")
                throw SchemaError("msgcat must be 'admin' or 'app' when provided", node->location);
            messages.push_back(MessageDef{attrs.at("name"), mt, cat, parse_members(*node), node->location});
        }
    }

    if (field_sections.empty() && message_sections.empty() && component_sections.empty()) {
        throw SchemaError("document must contain <fields>, <components> and/or <messages>", root.location);
    }
    return ParsedDocument{source, root.location, std::move(identity), std::move(fields), std::move(messages), std::move(components)};
}

inline std::string format_locations(std::vector<Location> locs) {
    std::sort(locs.begin(), locs.end());
    std::string out;
    for (std::size_t i = 0; i < locs.size(); ++i) {
        if (i != 0) out += " and ";
        out += locs[i].format();
    }
    return out;
}

inline std::vector<MemberDef> expand_members(
    const std::vector<MemberDef>& members,
    const std::map<std::string, ComponentDef>& components,
    bool inherited_required,
    std::vector<std::string>& stack,
    int& budget) {
    std::vector<MemberDef> expanded;
    for (const auto& m : members) {
        if (budget <= 0) throw SchemaError("expanded component member limit exceeded", m.location);
        --budget;
        const bool req = inherited_required && m.required;
        if (m.kind == "component") {
            const auto& comp = components.at(m.field_name);
            if (std::find(stack.begin(), stack.end(), comp.name) != stack.end()) {
                std::string chain;
                for (const auto& s : stack) chain += s + " -> ";
                chain += comp.name;
                throw SchemaError("recursive component reference: " + chain, m.location);
            }
            stack.push_back(comp.name);
            auto sub = expand_members(comp.members, components, req, stack, budget);
            stack.pop_back();
            for (auto& sm : sub) expanded.push_back(std::move(sm));
        } else if (m.kind == "group") {
            MemberDef g = m;
            g.required = req;
            g.members = expand_members(m.members, components, true, stack, budget);
            expanded.push_back(std::move(g));
        } else {
            MemberDef f = m;
            f.required = req;
            expanded.push_back(std::move(f));
        }
    }
    return expanded;
}

inline void check_member_references(
    const std::vector<MemberDef>& members,
    const std::map<std::string, FieldDef>& fields,
    const std::map<std::string, ComponentDef>& components,
    const std::string& context,
    std::vector<std::string>& stack,
    std::set<std::string>& verified_components) {
    for (const auto& m : members) {
        if (m.kind == "field") {
            if (!fields.contains(m.field_name)) {
                throw SchemaError("unknown field reference '" + m.field_name + "' in " + context, m.location);
            }
        } else if (m.kind == "component") {
            const auto it = components.find(m.field_name);
            if (it == components.end()) {
                throw SchemaError("unknown component reference '" + m.field_name + "' in " + context, m.location);
            }
            const auto& comp = it->second;
            if (std::find(stack.begin(), stack.end(), comp.name) != stack.end()) {
                std::string chain;
                for (const auto& s : stack) chain += s + " -> ";
                chain += comp.name;
                throw SchemaError("recursive component reference: " + chain, m.location);
            }
            if (verified_components.contains(comp.name)) continue;
            stack.push_back(comp.name);
            check_member_references(comp.members, fields, components, context, stack, verified_components);
            stack.pop_back();
            verified_components.insert(comp.name);
        } else if (m.kind == "group") {
            const auto it = fields.find(m.field_name);
            if (it == fields.end()) {
                throw SchemaError("unknown repeating group counter field '" + m.field_name + "' in " + context, m.location);
            }
            if (it->second.field_type != "NUMINGROUP" && it->second.field_type != "INT") {
                throw SchemaError("repeating group counter '" + m.field_name + "' must have NUMINGROUP or legacy INT type", m.location);
            }
            check_member_references(m.members, fields, components, context, stack, verified_components);
        }
    }
}

inline void validate_member_scope(
    const std::vector<MemberDef>& members,
    const std::map<std::string, FieldDef>& fields,
    const std::string& context,
    bool allow_managed = false) {
    std::set<std::uint32_t> seen;
    for (const auto& m : members) {
        const auto& f = fields.at(m.field_name);
        if (!seen.insert(f.number).second) {
            throw SchemaError("duplicate field tag " + std::to_string(f.number) + " in " + context, m.location);
        }
        if (m.kind == "field" && !allow_managed && managed_tags().contains(f.number)) {
            throw SchemaError(context + " references engine-managed FIX tag " + std::to_string(f.number), m.location);
        }
        if (m.kind == "group") {
            if (m.members.empty() || (m.members[0].kind != "field" && m.members[0].kind != "group")) {
                throw SchemaError("group '" + m.field_name + "' must begin with a field delimiter", m.location);
            }
            validate_member_scope(m.members, fields, "group '" + m.field_name + "'", allow_managed);
        }
    }
}

inline Schema load_schema(const std::vector<std::filesystem::path>& paths) {
    if (paths.empty()) throw SchemaError("at least one XML input is required");
    if (paths.size() > kMaxInputFiles)
        throw SchemaError("XML input count exceeds " + std::to_string(kMaxInputFiles) + " limit");

    std::vector<ParsedDocument> docs;
    std::size_t total = 0;
    for (const auto& path : paths) {
        std::string data = read_bounded(path, kMaxInputBytes, "XML input");
        total += data.size();
        if (total > kMaxTotalInputBytes) {
            throw SchemaError("combined XML inputs exceed " + std::to_string(kMaxTotalInputBytes) + " byte limit");
        }
        docs.push_back(parse_document(data, path.string()));
    }

    const auto& first_id = docs.front().identity;
    for (std::size_t i = 1; i < docs.size(); ++i) {
        if (docs[i].identity != first_id) {
            std::vector<Location> locs;
            for (const auto& d : docs) locs.push_back(d.root_location);
            throw SchemaError("conflicting FIX schema identity/version across XML inputs: " + format_locations(std::move(locs)));
        }
    }

    std::map<std::string, FieldDef> fields_by_name;
    std::map<std::uint32_t, std::string> name_by_number;
    for (const auto& doc : docs) {
        for (const auto& f : doc.fields) {
            const FieldDef* prev = nullptr;
            if (const auto it = fields_by_name.find(f.name); it != fields_by_name.end()) prev = &it->second;
            else if (const auto nit = name_by_number.find(f.number); nit != name_by_number.end()) prev = &fields_by_name.at(nit->second);
            if (prev != nullptr && !prev->same_signature(f)) {
                throw SchemaError(
                    "conflicting field definition for '" + f.name + "' (tag " + std::to_string(f.number) +
                    "); definitions at " + format_locations({prev->location, f.location}));
            }
            if (prev == nullptr) {
                fields_by_name.emplace(f.name, f);
                name_by_number.emplace(f.number, f.name);
            }
        }
    }

    std::map<std::string, ComponentDef> components_by_name;
    for (const auto& doc : docs) {
        for (const auto& c : doc.components) {
            if (const auto it = components_by_name.find(c.name); it != components_by_name.end()) {
                if (!it->second.same_signature(c)) {
                    throw SchemaError(
                        "conflicting component definition for '" + c.name + "'; definitions at " +
                        format_locations({it->second.location, c.location}));
                }
            } else {
                components_by_name.emplace(c.name, c);
            }
        }
    }

    std::map<std::string, MessageDef> messages_by_name;
    std::map<std::string, std::string> name_by_msg_type;
    for (const auto& doc : docs) {
        for (const auto& m : doc.messages) {
            const MessageDef* prev = nullptr;
            if (const auto it = messages_by_name.find(m.name); it != messages_by_name.end()) prev = &it->second;
            else if (const auto tit = name_by_msg_type.find(m.msg_type); tit != name_by_msg_type.end()) prev = &messages_by_name.at(tit->second);
            if (prev != nullptr && !prev->same_signature(m)) {
                throw SchemaError(
                    "conflicting message definition for '" + m.name + "' (MsgType '" + m.msg_type +
                    "'); definitions at " + format_locations({prev->location, m.location}));
            }
            if (prev == nullptr) {
                messages_by_name.emplace(m.name, m);
                name_by_msg_type.emplace(m.msg_type, m.name);
            }
        }
    }

    if (messages_by_name.empty()) {
        throw SchemaError("merged XML inputs contain no <message> definitions");
    }

    std::set<std::string> verified_components;
    std::map<std::string, std::vector<MemberDef>> expanded_components;
    for (const auto& [cname, comp] : components_by_name) {
        std::vector<std::string> stack{cname};
        check_member_references(comp.members, fields_by_name, components_by_name,
                                "component '" + cname + "'", stack, verified_components);
        int budget = 100'000;
        stack = {cname};
        auto exp = expand_members(comp.members, components_by_name, true, stack, budget);
        validate_member_scope(exp, fields_by_name, "component '" + cname + "'",
                              cname == "Header" || cname == "Trailer");
        expanded_components.emplace(cname, std::move(exp));
    }

    std::map<std::string, std::vector<MemberDef>> expanded_messages;
    for (const auto& [mname, msg] : messages_by_name) {
        std::vector<std::string> stack;
        check_member_references(msg.members, fields_by_name, components_by_name,
                                "message '" + mname + "'", stack, verified_components);
        int budget = 100'000;
        stack.clear();
        auto exp = expand_members(msg.members, components_by_name, true, stack, budget);
        validate_member_scope(exp, fields_by_name, "message '" + mname + "'");
        expanded_messages.emplace(mname, std::move(exp));
    }

    for (const auto& [mname, _msg] : messages_by_name) {
        std::map<std::string, const MemberDef*> setters;
        for (const auto& member : expanded_messages.at(mname)) {
            if (member.kind != "field") continue;
            const std::string setter = snake_case(member.field_name);
            if (builder_reserved_setters().contains(setter)) {
                throw SchemaError("field '" + member.field_name + "' generates reserved builder setter '" + setter + "'", member.location);
            }
            if (const auto it = setters.find(setter); it != setters.end()) {
                throw SchemaError("field names '" + it->second->field_name + "' and '" + member.field_name +
                                  "' both generate setter '" + setter + "'", member.location);
            }
            validate_xml_name(setter, "setter", member.location);
            setters.emplace(setter, &member);
        }
    }

    std::set<std::string> generated_symbols{"FieldDescriptor", "EnumValueDescriptor", "fields", "enum_values", "Schema"};
    for (const auto& [mname, msg] : messages_by_name) {
        for (const std::string& sym : {mname, mname + "View", mname + "Policy", mname + "Builder",
                                       mname + "_allowed_tags", mname + "_required_tags"}) {
            if (!generated_symbols.insert(sym).second) {
                throw SchemaError("message '" + mname + "' collides with generated C++ symbol '" + sym + "'", msg.location);
            }
        }
    }

    Schema schema{};
    schema.identity = first_id;
    schema.fields_by_name = std::move(fields_by_name);
    schema.messages_by_name = std::move(messages_by_name);
    schema.components_by_name = std::move(components_by_name);
    schema.expanded_messages = std::move(expanded_messages);
    schema.expanded_components = std::move(expanded_components);
    return schema;
}

inline std::vector<std::string> pair_name_variants(const std::string& name) {
    auto replace_all = [](std::string s, std::string_view from, std::string_view to) {
        std::size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }
        return s;
    };
    std::string compact = replace_all(name, "Security", "Sec");
    std::string expanded = (name.find("Security") == std::string::npos) ? replace_all(name, "Sec", "Security") : name;
    return {name, std::move(compact), std::move(expanded)};
}

inline Schema filter_schema_messages(const Schema& schema, const std::vector<std::string>& requested) {
    std::map<std::string, const MessageDef*> by_type;
    for (const auto& [_k, m] : schema.messages_by_name) by_type.emplace(m.msg_type, &m);

    std::map<std::string, MessageDef> selected_messages;
    for (const auto& raw : requested) {
        std::size_t b = 0, e = raw.size();
        while (b < e && std::isspace(static_cast<unsigned char>(raw[b]))) ++b;
        while (e > b && std::isspace(static_cast<unsigned char>(raw[e - 1]))) --e;
        const std::string token = raw.substr(b, e - b);
        if (token.empty()) continue;
        const MessageDef* found = nullptr;
        if (const auto it = schema.messages_by_name.find(token); it != schema.messages_by_name.end()) found = &it->second;
        else if (const auto tit = by_type.find(token); tit != by_type.end()) found = tit->second;
        if (!found) throw SchemaError("unknown message selector '" + token + "' for --messages");
        selected_messages.emplace(found->name, *found);
    }
    if (selected_messages.empty()) throw SchemaError("--messages selected no messages");

    std::set<std::string> used_fields;
    auto collect_fields = [&](auto&& self, const std::vector<MemberDef>& ms) -> void {
        for (const auto& m : ms) {
            used_fields.insert(m.field_name);
            if (m.kind == "group") self(self, m.members);
        }
    };

    std::map<std::string, ComponentDef> sel_comps;
    std::map<std::string, std::vector<MemberDef>> sel_exp_comps;
    for (const char* cname : {"Header", "Trailer"}) {
        if (const auto it = schema.components_by_name.find(cname); it != schema.components_by_name.end()) {
            sel_comps.emplace(cname, it->second);
            sel_exp_comps.emplace(cname, schema.expanded_components.at(cname));
            collect_fields(collect_fields, sel_exp_comps.at(cname));
        }
    }

    std::map<std::string, std::vector<MemberDef>> sel_exp_msgs;
    for (const auto& [name, _msg] : selected_messages) {
        const auto& exp = schema.expanded_messages.at(name);
        sel_exp_msgs.emplace(name, exp);
        collect_fields(collect_fields, exp);
    }

    std::vector<std::string> snap(used_fields.begin(), used_fields.end());
    for (const auto& fname : snap) {
        const auto& fdef = schema.fields_by_name.at(fname);
        if (fdef.field_type == "DATA" || fdef.field_type == "XMLDATA") {
            for (const auto& base : pair_name_variants(fdef.name)) {
                for (const char* suffix : {"Length", "Len"}) {
                    const std::string cand = base + suffix;
                    if (const auto it = schema.fields_by_name.find(cand);
                        it != schema.fields_by_name.end() && it->second.field_type == "LENGTH") {
                        used_fields.insert(cand);
                    }
                }
            }
        }
    }

    std::map<std::string, FieldDef> sel_fields;
    for (const auto& fname : used_fields) {
        if (const auto it = schema.fields_by_name.find(fname); it != schema.fields_by_name.end()) {
            sel_fields.emplace(fname, it->second);
        }
    }

    Schema subset{};
    subset.identity = schema.identity;
    subset.fields_by_name = std::move(sel_fields);
    subset.messages_by_name = std::move(selected_messages);
    subset.components_by_name = std::move(sel_comps);
    subset.expanded_messages = std::move(sel_exp_msgs);
    subset.expanded_components = std::move(sel_exp_comps);
    return subset;
}

inline std::map<std::string, std::set<std::uint32_t>> scan_fix_logs(const std::vector<std::filesystem::path>& log_paths) {
    std::map<std::string, std::set<std::uint32_t>> observed;
    for (const auto& path : log_paths) {
        const std::string data = read_bounded(path, kMaxLogBytes, "FIX log");
        const std::size_t size = data.size();
        std::size_t start = 0;
        while (start < size) {
            const std::size_t pos = data.find("8=FIX", start);
            if (pos == std::string::npos) break;
            std::size_t delim_pos = std::string::npos;
            char delim = '\0';
            const std::size_t limit = std::min(size, pos + 20);
            for (std::size_t idx = pos + 5; idx < limit; ++idx) {
                const char b = data[idx];
                if (b == '\x01' || b == '|') {
                    delim_pos = idx;
                    delim = b;
                    break;
                }
                if (b == '\n' || b == '\r') break;
            }
            if (delim_pos == std::string::npos) {
                start = pos + 5;
                continue;
            }

            std::string msg_type;
            std::set<std::uint32_t> tags;
            std::size_t cursor = delim_pos + 1;
            while (cursor < size) {
                if (delim == '|' && (data[cursor] == '\n' || data[cursor] == '\r')) break;
                const std::size_t eq_limit = std::min(size, cursor + 16);
                std::size_t eq = std::string::npos;
                for (std::size_t k = cursor; k < eq_limit; ++k) {
                    if (data[k] == '=') { eq = k; break; }
                }
                if (eq == std::string::npos || eq == cursor) break;
                const std::string_view tag_sv{data.data() + cursor, eq - cursor};
                if (!is_all_digits(tag_sv)) break;
                std::uint64_t tag64 = 0;
                const auto r = std::from_chars(tag_sv.data(), tag_sv.data() + tag_sv.size(), tag64);
                if (r.ec != std::errc{} || tag64 == 0 || tag64 > kMaxFixTag) break;
                const auto tag = static_cast<std::uint32_t>(tag64);

                std::size_t end = data.find(delim, eq + 1);
                if (delim == '|') {
                    const std::size_t nl = data.find('\n', eq + 1);
                    if (nl != std::string::npos && (end == std::string::npos || nl < end)) end = nl;
                }
                if (end == std::string::npos) end = size;
                if (tag == 35) {
                    std::size_t vb = eq + 1, ve = end;
                    while (vb < ve && std::isspace(static_cast<unsigned char>(data[vb]))) ++vb;
                    while (ve > vb && std::isspace(static_cast<unsigned char>(data[ve - 1]))) --ve;
                    if (ve > vb) msg_type.assign(data.data() + vb, ve - vb);
                }
                tags.insert(tag);
                cursor = end + 1;
                if (tag == 10) break;
            }
            if (!msg_type.empty()) {
                observed[msg_type].insert(tags.begin(), tags.end());
            }
            start = std::max(pos + 5, cursor);
        }
    }
    return observed;
}

struct PruneStats {
    std::size_t messages_before{};
    std::size_t messages_after{};
    std::size_t fields_before{};
    std::size_t fields_after{};
    std::size_t observed_msg_types{};
};

inline std::pair<Schema, PruneStats> prune_schema_from_observations(
    const Schema& schema,
    const std::map<std::string, std::set<std::uint32_t>>& observed,
    bool keep_all_fields) {
    if (observed.empty()) throw SchemaError("no FIX messages found in --from-fix-log input(s)");

    std::map<std::string, const MessageDef*> by_type;
    for (const auto& [_k, m] : schema.messages_by_name) by_type.emplace(m.msg_type, &m);

    std::map<std::string, MessageDef> selected_messages;
    for (const auto& [mt, _tags] : observed) {
        if (const auto it = by_type.find(mt); it != by_type.end()) {
            selected_messages.emplace(it->second->name, *it->second);
        }
    }
    for (const auto& [_k, msg] : schema.messages_by_name) {
        if (msg.category == "admin" || admin_msg_types().contains(msg.msg_type)) {
            selected_messages.emplace(msg.name, msg);
        }
    }
    if (selected_messages.empty()) throw SchemaError("--from-fix-log matched no messages in schema");

    std::map<std::uint32_t, std::uint32_t> partner_by_tag;
    for (const auto& [_k, fdef] : schema.fields_by_name) {
        if (fdef.field_type == "DATA" || fdef.field_type == "XMLDATA") {
            for (const auto& base : pair_name_variants(fdef.name)) {
                bool found = false;
                for (const char* suffix : {"Length", "Len"}) {
                    const std::string cand = base + suffix;
                    if (const auto it = schema.fields_by_name.find(cand);
                        it != schema.fields_by_name.end() && it->second.field_type == "LENGTH") {
                        partner_by_tag[fdef.number] = it->second.number;
                        partner_by_tag[it->second.number] = fdef.number;
                        found = true;
                        break;
                    }
                }
                if (found) break;
            }
        }
    }

    auto group_has_observed = [&](auto&& self, const std::vector<MemberDef>& ms, const std::set<std::uint32_t>& obs) -> bool {
        for (const auto& m : ms) {
            if (obs.contains(schema.fields_by_name.at(m.field_name).number)) return true;
            if (m.kind == "group" && self(self, m.members, obs)) return true;
        }
        return false;
    };

    auto prune_members = [&](auto&& self, const std::vector<MemberDef>& ms, const std::set<std::uint32_t>& obs, bool is_group_body) -> std::vector<MemberDef> {
        std::vector<MemberDef> kept;
        for (std::size_t idx = 0; idx < ms.size(); ++idx) {
            const auto& m = ms[idx];
            const std::uint32_t tag = schema.fields_by_name.at(m.field_name).number;
            const bool must_keep = m.required || (is_group_body && idx == 0);
            if (m.kind == "field") {
                if (must_keep || obs.contains(tag)) kept.push_back(m);
            } else {
                if (must_keep || obs.contains(tag) || group_has_observed(group_has_observed, m.members, obs)) {
                    MemberDef copy = m;
                    copy.members = self(self, m.members, obs, true);
                    kept.push_back(std::move(copy));
                }
            }
        }
        return kept;
    };

    std::set<std::string> used_fields;
    auto collect_fields = [&](auto&& self, const std::vector<MemberDef>& ms) -> void {
        for (const auto& m : ms) {
            used_fields.insert(m.field_name);
            if (m.kind == "group") self(self, m.members);
        }
    };

    std::map<std::string, ComponentDef> sel_comps;
    std::map<std::string, std::vector<MemberDef>> sel_exp_comps;
    for (const char* cname : {"Header", "Trailer"}) {
        if (const auto it = schema.components_by_name.find(cname); it != schema.components_by_name.end()) {
            sel_comps.emplace(cname, it->second);
            sel_exp_comps.emplace(cname, schema.expanded_components.at(cname));
            collect_fields(collect_fields, sel_exp_comps.at(cname));
        }
    }

    std::map<std::string, std::vector<MemberDef>> sel_exp_msgs;
    for (const auto& [name, msg] : selected_messages) {
        const auto& exp = schema.expanded_messages.at(name);
        const bool is_admin = msg.category == "admin" || admin_msg_types().contains(msg.msg_type);
        std::vector<MemberDef> pruned;
        if (keep_all_fields || is_admin || !observed.contains(msg.msg_type)) {
            pruned = exp;
        } else {
            std::set<std::uint32_t> obs = observed.at(msg.msg_type);
            std::vector<std::uint32_t> snap(obs.begin(), obs.end());
            for (std::uint32_t t : snap) {
                if (const auto pit = partner_by_tag.find(t); pit != partner_by_tag.end()) obs.insert(pit->second);
            }
            pruned = prune_members(prune_members, exp, obs, false);
        }
        collect_fields(collect_fields, pruned);
        sel_exp_msgs.emplace(name, std::move(pruned));
    }

    std::vector<std::string> uf_snap(used_fields.begin(), used_fields.end());
    for (const auto& fname : uf_snap) {
        const auto& fdef = schema.fields_by_name.at(fname);
        if (fdef.field_type == "DATA" || fdef.field_type == "XMLDATA") {
            if (const auto pit = partner_by_tag.find(fdef.number); pit != partner_by_tag.end()) {
                for (const auto& [cand_name, cand] : schema.fields_by_name) {
                    if (cand.number == pit->second) { used_fields.insert(cand_name); break; }
                }
            }
        }
    }

    std::map<std::string, FieldDef> sel_fields;
    for (const auto& fname : used_fields) {
        if (const auto it = schema.fields_by_name.find(fname); it != schema.fields_by_name.end()) {
            sel_fields.emplace(fname, it->second);
        }
    }

    PruneStats stats{
        schema.messages_by_name.size(),
        selected_messages.size(),
        schema.fields_by_name.size(),
        sel_fields.size(),
        observed.size(),
    };
    Schema subset{};
    subset.identity = schema.identity;
    subset.fields_by_name = std::move(sel_fields);
    subset.messages_by_name = std::move(selected_messages);
    subset.components_by_name = std::move(sel_comps);
    subset.expanded_messages = std::move(sel_exp_msgs);
    subset.expanded_components = std::move(sel_exp_comps);
    return {std::move(subset), stats};
}

} // namespace nimblefix::codegen
