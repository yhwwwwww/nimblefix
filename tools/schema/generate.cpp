#include "schema_model.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace nimblefix::codegen {
namespace {

std::string cpp_string(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 4);
    out.push_back('"');
    for (unsigned char b : value) {
        if (b == 0x22) out += "\\\"";
        else if (b == 0x5C) out += "\\\\";
        else if (b == 0x0A) out += "\\n";
        else if (b == 0x0D) out += "\\r";
        else if (b == 0x09) out += "\\t";
        else if (b >= 0x20 && b <= 0x7E) out.push_back(static_cast<char>(b));
        else {
            char oct[5];
            std::snprintf(oct, sizeof(oct), "\\%03o", static_cast<unsigned>(b));
            out.append(oct, 4);
        }
    }
    out.push_back('"');
    return out;
}

std::string hex_u64(std::uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%llxULL", static_cast<unsigned long long>(v));
    return std::string{buf};
}

std::string hex_u16(unsigned v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%04xu", v);
    return std::string{buf};
}

std::string byte_case(unsigned char b) {
    if (b == '\'') return "case '\\'':";
    if (b == '\\') return "case '\\\\':";
    if (b >= 0x20 && b <= 0x7E) {
        std::string s = "case '";
        s.push_back(static_cast<char>(b));
        s += "':";
        return s;
    }
    return "case static_cast<char>(" + std::to_string(static_cast<unsigned>(b)) + "):";
}

std::vector<std::string> validate_namespace(std::string_view value) {
    std::vector<std::string> parts;
    std::size_t pos = 0;
    for (;;) {
        const std::size_t sep = value.find("::", pos);
        const std::string_view part = (sep == std::string_view::npos)
            ? value.substr(pos)
            : value.substr(pos, sep - pos);
        if (!is_ascii_identifier(part) || cpp_keywords().contains(part) ||
            part.find("__") != std::string_view::npos) {
            throw SchemaError("invalid C++ namespace '" + std::string{value} +
                              "'; expected identifiers separated by '::'");
        }
        parts.emplace_back(part);
        if (sep == std::string_view::npos) break;
        pos = sep + 2;
    }
    if (parts.empty()) {
        throw SchemaError("invalid C++ namespace '" + std::string{value} + "'");
    }
    if (parts[0] == "std") {
        throw SchemaError("generated types cannot be added to the reserved std namespace");
    }
    return parts;
}

bool field_needs_policy_check(const FieldDef& field) {
    static const std::set<std::string_view> kCheckTypes{
        "BOOLEAN", "UTCDATE", "UTCDATEONLY", "LOCALMKTDATE", "UTCTIMEONLY",
        "MONTHYEAR", "DAYOFMONTH", "TAGNUM", "MULTIPLEVALUESTRING",
        "MULTIPLECHARVALUE", "MULTIPLESTRINGVALUE", "LENGTH", "NUMINGROUP", "SEQNUM",
    };
    return !field.enum_values.empty() || kCheckTypes.contains(field.field_type);
}

std::string base_field_check(std::string_view fc) {
    if (fc == "character") return "::nimblefix::detail::check_char(f)";
    if (fc == "boolean") return "::nimblefix::detail::check_bool(f)";
    if (fc == "signed_integer") return "::nimblefix::detail::check_i64(f)";
    if (fc == "unsigned_integer") return "::nimblefix::detail::check_u64(f)";
    if (fc == "day_of_month") return "::nimblefix::detail::check_day_of_month(f)";
    if (fc == "tag_number") return "::nimblefix::detail::check_tag_num(f)";
    if (fc == "decimal") return "::nimblefix::detail::check_decimal(f)";
    if (fc == "utc_timestamp") return "::nimblefix::detail::check_utc_ts(f)";
    if (fc == "utc_date" || fc == "local_market_date") return "::nimblefix::detail::check_date(f)";
    if (fc == "utc_time") return "::nimblefix::detail::check_time(f)";
    if (fc == "month_year") return "::nimblefix::detail::check_month_year(f)";
    if (fc == "binary_data") return "::nimblefix::detail::check_binary(f)";
    if (fc == "text") return "::nimblefix::detail::check_text(f)";
    if (fc == "multiple_char_value") return "::nimblefix::detail::check_mvs(f, true)";
    return "::nimblefix::detail::check_mvs(f, false)";
}

