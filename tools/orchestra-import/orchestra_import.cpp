#include "orchestra_import.h"

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#include <pugixml.hpp>

namespace nimble::tools {

namespace {

auto
Fnv1a64(std::string_view data) -> std::uint64_t
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

auto
LocalName(std::string_view name) -> std::string_view
{
  const auto colon = name.find(':');
  if (colon == std::string_view::npos) {
    return name;
  }
  return name.substr(colon + 1U);
}

auto
MapOrchestraType(std::string_view xml_type) -> profile::ValueType
{
  if (xml_type == "String" || xml_type == "STRING" || xml_type == "MultipleStringValue" ||
      xml_type == "MULTIPLESTRINGVALUE" || xml_type == "MultipleCharValue" || xml_type == "DATA") {
    return profile::ValueType::kString;
  }
  if (xml_type == "int" || xml_type == "Int" || xml_type == "INT" || xml_type == "NumInGroup" ||
      xml_type == "NUMINGROUP" || xml_type == "Length" || xml_type == "SEQNUM") {
    return profile::ValueType::kInt;
  }
  if (xml_type == "char" || xml_type == "Char" || xml_type == "CHAR") {
    return profile::ValueType::kChar;
  }
  if (xml_type == "float" || xml_type == "Float" || xml_type == "PRICE" || xml_type == "QTY" || xml_type == "AMT" ||
      xml_type == "Percentage") {
    return profile::ValueType::kFloat;
  }
  if (xml_type == "Boolean" || xml_type == "BOOLEAN") {
    return profile::ValueType::kBoolean;
  }
  if (xml_type == "UTCTimestamp" || xml_type == "UTCTIMESTAMP" || xml_type == "UTCTimeOnly" ||
      xml_type == "UTCDATEONLY" || xml_type == "LocalMktDate") {
    return profile::ValueType::kTimestamp;
  }
  return profile::ValueType::kUnknown;
}

auto
ValueTypeToNfdType(profile::ValueType type) -> std::string_view
{
  switch (type) {
    case profile::ValueType::kString:
      return "string";
    case profile::ValueType::kInt:
      return "int";
    case profile::ValueType::kChar:
      return "char";
    case profile::ValueType::kFloat:
      return "float";
    case profile::ValueType::kBoolean:
      return "boolean";
    case profile::ValueType::kTimestamp:
      return "timestamp";
    case profile::ValueType::kUnknown:
      break;
  }
  return "unknown";
}

auto
PresenceRequired(std::string_view value, std::vector<profile::ContractWarning>* warnings, std::string trace_path)
  -> bool
{
  if (value.empty() || value == "optional" || value == "Optional") {
    return false;
  }
  if (value == "required" || value == "Required" || value == "Y") {
    return true;
  }
  warnings->push_back(profile::ContractWarning{
    .code = "unsupported-presence",
    .message =
      "unsupported presence '" + std::string(value) + "' at " + std::move(trace_path) + "; downgraded to optional",
  });
  return false;
}

auto
ParseUintAttr(const pugi::xml_node& node, std::initializer_list<const char*> names) -> std::optional<std::uint32_t>
{
  for (const auto* name : names) {
    const auto attribute = node.attribute(name);
    if (!attribute.empty()) {
      return attribute.as_uint();
    }
  }
  return std::nullopt;
}

auto
ParseStringAttr(const pugi::xml_node& node, std::initializer_list<const char*> names) -> std::string
{
  for (const auto* name : names) {
    const auto attribute = node.attribute(name);
    if (!attribute.empty()) {
      return attribute.as_string();
    }
  }
  return {};
}

struct OrchestraField
{
  std::uint32_t tag{ 0 };
  std::string name;
  profile::ValueType value_type{ profile::ValueType::kUnknown };
  std::vector<profile::EnumEntry> enum_values;
  std::string code_set_name;
};

struct OrchestraContext
{
  std::unordered_map<std::string, OrchestraField> fields_by_key;
  std::vector<OrchestraField> ordered_fields;
  std::unordered_map<std::string, pugi::xml_node> components_by_key;
  std::unordered_map<std::string, pugi::xml_node> groups_by_key;
  std::unordered_map<std::string, std::vector<profile::EnumEntry>> code_sets;
  std::unordered_set<std::uint32_t> emitted_groups;
  std::vector<profile::GroupDef> groups;
  std::vector<profile::ContractWarning> warnings;
};

auto
AddWarning(OrchestraContext* context, std::string code, std::string message) -> void
{
  context->warnings.push_back(profile::ContractWarning{ .code = std::move(code), .message = std::move(message) });
}

auto
RegisterNodeAliases(std::unordered_map<std::string, pugi::xml_node>* map, const pugi::xml_node& node) -> void
{
  const auto id = ParseStringAttr(node, { "id", "name" });
  const auto name = ParseStringAttr(node, { "name", "id" });
  const auto tag = ParseUintAttr(node, { "numInGroupId", "tag", "number" });
  if (!id.empty()) {
    (*map)[id] = node;
  }
  if (!name.empty()) {
    (*map)[name] = node;
  }
  if (tag.has_value()) {
    (*map)[std::to_string(tag.value())] = node;
  }
}

auto
FindSection(const pugi::xml_node& root, std::string_view section_name) -> pugi::xml_node
{
  for (const auto& child : root.children()) {
    if (LocalName(child.name()) == section_name) {
      return child;
    }
  }
  return {};
}

auto
ResolveField(const OrchestraContext& context, const pugi::xml_node& node) -> const OrchestraField*
{
  const auto id = ParseStringAttr(node, { "id", "fieldId", "tag", "number", "name" });
  const auto it = context.fields_by_key.find(id);
  if (it == context.fields_by_key.end()) {
    return nullptr;
  }
  return &it->second;
}

auto
ResolveNode(const std::unordered_map<std::string, pugi::xml_node>& map, const pugi::xml_node& node) -> pugi::xml_node
{
  const auto id = ParseStringAttr(node, { "id", "name", "groupId", "componentId" });
  const auto it = map.find(id);
  if (it == map.end()) {
    return {};
  }
  return it->second;
}

auto
TracePath(std::string_view owner, std::string_view id) -> std::string
{
  return std::string(owner) + '[' + std::string(id) + ']';
}

auto
ParseRole(std::string_view text) -> base::Result<profile::ContractRole>
{
  if (text == "initiator") {
    return profile::ContractRole::kInitiator;
  }
  if (text == "acceptor") {
    return profile::ContractRole::kAcceptor;
  }
  if (text == "any" || text.empty()) {
    return profile::ContractRole::kAny;
  }
  return base::Status::InvalidArgument("unsupported Orchestra role '" + std::string(text) + "'");
}

auto
ParseDirection(std::string_view text) -> base::Result<profile::ContractDirection>
{
  if (text == "send") {
    return profile::ContractDirection::kSend;
  }
  if (text == "receive") {
    return profile::ContractDirection::kReceive;
  }
  if (text == "both") {
    return profile::ContractDirection::kBoth;
  }
  if (text.empty()) {
    return profile::ContractDirection::kNone;
  }
  return base::Status::InvalidArgument("unsupported Orchestra direction '" + std::string(text) + "'");
}

auto
OppositeRole(profile::ContractRole role) -> profile::ContractRole
{
  switch (role) {
    case profile::ContractRole::kInitiator:
      return profile::ContractRole::kAcceptor;
    case profile::ContractRole::kAcceptor:
      return profile::ContractRole::kInitiator;
    case profile::ContractRole::kAny:
    case profile::ContractRole::kUnknown:
      break;
  }
  return profile::ContractRole::kUnknown;
}

auto
MarkMessageDirection(profile::ContractSidecar* contract,
                     std::string_view msg_type,
                     profile::ContractRole role,
                     profile::ContractDirection direction) -> base::Status
{
  const auto it = std::find_if(contract->messages.begin(), contract->messages.end(), [&](const auto& message) {
    return message.msg_type == msg_type;
  });
  if (it == contract->messages.end()) {
    return base::Status::InvalidArgument("flow references an unknown message type '" + std::string(msg_type) + "'");
  }

  const auto direction_bits = static_cast<std::uint32_t>(direction);
  if (role == profile::ContractRole::kInitiator) {
    it->initiator_direction =
      static_cast<profile::ContractDirection>(static_cast<std::uint32_t>(it->initiator_direction) | direction_bits);
  } else if (role == profile::ContractRole::kAcceptor) {
    it->acceptor_direction =
      static_cast<profile::ContractDirection>(static_cast<std::uint32_t>(it->acceptor_direction) | direction_bits);
  } else if (role == profile::ContractRole::kAny) {
    it->initiator_direction =
      static_cast<profile::ContractDirection>(static_cast<std::uint32_t>(it->initiator_direction) | direction_bits);
    it->acceptor_direction =
      static_cast<profile::ContractDirection>(static_cast<std::uint32_t>(it->acceptor_direction) | direction_bits);
  }
  return base::Status::Ok();
}

auto
MakeFieldRule(std::uint32_t tag, bool required) -> profile::FieldRule
{
  return profile::FieldRule{
    .tag = tag,
    .flags = required ? static_cast<std::uint32_t>(profile::FieldRuleFlags::kRequired) : 0U,
  };
}

auto
DeduplicateFieldRules(std::vector<profile::FieldRule>* field_rules) -> void
{
  std::unordered_set<std::uint32_t> seen;
  std::vector<profile::FieldRule> unique;
  unique.reserve(field_rules->size());
  for (const auto& field_rule : *field_rules) {
    if (!seen.insert(field_rule.tag).second) {
      continue;
    }
    unique.push_back(field_rule);
  }
  *field_rules = std::move(unique);
}

auto
FindContainer(const pugi::xml_node& node) -> pugi::xml_node
{
  for (const auto& child : node.children()) {
    const auto local = LocalName(child.name());
    if (local == "structure" || local == "members") {
      return child;
    }
  }
  return node;
}

auto
ParseRuleNode(const pugi::xml_node& rule_node,
              std::string_view msg_type,
              std::optional<std::uint32_t> inferred_field_tag,
              const OrchestraContext& context,
              std::vector<profile::ContractConditionalFieldRule>* out_rules,
              std::vector<profile::ContractWarning>* warnings,
              std::string trace_prefix) -> base::Status
{
  const auto rule_id = ParseStringAttr(rule_node, { "id", "name" });
  const auto field_id = ParseStringAttr(rule_node, { "fieldId", "fieldRef", "tag", "number" });
  std::uint32_t field_tag = inferred_field_tag.value_or(0U);
  if (!field_id.empty()) {
    const auto it = context.fields_by_key.find(field_id);
    if (it == context.fields_by_key.end()) {
      return base::Status::InvalidArgument("Orchestra rule '" + rule_id + "' references an unknown field");
    }
    field_tag = it->second.tag;
  }
  if (field_tag == 0U) {
    warnings->push_back(profile::ContractWarning{
      .code = "unsupported-rule",
      .message = "rule '" + rule_id + "' has no target field at " + trace_prefix,
    });
    return base::Status::Ok();
  }

  const auto when_field_id = ParseStringAttr(rule_node, { "whenFieldId", "whenField", "whenTag" });
  const auto when_value = ParseStringAttr(rule_node, { "whenValue", "equals" });
  const auto when_operator = ParseStringAttr(rule_node, { "whenOperator", "operator" });
  const auto presence = ParseStringAttr(rule_node, { "presence", "effect" });
  if (!when_operator.empty() && when_operator != "=" && when_operator != "eq") {
    warnings->push_back(profile::ContractWarning{
      .code = "unsupported-condition-operator",
      .message = "rule '" + rule_id + "' uses unsupported operator '" + when_operator + "' at " + trace_prefix,
    });
    return base::Status::Ok();
  }
  if (when_field_id.empty() || when_value.empty()) {
    warnings->push_back(profile::ContractWarning{
      .code = "unsupported-rule-shape",
      .message = "rule '" + rule_id + "' is missing a simple whenField/whenValue predicate at " + trace_prefix,
    });
    return base::Status::Ok();
  }

  const auto when_field_it = context.fields_by_key.find(when_field_id);
  if (when_field_it == context.fields_by_key.end()) {
    return base::Status::InvalidArgument("Orchestra rule '" + rule_id + "' references an unknown condition field");
  }

  profile::ContractConditionKind kind = profile::ContractConditionKind::kRequired;
  if (presence == "required") {
    kind = profile::ContractConditionKind::kRequired;
  } else if (presence == "forbidden") {
    kind = profile::ContractConditionKind::kForbidden;
  } else {
    warnings->push_back(profile::ContractWarning{
      .code = "unsupported-rule-presence",
      .message = "rule '" + rule_id + "' uses unsupported presence '" + presence + "' at " + trace_prefix,
    });
    return base::Status::Ok();
  }

  out_rules->push_back(profile::ContractConditionalFieldRule{
    .rule_id = rule_id,
    .msg_type = std::string(msg_type),
    .field_tag = field_tag,
    .condition = kind,
    .when_tag = when_field_it->second.tag,
    .when_value = when_value,
    .trace = profile::ContractTrace{ .source_id = rule_id, .source_path = std::move(trace_prefix) },
  });
  return base::Status::Ok();
}

auto
ExpandMembers(const pugi::xml_node& owner,
              std::string_view owner_trace,
              OrchestraContext* context,
              std::string_view msg_type,
              std::vector<profile::FieldRule>* out_field_rules,
              std::vector<profile::ContractConditionalFieldRule>* conditional_rules) -> base::Status;

auto
BuildGroup(const pugi::xml_node& group_node,
           OrchestraContext* context,
           std::vector<profile::ContractConditionalFieldRule>* conditional_rules) -> base::Result<profile::GroupDef>
{
  const auto count_tag = ParseUintAttr(group_node, { "numInGroupId", "id", "tag", "number" });
  if (!count_tag.has_value()) {
    return base::Status::InvalidArgument("Orchestra group is missing a count tag");
  }
  if (context->emitted_groups.contains(count_tag.value())) {
    for (const auto& existing : context->groups) {
      if (existing.count_tag == count_tag.value()) {
        return existing;
      }
    }
  }

  std::vector<profile::FieldRule> field_rules;
  const auto group_name = ParseStringAttr(group_node, { "name", "id" });
  auto status = ExpandMembers(group_node,
                              TracePath("group", group_name.empty() ? std::to_string(count_tag.value()) : group_name),
                              context,
                              {},
                              &field_rules,
                              conditional_rules);
  if (!status.ok()) {
    return status;
  }
  DeduplicateFieldRules(&field_rules);
  if (field_rules.empty()) {
    return base::Status::InvalidArgument("Orchestra group '" + group_name + "' has no resolved members");
  }

  profile::GroupDef group{
    .count_tag = count_tag.value(),
    .delimiter_tag = field_rules.front().tag,
    .name = group_name.empty() ? std::to_string(count_tag.value()) : group_name,
    .field_rules = std::move(field_rules),
    .flags = 0U,
  };
  context->emitted_groups.insert(count_tag.value());
  context->groups.push_back(group);
  return group;
}

auto
ExpandMembers(const pugi::xml_node& owner,
              std::string_view owner_trace,
              OrchestraContext* context,
              std::string_view msg_type,
              std::vector<profile::FieldRule>* out_field_rules,
              std::vector<profile::ContractConditionalFieldRule>* conditional_rules) -> base::Status
{
  const auto container = FindContainer(owner);
  for (const auto& child : container.children()) {
    const auto local = LocalName(child.name());
    if (local == "fieldRef" || local == "field") {
      const auto* field = ResolveField(*context, child);
      if (field == nullptr) {
        return base::Status::InvalidArgument("Orchestra member references an unknown field at " +
                                             std::string(owner_trace));
      }
      const auto required = PresenceRequired(
        ParseStringAttr(child, { "presence", "required" }), &context->warnings, std::string(owner_trace));
      out_field_rules->push_back(MakeFieldRule(field->tag, required));

      for (const auto& nested : child.children()) {
        if (LocalName(nested.name()) == "rule") {
          auto status = ParseRuleNode(nested,
                                      msg_type,
                                      field->tag,
                                      *context,
                                      conditional_rules,
                                      &context->warnings,
                                      std::string(owner_trace) + "/fieldRule");
          if (!status.ok()) {
            return status;
          }
        }
      }
      continue;
    }
    if (local == "componentRef" || local == "component") {
      const auto component_node = ResolveNode(context->components_by_key, child);
      if (!component_node) {
        return base::Status::InvalidArgument("Orchestra component reference could not be resolved at " +
                                             std::string(owner_trace));
      }
      auto status = ExpandMembers(
        component_node, std::string(owner_trace) + "/component", context, msg_type, out_field_rules, conditional_rules);
      if (!status.ok()) {
        return status;
      }
      continue;
    }
    if (local == "groupRef" || local == "group") {
      const auto group_node = ResolveNode(context->groups_by_key, child);
      if (!group_node) {
        return base::Status::InvalidArgument("Orchestra group reference could not be resolved at " +
                                             std::string(owner_trace));
      }
      auto group = BuildGroup(group_node, context, conditional_rules);
      if (!group.ok()) {
        return group.status();
      }
      const auto required = PresenceRequired(
        ParseStringAttr(child, { "presence", "required" }), &context->warnings, std::string(owner_trace));
      out_field_rules->push_back(MakeFieldRule(group.value().count_tag, required));
      continue;
    }
    if (local == "rule") {
      auto status = ParseRuleNode(child,
                                  msg_type,
                                  std::nullopt,
                                  *context,
                                  conditional_rules,
                                  &context->warnings,
                                  std::string(owner_trace) + "/rule");
      if (!status.ok()) {
        return status;
      }
      continue;
    }

    if (local == "annotation" || local == "documentation") {
      continue;
    }
    AddWarning(context,
               "unsupported-structure-member",
               "unsupported Orchestra member '" + std::string(local) + "' at " + std::string(owner_trace));
  }
  return base::Status::Ok();
}

auto
ParseCodeSets(const pugi::xml_node& root, OrchestraContext* context) -> void
{
  const auto section = FindSection(root, "codeSets");
  if (!section) {
    return;
  }
  for (const auto& code_set_node : section.children()) {
    if (LocalName(code_set_node.name()) != "codeSet") {
      continue;
    }
    const auto code_set_name = ParseStringAttr(code_set_node, { "name", "id" });
    std::vector<profile::EnumEntry> values;
    for (const auto& code_node : code_set_node.children()) {
      if (LocalName(code_node.name()) != "code") {
        continue;
      }
      values.push_back(profile::EnumEntry{
        .value = ParseStringAttr(code_node, { "value", "id" }),
        .name = ParseStringAttr(code_node, { "name", "description" }),
      });
    }
    context->code_sets.emplace(code_set_name, std::move(values));
  }
}

auto
ParseFields(const pugi::xml_node& root, OrchestraContext* context) -> base::Status
{
  const auto section = FindSection(root, "fields");
  if (!section) {
    return base::Status::InvalidArgument("Orchestra repository is missing a fields section");
  }

  std::unordered_set<std::uint32_t> seen_tags;
  for (const auto& field_node : section.children()) {
    if (LocalName(field_node.name()) != "field") {
      continue;
    }

    const auto tag = ParseUintAttr(field_node, { "id", "tag", "number" });
    if (!tag.has_value()) {
      return base::Status::InvalidArgument("Orchestra field is missing a numeric tag");
    }
    if (!seen_tags.insert(tag.value()).second) {
      return base::Status::AlreadyExists("duplicate Orchestra field tag " + std::to_string(tag.value()));
    }

    OrchestraField field;
    field.tag = tag.value();
    field.name = ParseStringAttr(field_node, { "name", "abbrName" });
    field.value_type = MapOrchestraType(ParseStringAttr(field_node, { "type", "baseType" }));
    field.code_set_name = ParseStringAttr(field_node, { "codeSet", "codeset" });
    if (!field.code_set_name.empty()) {
      const auto code_set_it = context->code_sets.find(field.code_set_name);
      if (code_set_it == context->code_sets.end()) {
        return base::Status::InvalidArgument("field '" + field.name + "' references an unknown code set");
      }
      field.enum_values = code_set_it->second;
    }
    for (const auto& value_node : field_node.children()) {
      if (LocalName(value_node.name()) != "code" && LocalName(value_node.name()) != "value") {
        continue;
      }
      field.enum_values.push_back(profile::EnumEntry{
        .value = ParseStringAttr(value_node, { "value", "enum", "id" }),
        .name = ParseStringAttr(value_node, { "name", "description" }),
      });
    }
    context->ordered_fields.push_back(field);
    context->fields_by_key[std::to_string(field.tag)] = field;
    context->fields_by_key[field.name] = field;
    const auto id = ParseStringAttr(field_node, { "id" });
    if (!id.empty()) {
      context->fields_by_key[id] = field;
    }
    const auto abbr_name = ParseStringAttr(field_node, { "abbrName" });
    if (!abbr_name.empty()) {
      context->fields_by_key[abbr_name] = field;
    }
  }

  std::sort(context->ordered_fields.begin(), context->ordered_fields.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.tag < rhs.tag;
  });
  return base::Status::Ok();
}

auto
RegisterReferences(const pugi::xml_node& root, OrchestraContext* context) -> void
{
  if (const auto components = FindSection(root, "components")) {
    for (const auto& node : components.children()) {
      if (LocalName(node.name()) == "component") {
        RegisterNodeAliases(&context->components_by_key, node);
      }
    }
  }
  if (const auto groups = FindSection(root, "groups")) {
    for (const auto& node : groups.children()) {
      if (LocalName(node.name()) == "group") {
        RegisterNodeAliases(&context->groups_by_key, node);
      }
    }
  }
}

auto
ParseMessages(const pugi::xml_node& root,
              OrchestraContext* context,
              profile::NormalizedDictionary* dictionary,
              profile::ContractSidecar* contract) -> base::Status
{
  const auto section = FindSection(root, "messages");
  if (!section) {
    return base::Status::InvalidArgument("Orchestra repository is missing a messages section");
  }

  for (const auto& message_node : section.children()) {
    if (LocalName(message_node.name()) != "message") {
      continue;
    }
    const auto msg_type = ParseStringAttr(message_node, { "msgType", "msgtype", "id" });
    if (msg_type.empty()) {
      return base::Status::InvalidArgument("Orchestra message is missing msgType");
    }
    const auto name = ParseStringAttr(message_node, { "name", "id" });
    const auto category = ParseStringAttr(message_node, { "category", "msgCat", "msgcat" });
    std::vector<profile::FieldRule> field_rules;
    auto status = ExpandMembers(
      message_node, TracePath("message", msg_type), context, msg_type, &field_rules, &contract->conditional_rules);
    if (!status.ok()) {
      return status;
    }
    DeduplicateFieldRules(&field_rules);

    const auto is_admin = category == "Admin" || category == "admin";
    dictionary->messages.push_back(profile::MessageDef{
      .msg_type = msg_type,
      .name = name,
      .field_rules = field_rules,
      .flags = is_admin ? static_cast<std::uint32_t>(profile::MessageFlags::kAdmin) : 0U,
    });
    contract->messages.push_back(profile::ContractMessage{
      .msg_type = msg_type,
      .name = name,
      .admin = is_admin,
      .initiator_direction = profile::ContractDirection::kNone,
      .acceptor_direction = profile::ContractDirection::kNone,
      .trace = profile::ContractTrace{ .source_id = msg_type, .source_path = TracePath("message", msg_type) },
    });

    for (const auto& field_rule : field_rules) {
      const auto field_it = context->fields_by_key.find(std::to_string(field_rule.tag));
      if (field_it == context->fields_by_key.end() || field_it->second.enum_values.empty()) {
        continue;
      }
      contract->enum_rules.push_back(profile::ContractEnumRule{
        .rule_id = msg_type + "-" + std::to_string(field_rule.tag) + "-codeset",
        .msg_type = msg_type,
        .field_tag = field_rule.tag,
        .allowed_values =
          [&]() {
            std::vector<std::string> values;
            values.reserve(field_it->second.enum_values.size());
            for (const auto& entry : field_it->second.enum_values) {
              values.push_back(entry.value);
            }
            return values;
          }(),
        .trace =
          profile::ContractTrace{ .source_id = field_it->second.code_set_name.empty() ? field_it->second.name
                                                                                      : field_it->second.code_set_name,
                                  .source_path = TracePath("message", msg_type) + "/codeset" },
      });
    }
  }

  return base::Status::Ok();
}

auto
ParseFlows(const pugi::xml_node& root, OrchestraContext* context, profile::ContractSidecar* contract) -> base::Status
{
  const auto section = FindSection(root, "flows");
  if (!section) {
    return base::Status::Ok();
  }

  for (const auto& flow_node : section.children()) {
    if (LocalName(flow_node.name()) != "flow") {
      continue;
    }
    const auto flow_id = ParseStringAttr(flow_node, { "id", "name" });
    const auto flow_name = ParseStringAttr(flow_node, { "name", "id" });
    for (const auto& edge_node : flow_node.children()) {
      if (LocalName(edge_node.name()) != "edge") {
        continue;
      }
      auto from_role = ParseRole(ParseStringAttr(edge_node, { "fromRole", "from" }));
      if (!from_role.ok()) {
        return from_role.status();
      }
      auto to_role = ParseRole(ParseStringAttr(edge_node, { "toRole", "to" }));
      if (!to_role.ok()) {
        return to_role.status();
      }
      const auto from_msg_type = ParseStringAttr(edge_node, { "fromMsgType", "msgType", "requestMsgType" });
      const auto to_msg_type = ParseStringAttr(edge_node, { "toMsgType", "responseMsgType" });
      if (from_msg_type.empty()) {
        AddWarning(
          context, "unsupported-flow-shape", "flow edge '" + flow_id + "' is missing fromMsgType and was skipped");
        continue;
      }

      auto status = MarkMessageDirection(contract, from_msg_type, from_role.value(), profile::ContractDirection::kSend);
      if (!status.ok()) {
        return status;
      }
      const auto opposite_from = OppositeRole(from_role.value());
      if (opposite_from != profile::ContractRole::kUnknown) {
        status = MarkMessageDirection(contract, from_msg_type, opposite_from, profile::ContractDirection::kReceive);
        if (!status.ok()) {
          return status;
        }
      }

      if (!to_msg_type.empty()) {
        status = MarkMessageDirection(contract, to_msg_type, to_role.value(), profile::ContractDirection::kSend);
        if (!status.ok()) {
          return status;
        }
        const auto opposite_to = OppositeRole(to_role.value());
        if (opposite_to != profile::ContractRole::kUnknown) {
          status = MarkMessageDirection(contract, to_msg_type, opposite_to, profile::ContractDirection::kReceive);
          if (!status.ok()) {
            return status;
          }
        }
      }

      contract->flow_edges.push_back(profile::ContractFlowEdge{
        .edge_id = flow_id,
        .from_role = from_role.value(),
        .from_msg_type = from_msg_type,
        .to_role = to_role.value(),
        .to_msg_type = to_msg_type,
        .name = flow_name,
        .trace = profile::ContractTrace{ .source_id = flow_id, .source_path = TracePath("flow", flow_id) },
      });
    }
  }

  return base::Status::Ok();
}

auto
ParseServiceSubsets(const pugi::xml_node& root, OrchestraContext* context, profile::ContractSidecar* contract)
  -> base::Status
{
  const auto service_section = FindSection(root, "serviceSubsets");
  const auto services_section = FindSection(root, "services");
  const auto section = service_section ? service_section : services_section;
  if (!section) {
    return base::Status::Ok();
  }

  for (const auto& service_node : section.children()) {
    const auto local = LocalName(service_node.name());
    if (local != "serviceSubset" && local != "service") {
      continue;
    }
    const auto service_name = ParseStringAttr(service_node, { "name", "id" });
    for (const auto& message_ref : service_node.children()) {
      if (LocalName(message_ref.name()) != "messageRef" && LocalName(message_ref.name()) != "message") {
        continue;
      }
      const auto msg_type = ParseStringAttr(message_ref, { "msgType", "msgtype", "messageType" });
      auto role = ParseRole(ParseStringAttr(message_ref, { "role", "actor" }));
      if (!role.ok()) {
        return role.status();
      }
      const auto direction_text = ParseStringAttr(message_ref, { "direction" });
      auto direction = direction_text.empty()
                         ? base::Result<profile::ContractDirection>(profile::ContractDirection::kBoth)
                         : ParseDirection(direction_text);
      if (!direction.ok()) {
        return direction.status();
      }
      const auto message_it = std::find_if(contract->messages.begin(), contract->messages.end(), [&](const auto& msg) {
        return msg.msg_type == msg_type;
      });
      if (message_it == contract->messages.end()) {
        return base::Status::InvalidArgument("service subset '" + service_name + "' references an unknown message");
      }
      contract->service_messages.push_back(profile::ContractServiceMessage{
        .service_name = service_name,
        .role = role.value(),
        .direction = direction.value(),
        .msg_type = msg_type,
        .trace = profile::ContractTrace{ .source_id = service_name, .source_path = TracePath("service", service_name) },
      });
    }
  }

  return base::Status::Ok();
}

auto
FinalizeContract(profile::ContractSidecar* contract) -> void
{
  std::set<std::string> semantics{
    "conditional-field-rules", "enum-code-constraints", "flow-edges", "role-direction-restrictions", "service-subsets",
  };
  contract->supported_semantics.assign(semantics.begin(), semantics.end());
}

auto
MakeDictionary(const OrchestraContext& context, std::uint64_t profile_id) -> profile::NormalizedDictionary
{
  profile::NormalizedDictionary dictionary;
  dictionary.profile_id = profile_id;
  dictionary.fields.reserve(context.ordered_fields.size());
  for (const auto& field : context.ordered_fields) {
    dictionary.fields.push_back(profile::FieldDef{
      .tag = field.tag,
      .name = field.name,
      .value_type = field.value_type,
      .flags = 0U,
      .enum_values = field.enum_values,
    });
  }
  dictionary.groups = context.groups;
  return dictionary;
}

auto
SerializeDictionaryBody(const profile::NormalizedDictionary& dictionary) -> std::string
{
  std::ostringstream body;
  for (const auto& field : dictionary.fields) {
    body << "field|" << field.tag << '|' << field.name << '|' << ValueTypeToNfdType(field.value_type) << "|0\n";
    for (const auto& entry : field.enum_values) {
      body << "enum|" << field.tag << '|' << entry.value << '|' << entry.name << '\n';
    }
  }
  body << '\n';
  for (const auto& message : dictionary.messages) {
    body << "message|" << message.msg_type << '|' << message.name << '|' << message.flags << '|';
    for (std::size_t index = 0; index < message.field_rules.size(); ++index) {
      if (index != 0U) {
        body << ',';
      }
      body << message.field_rules[index].tag << ':' << (message.field_rules[index].required() ? 'r' : 'o');
    }
    body << '\n';
  }
  body << '\n';
  for (const auto& group : dictionary.groups) {
    body << "group|" << group.count_tag << '|' << group.delimiter_tag << '|' << group.name << "|0|";
    for (std::size_t index = 0; index < group.field_rules.size(); ++index) {
      if (index != 0U) {
        body << ',';
      }
      body << group.field_rules[index].tag << ':' << (group.field_rules[index].required() ? 'r' : 'o');
    }
    body << '\n';
  }
  return body.str();
}

} // namespace

