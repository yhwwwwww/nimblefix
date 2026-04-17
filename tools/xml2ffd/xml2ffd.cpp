#include "xml2ffd.h"

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <pugixml.hpp>

namespace fastfix::tools {

namespace {

auto
Fnv1a64(const std::string& data) -> std::uint64_t
{
  constexpr std::uint64_t kOffset = 14695981039346656037ULL;
  constexpr std::uint64_t kPrime = 1099511628211ULL;
  auto hash = kOffset;
  for (const auto byte : data) {
    hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(byte));
    hash *= kPrime;
  }
  return hash;
}

struct FieldInfo
{
  std::uint32_t tag{ 0 };
  std::string name;
  std::string ffd_type;
  std::vector<std::pair<std::string, std::string>> enum_values;
};

struct FieldRuleEntry
{
  std::uint32_t tag{ 0 };
  bool required{ false };
};

struct GroupEntry
{
  std::uint32_t count_tag{ 0 };
  std::uint32_t delimiter_tag{ 0 };
  std::string name;
  std::vector<FieldRuleEntry> field_rules;
};

auto
MapXmlType(const std::string& xml_type) -> std::string
{
  if (xml_type == "STRING" || xml_type == "MULTIPLEVALUESTRING" || xml_type == "MULTIPLECHARVALUE" ||
      xml_type == "MULTIPLESTRINGVALUE" || xml_type == "DATA" || xml_type == "XMLDATA" || xml_type == "EXCHANGE" ||
      xml_type == "COUNTRY" || xml_type == "CURRENCY" || xml_type == "LANGUAGE" || xml_type == "TZTIMEONLY" ||
      xml_type == "TZTIMESTAMP") {
    return "string";
  }
  if (xml_type == "INT" || xml_type == "LENGTH" || xml_type == "SEQNUM" || xml_type == "NUMINGROUP" ||
      xml_type == "TAGNUM" || xml_type == "DAYOFMONTH") {
    return "int";
  }
  if (xml_type == "CHAR") {
    return "char";
  }
  if (xml_type == "PRICE" || xml_type == "QTY" || xml_type == "FLOAT" || xml_type == "PRICEOFFSET" ||
      xml_type == "PERCENTAGE" || xml_type == "AMT") {
    return "float";
  }
  if (xml_type == "BOOLEAN") {
    return "boolean";
  }
  if (xml_type == "UTCTIMESTAMP" || xml_type == "UTCDATE" || xml_type == "UTCDATEONLY" || xml_type == "UTCTIMEONLY" ||
      xml_type == "LOCALMKTDATE" || xml_type == "MONTHYEAR") {
    return "timestamp";
  }
  return "unknown";
}

using FieldMap = std::map<std::string, FieldInfo>;
using ComponentMap = std::map<std::string, pugi::xml_node>;

void
ExpandNode(const pugi::xml_node& parent,
           const FieldMap& field_map,
           const ComponentMap& component_map,
           std::vector<FieldRuleEntry>& out_rules,
           std::vector<GroupEntry>& out_groups)
{
  for (const auto& child : parent.children()) {
    const std::string tag_name = child.name();

    if (tag_name == "field") {
      const std::string field_name = child.attribute("name").as_string();
      const bool required = std::string(child.attribute("required").as_string()) == "Y";
      auto iterator = field_map.find(field_name);
      if (iterator != field_map.end()) {
        out_rules.push_back({ iterator->second.tag, required });
      }
    } else if (tag_name == "component") {
      const std::string comp_name = child.attribute("name").as_string();
      auto iterator = component_map.find(comp_name);
      if (iterator != component_map.end()) {
        ExpandNode(iterator->second, field_map, component_map, out_rules, out_groups);
      }
    } else if (tag_name == "group") {
      const std::string group_field_name = child.attribute("name").as_string();
      const bool group_required = std::string(child.attribute("required").as_string()) == "Y";
      auto field_iterator = field_map.find(group_field_name);
      if (field_iterator == field_map.end()) {
        continue;
      }

      const auto count_tag = field_iterator->second.tag;

      // Add count field to the message's field rules.
      out_rules.push_back({ count_tag, group_required });

      // Build group entry.
      GroupEntry group;
      group.count_tag = count_tag;
      group.name = group_field_name;

      std::vector<GroupEntry> nested_groups;
      ExpandNode(child, field_map, component_map, group.field_rules, nested_groups);

      if (!group.field_rules.empty()) {
        group.delimiter_tag = group.field_rules[0].tag;
      }

      out_groups.push_back(std::move(group));
      for (auto& nested : nested_groups) {
        out_groups.push_back(std::move(nested));
      }
    }
  }
}

} // namespace