std::string render_header(const Schema& schema, std::string_view ns_arg) {
    const auto ns_parts = validate_namespace(ns_arg);
    std::string ns;
    for (std::size_t i = 0; i < ns_parts.size(); ++i) {
        if (i != 0) ns += "::";
        ns += ns_parts[i];
    }
    if (ns_parts.size() == 1 && ns_parts[0] == "nimblefix") {
        std::vector<std::string> collisions;
        for (const auto& [mname, _m] : schema.messages_by_name) {
            if (nimblefix_root_symbols().contains(mname)) collisions.push_back(mname);
        }
        if (!collisions.empty()) {
            std::string joined;
            for (std::size_t i = 0; i < collisions.size(); ++i) {
                if (i != 0) joined += ", ";
                joined += collisions[i];
            }
            throw SchemaError("generated message name collides with nimblefix core symbol(s): " + joined);
        }
    }

    std::vector<const FieldDef*> fields;
    fields.reserve(schema.fields_by_name.size());
    for (const auto& [_k, f] : schema.fields_by_name) fields.push_back(&f);
    std::sort(fields.begin(), fields.end(), [](const FieldDef* a, const FieldDef* b) {
        return a->number < b->number;
    });
    std::map<std::string, std::size_t> field_indexes;
    for (std::size_t i = 0; i < fields.size(); ++i) field_indexes.emplace(fields[i]->name, i);

    std::vector<const MessageDef*> messages;
    messages.reserve(schema.messages_by_name.size());
    for (const auto& [_k, m] : schema.messages_by_name) messages.push_back(&m);
    std::sort(messages.begin(), messages.end(), [](const MessageDef* a, const MessageDef* b) {
        return std::tie(a->name, a->msg_type) < std::tie(b->name, b->msg_type);
    });

    std::vector<const ComponentDef*> components;
    components.reserve(schema.components_by_name.size());
    for (const auto& [_k, c] : schema.components_by_name) components.push_back(&c);
    std::sort(components.begin(), components.end(), [](const ComponentDef* a, const ComponentDef* b) {
        return a->name < b->name;
    });

    std::map<std::string, std::string> id_map(schema.identity.begin(), schema.identity.end());
    std::string version;
    for (const char* k : {"major", "minor", "servicepack"}) {
        if (const auto it = id_map.find(k); it != id_map.end() && !it->second.empty()) {
            if (!version.empty()) version.push_back('.');
            version += it->second;
        }
    }
    const std::string schema_type = id_map.contains("type") ? id_map.at("type") : "FIX";

    struct EnumEntry { std::uint32_t tag; std::string value; std::string desc; };
    std::vector<EnumEntry> enum_values;
    std::map<std::uint32_t, std::pair<std::size_t, std::size_t>> enum_ranges;
    for (const FieldDef* f : fields) {
        const std::size_t start = enum_values.size();
        for (const auto& [val, desc] : f->enum_values) {
            enum_values.push_back({f->number, val, desc});
        }
        enum_ranges[f->number] = {start, f->enum_values.size()};
    }

    auto replace_security = [](std::string s) {
        std::size_t pos = 0;
        while ((pos = s.find("Security", pos)) != std::string::npos) {
            s.replace(pos, 8, "Sec");
            pos += 3;
        }
        return s;
    };

    std::map<std::uint32_t, std::uint32_t> field_pair_tags;
    for (const FieldDef* f : fields) {
        if (f->field_type == "DATA" || f->field_type == "XMLDATA") {
            const std::string pname = replace_security(f->name);
            const std::array<std::string, 4> cands{
                f->name + "Length", f->name + "Len", pname + "Length", pname + "Len"};
            const FieldDef* matched = nullptr;
            for (const auto& c : cands) {
                if (const auto it = schema.fields_by_name.find(c);
                    it != schema.fields_by_name.end() && it->second.field_type == "LENGTH") {
                    matched = &it->second;
                    break;
                }
            }
            if (!matched) {
                throw SchemaError("DATA field '" + f->name + "' has no matching LENGTH field", f->location);
            }
            field_pair_tags[f->number] = matched->number;
        } else if (f->field_type == "LENGTH" || f->field_type == "NUMINGROUP") {
            std::vector<std::string> bases;
            if (f->name.ends_with("Length")) bases.push_back(f->name.substr(0, f->name.size() - 6));
            if (f->name.ends_with("Len")) bases.push_back(f->name.substr(0, f->name.size() - 3));
            std::vector<std::string> cands;
            for (const auto& b : bases) {
                if (b.empty()) continue;
                cands.push_back(b);
                cands.push_back(replace_security(b));
                if (b.find("Security") == std::string::npos && b.find("Sec") != std::string::npos) {
                    std::string exp = b;
                    std::size_t pos = 0;
                    while ((pos = exp.find("Sec", pos)) != std::string::npos) {
                        exp.replace(pos, 3, "Security");
                        pos += 8;
                    }
                    cands.push_back(std::move(exp));
                }
            }
            for (const auto& c : cands) {
                if (const auto it = schema.fields_by_name.find(c);
                    it != schema.fields_by_name.end() &&
                    (it->second.field_type == "DATA" || it->second.field_type == "XMLDATA")) {
                    field_pair_tags[f->number] = it->second.number;
                    break;
                }
            }
        }
    }

    // Deterministic interned group keys (canonical Python-repr-like string for stable ordering).
    auto make_group_key = [&](auto&& self, const MemberDef& m) -> std::string {
        std::string key = "(" + std::to_string(field_indexes.at(m.field_name)) + ", " +
                          (m.required ? "True" : "False") + ", (";
        for (std::size_t i = 0; i < m.members.size(); ++i) {
            const auto& ch = m.members[i];
            if (i != 0) key += ", ";
            if (ch.kind == "field") {
                key += "('f', " + std::to_string(field_indexes.at(ch.field_name)) + ", " +
                       (ch.required ? "True" : "False") + ")";
            } else {
                key += "('g', " + std::to_string(field_indexes.at(ch.field_name)) + ", " +
                       (ch.required ? "True" : "False") + ", " + self(self, ch) + ")";
            }
        }
        if (m.members.size() == 1) key += ",";
        key += "))";
        return key;
    };

    std::map<std::string, const MemberDef*> group_by_key;
    auto collect_groups = [&](auto&& self, const std::vector<MemberDef>& ms) -> void {
        for (const auto& m : ms) {
            if (m.kind != "group") continue;
            const std::string k = make_group_key(make_group_key, m);
            group_by_key.emplace(k, &m);
            self(self, m.members);
        }
    };
    for (const auto& [_k, ms] : schema.expanded_messages) collect_groups(collect_groups, ms);
    for (const auto& [_k, ms] : schema.expanded_components) collect_groups(collect_groups, ms);

    std::vector<std::string> group_keys;
    group_keys.reserve(group_by_key.size());
    std::map<std::string, std::size_t> group_indexes;
    std::map<std::string, std::vector<std::size_t>> group_names_by_field;
    for (const auto& [k, gptr] : group_by_key) {
        const std::size_t idx = group_keys.size();
        group_keys.push_back(k);
        group_indexes.emplace(k, idx);
        group_names_by_field[gptr->field_name].push_back(idx);
    }

    auto group_symbol = [&](std::size_t idx, std::string_view suffix) -> std::string {
        const MemberDef* g = group_by_key.at(group_keys[idx]);
        std::string base = g->field_name +
            (group_names_by_field.at(g->field_name).size() > 1 ? ("Group" + std::to_string(idx)) : "Group");
        base.append(suffix);
        return base;
    };

    auto group_depth = [&](auto&& self, std::size_t idx, std::vector<std::size_t>& active) -> std::size_t {
        if (std::find(active.begin(), active.end(), idx) != active.end()) {
            throw SchemaError("recursive repeating group nesting");
        }
        active.push_back(idx);
        std::size_t max_child = 0;
        for (const auto& m : group_by_key.at(group_keys[idx])->members) {
            if (m.kind == "group") {
                const std::size_t cidx = group_indexes.at(make_group_key(make_group_key, m));
                max_child = std::max(max_child, self(self, cidx, active));
            }
        }
        active.pop_back();
        return 1 + max_child;
    };

    std::vector<std::size_t> group_emit_order(group_keys.size());
    std::vector<std::size_t> group_depths(group_keys.size());
    for (std::size_t i = 0; i < group_keys.size(); ++i) {
        group_emit_order[i] = i;
        std::vector<std::size_t> act;
        group_depths[i] = group_depth(group_depth, i, act);
    }
    std::sort(group_emit_order.begin(), group_emit_order.end(), [&](std::size_t a, std::size_t b) {
        return std::tie(group_depths[a], a) < std::tie(group_depths[b], b);
    });

    // Exact canonical SHA-256 fingerprint matching runtime XML schema loader (nimblefix-schema-v3\0codegen-3\0).
    Sha256 sha;
    static constexpr char kFpPrefix[] = "nimblefix-schema-v3\0codegen-3\0";
    sha.update(std::string_view{kFpPrefix, sizeof(kFpPrefix) - 1});
    auto add_text = [&](std::string_view v) {
        sha.update(std::to_string(v.size()));
        sha.update_byte(':');
        sha.update(v);
    };
    auto add_number = [&](std::uint64_t v) {
        sha.update(std::to_string(v));
        sha.update_byte(';');
    };
    auto add_members_fp = [&](auto&& self, const std::vector<MemberDef>& ms) -> void {
        add_number(ms.size());
        for (const auto& m : ms) {
            sha.update_byte(m.kind == "group" ? 'g' : 'f');
            add_number(schema.fields_by_name.at(m.field_name).number);
            sha.update_byte(m.required ? '1' : '0');
            if (m.kind == "group") self(self, m.members);
        }
    };
    add_number(schema.identity.size());
    for (const auto& [k, v] : schema.identity) { add_text(k); add_text(v); }
    add_number(fields.size());
    for (const FieldDef* f : fields) {
        add_number(f->number);
        add_text(f->name);
        add_text(f->field_type);
        add_number(f->enum_values.size());
        for (const auto& [ev, desc] : f->enum_values) { add_text(ev); add_text(desc); }
    }
    add_number(schema.expanded_components.size());
    for (const auto& [cname, ms] : schema.expanded_components) {
        add_text(cname);
        add_members_fp(add_members_fp, ms);
    }
    add_number(messages.size());
    for (const MessageDef* m : messages) {
        add_text(m->name);
        add_text(m->msg_type);
        add_text(m->category);
        add_members_fp(add_members_fp, schema.expanded_messages.at(m->name));
    }
    const std::string fingerprint = sha.hex_digest();

    std::string out;
    out.reserve(fields.size() * 2048 + messages.size() * 4096);
    auto emit = [&](std::string_view line) {
        out.append(line);
        out.push_back('\n');
    };

    emit("// Generated by nimblefix-schema-gen. Do not edit.");
    emit("// FIX schema version " + version + "; normalized fingerprint " + fingerprint + ".");
    emit("#pragma once");
    emit("");
    emit("#include <array>");
    emit("#include <cstddef>");
    emit("#include <cstdint>");
    emit("#include <span>");
    emit("#include <string_view>");
    emit("#include <type_traits>");
    emit("#include <utility>");
    emit("#include <nimblefix/message.hpp>");
    emit("#include <nimblefix/group.hpp>");
    emit("#include <nimblefix/schema.hpp>");
    emit("#include <nimblefix/stream_encoder.hpp>");
    emit("");
    emit("namespace " + ns + " {");
    emit("");
    emit("using FieldDescriptor = ::nimblefix::FieldDescriptor;");
    emit("using EnumValueDescriptor = ::nimblefix::EnumValueDescriptor;");
    emit("using GroupDescriptor = ::nimblefix::GroupDescriptor;");
    emit("using ComponentDescriptor = ::nimblefix::ComponentDescriptor;");
    emit("using MessageDescriptor = ::nimblefix::MessageDescriptor;");
    emit("using MessageMemberDescriptor = ::nimblefix::MessageMemberDescriptor;");
    emit("");
    emit("inline constexpr std::array<EnumValueDescriptor, " + std::to_string(enum_values.size()) + "> enum_values{{");
    for (const auto& e : enum_values) {
        emit("    {" + std::to_string(e.tag) + "u, " + cpp_string(e.value) + ", " + cpp_string(e.desc) + "},");
    }
    emit("}};");
    emit("");
    emit("inline constexpr std::array<FieldDescriptor, " + std::to_string(fields.size()) + "> fields{{");
    for (const FieldDef* f : fields) {
        const auto [start, cnt] = enum_ranges.at(f->number);
        const std::string enum_span = cnt != 0
            ? ("std::span<const EnumValueDescriptor>{enum_values.data() + " + std::to_string(start) + ", " + std::to_string(cnt) + "}")
            : "std::span<const EnumValueDescriptor>{}";
        std::uint32_t length_tag = 0;
        std::uint32_t data_tag = (f->field_type == "DATA" || f->field_type == "XMLDATA") ? f->number : 0U;
        if (f->field_type == "DATA" || f->field_type == "XMLDATA") {
            length_tag = field_pair_tags.at(f->number);
        } else if (field_pair_tags.contains(f->number) && (f->field_type == "LENGTH" || f->field_type == "NUMINGROUP")) {
            data_tag = field_pair_tags.at(f->number);
        }
        emit("    {" + std::to_string(f->number) + "u, " + cpp_string(f->name) + ", " + cpp_string(f->field_type) +
             ", ::nimblefix::FieldClass::" + fix_field_classes().at(f->field_type) +
             ", ::nimblefix::WireType::" + fix_wire_types().at(f->field_type) +
             ", " + enum_span + ", " + std::to_string(length_tag) + "u, " + std::to_string(data_tag) + "u},");
    }
    emit("}};");
    emit("");
    emit("namespace field {");
    for (const FieldDef* f : fields) {
        const std::string& wt = fix_wire_types().at(f->field_type);
        std::string_view kind = "string";
        if (wt == "character") kind = "character";
        else if (wt == "decimal") kind = "decimal";
        else if (f->field_type == "INT") kind = "signed_integer";
        else if (wt == "integer") kind = "unsigned_integer";
        const std::string ident = cpp_keywords().contains(f->name) ? (f->name + "_") : f->name;
        emit("using " + ident + " = ::nimblefix::Field<" + std::to_string(f->number) + "u, ::nimblefix::FieldKind::" + std::string{kind} + ">;");
    }
    emit("} // namespace field");
    emit("");

    auto emit_members = [&](const std::string& arr_name, const std::vector<MemberDef>& ms, bool emit_req) -> std::pair<bool, std::uint64_t> {
        emit("inline constexpr std::array<MessageMemberDescriptor, " + std::to_string(ms.size()) + "> " + arr_name + "{{");
        std::vector<std::pair<std::uint32_t, std::size_t>> by_tag;
        std::vector<std::size_t> req_indices;
        std::vector<std::uint32_t> gtags;
        std::uint64_t bloom = 0;
        for (std::size_t idx = 0; idx < ms.size(); ++idx) {
            const auto& m = ms[idx];
            const std::uint32_t tnum = schema.fields_by_name.at(m.field_name).number;
            by_tag.emplace_back(tnum, idx);
            if (m.required) req_indices.push_back(idx);
            if (m.kind == "field") {
                emit("    {::nimblefix::MemberKind::field, " + std::to_string(field_indexes.at(m.field_name)) +
                     "u, 0u, " + (m.required ? "true" : "false") + "},");
            } else {
                const std::size_t gidx = group_indexes.at(make_group_key(make_group_key, m));
                gtags.push_back(tnum);
                bloom |= (std::uint64_t{1} << (tnum & 63U));
                emit("    {::nimblefix::MemberKind::group, 0u, " + std::to_string(gidx) +
                     "u, " + (m.required ? "true" : "false") + "},");
            }
        }
        emit("}};");
        emit("");
        std::sort(by_tag.begin(), by_tag.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        emit("inline constexpr std::array<::nimblefix::MemberTagEntry, " + std::to_string(by_tag.size()) + "> " + arr_name + "_by_tag{{");
        for (const auto& [tnum, idx] : by_tag) {
            emit("    {" + std::to_string(tnum) + "u, " + std::to_string(idx) + "u},");
        }
        emit("}};");
        emit("");
        if (emit_req) {
            emit("inline constexpr std::array<std::uint32_t, " + std::to_string(req_indices.size()) + "> " + arr_name + "_required{{");
            for (std::size_t idx : req_indices) emit("    " + std::to_string(idx) + "u,");
            emit("}};");
            emit("");
        }
        if (!gtags.empty()) {
            emit("inline constexpr std::array<std::uint32_t, " + std::to_string(gtags.size()) + "> " + arr_name + "_group_tags{{");
            for (std::uint32_t tnum : gtags) emit("    " + std::to_string(tnum) + "u,");
            emit("}};");
            emit("");
        }
        return {!gtags.empty(), bloom};
    };

    std::vector<std::pair<bool, std::uint64_t>> group_meta, comp_meta, msg_meta;
    for (std::size_t i = 0; i < group_keys.size(); ++i) {
        group_meta.push_back(emit_members("group_" + std::to_string(i) + "_members", group_by_key.at(group_keys[i])->members, true));
    }
    for (std::size_t i = 0; i < components.size(); ++i) {
        comp_meta.push_back(emit_members("component_" + std::to_string(i) + "_members", schema.expanded_components.at(components[i]->name), false));
    }
    for (std::size_t i = 0; i < messages.size(); ++i) {
        msg_meta.push_back(emit_members("message_" + std::to_string(i) + "_members", schema.expanded_messages.at(messages[i]->name), true));
    }

    emit("inline constexpr std::array<GroupDescriptor, " + std::to_string(group_keys.size()) + "> groups{{");
    for (std::size_t i = 0; i < group_keys.size(); ++i) {
        const MemberDef* g = group_by_key.at(group_keys[i]);
        const std::uint32_t counter = schema.fields_by_name.at(g->field_name).number;
        const std::uint32_t delim = schema.fields_by_name.at(g->members[0].field_name).number;
        const auto [has_gt, bloom] = group_meta[i];
        const std::string gt_expr = has_gt
            ? ("std::span<const std::uint32_t>{group_" + std::to_string(i) + "_members_group_tags}")
            : "std::span<const std::uint32_t>{}";
        emit("    {" + cpp_string(g->field_name) + ", " + std::to_string(counter) + "u, " + std::to_string(delim) + "u, "
             "std::span<const MessageMemberDescriptor>{group_" + std::to_string(i) + "_members}, "
             "std::span<const ::nimblefix::MemberTagEntry>{group_" + std::to_string(i) + "_members_by_tag}, "
             "std::span<const std::uint32_t>{group_" + std::to_string(i) + "_members_required}, " +
             gt_expr + ", " + hex_u64(bloom) + ", true},");
    }
    emit("}};");
    emit("");
    auto session_req_mask_for = [&](const std::vector<MemberDef>& ms) -> std::uint16_t {
        std::uint16_t mask = 0;
        for (const auto& m : ms) {
            if (!m.required || m.kind != "field") continue;
            switch (schema.fields_by_name.at(m.field_name).number) {
            case tag::begin_string:               mask |= (1u << 0); break;
            case tag::body_length:                mask |= (1u << 1); break;
            case tag::msg_type:                   mask |= (1u << 2); break;
            case tag::sender_comp_id:             mask |= (1u << 3); break;
            case tag::target_comp_id:             mask |= (1u << 4); break;
            case tag::msg_seq_num:                mask |= (1u << 5); break;
            case tag::sending_time:               mask |= (1u << 6); break;
            case tag::poss_dup_flag:              mask |= (1u << 7); break;
            case tag::orig_sending_time:          mask |= (1u << 8); break;
            case tag::last_msg_seq_num_processed: mask |= (1u << 9); break;
            case tag::appl_ver_id:                mask |= (1u << 10); break;
            case tag::check_sum:                  mask |= (1u << 11); break;
            default: break;
            }
        }
        return mask;
    };
    emit("inline constexpr std::array<ComponentDescriptor, " + std::to_string(components.size()) + "> components{{");
    for (std::size_t i = 0; i < components.size(); ++i) {
        const auto [has_gt, bloom] = comp_meta[i];
        const std::string gt_expr = has_gt
            ? ("std::span<const std::uint32_t>{component_" + std::to_string(i) + "_members_group_tags}")
            : "std::span<const std::uint32_t>{}";
        const std::uint16_t req_smask = session_req_mask_for(schema.expanded_components.at(components[i]->name));
        emit("    {" + cpp_string(components[i]->name) +
             ", std::span<const MessageMemberDescriptor>{component_" + std::to_string(i) + "_members}, "
             "std::span<const ::nimblefix::MemberTagEntry>{component_" + std::to_string(i) + "_members_by_tag}, " +
             gt_expr + ", " + hex_u64(bloom) + ", true, " + std::to_string(req_smask) + "u},");
    }
    emit("}};");
    emit("");
    emit("inline constexpr std::array<MessageDescriptor, " + std::to_string(messages.size()) + "> messages{{");
    for (std::size_t i = 0; i < messages.size(); ++i) {
        const auto [has_gt, bloom] = msg_meta[i];
        const std::string gt_expr = has_gt
            ? ("std::span<const std::uint32_t>{message_" + std::to_string(i) + "_members_group_tags}")
            : "std::span<const std::uint32_t>{}";
        emit("    {" + cpp_string(messages[i]->name) + ", " + cpp_string(messages[i]->msg_type) + ", " +
             cpp_string(messages[i]->category) +
             ", std::span<const MessageMemberDescriptor>{message_" + std::to_string(i) + "_members}, "
             "std::span<const ::nimblefix::MemberTagEntry>{message_" + std::to_string(i) + "_members_by_tag}, "
             "std::span<const std::uint32_t>{message_" + std::to_string(i) + "_members_required}, " +
             gt_expr + ", " + hex_u64(bloom) + ", true},");
    }
    emit("}};");
    emit("");

    int header_comp_idx = -1, trailer_comp_idx = -1;
    for (int i = 0; i < static_cast<int>(components.size()); ++i) {
        if (components[static_cast<std::size_t>(i)]->name == "Header") header_comp_idx = i;
        if (components[static_cast<std::size_t>(i)]->name == "Trailer") trailer_comp_idx = i;
    }
    std::array<int, 128> single_char_msg;
    single_char_msg.fill(-1);
    for (std::size_t i = 0; i < messages.size(); ++i) {
        if (messages[i]->msg_type.size() == 1) {
            const auto code = static_cast<unsigned char>(messages[i]->msg_type[0]);
            if (code < 128 && single_char_msg[code] == -1) single_char_msg[code] = static_cast<int>(i);
        }
    }
    std::vector<std::pair<std::uint32_t, std::uint32_t>> schema_pairs;
    bool builtin_only_pairs = true;
    for (const FieldDef* f : fields) {
        if (f->field_type == "DATA" || f->field_type == "XMLDATA") {
            const std::pair<std::uint32_t, std::uint32_t> pr{field_pair_tags.at(f->number), f->number};
            schema_pairs.push_back(pr);
            if (!standard_length_data_pairs().contains(pr)) builtin_only_pairs = false;
        }
    }

    emit("inline constexpr std::array<std::int16_t, 128> single_char_message_index{{");
    {
        std::string line = "    ";
        for (std::size_t i = 0; i < 128; ++i) {
            if (i != 0) line += ", ";
            line += std::to_string(single_char_msg[i]);
        }
        emit(line);
    }
    emit("}};");
    emit("");
    emit("namespace detail_validate {");
    emit("[[nodiscard]] inline ::nimblefix::Status fast_validate_message(");
    emit("    std::string_view msg_type, std::size_t msg_type_offset,");
    emit("    std::span<const ::nimblefix::FieldView> fields,");
    emit("    ::nimblefix::ValidationLevel level,");
    emit("    ::nimblefix::detail::ParsedFieldIndex* out_index) noexcept;");
    emit("} // namespace detail_validate");
    emit("");
    emit("inline constexpr ::nimblefix::SchemaDescriptor schema_descriptor{");
    emit("    " + cpp_string(schema_type) + ", " + cpp_string(version) + ", " + cpp_string(fingerprint) + ",");
    emit("    std::span<const FieldDescriptor>{fields}, std::span<const GroupDescriptor>{groups},");
    emit("    std::span<const ComponentDescriptor>{components}, std::span<const MessageDescriptor>{messages},");
    emit("    " + std::to_string(header_comp_idx) + ", " + std::to_string(trailer_comp_idx) +
         ", std::span<const std::int16_t>{single_char_message_index},");
    emit("    &detail_validate::fast_validate_message");
    emit("};");
    emit("");
    emit("inline constexpr std::array<::nimblefix::LengthDataPair, " + std::to_string(schema_pairs.size()) + "> schema_length_data_pairs{{");
    for (const auto& [lt, dt] : schema_pairs) {
        emit("    {" + std::to_string(lt) + "u, " + std::to_string(dt) + "u},");
    }
    emit("}};");
    emit("");
    emit("struct Schema {");
    emit("    static constexpr std::string_view type = " + cpp_string(schema_type) + ";");
    emit("    static constexpr std::string_view version = " + cpp_string(version) + ";");
    emit("    static constexpr std::string_view fingerprint = " + cpp_string(fingerprint) + ";");
    emit("    static constexpr std::size_t field_count = " + std::to_string(fields.size()) + ";");
    emit("    static constexpr std::size_t enum_value_count = " + std::to_string(enum_values.size()) + ";");
    emit("    static constexpr std::size_t message_count = " + std::to_string(messages.size()) + ";");
    emit("    static constexpr std::size_t component_count = " + std::to_string(components.size()) + ";");
    emit("    static constexpr std::size_t group_count = " + std::to_string(group_keys.size()) + ";");
    emit("    inline static constexpr const auto& fields = ::" + ns + "::fields;");
    emit("    inline static constexpr const auto& enum_values = ::" + ns + "::enum_values;");
    emit("    [[nodiscard]] static constexpr const ::nimblefix::SchemaDescriptor& descriptor() noexcept { return schema_descriptor; }");
    emit("    [[nodiscard]] static constexpr const ::nimblefix::MessageDescriptor* find_message(std::string_view value) noexcept { return descriptor().find_message(value); }");
    emit("    [[nodiscard]] static constexpr const ::nimblefix::FieldDescriptor* find_field(std::uint32_t tag) noexcept { return descriptor().find_field(tag); }");
    emit(std::string{"    [[nodiscard]] static constexpr ::nimblefix::WireHints wire_hints() noexcept { return {std::span<const ::nimblefix::LengthDataPair>{schema_length_data_pairs}, true, "} +
         (builtin_only_pairs ? "true" : "false") + "}; }");
    emit("    template<class Visitor> static void for_each_message(Visitor&& visitor);");
    emit("};");
    emit("");
    emit("} // namespace " + ns);
    emit("");
    emit("namespace nimblefix {");
    for (const FieldDef* f : fields) {
        const std::string& fc = fix_field_classes().at(f->field_type);
        emit("template<>");
        emit("struct SchemaField<" + ns + "::Schema, " + std::to_string(f->number) + "u> : std::integral_constant<FieldClass, FieldClass::" + fc + "> {");
        emit("    static constexpr FieldClass field_class = FieldClass::" + fc + ";");
        emit("};");
    }
    emit("} // namespace nimblefix");
    emit("");
    emit("namespace " + ns + " {");
    emit("");

    // detail_stream
    emit("namespace detail_stream {");
    for (const FieldDef* f : fields) {
        if (!field_needs_policy_check(*f)) continue;
        const std::string& fc = fix_field_classes().at(f->field_type);
        const std::string& wt = fix_wire_types().at(f->field_type);
        if (wt == "character") {
            if (fc == "boolean") {
                emit("[[nodiscard]] inline bool valid_" + std::to_string(f->number) + "(char c) noexcept { return c == 'Y' || c == 'N'; }");
            } else if (!f->enum_values.empty()) {
                std::string cases;
                for (const auto& [e, _d] : f->enum_values) {
                    if (e.size() == 1) {
                        if (!cases.empty()) cases.push_back(' ');
                        cases += byte_case(static_cast<unsigned char>(e[0]));
                    }
                }
                emit("[[nodiscard]] inline bool valid_" + std::to_string(f->number) + "(char c) noexcept { switch (c) { " + cases + " return true; default: return false; } }");
            }
        } else if (wt == "integer") {
            std::vector<std::string> conds;
            if (fc == "unsigned_integer" || fc == "day_of_month" || fc == "tag_number") conds.push_back("v >= 0");
            if (fc == "day_of_month") conds.push_back("v >= 1 && v <= 31");
            else if (fc == "tag_number") conds.push_back("v >= 1 && v <= static_cast<std::int64_t>(::nimblefix::detail::maximum_fix_tag)");
            auto join_conds = [&]() {
                std::string s;
                for (std::size_t i = 0; i < conds.size(); ++i) {
                    if (i != 0) s += " && ";
                    s += conds[i];
                }
                return s;
            };
            if (!f->enum_values.empty()) {
                std::vector<std::string> ivals;
                for (const auto& [e, _d] : f->enum_values) {
                    std::string_view sv = e;
                    if (!sv.empty() && sv[0] == '-') sv.remove_prefix(1);
                    if (is_all_digits(sv)) ivals.push_back(e);
                }
                if (!ivals.empty()) {
                    std::string cases;
                    for (std::size_t i = 0; i < ivals.size(); ++i) {
                        if (i != 0) cases.push_back(' ');
                        cases += "case " + ivals[i] + "LL:";
                    }
                    const std::string pre = conds.empty() ? "" : ("if (!(" + join_conds() + ")) return false; ");
                    emit("[[nodiscard]] inline bool valid_" + std::to_string(f->number) + "(std::int64_t v) noexcept { " + pre + "switch (v) { " + cases + " return true; default: return false; } }");
                    continue;
                }
            }
            const std::string expr = conds.empty() ? "true" : join_conds();
            emit("[[nodiscard]] inline bool valid_" + std::to_string(f->number) + "(std::int64_t v) noexcept { (void)v; return " + expr + "; }");
        } else if (wt == "string") {
            if (fc == "utc_date" || fc == "local_market_date") {
                emit("[[nodiscard]] inline bool valid_" + std::to_string(f->number) + "(std::string_view v) noexcept { return ::nimblefix::detail::stream::is_valid_ymd8(v); }");
            } else if (fc == "utc_time") {
                emit("[[nodiscard]] inline bool valid_" + std::to_string(f->number) + "(std::string_view v) noexcept { return ::nimblefix::detail::stream::is_valid_utc_time(v); }");
            } else if (fc == "month_year") {
                emit("[[nodiscard]] inline bool valid_" + std::to_string(f->number) + "(std::string_view v) noexcept { return ::nimblefix::detail::stream::is_valid_month_year(v); }");
            } else if (fc == "multiple_value_string" || fc == "multiple_char_value" || fc == "multiple_string_value") {
                const std::string single_ch = (fc == "multiple_char_value") ? "true" : "false";
                if (!f->enum_values.empty()) {
                    bool all1 = true;
                    for (const auto& [e, _d] : f->enum_values) if (e.size() != 1) all1 = false;
                    if (all1) {
                        std::string cases;
                        for (const auto& [e, _d] : f->enum_values) {
                            if (!cases.empty()) cases.push_back(' ');
                            cases += byte_case(static_cast<unsigned char>(e[0]));
                        }
                        emit("[[nodiscard]] inline bool valid_" + std::to_string(f->number) + "(std::string_view v) noexcept {");
                        emit("    if (!::nimblefix::detail::stream::is_valid_mvs_syntax(v, " + single_ch + ")) return false;");
                        emit("    for (std::size_t pos = 0; pos < v.size();) {");
                        emit("        const auto sp = v.find(' ', pos);");
                        emit("        const auto end = sp == std::string_view::npos ? v.size() : sp;");
                        emit("        if (end - pos != 1) return false;");
                        emit("        switch (v[pos]) { " + cases + " break; default: return false; }");
                        emit("        if (sp == std::string_view::npos) break;");
                        emit("        pos = sp + 1;");
                        emit("    }");
                        emit("    return true;");
                        emit("}");
                    } else {
                        std::string cond;
                        for (std::size_t i = 0; i < f->enum_values.size(); ++i) {
                            if (i != 0) cond += " || ";
                            cond += "tok == " + cpp_string(f->enum_values[i].first);
                        }
                        emit("[[nodiscard]] inline bool valid_" + std::to_string(f->number) + "(std::string_view v) noexcept {");
                        emit("    if (!::nimblefix::detail::stream::is_valid_mvs_syntax(v, " + single_ch + ")) return false;");
                        emit("    for (std::size_t pos = 0; pos < v.size();) {");
                        emit("        const auto sp = v.find(' ', pos);");
                        emit("        const auto end = sp == std::string_view::npos ? v.size() : sp;");
                        emit("        const auto tok = v.substr(pos, end - pos);");
                        emit("        if (!(" + cond + ")) return false;");
                        emit("        if (sp == std::string_view::npos) break;");
                        emit("        pos = sp + 1;");
                        emit("    }");
                        emit("    return true;");
                        emit("}");
                    }
                } else {
                    emit("[[nodiscard]] inline bool valid_" + std::to_string(f->number) + "(std::string_view v) noexcept { return ::nimblefix::detail::stream::is_valid_mvs_syntax(v, " + single_ch + "); }");
                }
            } else if (!f->enum_values.empty()) {
                std::string cond;
                for (std::size_t i = 0; i < f->enum_values.size(); ++i) {
                    if (i != 0) cond += " || ";
                    cond += "v == " + cpp_string(f->enum_values[i].first);
                }
                emit("[[nodiscard]] inline bool valid_" + std::to_string(f->number) + "(std::string_view v) noexcept { return " + cond + "; }");
            }
        }
    }
    emit("} // namespace detail_stream");
    emit("");

    // detail_validate
    auto scalar_check_expr = [&](const FieldDef& f) -> std::string {
        if (!f.enum_values.empty()) return "check_field_" + std::to_string(f.number) + "(f, level)";
        return base_field_check(fix_field_classes().at(f.field_type));
    };

    auto level_len_data_pairs = [&](const std::vector<MemberDef>& ms) {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> res;
        std::set<std::pair<std::uint32_t, std::uint32_t>> seen_p;
        for (const auto& m : ms) {
            if (m.kind != "field") continue;
            const auto& f = schema.fields_by_name.at(m.field_name);
            if (f.field_type == "DATA" || f.field_type == "XMLDATA") {
                if (const auto it = field_pair_tags.find(f.number); it != field_pair_tags.end() && it->second != 0) {
                    const std::pair<std::uint32_t, std::uint32_t> pr{it->second, f.number};
                    if (seen_p.insert(pr).second) res.push_back(pr);
                }
            } else if ((f.field_type == "LENGTH" || f.field_type == "NUMINGROUP") && field_pair_tags.contains(f.number)) {
                const std::uint32_t dt = field_pair_tags.at(f.number);
                if (dt != 0) {
                    const std::pair<std::uint32_t, std::uint32_t> pr{f.number, dt};
                    if (seen_p.insert(pr).second) res.push_back(pr);
                }
            }
        }
        return res;
    };

    emit("namespace detail_validate {");
    for (const FieldDef* f : fields) {
        if (f->enum_values.empty()) continue;
        const std::string& fc = fix_field_classes().at(f->field_type);
        std::vector<unsigned char> e1;
        std::vector<unsigned> e2;
        std::vector<std::string> e_other;
        for (const auto& [e, _d] : f->enum_values) {
            if (e.size() == 1) e1.push_back(static_cast<unsigned char>(e[0]));
            else if (e.size() == 2) e2.push_back((static_cast<unsigned>(static_cast<unsigned char>(e[0])) << 8U) | static_cast<unsigned char>(e[1]));
            else e_other.push_back(e);
        }
        if (!e1.empty() && e2.empty() && e_other.empty()) {
            std::string cases;
            for (std::size_t i = 0; i < e1.size(); ++i) {
                if (i != 0) cases.push_back(' ');
                cases += byte_case(e1[i]);
            }
            emit("[[nodiscard]] inline bool enum_ok_" + std::to_string(f->number) + "(std::string_view v) noexcept { if (v.size() != 1) return false; switch (v[0]) { " + cases + " return true; default: return false; } }");
        } else {
            emit("[[nodiscard]] inline bool enum_ok_" + std::to_string(f->number) + "(std::string_view v) noexcept {");
            emit("    switch (v.size()) {");
            if (!e1.empty()) {
                std::string cases;
                for (std::size_t i = 0; i < e1.size(); ++i) {
                    if (i != 0) cases.push_back(' ');
                    cases += byte_case(e1[i]);
                }
                emit("    case 1: switch (v[0]) { " + cases + " return true; default: return false; }");
            }
            if (!e2.empty()) {
                std::string cases2;
                for (std::size_t i = 0; i < e2.size(); ++i) {
                    if (i != 0) cases2.push_back(' ');
                    cases2 += "case " + hex_u16(e2[i]) + ":";
                }
                emit("    case 2: switch ((static_cast<unsigned>(static_cast<unsigned char>(v[0])) << 8U) | static_cast<unsigned char>(v[1])) { " + cases2 + " return true; default: return false; }");
            }
            if (!e_other.empty()) {
                std::string cond;
                for (std::size_t i = 0; i < e_other.size(); ++i) {
                    if (i != 0) cond += " || ";
                    cond += "v == " + cpp_string(e_other[i]);
                }
                emit("    default: return " + cond + ";");
            } else {
                emit("    default: return false;");
            }
            emit("    }");
            emit("}");
        }
        std::string enum_call;
        if (fc == "multiple_value_string" || fc == "multiple_char_value" || fc == "multiple_string_value") {
            emit("[[nodiscard]] inline bool enum_mvs_ok_" + std::to_string(f->number) + "(std::string_view v) noexcept {");
            emit("    for (std::size_t first = 0; first < v.size();) {");
            emit("        const auto sep = v.find(' ', first);");
            emit("        const auto last = sep == std::string_view::npos ? v.size() : sep;");
            emit("        if (!enum_ok_" + std::to_string(f->number) + "(v.substr(first, last - first))) return false;");
            emit("        first = sep == std::string_view::npos ? v.size() : sep + 1;");
            emit("    }");
            emit("    return true;");
            emit("}");
            enum_call = "enum_mvs_ok_" + std::to_string(f->number) + "(f.value)";
        } else {
            enum_call = "enum_ok_" + std::to_string(f->number) + "(f.value)";
        }
        emit("[[nodiscard]] inline ::nimblefix::Status check_field_" + std::to_string(f->number) +
             "(const ::nimblefix::FieldView& f, ::nimblefix::ValidationLevel level) noexcept { "
             "if (const auto st = " + base_field_check(fc) + "; !st.ok()) return st; "
             "if (level == ::nimblefix::ValidationLevel::strict && !" + enum_call +
             ") return {::nimblefix::ErrorCode::invalid_value, static_cast<int>(f.tag), f.offset}; "
             "return ::nimblefix::Status::success(); }");
    }

    auto emit_tag_pred = [&](const std::string& fn_name, const std::set<std::uint32_t>& tags) {
        if (tags.empty()) {
            emit("[[nodiscard]] inline bool " + fn_name + "(std::uint32_t) noexcept { return false; }");
            return;
        }
        std::string cases;
        for (std::uint32_t t : tags) {
            if (!cases.empty()) cases.push_back(' ');
            cases += "case " + std::to_string(t) + "u:";
        }
        emit("[[nodiscard]] inline bool " + fn_name + "(std::uint32_t tag) noexcept { switch (tag) { " + cases + " return true; default: return false; } }");
    };

    const std::vector<MemberDef> empty_ms;
    const auto& header_members = schema.expanded_components.contains("Header") ? schema.expanded_components.at("Header") : empty_ms;
    const auto& trailer_members = schema.expanded_components.contains("Trailer") ? schema.expanded_components.at("Trailer") : empty_ms;
    std::set<std::uint32_t> header_scalar_tags, header_all_tags, trailer_scalar_tags, trailer_all_tags;
    std::vector<std::tuple<std::uint32_t, std::size_t, bool>> header_groups, trailer_groups;
    std::vector<const FieldDef*> env_scalars;
    std::vector<bool> env_scalar_required;
    std::set<std::uint32_t> env_scalar_seen;
    std::vector<MemberDef> env_all_members;
    for (const auto& m : header_members) {
        const auto& fd = schema.fields_by_name.at(m.field_name);
        const std::uint32_t t = fd.number;
        header_all_tags.insert(t);
        env_all_members.push_back(m);
        if (m.kind == "field") {
            header_scalar_tags.insert(t);
            if (!session_or_frame_tags().contains(t) && env_scalar_seen.insert(t).second) {
                env_scalars.push_back(&fd);
                env_scalar_required.push_back(m.required);
            }
        } else {
            header_groups.emplace_back(t, group_indexes.at(make_group_key(make_group_key, m)), m.required);
        }
    }
    for (const auto& m : trailer_members) {
        const auto& fd = schema.fields_by_name.at(m.field_name);
        const std::uint32_t t = fd.number;
        trailer_all_tags.insert(t);
        env_all_members.push_back(m);
        if (m.kind == "field") {
            trailer_scalar_tags.insert(t);
            if (!session_or_frame_tags().contains(t) && env_scalar_seen.insert(t).second) {
                env_scalars.push_back(&fd);
                env_scalar_required.push_back(m.required);
            }
        } else {
            trailer_groups.emplace_back(t, group_indexes.at(make_group_key(make_group_key, m)), m.required);
        }
    }
    emit_tag_pred("is_header_scalar_tag", header_scalar_tags);
    emit_tag_pred("is_header_tag", header_all_tags);
    emit_tag_pred("is_trailer_scalar_tag", trailer_scalar_tags);
    emit_tag_pred("is_trailer_tag", trailer_all_tags);

    if (env_scalars.empty()) {
        emit("[[nodiscard]] inline int envelope_scalar_index(std::uint32_t) noexcept { return -1; }");
        emit("[[nodiscard]] inline ::nimblefix::Status check_envelope_scalar(const ::nimblefix::FieldView&, ::nimblefix::ValidationLevel) noexcept { return ::nimblefix::Status::success(); }");
    } else {
        emit("[[nodiscard]] inline int envelope_scalar_index(std::uint32_t tag) noexcept {");
        emit("    switch (tag) {");
        for (std::size_t ei = 0; ei < env_scalars.size(); ++ei) {
            emit("    case " + std::to_string(env_scalars[ei]->number) + "u: return " + std::to_string(ei) + ";");
        }
        emit("    default: return -1;");
        emit("    }");
        emit("}");
        emit("[[gnu::noinline, gnu::cold]] [[nodiscard]] inline ::nimblefix::Status check_envelope_scalar(const ::nimblefix::FieldView& f, ::nimblefix::ValidationLevel level) noexcept {");
        emit("    (void)level;");
        emit("    switch (f.tag) {");
        for (const FieldDef* ef : env_scalars) {
            emit("    case " + std::to_string(ef->number) + "u: return " + scalar_check_expr(*ef) + ";");
        }
        emit("    default: return ::nimblefix::Status::success();");
        emit("    }");
        emit("}");
    }
    const auto env_ld_pairs = level_len_data_pairs(env_all_members);
    std::set<std::uint32_t> env_ld_tags;
    for (const auto& [lt, dt] : env_ld_pairs) {
        env_ld_tags.insert(lt);
        env_ld_tags.insert(dt);
    }
    if (!env_ld_pairs.empty()) {
        emit("[[gnu::noinline, gnu::cold]] [[nodiscard]] inline ::nimblefix::Status check_envelope_len_data(std::span<const ::nimblefix::FieldView> fields) noexcept {");
        for (const auto& [lt, dt] : env_ld_pairs) {
            emit("    int ld_len_" + std::to_string(lt) + " = -1, ld_data_" + std::to_string(dt) + " = -1;");
        }
        emit("    for (std::size_t i = 0; i < fields.size(); ++i) {");
        emit("        switch (fields[i].tag) {");
        for (const auto& [lt, dt] : env_ld_pairs) {
            emit("        case " + std::to_string(lt) + "u: if (ld_len_" + std::to_string(lt) + " < 0) ld_len_" + std::to_string(lt) + " = static_cast<int>(i); break;");
            emit("        case " + std::to_string(dt) + "u: if (ld_data_" + std::to_string(dt) + " < 0) ld_data_" + std::to_string(dt) + " = static_cast<int>(i); break;");
        }
        emit("        default: break;");
        emit("        }");
        emit("    }");
        for (const auto& [lt, dt] : env_ld_pairs) {
            emit("    if (const auto st = ::nimblefix::detail::check_len_data_pair(fields, " + std::to_string(lt) + "u, ld_len_" + std::to_string(lt) + ", ld_data_" + std::to_string(dt) + "); !st.ok()) return st;");
        }
        emit("    return ::nimblefix::Status::success();");
        emit("}");
    }

    // Leaf-first group validators
    for (std::size_t g_idx : group_emit_order) {
        const MemberDef* g = group_by_key.at(group_keys[g_idx]);
        const auto& gm = g->members;
        const std::uint32_t count_tag = schema.fields_by_name.at(g->field_name).number;
        const auto& delim_m = gm[0];
        const std::uint32_t delim_tag = schema.fields_by_name.at(delim_m.field_name).number;
        std::set<std::uint32_t> g_tags;
        for (const auto& m : gm) g_tags.insert(schema.fields_by_name.at(m.field_name).number);
        emit_tag_pred("group_contains_" + std::to_string(g_idx), g_tags);

        const std::size_t seen_words = std::max<std::size_t>(1, (gm.size() + 63) / 64);
        const auto ld_pairs = level_len_data_pairs(gm);
        std::set<std::uint32_t> ld_len_set, ld_data_set;
        for (const auto& [lt, dt] : ld_pairs) { ld_len_set.insert(lt); ld_data_set.insert(dt); }
        std::map<std::size_t, std::vector<std::pair<std::size_t, std::uint32_t>>> req_by_word;
        for (std::size_t mi = 0; mi < gm.size(); ++mi) {
            if (gm[mi].required) {
                req_by_word[mi / 64].emplace_back(mi % 64, schema.fields_by_name.at(gm[mi].field_name).number);
            }
        }

        emit("[[nodiscard]] inline ::nimblefix::Status validate_group_" + std::to_string(g_idx) + "(");
        emit("    std::span<const ::nimblefix::FieldView> fields,");
        emit("    std::size_t& cursor,");
        emit("    ::nimblefix::ValidationLevel level,");
        emit("    ::nimblefix::detail::FastAncestorStack& ancestors,");
        emit("    ::nimblefix::Status& first_content_err,");
        emit("    std::size_t depth) noexcept {");
        emit("    (void)level; (void)ancestors; (void)first_content_err;");
        emit("    if (depth > ::nimblefix::detail::max_schema_group_depth || cursor >= fields.size())");
        emit("        return {::nimblefix::ErrorCode::invalid_value, 0, cursor};");
        emit("    std::uint64_t count{};");
        emit("    if (!::nimblefix::detail::parse_unsigned(fields[cursor].value, count) || count > fields.size() - cursor - 1)");
        emit("        return {::nimblefix::ErrorCode::invalid_value, " + std::to_string(count_tag) + ", fields[cursor].offset};");
        emit("    ++cursor;");
        emit("    for (std::uint64_t entry_i = 0; entry_i < count; ++entry_i) {");
        emit("        if (cursor >= fields.size() || fields[cursor].tag != " + std::to_string(delim_tag) + "u)");
        emit("            return {::nimblefix::ErrorCode::invalid_value, " + std::to_string(delim_tag) + ", cursor < fields.size() ? fields[cursor].offset : 0};");
        if (seen_words == 1) emit("        std::uint64_t seen = 1ULL; (void)seen;");
        else emit("        std::uint64_t seen[" + std::to_string(seen_words) + "]{}; seen[0] = 1ULL; (void)seen;");
        emit("        ::nimblefix::Status entry_dup_err{};");
        emit("        ::nimblefix::Status entry_scalar_err{};");
        emit("        ::nimblefix::Status entry_unknown_err{};");
        if (!ld_pairs.empty()) {
            emit("        bool has_ld = false;");
            for (const auto& [lt, dt] : ld_pairs) {
                emit("        int ld_len_" + std::to_string(lt) + " = -1, ld_data_" + std::to_string(dt) + " = -1;");
            }
        }
        if (delim_m.kind == "field") {
            const auto& df = schema.fields_by_name.at(delim_m.field_name);
            emit("        if (level != ::nimblefix::ValidationLevel::structure) {");
            emit("            const auto& f = fields[cursor];");
            emit("            if (const auto st = " + scalar_check_expr(df) + "; !st.ok()) entry_scalar_err = st;");
            if (ld_len_set.contains(df.number)) emit("            ld_len_" + std::to_string(df.number) + " = static_cast<int>(cursor); has_ld = true;");
            if (ld_data_set.contains(df.number)) emit("            ld_data_" + std::to_string(df.number) + " = static_cast<int>(cursor); has_ld = true;");
            emit("        }");
            emit("        ++cursor;");
        } else {
            const std::size_t ng = group_indexes.at(make_group_key(make_group_key, delim_m));
            emit("        if (ancestors.count < ::nimblefix::detail::FastAncestorStack::max_depth) ancestors.predicates[ancestors.count++] = &group_contains_" + std::to_string(g_idx) + ";");
            emit("        const auto dgst = validate_group_" + std::to_string(ng) + "(fields, cursor, level, ancestors, entry_scalar_err, depth + 1);");
            emit("        if (ancestors.count > 0) --ancestors.count;");
            emit("        if (!dgst.ok()) return dgst;");
        }
        emit("        bool entry_done = false;");
        emit("        while (cursor < fields.size() && !entry_done) {");
        emit("            const auto& f = fields[cursor];");
        emit("            const auto tag = f.tag;");
        emit("            if (tag == " + std::to_string(delim_tag) + "u) break;");
        emit("            switch (tag) {");
        for (std::size_t mi = 1; mi < gm.size(); ++mi) {
            const auto& m = gm[mi];
            const auto& mf = schema.fields_by_name.at(m.field_name);
            const std::string seen_expr = (seen_words == 1) ? "seen" : ("seen[" + std::to_string(mi / 64) + "]");
            const std::size_t bit = mi % 64;
            emit("            case " + std::to_string(mf.number) + "u: {");
            if (m.kind == "field") {
                emit("                if (level != ::nimblefix::ValidationLevel::structure) {");
                emit("                    if ((" + seen_expr + " & (std::uint64_t{1} << " + std::to_string(bit) + "u)) != 0) { if (entry_dup_err.ok()) entry_dup_err = {::nimblefix::ErrorCode::duplicate_field, " + std::to_string(mf.number) + ", f.offset}; }");
                emit("                    else { " + seen_expr + " |= (std::uint64_t{1} << " + std::to_string(bit) + "u); if (entry_scalar_err.ok()) { if (const auto st = " + scalar_check_expr(mf) + "; !st.ok()) entry_scalar_err = st; } }");
                if (ld_len_set.contains(mf.number)) emit("                    if (ld_len_" + std::to_string(mf.number) + " < 0) { ld_len_" + std::to_string(mf.number) + " = static_cast<int>(cursor); has_ld = true; }");
                if (ld_data_set.contains(mf.number)) emit("                    if (ld_data_" + std::to_string(mf.number) + " < 0) { ld_data_" + std::to_string(mf.number) + " = static_cast<int>(cursor); has_ld = true; }");
                emit("                }");
                emit("                ++cursor;");
                emit("                break;");
            } else {
                const std::size_t ng = group_indexes.at(make_group_key(make_group_key, m));
                emit("                if (level != ::nimblefix::ValidationLevel::structure) {");
                emit("                    if ((" + seen_expr + " & (std::uint64_t{1} << " + std::to_string(bit) + "u)) != 0) { if (entry_dup_err.ok()) entry_dup_err = {::nimblefix::ErrorCode::duplicate_field, " + std::to_string(mf.number) + ", f.offset}; }");
                emit("                    else { " + seen_expr + " |= (std::uint64_t{1} << " + std::to_string(bit) + "u); }");
                emit("                }");
                emit("                if (ancestors.count < ::nimblefix::detail::FastAncestorStack::max_depth) ancestors.predicates[ancestors.count++] = &group_contains_" + std::to_string(g_idx) + ";");
                emit("                const auto gst = validate_group_" + std::to_string(ng) + "(fields, cursor, level, ancestors, entry_scalar_err, depth + 1);");
                emit("                if (ancestors.count > 0) --ancestors.count;");
                emit("                if (!gst.ok()) return gst;");
                emit("                break;");
            }
            emit("            }");
        }
        emit("            default:");
        emit("                if (tag == 10u || ancestors.contains(tag)) { entry_done = true; break; }");
        emit("                if (level == ::nimblefix::ValidationLevel::strict && entry_unknown_err.ok())");
        emit("                    entry_unknown_err = {::nimblefix::ErrorCode::invalid_value, static_cast<int>(tag), f.offset};");
        emit("                ++cursor;");
        emit("                break;");
        emit("            }");
        emit("        }");
        emit("        if (level != ::nimblefix::ValidationLevel::structure && first_content_err.ok()) {");
        emit("            if (!entry_dup_err.ok()) first_content_err = entry_dup_err;");
        emit("            else if (!entry_scalar_err.ok()) first_content_err = entry_scalar_err;");
        if (!ld_pairs.empty()) {
            emit("            else if (has_ld) {");
            for (const auto& [lt, dt] : ld_pairs) {
                emit("                if (first_content_err.ok()) { if (const auto ldst = ::nimblefix::detail::check_len_data_pair(fields, " + std::to_string(lt) + "u, ld_len_" + std::to_string(lt) + ", ld_data_" + std::to_string(dt) + "); !ldst.ok()) first_content_err = ldst; }");
            }
            emit("            }");
        }
        if (!req_by_word.empty()) {
            emit("            if (first_content_err.ok()) {");
            for (const auto& [w, bits] : req_by_word) {
                std::uint64_t mask = 0;
                for (const auto& [b, _rt] : bits) mask |= (std::uint64_t{1} << b);
                const std::string seen_expr = (seen_words == 1) ? "seen" : ("seen[" + std::to_string(w) + "]");
                emit("                if ((" + seen_expr + " & " + hex_u64(mask) + ") != " + hex_u64(mask) + ") {");
                for (std::size_t k = 0; k < bits.size(); ++k) {
                    const auto [b, rtag] = bits[k];
                    const std::string prefix = (k == 0) ? "                    if (" : "                    else if (";
                    emit(prefix + "(" + seen_expr + " & (std::uint64_t{1} << " + std::to_string(b) + "u)) == 0) first_content_err = {::nimblefix::ErrorCode::required_field_missing, " + std::to_string(rtag) + "};");
                }
                emit("                }");
            }
            emit("            }");
        }
        emit("            if (first_content_err.ok() && !entry_unknown_err.ok()) first_content_err = entry_unknown_err;");
        emit("        }");
        emit("    }");
        emit("    return ::nimblefix::Status::success();");
        emit("}");
    }

    // Per-message single-pass validators
    for (std::size_t msg_idx = 0; msg_idx < messages.size(); ++msg_idx) {
        const MessageDef* msg = messages[msg_idx];
        const auto& mm = schema.expanded_messages.at(msg->name);
        bool msg_has_groups = false;
        std::set<std::uint32_t> msg_tags;
        for (const auto& m : mm) {
            if (m.kind == "group") msg_has_groups = true;
            msg_tags.insert(schema.fields_by_name.at(m.field_name).number);
        }
        if (msg_has_groups) emit_tag_pred("msg_contains_" + std::to_string(msg_idx), msg_tags);

        const std::size_t seen_words = std::max<std::size_t>(1, (mm.size() + 63) / 64);
        const auto ld_pairs = level_len_data_pairs(mm);
        std::set<std::uint32_t> ld_len_set, ld_data_set;
        for (const auto& [lt, dt] : ld_pairs) { ld_len_set.insert(lt); ld_data_set.insert(dt); }
        std::map<std::size_t, std::vector<std::pair<std::size_t, std::uint32_t>>> req_by_word;
        for (std::size_t mi = 0; mi < mm.size(); ++mi) {
            if (mm[mi].required) req_by_word[mi / 64].emplace_back(mi % 64, schema.fields_by_name.at(mm[mi].field_name).number);
        }
        std::vector<std::tuple<std::uint32_t, std::size_t, bool>> act_hdr_g, act_trl_g;
        for (const auto& [t, gi, req] : header_groups) if (!msg_tags.contains(t) && !session_or_frame_tags().contains(t)) act_hdr_g.emplace_back(t, gi, req);
        for (const auto& [t, gi, req] : trailer_groups) if (!msg_tags.contains(t) && !session_or_frame_tags().contains(t)) act_trl_g.emplace_back(t, gi, req);
        const bool needs_ancestors = msg_has_groups || !act_hdr_g.empty() || !act_trl_g.empty();
        const std::size_t env_total_bits = env_scalars.size() + act_hdr_g.size() + act_trl_g.size();
        const std::size_t env_seen_words = env_total_bits == 0 ? 0 : (env_total_bits + 63) / 64;
        std::map<std::size_t, std::vector<std::pair<std::size_t, std::uint32_t>>> env_req_by_word;
        for (std::size_t ei = 0; ei < env_scalars.size(); ++ei) {
            if (env_scalar_required[ei] && !msg_tags.contains(env_scalars[ei]->number)) {
                env_req_by_word[ei / 64].emplace_back(ei % 64, env_scalars[ei]->number);
            }
        }
        {
            std::size_t gbit = env_scalars.size();
            for (const auto& [ht, _gi, req] : act_hdr_g) {
                if (req) env_req_by_word[gbit / 64].emplace_back(gbit % 64, ht);
                ++gbit;
            }
            for (const auto& [tt, _gi, req] : act_trl_g) {
                if (req) env_req_by_word[gbit / 64].emplace_back(gbit % 64, tt);
                ++gbit;
            }
        }

        emit("[[nodiscard]] inline ::nimblefix::Status validate_msg_" + std::to_string(msg_idx) + "(");
        emit("    std::span<const ::nimblefix::FieldView> fields,");
        emit("    ::nimblefix::ValidationLevel level,");
        emit("    ::nimblefix::detail::ParsedFieldIndex* out_index) noexcept {");
        emit("    (void)level;");
        emit("    if (out_index != nullptr) {");
        emit("        if (fields.size() < 0xFFFFu) out_index->begin(&schema_descriptor, " + std::to_string(msg_idx) + "u);");
        emit("        else { out_index->invalidate(); out_index = nullptr; }");
        emit("    }");
        emit("    auto fail_ret = [&](::nimblefix::Status st) noexcept -> ::nimblefix::Status {");
        emit("        if (out_index != nullptr) out_index->invalidate();");
        emit("        return st;");
        emit("    };");
        if (seen_words == 1) emit("    std::uint64_t seen = 0; (void)seen;");
        else emit("    std::uint64_t seen[" + std::to_string(seen_words) + "]{}; (void)seen;");
        if (env_seen_words == 1) emit("    std::uint64_t env_seen = 0; (void)env_seen;");
        else if (env_seen_words > 1) emit("    std::uint64_t env_seen[" + std::to_string(env_seen_words) + "]{}; (void)env_seen;");
        if (!env_ld_pairs.empty()) emit("    bool has_env_ld = false;");
        emit("    std::uint8_t section = 0;");
        emit("    ::nimblefix::Status first_content_err{};");
        emit("    ::nimblefix::Status first_order_err{};");
        emit("    ::nimblefix::Status first_unknown_err{};");
        if (needs_ancestors) {
            emit("    ::nimblefix::detail::FastAncestorStack ancestors{};");
            if (!trailer_all_tags.empty()) emit("    ancestors.trailer_pred = &is_trailer_tag;");
        }
        if (!ld_pairs.empty()) {
            emit("    bool has_ld = false;");
            for (const auto& [lt, dt] : ld_pairs) {
                emit("    int ld_len_" + std::to_string(lt) + " = -1, ld_data_" + std::to_string(dt) + " = -1;");
            }
        }
        emit("    for (std::size_t cursor = 0; cursor < fields.size();) {");
        emit("        const auto& f = fields[cursor];");
        emit("        const auto tag = f.tag;");
        emit("        switch (tag) {");
        emit("        case 8u: case 9u: case 34u: case 35u: case 43u:");
        emit("        case 49u: case 52u: case 56u: case 122u: case 369u: case 1128u:");
        emit("            if (section != 0u) [[unlikely]] {");
        emit("                if (level == ::nimblefix::ValidationLevel::strict && first_order_err.ok())");
        emit("                    first_order_err = {::nimblefix::ErrorCode::invalid_value, static_cast<int>(tag), f.offset};");
        emit("            }");
        emit("            ++cursor;");
        emit("            break;");
        emit("        case 10u:");
        emit("            section = 2u;");
        emit("            ++cursor;");
        emit("            break;");
        for (std::size_t mi = 0; mi < mm.size(); ++mi) {
            const auto& m = mm[mi];
            const auto& mf = schema.fields_by_name.at(m.field_name);
            if (session_or_frame_tags().contains(mf.number)) continue;
            const std::string seen_expr = (seen_words == 1) ? "seen" : ("seen[" + std::to_string(mi / 64) + "]");
            const std::size_t bit = mi % 64;
            emit("        case " + std::to_string(mf.number) + "u: {");
            emit("            if (section == 2u) [[unlikely]] {");
            emit("                if (level == ::nimblefix::ValidationLevel::strict && first_order_err.ok())");
            emit("                    first_order_err = {::nimblefix::ErrorCode::invalid_value, " + std::to_string(mf.number) + ", f.offset};");
            emit("            } else { section = 1u; }");
            emit("            if ((" + seen_expr + " & (std::uint64_t{1} << " + std::to_string(bit) + "u)) != 0) return fail_ret({::nimblefix::ErrorCode::duplicate_field, " + std::to_string(mf.number) + ", f.offset});");
            emit("            " + seen_expr + " |= (std::uint64_t{1} << " + std::to_string(bit) + "u);");
            if (m.kind == "field") {
                emit("            if (out_index != nullptr) out_index->record(" + std::to_string(mi) + "u, cursor);");
                emit("            if (first_content_err.ok()) { if (const auto st = " + scalar_check_expr(mf) + "; !st.ok()) first_content_err = st; }");
                if (ld_len_set.contains(mf.number)) emit("            ld_len_" + std::to_string(mf.number) + " = static_cast<int>(cursor); has_ld = true;");
                if (ld_data_set.contains(mf.number)) emit("            ld_data_" + std::to_string(mf.number) + " = static_cast<int>(cursor); has_ld = true;");
                emit("            ++cursor;");
                emit("            break;");
            } else {
                const std::size_t g_idx = group_indexes.at(make_group_key(make_group_key, m));
                emit("            const std::size_t g_start = cursor;");
                emit("            ancestors.predicates[ancestors.count++] = &msg_contains_" + std::to_string(msg_idx) + ";");
                emit("            const auto gst = validate_group_" + std::to_string(g_idx) + "(fields, cursor, level, ancestors, first_content_err, 1);");
                emit("            --ancestors.count;");
                emit("            if (!gst.ok()) return fail_ret(gst);");
                emit("            if (out_index != nullptr) out_index->record(" + std::to_string(mi) + "u, g_start, cursor);");
                emit("            break;");
            }
            emit("        }");
        }
        emit("        default:");
        std::size_t env_group_bit_idx = env_scalars.size();
        for (const auto& [ht, hgi, _req] : act_hdr_g) {
            const std::string env_expr = (env_seen_words == 1) ? "env_seen" : ("env_seen[" + std::to_string(env_group_bit_idx / 64) + "]");
            const std::size_t bit = env_group_bit_idx % 64;
            ++env_group_bit_idx;
            emit("            if (tag == " + std::to_string(ht) + "u) {");
            emit("                if (section != 0u) [[unlikely]] {");
            emit("                    if (level == ::nimblefix::ValidationLevel::strict && first_order_err.ok())");
            emit("                        first_order_err = {::nimblefix::ErrorCode::invalid_value, " + std::to_string(ht) + ", f.offset};");
            emit("                }");
            emit("                if ((" + env_expr + " & (std::uint64_t{1} << " + std::to_string(bit) + "u)) != 0) return fail_ret({::nimblefix::ErrorCode::duplicate_field, " + std::to_string(ht) + ", f.offset});");
            emit("                " + env_expr + " |= (std::uint64_t{1} << " + std::to_string(bit) + "u);");
            emit("                ancestors.predicates[ancestors.count++] = &is_header_tag;");
            emit("                const auto hst = validate_group_" + std::to_string(hgi) + "(fields, cursor, level, ancestors, first_content_err, 1);");
            emit("                --ancestors.count;");
            emit("                if (!hst.ok()) return fail_ret(hst);");
            emit("                break;");
            emit("            }");
        }
        for (const auto& [tt, tgi, _req] : act_trl_g) {
            const std::string env_expr = (env_seen_words == 1) ? "env_seen" : ("env_seen[" + std::to_string(env_group_bit_idx / 64) + "]");
            const std::size_t bit = env_group_bit_idx % 64;
            ++env_group_bit_idx;
            emit("            if (tag == " + std::to_string(tt) + "u) {");
            emit("                section = 2u;");
            emit("                if ((" + env_expr + " & (std::uint64_t{1} << " + std::to_string(bit) + "u)) != 0) return fail_ret({::nimblefix::ErrorCode::duplicate_field, " + std::to_string(tt) + ", f.offset});");
            emit("                " + env_expr + " |= (std::uint64_t{1} << " + std::to_string(bit) + "u);");
            emit("                ancestors.predicates[ancestors.count++] = &is_trailer_tag;");
            emit("                const auto tst = validate_group_" + std::to_string(tgi) + "(fields, cursor, level, ancestors, first_content_err, 1);");
            emit("                --ancestors.count;");
            emit("                if (!tst.ok()) return fail_ret(tst);");
            emit("                break;");
            emit("            }");
        }
        if (!env_scalars.empty()) {
            emit("            if (const int env_idx = envelope_scalar_index(tag); env_idx >= 0) {");
            if (trailer_scalar_tags.empty()) {
                emit("                if (section != 0u) [[unlikely]] {");
                emit("                    if (level == ::nimblefix::ValidationLevel::strict && first_order_err.ok())");
                emit("                        first_order_err = {::nimblefix::ErrorCode::invalid_value, static_cast<int>(tag), f.offset};");
                emit("                }");
            } else {
                emit("                if (is_trailer_scalar_tag(tag)) { section = 2u; }");
                emit("                else if (section != 0u) [[unlikely]] {");
                emit("                    if (level == ::nimblefix::ValidationLevel::strict && first_order_err.ok())");
                emit("                        first_order_err = {::nimblefix::ErrorCode::invalid_value, static_cast<int>(tag), f.offset};");
                emit("                }");
            }
            if (env_seen_words == 1) {
                emit("                const auto env_mask = std::uint64_t{1} << static_cast<unsigned>(env_idx);");
                emit("                if ((env_seen & env_mask) != 0) return fail_ret({::nimblefix::ErrorCode::duplicate_field, static_cast<int>(tag), f.offset});");
                emit("                env_seen |= env_mask;");
            } else {
                emit("                const auto env_mask = std::uint64_t{1} << (static_cast<unsigned>(env_idx) & 63u);");
                emit("                auto& env_word = env_seen[static_cast<std::size_t>(env_idx) >> 6u];");
                emit("                if ((env_word & env_mask) != 0) return fail_ret({::nimblefix::ErrorCode::duplicate_field, static_cast<int>(tag), f.offset});");
                emit("                env_word |= env_mask;");
            }
            emit("                if (first_content_err.ok()) { if (const auto st = check_envelope_scalar(f, level); !st.ok()) first_content_err = st; }");
            if (!env_ld_pairs.empty()) {
                std::string cond;
                for (std::uint32_t t : env_ld_tags) {
                    if (!cond.empty()) cond += " || ";
                    cond += "tag == " + std::to_string(t) + "u";
                }
                emit("                if (" + cond + ") has_env_ld = true;");
            }
            emit("                ++cursor;");
            emit("                break;");
            emit("            }");
        }
        emit("            if (level == ::nimblefix::ValidationLevel::strict && first_unknown_err.ok())");
        emit("                first_unknown_err = {::nimblefix::ErrorCode::invalid_value, static_cast<int>(tag), f.offset};");
        emit("            ++cursor;");
        emit("            break;");
        emit("        }");
        emit("    }");
        emit("    if (!first_content_err.ok()) return fail_ret(first_content_err);");
        if (!ld_pairs.empty()) {
            emit("    if (has_ld) {");
            for (const auto& [lt, dt] : ld_pairs) {
                emit("        if (const auto ldst = ::nimblefix::detail::check_len_data_pair(fields, " + std::to_string(lt) + "u, ld_len_" + std::to_string(lt) + ", ld_data_" + std::to_string(dt) + "); !ldst.ok()) return fail_ret(ldst);");
            }
            emit("    }");
        }
        if (!env_ld_pairs.empty()) {
            emit("    if (has_env_ld) {");
            emit("        if (const auto eldst = check_envelope_len_data(fields); !eldst.ok()) return fail_ret(eldst);");
            emit("    }");
        }
        emit("    if (!first_order_err.ok()) return fail_ret(first_order_err);");
        if (!req_by_word.empty() || !env_req_by_word.empty()) {
            emit("    if (level != ::nimblefix::ValidationLevel::structure) {");
            for (const auto& [w, bits] : req_by_word) {
                std::uint64_t mask = 0;
                for (const auto& [b, _rt] : bits) mask |= (std::uint64_t{1} << b);
                const std::string seen_expr = (seen_words == 1) ? "seen" : ("seen[" + std::to_string(w) + "]");
                emit("        if ((" + seen_expr + " & " + hex_u64(mask) + ") != " + hex_u64(mask) + ") {");
                for (const auto& [b, rtag] : bits) {
                    emit("            if ((" + seen_expr + " & (std::uint64_t{1} << " + std::to_string(b) + "u)) == 0) return fail_ret({::nimblefix::ErrorCode::required_field_missing, " + std::to_string(rtag) + "});");
                }
                emit("        }");
            }
            for (const auto& [w, bits] : env_req_by_word) {
                std::uint64_t mask = 0;
                for (const auto& [b, _rt] : bits) mask |= (std::uint64_t{1} << b);
                const std::string env_expr = (env_seen_words == 1) ? "env_seen" : ("env_seen[" + std::to_string(w) + "]");
                emit("        if ((" + env_expr + " & " + hex_u64(mask) + ") != " + hex_u64(mask) + ") {");
                for (const auto& [b, rtag] : bits) {
                    emit("            if ((" + env_expr + " & (std::uint64_t{1} << " + std::to_string(b) + "u)) == 0) return fail_ret({::nimblefix::ErrorCode::required_field_missing, " + std::to_string(rtag) + "});");
                }
                emit("        }");
            }
            emit("    }");
        }
        emit("    if (!first_unknown_err.ok()) return fail_ret(first_unknown_err);");
        emit("    return ::nimblefix::Status::success();");
        emit("}");
    }

    // Top-level O(1) MsgType switch dispatcher
    std::vector<std::pair<unsigned char, std::size_t>> m1;
    std::vector<std::pair<unsigned, std::size_t>> m2;
    std::vector<std::pair<std::string, std::size_t>> m_other;
    for (std::size_t idx = 0; idx < messages.size(); ++idx) {
        const auto& mt = messages[idx]->msg_type;
        if (mt.size() == 1) m1.emplace_back(static_cast<unsigned char>(mt[0]), idx);
        else if (mt.size() == 2) m2.emplace_back((static_cast<unsigned>(static_cast<unsigned char>(mt[0])) << 8U) | static_cast<unsigned char>(mt[1]), idx);
        else m_other.emplace_back(mt, idx);
    }
    emit("[[nodiscard]] inline ::nimblefix::Status fast_validate_message(");
    emit("    std::string_view msg_type, std::size_t msg_type_offset,");
    emit("    std::span<const ::nimblefix::FieldView> fields,");
    emit("    ::nimblefix::ValidationLevel level,");
    emit("    ::nimblefix::detail::ParsedFieldIndex* out_index) noexcept {");
    if (!m1.empty()) {
        emit("    if (msg_type.size() == 1) {");
        emit("        switch (msg_type[0]) {");
        for (const auto& [b, idx] : m1) emit("        " + byte_case(b) + " return validate_msg_" + std::to_string(idx) + "(fields, level, out_index);");
        emit("        default: break;");
        emit("        }");
        emit("    }");
    }
    if (!m2.empty()) {
        emit(std::string{"    "} + (m1.empty() ? "if" : "else if") + " (msg_type.size() == 2) {");
        emit("        switch ((static_cast<unsigned>(static_cast<unsigned char>(msg_type[0])) << 8U) | static_cast<unsigned char>(msg_type[1])) {");
        for (const auto& [code, idx] : m2) emit("        case " + hex_u16(code) + ": return validate_msg_" + std::to_string(idx) + "(fields, level, out_index);");
        emit("        default: break;");
        emit("        }");
        emit("    }");
    }
    for (const auto& [mt, idx] : m_other) {
        emit("    if (msg_type == " + cpp_string(mt) + ") return validate_msg_" + std::to_string(idx) + "(fields, level, out_index);");
    }
    emit("    if (out_index != nullptr) out_index->invalidate();");
    emit("    return {::nimblefix::ErrorCode::unsupported, 35, msg_type_offset};");
    emit("}");
    emit("} // namespace detail_validate");
    emit("");

    // Group policies
    for (std::size_t gi = 0; gi < group_keys.size(); ++gi) {
        const MemberDef* g = group_by_key.at(group_keys[gi]);
        const std::string policy = group_symbol(gi, "EntryPolicy");
        std::vector<std::uint32_t> allowed, required;
        for (const auto& m : g->members) {
            const std::uint32_t t = schema.fields_by_name.at(m.field_name).number;
            allowed.push_back(t);
            if (m.required) required.push_back(t);
        }
        emit("struct " + policy + " {");
        emit("    static constexpr bool is_group_entry = true;");
        emit("    static constexpr bool has_field_policy_check_traits = true;");
        emit("    static constexpr std::string_view msg_type = \"__entry__\";");
        emit("    inline static constexpr std::array<std::uint32_t, " + std::to_string(allowed.size()) + "> allowed_tags{{");
        for (std::uint32_t t : allowed) emit("        " + std::to_string(t) + "u,");
        emit("    }};");
        emit("    inline static constexpr std::array<std::uint32_t, " + std::to_string(required.size()) + "> required_tags{{");
        for (std::uint32_t t : required) emit("        " + std::to_string(t) + "u,");
        emit("    }};");
        emit("    static constexpr bool enum_value_allowed(std::uint32_t tag, std::string_view value) noexcept {");
        emit("        switch (tag) {");
        for (const auto& m : g->members) {
            if (m.kind != "field") continue;
            const auto& f = schema.fields_by_name.at(m.field_name);
            if (!field_needs_policy_check(f)) continue;
            emit("        case " + std::to_string(f.number) + "u:");
            emit("            return ::nimblefix::is_valid_schema_field_value(fields[" + std::to_string(field_indexes.at(f.name)) + "], value);");
        }
        emit("        default:");
        emit("            (void)value;");
        emit("            return true;");
        emit("        }");
        emit("    }");
        emit("};");
        emit("");
    }

    // Message policies
    for (const MessageDef* msg : messages) {
        const std::string policy = msg->name + "Policy";
        const auto& mm = schema.expanded_messages.at(msg->name);
        std::vector<std::uint32_t> allowed, required;
        for (const auto& m : mm) {
            const std::uint32_t t = schema.fields_by_name.at(m.field_name).number;
            allowed.push_back(t);
            if (m.required) required.push_back(t);
        }
        emit("struct " + policy + " {");
        emit("    using schema_type = Schema;");
        emit("    static constexpr bool has_field_policy_check_traits = true;");
        emit("    static constexpr std::string_view msg_type = " + cpp_string(msg->msg_type) + ";");
        emit("    inline static constexpr std::array<std::uint32_t, " + std::to_string(allowed.size()) + "> allowed_tags{{");
        for (std::uint32_t t : allowed) emit("        " + std::to_string(t) + "u,");
        emit("    }};");
        emit("    inline static constexpr std::array<std::uint32_t, " + std::to_string(required.size()) + "> required_tags{{");
        for (std::uint32_t t : required) emit("        " + std::to_string(t) + "u,");
        emit("    }};");
        emit("    static constexpr bool enum_value_allowed(std::uint32_t tag, std::string_view value) noexcept {");
        emit("        switch (tag) {");
        for (const auto& m : mm) {
            if (m.kind != "field") continue;
            const auto& f = schema.fields_by_name.at(m.field_name);
            if (!field_needs_policy_check(f)) continue;
            emit("        case " + std::to_string(f.number) + "u:");
            emit("            return ::nimblefix::is_valid_schema_field_value(fields[" + std::to_string(field_indexes.at(f.name)) + "], value);");
        }
        emit("        default:");
        emit("            (void)value;");
        emit("            return true;");
        emit("        }");
        emit("    }");
        emit("};");
        emit("");
    }

    emit("} // namespace " + ns);
    emit("");
    emit("namespace nimblefix {");
    for (std::size_t gi = 0; gi < group_keys.size(); ++gi) {
        const MemberDef* g = group_by_key.at(group_keys[gi]);
        const std::string policy = ns + "::" + group_symbol(gi, "EntryPolicy");
        for (std::size_t rank = 0; rank < g->members.size(); ++rank) {
            const auto& m = g->members[rank];
            const auto& f = schema.fields_by_name.at(m.field_name);
            emit("template<>");
            emit("struct WireRank<" + policy + ", " + std::to_string(f.number) + "> : std::integral_constant<std::size_t, " + std::to_string(rank) + "> {};");
            emit("template<>");
            emit("struct FieldWireType<" + policy + ", " + std::to_string(f.number) + "> : std::integral_constant<WireType, WireType::" + fix_wire_types().at(f.field_type) + "> {};");
            if (m.kind == "group") {
                emit("template<>");
                emit("struct GroupCountTag<" + policy + ", " + std::to_string(f.number) + "> : std::true_type {};");
            } else if (field_needs_policy_check(f)) {
                emit("template<>");
                emit("struct FieldNeedsPolicyCheck<" + policy + ", " + std::to_string(f.number) + "> : std::true_type {};");
            }
            if (field_pair_tags.contains(f.number)) {
                emit("template<>");
                emit("struct FieldPairTag<" + policy + ", " + std::to_string(f.number) + "> : std::integral_constant<std::uint32_t, " + std::to_string(field_pair_tags.at(f.number)) + "u> {};");
            }
        }
    }
    emit("} // namespace nimblefix");
    emit("");
    emit("namespace " + ns + " {");
    emit("");

    auto emit_stream_writer_body = [&](const std::string& writer_cls, const std::string& policy,
                                       const std::vector<MemberDef>& ms, bool is_entry, std::uint32_t delim_tag = 0) {
        std::vector<std::pair<std::size_t, std::uint32_t>> req_members;
        std::map<std::uint32_t, std::size_t> req_bit_by_tag;
        for (std::size_t i = 0; i < ms.size(); ++i) {
            if (ms[i].required) {
                const std::uint32_t t = schema.fields_by_name.at(ms[i].field_name).number;
                req_bit_by_tag[t] = req_members.size();
                req_members.emplace_back(i, t);
            }
        }
        const std::size_t req_words = (req_members.size() + 63) / 64;
        const bool has_req = !req_members.empty();

        if (!is_entry) {
            emit("    [[nodiscard]] ::nimblefix::detail::StandardHeaderWriter header() noexcept {");
            emit("        return ::nimblefix::detail::StandardHeaderWriter{buf_};");
            emit("    }");
        }

        for (const auto& m : ms) {
            if (m.kind != "field") continue;
            const auto& f = schema.fields_by_name.at(m.field_name);
            const std::string setter = snake_case(f.name);
            const std::string& wt = fix_wire_types().at(f.field_type);
            const std::string tag_prefix = std::to_string(f.number) + "=";
            const std::size_t plen = tag_prefix.size();
            const bool checked = field_needs_policy_check(f);
            const bool is_len_pair = field_pair_tags.contains(f.number) && (f.field_type == "LENGTH" || f.field_type == "NUMINGROUP");
            const bool is_data_pair = (f.field_type == "DATA" || f.field_type == "XMLDATA");
            const std::string val_arg = checked ? (", &detail_stream::valid_" + std::to_string(f.number)) : "";
            std::string emit_call;
            if (is_data_pair) {
                const std::uint32_t lt = field_pair_tags.at(f.number);
                const std::string lpref = std::to_string(lt) + "=";
                emit_call = "buf_.emit_data<" + std::to_string(lt) + "u, " + std::to_string(f.number) + "u>(" +
                            cpp_string(lpref) + ", " + std::to_string(lpref.size()) + "u, " +
                            cpp_string(tag_prefix) + ", " + std::to_string(plen) + "u, std::forward<V>(value))";
            } else if (wt == "character") {
                emit_call = "buf_.emit_char<" + std::to_string(f.number) + "u>(" + cpp_string(tag_prefix) + ", " + std::to_string(plen) + "u, std::forward<V>(value)" + val_arg + ")";
            } else if (wt == "integer") {
                const std::string parg = is_len_pair ? (", " + std::to_string(field_pair_tags.at(f.number)) + "u") : "";
                emit_call = "buf_.emit_int<" + std::to_string(f.number) + "u" + parg + ">(" + cpp_string(tag_prefix) + ", " + std::to_string(plen) + "u, std::forward<V>(value)" + val_arg + ")";
            } else if (wt == "decimal") {
                emit_call = "buf_.emit_decimal<" + std::to_string(f.number) + "u>(" + cpp_string(tag_prefix) + ", " + std::to_string(plen) + "u, std::forward<V>(value))";
            } else if (wt == "utc_timestamp") {
                emit_call = "buf_.emit_utc_timestamp<" + std::to_string(f.number) + "u>(" + cpp_string(tag_prefix) + ", " + std::to_string(plen) + "u, std::forward<V>(value))";
            } else {
                emit_call = "buf_.emit_str<" + std::to_string(f.number) + "u>(" + cpp_string(tag_prefix) + ", " + std::to_string(plen) + "u, std::forward<V>(value)" + val_arg + ")";
            }
            std::vector<std::string> post_ops;
            if (is_entry) {
                if (f.number == delim_tag) post_ops.push_back("first_field_ = false;");
                else post_ops.push_back("if (first_field_) { buf_.fail(::nimblefix::ErrorCode::required_field_missing, " + std::to_string(delim_tag) + "); }");
            }
            if (const auto rbit = req_bit_by_tag.find(f.number); rbit != req_bit_by_tag.end()) {
                const std::size_t bit = rbit->second;
                if (req_words == 1) post_ops.push_back("req_mask_ |= (std::uint64_t{1} << " + std::to_string(bit) + "u);");
                else post_ops.push_back("req_mask_[" + std::to_string(bit / 64) + "u] |= (std::uint64_t{1} << " + std::to_string(bit % 64) + "u);");
            }
            std::string body;
            if (!post_ops.empty()) {
                body = "if (" + emit_call + ") { ";
                for (std::size_t k = 0; k < post_ops.size(); ++k) {
                    if (k != 0) body.push_back(' ');
                    body += post_ops[k];
                }
                body += " } return *this;";
            } else {
                body = "(void)" + emit_call + "; return *this;";
            }
            emit("    template<class V> " + writer_cls + "& " + setter + "(V&& value) noexcept { " + body + " }");
        }

        for (const auto& m : ms) {
            if (m.kind != "group") continue;
            const auto& counter = schema.fields_by_name.at(m.field_name);
            const std::string setter = snake_case(counter.name);
            const std::size_t gi = group_indexes.at(make_group_key(make_group_key, m));
            const std::string scope_cls = group_symbol(gi, "StreamScope");
            const std::string tag_prefix = std::to_string(counter.number) + "=";
            emit("    template<class Fn>");
            emit("    " + writer_cls + "& " + setter + "(std::size_t count, Fn&& fn) noexcept {");
            if (is_entry) {
                if (counter.number == delim_tag) emit("        first_field_ = false;");
                else emit("        if (first_field_) { buf_.fail(::nimblefix::ErrorCode::required_field_missing, " + std::to_string(delim_tag) + "); return *this; }");
            }
            if (const auto rbit = req_bit_by_tag.find(counter.number); rbit != req_bit_by_tag.end()) {
                const std::size_t bit = rbit->second;
                if (req_words == 1) emit("        req_mask_ |= (std::uint64_t{1} << " + std::to_string(bit) + "u);");
                else emit("        req_mask_[" + std::to_string(bit / 64) + "u] |= (std::uint64_t{1} << " + std::to_string(bit % 64) + "u);");
            }
            emit("        buf_.enter_body_field();");
            emit("        buf_.write_tag_u64(" + cpp_string(tag_prefix) + ", " + std::to_string(tag_prefix.size()) + "u, static_cast<std::uint64_t>(count));");
            emit("        " + scope_cls + " scope{buf_, count};");
            emit("        fn(scope);");
            emit("        scope.finish();");
            emit("        return *this;");
            emit("    }");
        }

        emit("    template<std::uint32_t Tag, class V>");
        emit("    " + writer_cls + "& set(V&& value) noexcept {");
        emit("        static_assert(Tag > 0 && Tag <= ::nimblefix::detail::maximum_fix_tag, \"FIX tags must be in 1..999999999\");");
        emit("        static_assert(!::nimblefix::detail::is_managed_tag(Tag), \"session and framing tags are engine managed\");");
        emit("        static_assert(::nimblefix::detail::policy_allows_tag<" + policy + ", Tag>(), \"tag is not allowed by this generated message\");");
        emit("        using U = ::nimblefix::detail::unwrap_optional_t<V>;");
        emit("        static_assert(::nimblefix::detail::supported_value_v<U>, \"unsupported static FIX field value type\");");
        emit("        static_assert(::nimblefix::detail::wire_value_compatible_v<::nimblefix::FieldWireType<" + policy + ", Tag>::value, U>,");
        emit("            \"value type does not match the generated FIX field type\");");
        std::size_t s_idx = 0;
        for (const auto& m : ms) {
            if (m.kind != "field") continue;
            const auto& f = schema.fields_by_name.at(m.field_name);
            const std::string setter = snake_case(f.name);
            emit(std::string{"        "} + (s_idx == 0 ? "if constexpr" : "else if constexpr") +
                 " (Tag == " + std::to_string(f.number) + "u) return " + setter + "(std::forward<V>(value));");
            ++s_idx;
        }
        if (s_idx != 0) emit("        else { static_assert(Tag == 0, \"use the named group(count, fn) setter for repeating groups\"); return *this; }");
        else emit("        (void)value; return *this;");
        emit("    }");
        emit("    [[nodiscard]] ::nimblefix::Status status() const noexcept { return buf_.status(); }");
        emit("    [[nodiscard]] ::nimblefix::Status validate_required() const noexcept {");
        emit("        if (!buf_.ok()) return buf_.status();");
        if (is_entry) emit("        if (first_field_) return ::nimblefix::Status{::nimblefix::ErrorCode::required_field_missing, " + std::to_string(delim_tag) + "};");
        if (has_req) {
            if (req_words == 1) {
                const std::uint64_t all_mask = (req_members.size() == 64) ? ~std::uint64_t{0} : ((std::uint64_t{1} << req_members.size()) - 1ULL);
                emit("        if ((req_mask_ & " + std::to_string(all_mask) + "ULL) != " + std::to_string(all_mask) + "ULL) {");
                for (std::size_t bit = 0; bit < req_members.size(); ++bit) {
                    emit("            if ((req_mask_ & (std::uint64_t{1} << " + std::to_string(bit) + "u)) == 0) return ::nimblefix::Status{::nimblefix::ErrorCode::required_field_missing, " + std::to_string(req_members[bit].second) + "};");
                }
                emit("        }");
            } else {
                for (std::size_t bit = 0; bit < req_members.size(); ++bit) {
                    emit("        if ((req_mask_[" + std::to_string(bit / 64) + "u] & (std::uint64_t{1} << " + std::to_string(bit % 64) + "u)) == 0) return ::nimblefix::Status{::nimblefix::ErrorCode::required_field_missing, " + std::to_string(req_members[bit].second) + "};");
                }
            }
        }
        emit("        return ::nimblefix::Status::success();");
        emit("    }");
        emit("    void reset_entry_state() noexcept {");
        if (is_entry) emit("        first_field_ = true;");
        if (has_req) {
            if (req_words == 1) emit("        req_mask_ = 0;");
            else emit("        req_mask_.fill(0);");
        }
        emit("    }");
        emit("private:");
        emit("    ::nimblefix::detail::StreamBuffer& buf_;");
        if (is_entry) emit("    bool first_field_{true};");
        if (has_req) {
            if (req_words == 1) emit("    std::uint64_t req_mask_{0};");
            else emit("    std::array<std::uint64_t, " + std::to_string(req_words) + "> req_mask_{};");
        }
    };

    // Group StreamWriters & StreamScopes leaf-first
    for (std::size_t gi : group_emit_order) {
        const MemberDef* g = group_by_key.at(group_keys[gi]);
        const std::string entry_writer = group_symbol(gi, "EntryStreamWriter");
        const std::string scope_cls = group_symbol(gi, "StreamScope");
        const std::string policy = group_symbol(gi, "EntryPolicy");
        const std::uint32_t count_tag = schema.fields_by_name.at(g->field_name).number;
        const std::uint32_t delim_tag = schema.fields_by_name.at(g->members[0].field_name).number;
        emit("class " + entry_writer + " {");
        emit("public:");
        emit("    using policy_type = " + policy + ";");
        emit("    static constexpr bool is_group_entry = true;");
        emit("    explicit " + entry_writer + "(::nimblefix::detail::StreamBuffer& buf) noexcept : buf_(buf) {}");
        emit_stream_writer_body(entry_writer, policy, g->members, true, delim_tag);
        emit("};");
        emit("");
        emit("class " + scope_cls + " {");
        emit("public:");
        emit("    static constexpr std::uint32_t count_tag = " + std::to_string(count_tag) + "u;");
        emit("    static constexpr std::uint32_t delimiter_tag = " + std::to_string(delim_tag) + "u;");
        emit("    " + scope_cls + "(::nimblefix::detail::StreamBuffer& buf, std::size_t expected_count) noexcept");
        emit("        : buf_(buf), expected_count_(expected_count), active_entry_(buf) {}");
        emit("    " + entry_writer + "& entry() noexcept {");
        emit("        if (has_active_entry_) {");
        emit("            buf_.fail(buf_.check_pending_pairs());");
        emit("            buf_.fail(active_entry_.validate_required());");
        emit("        }");
        emit("        if (written_count_ >= expected_count_) {");
        emit("            buf_.fail(::nimblefix::ErrorCode::invalid_value, " + std::to_string(count_tag) + ");");
        emit("        }");
        emit("        ++written_count_;");
        emit("        has_active_entry_ = true;");
        emit("        active_entry_.reset_entry_state();");
        emit("        return active_entry_;");
        emit("    }");
        emit("    void finish() noexcept {");
        emit("        if (has_active_entry_) {");
        emit("            buf_.fail(buf_.check_pending_pairs());");
        emit("            buf_.fail(active_entry_.validate_required());");
        emit("            has_active_entry_ = false;");
        emit("        }");
        emit("        if (written_count_ != expected_count_) {");
        emit("            buf_.fail(::nimblefix::ErrorCode::invalid_value, " + std::to_string(count_tag) + ");");
        emit("        }");
        emit("    }");
        emit("private:");
        emit("    ::nimblefix::detail::StreamBuffer& buf_;");
        emit("    std::size_t expected_count_{};");
        emit("    std::size_t written_count_{};");
        emit("    bool has_active_entry_{false};");
        emit("    " + entry_writer + " active_entry_;");
        emit("};");
        emit("");
    }

    // Messages: View, Fixed, StreamWriter, Message tag struct
    for (std::size_t idx = 0; idx < messages.size(); ++idx) {
        const MessageDef* msg = messages[idx];
        const std::string& name = msg->name;
        const std::string policy = name + "Policy";
        const std::string fixed_name = name + "Fixed";
        const std::string writer_name = name + "StreamWriter";
        const auto& mm = schema.expanded_messages.at(name);

        emit("struct " + name + ";");
        emit("class " + name + "View : public ::nimblefix::TypedMessageView<Schema> {");
        emit("    using Base = ::nimblefix::TypedMessageView<Schema>;");
        emit("public:");
        emit("    explicit " + name + "View(const ::nimblefix::FlatMessageView& flat) noexcept : Base(flat, messages[" + std::to_string(idx) + "]) {}");
        for (std::size_t mi = 0; mi < mm.size(); ++mi) {
            if (mm[mi].kind != "field") continue;
            const auto& f = schema.fields_by_name.at(mm[mi].field_name);
            emit("    [[nodiscard]] auto " + snake_case(f.name) + "() const noexcept {");
            emit("        return this->template get_indexed<" + std::to_string(f.number) + "u, " + std::to_string(idx) + "u, " + std::to_string(mi) + "u>();");
            emit("    }");
        }
        for (std::size_t mi = 0; mi < mm.size(); ++mi) {
            if (mm[mi].kind != "group") continue;
            const auto& f = schema.fields_by_name.at(mm[mi].field_name);
            emit("    [[nodiscard]] auto " + snake_case(mm[mi].field_name) + "() const noexcept {");
            emit("        return this->template group_indexed<" + std::to_string(f.number) + "u, " + std::to_string(idx) + "u, " + std::to_string(mi) + "u>();");
            emit("    }");
        }
        emit("};");
        emit("");

        emit("template<class Shape, std::size_t ByteCapacity = 512>");
        emit("class " + fixed_name + " : public ::nimblefix::FixedShapeMessage<" + cpp_string(msg->msg_type) + ", Shape, ByteCapacity, " + policy + "> {");
        emit("    using Base = ::nimblefix::FixedShapeMessage<" + cpp_string(msg->msg_type) + ", Shape, ByteCapacity, " + policy + ">;");
        emit("public:");
        emit("    using Base::Base;");
        for (const auto& m : mm) {
            if (m.kind != "field") continue;
            const auto& f = schema.fields_by_name.at(m.field_name);
            emit("    template<class V>");
            emit("    [[nodiscard]] ::nimblefix::Status " + snake_case(f.name) + "(V&& value) noexcept {");
            emit("        return this->template set<" + std::to_string(f.number) + ">(std::forward<V>(value));");
            emit("    }");
        }
        emit("};");
        emit("");

        emit("class " + writer_name + " {");
        emit("public:");
        emit("    using schema_type = Schema;");
        emit("    using policy_type = " + policy + ";");
        emit("    static constexpr std::string_view msg_type = " + cpp_string(msg->msg_type) + ";");
        emit("    explicit " + writer_name + "(::nimblefix::detail::StreamBuffer& buf) noexcept : buf_(buf) {}");
        emit_stream_writer_body(writer_name, policy, mm, false);
        emit("};");
        emit("");

        emit("struct " + name + " {");
        emit("    using schema_type = Schema;");
        emit("    using view_type = " + name + "View;");
        emit("    using policy_type = " + policy + ";");
        emit("    using stream_writer_type = " + writer_name + ";");
        emit("    using Writer = " + writer_name + ";");
        emit("    template<class Shape, std::size_t ByteCapacity = 512>");
        emit("    using fixed_type = " + fixed_name + "<Shape, ByteCapacity>;");
        emit("    template<std::size_t ByteCapacity, class... FieldSpecs>");
        emit("    using Fixed = " + fixed_name + "<::nimblefix::Fields<FieldSpecs...>, ByteCapacity>;");
        emit("    static constexpr std::string_view msg_type = " + cpp_string(msg->msg_type) + ";");
        emit("    inline static constexpr const MessageDescriptor& descriptor = messages[" + std::to_string(idx) + "];");
        emit("};");
        emit("");
    }

    emit("template<class Visitor>");
    emit("inline void Schema::for_each_message(Visitor&& visitor) {");
    emit("    auto&& callable = visitor;");
    for (const MessageDef* msg : messages) emit("    callable.template operator()<" + msg->name + ">();");
    emit("}");
    emit("");
    emit("} // namespace " + ns);
    emit("");
    emit("namespace nimblefix {");
    for (const MessageDef* msg : messages) {
        const std::string policy = ns + "::" + msg->name + "Policy";
        const auto& mm = schema.expanded_messages.at(msg->name);
        for (std::size_t rank = 0; rank < mm.size(); ++rank) {
            const auto& m = mm[rank];
            const auto& f = schema.fields_by_name.at(m.field_name);
            emit("template<>");
            emit("struct WireRank<" + policy + ", " + std::to_string(f.number) + "> : std::integral_constant<std::size_t, " + std::to_string(rank) + "> {};");
            emit("template<>");
            emit("struct FieldWireType<" + policy + ", " + std::to_string(f.number) + "> : std::integral_constant<WireType, WireType::" + fix_wire_types().at(f.field_type) + "> {};");
            if (m.kind == "group") {
                emit("template<>");
                emit("struct GroupCountTag<" + policy + ", " + std::to_string(f.number) + "> : std::true_type {};");
            } else if (field_needs_policy_check(f)) {
                emit("template<>");
                emit("struct FieldNeedsPolicyCheck<" + policy + ", " + std::to_string(f.number) + "> : std::true_type {};");
            }
            if (field_pair_tags.contains(f.number)) {
                emit("template<>");
                emit("struct FieldPairTag<" + policy + ", " + std::to_string(f.number) + "> : std::integral_constant<std::uint32_t, " + std::to_string(field_pair_tags.at(f.number)) + "u> {};");
            }
        }
    }
    emit("} // namespace nimblefix");
    emit("");
    emit("namespace " + ns + " {");
    for (const MessageDef* msg : messages) {
        const auto& mm = schema.expanded_messages.at(msg->name);
        emit("inline constexpr std::array<std::uint32_t, " + std::to_string(mm.size()) + "> " + msg->name + "_allowed_tags{{");
        for (const auto& m : mm) emit("    " + std::to_string(schema.fields_by_name.at(m.field_name).number) + "u,");
        emit("}};");
        std::vector<std::uint32_t> reqs;
        for (const auto& m : mm) if (m.required) reqs.push_back(schema.fields_by_name.at(m.field_name).number);
        emit("inline constexpr std::array<std::uint32_t, " + std::to_string(reqs.size()) + "> " + msg->name + "_required_tags{{");
        for (std::uint32_t t : reqs) emit("    " + std::to_string(t) + "u,");
        emit("}};");
        emit("");
    }
    emit("} // namespace " + ns);
    return out;
}

bool write_if_changed(const std::filesystem::path& output, const std::string& content) {
    if (std::filesystem::exists(output)) {
        std::ifstream existing(output, std::ios::binary);
        if (existing) {
            std::string cur((std::istreambuf_iterator<char>(existing)), std::istreambuf_iterator<char>());
            if (cur == content) return false;
        }
    }
    if (output.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(output.parent_path(), ec);
        if (ec) throw SchemaError("cannot create directory for output " + output.string() + ": " + ec.message());
    }
    const std::filesystem::path dir = output.has_parent_path() ? output.parent_path() : std::filesystem::path{"."};
    std::string tmpl = (dir / ("." + output.filename().string() + ".XXXXXX")).string();
    std::vector<char> tmpl_buf(tmpl.begin(), tmpl.end());
    tmpl_buf.push_back('\0');
    const int fd = ::mkstemp(tmpl_buf.data());
    if (fd < 0) throw SchemaError("cannot create temporary file for output " + output.string());
    const std::string temp_path(tmpl_buf.data());
    (void)::fchmod(fd, 0644);
    std::size_t off = 0;
    while (off < content.size()) {
        const ssize_t n = ::write(fd, content.data() + off, content.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            ::unlink(temp_path.c_str());
            throw SchemaError("cannot write output " + output.string());
        }
        off += static_cast<std::size_t>(n);
    }
    ::close(fd);
    if (::rename(temp_path.c_str(), output.c_str()) != 0) {
        ::unlink(temp_path.c_str());
        throw SchemaError("cannot finalize output " + output.string());
    }
    return true;
}

void print_usage() {
    std::cerr << "Usage: nimblefix-schema-gen --output HEADER --namespace NS [--messages A,B] "
                 "[--from-fix-log LOG]... [--keep-all-fields] XML...\n";
}

} // namespace
} // namespace nimblefix::codegen