auto
ImportOrchestraXml(std::string_view xml_content, std::uint64_t profile_id, std::string_view source_name)
  -> base::Result<OrchestraImportResult>
{
  pugi::xml_document document;
  const auto parse_result = document.load_buffer(xml_content.data(), xml_content.size());
  if (!parse_result) {
    return base::Status::FormatError(std::string("failed to parse Orchestra XML: ") + parse_result.description());
  }

  auto root = document.document_element();
  if (!root) {
    return base::Status::FormatError("Orchestra XML is empty");
  }

  OrchestraContext context;
  ParseCodeSets(root, &context);
  auto field_status = ParseFields(root, &context);
  if (!field_status.ok()) {
    return field_status;
  }
  RegisterReferences(root, &context);

  OrchestraImportResult result;
  result.dictionary = MakeDictionary(context, profile_id);
  result.contract.profile_id = profile_id;
  result.contract.source_kind = "fix-orchestra";
  result.contract.source_name =
    source_name.empty() ? ParseStringAttr(root, { "name", "id" }) : std::string(source_name);

  auto message_status = ParseMessages(root, &context, &result.dictionary, &result.contract);
  if (!message_status.ok()) {
    return message_status;
  }
  auto flow_status = ParseFlows(root, &context, &result.contract);
  if (!flow_status.ok()) {
    return flow_status;
  }
  auto service_status = ParseServiceSubsets(root, &context, &result.contract);
  if (!service_status.ok()) {
    return service_status;
  }

  std::sort(context.groups.begin(), context.groups.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.count_tag < rhs.count_tag;
  });
  result.dictionary.groups = context.groups;

  auto body = SerializeDictionaryBody(result.dictionary);
  result.dictionary.schema_hash = Fnv1a64(body);
  result.contract.schema_hash = result.dictionary.schema_hash;
  result.contract.warnings = context.warnings;
  FinalizeContract(&result.contract);

  auto normalized_contract = profile::LoadContractSidecarText(profile::SerializeContractSidecar(result.contract));
  if (!normalized_contract.ok()) {
    return normalized_contract.status();
  }
  result.contract = std::move(normalized_contract).value();

  result.warnings.reserve(result.contract.warnings.size());
  for (const auto& warning : result.contract.warnings) {
    result.warnings.push_back(warning.code + ": " + warning.message);
  }

  return result;
}

auto
SerializeDictionaryAsNfd(const profile::NormalizedDictionary& dictionary) -> std::string
{
  auto body = SerializeDictionaryBody(dictionary);
  const auto schema_hash = Fnv1a64(body);

  std::ostringstream stream;
  stream << "profile_id=" << dictionary.profile_id << '\n';
  stream << "schema_hash=0x" << std::hex << std::setfill('0') << std::setw(16) << schema_hash << std::dec << "\n\n";
  stream << body;
  return stream.str();
}

auto
ResolveOrchestraCacheLayout(const std::filesystem::path& cache_root, std::uint64_t profile_id) -> OrchestraCacheLayout
{
  const auto root = cache_root / "profiles" / std::to_string(profile_id);
  return OrchestraCacheLayout{
    .root = root,
    .dictionary_nfd = root / "dictionary.nfd",
    .contract_sidecar = root / "contract.nfct",
  };
}

} // namespace nimble::tools