auto
ConvertXmlToFfd(const std::string& xml_content, std::uint64_t profile_id) -> std::string
{
  pugi::xml_document doc;
  const auto result = doc.load_string(xml_content.c_str());
  if (!result) {
    throw std::runtime_error(std::string("failed to parse XML: ") + result.description());
  }

  const auto fix_node = doc.child("fix");
  if (!fix_node) {
    throw std::runtime_error("XML does not contain a <fix> root element");
  }

  // 1. Build field map from <fields> section.
  FieldMap field_map;
  const auto fields_node = fix_node.child("fields");
  for (const auto& field : fields_node.children("field")) {
    FieldInfo info;
    info.tag = field.attribute("number").as_uint();
    info.name = field.attribute("name").as_string();
    const std::string xml_type = field.attribute("type").as_string();
    info.ffd_type = MapXmlType(xml_type);
    for (const auto& value_node : field.children("value")) {
      info.enum_values.emplace_back(value_node.attribute("enum").as_string(),
                                    value_node.attribute("description").as_string());
    }
    field_map[info.name] = info;
  }

  // 2. Build component map from <components> section.
  ComponentMap component_map;
  const auto components_node = fix_node.child("components");
  if (components_node) {
    for (const auto& comp : components_node.children("component")) {
      const std::string name = comp.attribute("name").as_string();
      component_map[name] = comp;
    }
  }

  // 3. Collect all fields sorted by tag.
  std::vector<FieldInfo> sorted_fields;
  sorted_fields.reserve(field_map.size());
  for (const auto& [name, info] : field_map) {
    sorted_fields.push_back(info);
  }
  std::sort(
    sorted_fields.begin(), sorted_fields.end(), [](const FieldInfo& a, const FieldInfo& b) { return a.tag < b.tag; });

  // 4. Process messages.
  struct MessageEntry
  {
    std::string msg_type;
    std::string name;
    std::uint32_t flags{ 0 };
    std::vector<FieldRuleEntry> field_rules;
  };

  std::vector<MessageEntry> messages;
  std::vector<GroupEntry> all_groups;

  const auto messages_node = fix_node.child("messages");
  for (const auto& msg : messages_node.children("message")) {
    MessageEntry entry;
    entry.name = msg.attribute("name").as_string();
    entry.msg_type = msg.attribute("msgtype").as_string();
    const std::string msgcat = msg.attribute("msgcat").as_string();
    entry.flags = (msgcat == "admin") ? 1U : 0U;

    std::vector<GroupEntry> msg_groups;
    ExpandNode(msg, field_map, component_map, entry.field_rules, msg_groups);

    messages.push_back(std::move(entry));
    for (auto& group : msg_groups) {
      all_groups.push_back(std::move(group));
    }
  }

  // 5. Also process header/trailer for group definitions (but don't emit them
  // as messages).
  const auto header_node = fix_node.child("header");
  if (header_node) {
    std::vector<FieldRuleEntry> dummy_rules;
    ExpandNode(header_node, field_map, component_map, dummy_rules, all_groups);
  }
  const auto trailer_node = fix_node.child("trailer");
  if (trailer_node) {
    std::vector<FieldRuleEntry> dummy_rules;
    ExpandNode(trailer_node, field_map, component_map, dummy_rules, all_groups);
  }

  // 6. Deduplicate groups by count_tag.
  {
    std::map<std::uint32_t, bool> seen;
    std::vector<GroupEntry> unique;
    for (auto& group : all_groups) {
      if (seen.find(group.count_tag) == seen.end()) {
        seen[group.count_tag] = true;
        unique.push_back(std::move(group));
      }
    }
    all_groups = std::move(unique);
  }

  // 7. Build body first so we can hash it for schema_hash.
  std::ostringstream body;

  for (const auto& field : sorted_fields) {
    body << "field|" << field.tag << '|' << field.name << '|' << field.ffd_type << "|0\n";
    for (const auto& [ev, desc] : field.enum_values) {
      body << "enum|" << field.tag << '|' << ev << '|' << desc << '\n';
    }
  }
  body << '\n';

  for (const auto& msg : messages) {
    body << "message|" << msg.msg_type << '|' << msg.name << '|' << msg.flags << '|';
    for (std::size_t i = 0; i < msg.field_rules.size(); ++i) {
      if (i > 0)
        body << ',';
      body << msg.field_rules[i].tag << ':' << (msg.field_rules[i].required ? 'r' : 'o');
    }
    body << '\n';
  }
  body << '\n';

  for (const auto& group : all_groups) {
    body << "group|" << group.count_tag << '|' << group.delimiter_tag << '|' << group.name << "|0|";
    for (std::size_t i = 0; i < group.field_rules.size(); ++i) {
      if (i > 0)
        body << ',';
      body << group.field_rules[i].tag << ':' << (group.field_rules[i].required ? 'r' : 'o');
    }
    body << '\n';
  }

  const auto body_str = body.str();
  const auto schema_hash = Fnv1a64(body_str);

  // 8. Emit .ffd output with auto-computed schema_hash.
  std::ostringstream out;
  out << "profile_id=" << profile_id << '\n';
  out << "schema_hash=0x" << std::hex << std::setfill('0') << std::setw(16) << schema_hash << std::dec << '\n';
  out << '\n' << body_str;

  return out.str();
}

} // namespace fastfix::tools