int main(int argc, char** argv) {
    using namespace nimblefix::codegen;
    std::filesystem::path output;
    std::string ns;
    std::string messages_csv;
    std::vector<std::filesystem::path> fix_logs;
    bool keep_all_fields = false;
    std::vector<std::filesystem::path> xml_files;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "-h" || arg == "--help") {
            print_usage();
            return 0;
        }
        if (arg == "--keep-all-fields") {
            keep_all_fields = true;
            continue;
        }
        auto need_val = [&](std::string_view flag) -> std::string_view {
            if (i + 1 >= argc) {
                std::cerr << "schema codegen: missing value for " << flag << '\n';
                std::exit(2);
            }
            return argv[++i];
        };
        if (arg == "--output") output = need_val(arg);
        else if (arg == "--namespace") ns = std::string{need_val(arg)};
        else if (arg == "--messages") messages_csv = std::string{need_val(arg)};
        else if (arg == "--from-fix-log") fix_logs.emplace_back(need_val(arg));
        else if (arg.starts_with("--")) {
            std::cerr << "schema codegen: unknown option " << arg << '\n';
            return 2;
        } else {
            xml_files.emplace_back(arg);
        }
    }

    if (output.empty() || ns.empty() || xml_files.empty()) {
        print_usage();
        return 2;
    }

    try {
        Schema schema = load_schema(xml_files);
        if (!fix_logs.empty()) {
            const auto observed = scan_fix_logs(fix_logs);
            auto [pruned, stats] = prune_schema_from_observations(schema, observed, keep_all_fields);
            schema = std::move(pruned);
            std::cerr << "schema codegen: pruned from FIX log: "
                      << stats.messages_before << "->" << stats.messages_after << " messages, "
                      << stats.fields_before << "->" << stats.fields_after << " fields\n";
        }
        if (!messages_csv.empty()) {
            std::vector<std::string> req;
            std::size_t pos = 0;
            for (;;) {
                const std::size_t comma = messages_csv.find(',', pos);
                req.push_back(comma == std::string::npos
                    ? messages_csv.substr(pos)
                    : messages_csv.substr(pos, comma - pos));
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
            schema = filter_schema_messages(schema, req);
        }
        const std::string header = render_header(schema, ns);
        (void)write_if_changed(output, header);
    } catch (const SchemaError& exc) {
        std::cerr << "schema codegen: " << exc.what() << '\n';
        return 2;
    } catch (const std::exception& exc) {
        std::cerr << "schema codegen: " << exc.what() << '\n';
        return 2;
    }
    return 0;
}
