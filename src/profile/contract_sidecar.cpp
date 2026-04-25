#include "nimblefix/profile/contract_sidecar.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <set>
#include <sstream>
#include <tuple>
#include <type_traits>
#include <unordered_set>

namespace nimble::profile {

namespace {

constexpr char kHeaderAssignmentSeparator = '=';
constexpr char kRecordSeparator = '|';
constexpr char kEscapePrefix = '\\';

auto
SerializeWithoutContractId(const ContractSidecar& contract) -> std::string;

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
Trim(std::string_view input) -> std::string_view
{
  std::size_t begin = 0;
  while (begin < input.size() && std::isspace(static_cast<unsigned char>(input[begin])) != 0) {
    ++begin;
  }

  std::size_t end = input.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
    --end;
  }

  return input.substr(begin, end - begin);
}

auto
EscapeToken(std::string_view input) -> std::string
{
  std::string escaped;
  escaped.reserve(input.size());
  for (const auto ch : input) {
    if (ch == kEscapePrefix || ch == kRecordSeparator || ch == '\n' || ch == '\r') {
      escaped.push_back(kEscapePrefix);
      if (ch == '\n') {
        escaped.push_back('n');
      } else if (ch == '\r') {
        escaped.push_back('r');
      } else {
        escaped.push_back(ch);
      }
      continue;
    }
    escaped.push_back(ch);
  }
  return escaped;
}

auto
UnescapeToken(std::string_view input) -> base::Result<std::string>
{
  std::string value;
  value.reserve(input.size());
  for (std::size_t index = 0; index < input.size(); ++index) {
    const auto ch = input[index];
    if (ch != kEscapePrefix) {
      value.push_back(ch);
      continue;
    }
    if (index + 1 >= input.size()) {
      return base::Status::FormatError("contract sidecar contains a trailing escape");
    }
    const auto escaped = input[++index];
    if (escaped == 'n') {
      value.push_back('\n');
    } else if (escaped == 'r') {
      value.push_back('\r');
    } else if (escaped == kEscapePrefix || escaped == kRecordSeparator) {
      value.push_back(escaped);
    } else {
      return base::Status::FormatError("contract sidecar contains an unsupported escape sequence");
    }
  }
  return value;
}

auto
SplitEscaped(std::string_view input, char delimiter) -> base::Result<std::vector<std::string>>
{
  std::vector<std::string> parts;
  std::string current;
  bool escaping = false;
  for (const auto ch : input) {
    if (escaping) {
      current.push_back(kEscapePrefix);
      current.push_back(ch);
      escaping = false;
      continue;
    }
    if (ch == kEscapePrefix) {
      escaping = true;
      continue;
    }
    if (ch == delimiter) {
      auto unescaped = UnescapeToken(current);
      if (!unescaped.ok()) {
        return unescaped.status();
      }
      parts.push_back(std::move(unescaped).value());
      current.clear();
      continue;
    }
    current.push_back(ch);
  }
  if (escaping) {
    return base::Status::FormatError("contract sidecar contains an unterminated escape sequence");
  }
  auto unescaped = UnescapeToken(current);
  if (!unescaped.ok()) {
    return unescaped.status();
  }
  parts.push_back(std::move(unescaped).value());
  return parts;
}

auto
SerializeSemantics(std::span<const std::string> semantics) -> std::string
{
  std::ostringstream stream;
  for (std::size_t index = 0; index < semantics.size(); ++index) {
    if (index != 0U) {
      stream << ',';
    }
    stream << EscapeToken(semantics[index]);
  }
  return stream.str();
}

auto
ParseSemantics(std::string_view token) -> base::Result<std::vector<std::string>>
{
  if (Trim(token).empty()) {
    return std::vector<std::string>{};
  }
  return SplitEscaped(token, ',');
}

auto
CanonicalizeStringVector(std::vector<std::string>* values) -> void
{
  std::sort(values->begin(), values->end());
  values->erase(std::unique(values->begin(), values->end()), values->end());
}

auto
CanonicalizeContract(ContractSidecar* contract) -> void
{
  CanonicalizeStringVector(&contract->supported_semantics);

  std::sort(contract->warnings.begin(), contract->warnings.end(), [](const auto& lhs, const auto& rhs) {
    return std::tie(lhs.code, lhs.message) < std::tie(rhs.code, rhs.message);
  });

  std::sort(contract->messages.begin(), contract->messages.end(), [](const auto& lhs, const auto& rhs) {
    return std::tie(lhs.msg_type,
                    lhs.name,
                    lhs.admin,
                    lhs.initiator_direction,
                    lhs.acceptor_direction,
                    lhs.trace.source_id,
                    lhs.trace.source_path) < std::tie(rhs.msg_type,
                                                      rhs.name,
                                                      rhs.admin,
                                                      rhs.initiator_direction,
                                                      rhs.acceptor_direction,
                                                      rhs.trace.source_id,
                                                      rhs.trace.source_path);
  });

  std::sort(
    contract->conditional_rules.begin(), contract->conditional_rules.end(), [](const auto& lhs, const auto& rhs) {
      return std::tie(lhs.rule_id,
                      lhs.msg_type,
                      lhs.field_tag,
                      lhs.condition,
                      lhs.when_tag,
                      lhs.when_value,
                      lhs.trace.source_id,
                      lhs.trace.source_path) < std::tie(rhs.rule_id,
                                                        rhs.msg_type,
                                                        rhs.field_tag,
                                                        rhs.condition,
                                                        rhs.when_tag,
                                                        rhs.when_value,
                                                        rhs.trace.source_id,
                                                        rhs.trace.source_path);
    });

  for (auto& rule : contract->enum_rules) {
    CanonicalizeStringVector(&rule.allowed_values);
  }
  std::sort(contract->enum_rules.begin(), contract->enum_rules.end(), [](const auto& lhs, const auto& rhs) {
    return std::tie(
             lhs.rule_id, lhs.msg_type, lhs.field_tag, lhs.allowed_values, lhs.trace.source_id, lhs.trace.source_path) <
           std::tie(
             rhs.rule_id, rhs.msg_type, rhs.field_tag, rhs.allowed_values, rhs.trace.source_id, rhs.trace.source_path);
  });

  std::sort(contract->service_messages.begin(), contract->service_messages.end(), [](const auto& lhs, const auto& rhs) {
    return std::tie(
             lhs.service_name, lhs.role, lhs.direction, lhs.msg_type, lhs.trace.source_id, lhs.trace.source_path) <
           std::tie(
             rhs.service_name, rhs.role, rhs.direction, rhs.msg_type, rhs.trace.source_id, rhs.trace.source_path);
  });

  std::sort(contract->flow_edges.begin(), contract->flow_edges.end(), [](const auto& lhs, const auto& rhs) {
    return std::tie(lhs.edge_id,
                    lhs.from_role,
                    lhs.from_msg_type,
                    lhs.to_role,
                    lhs.to_msg_type,
                    lhs.name,
                    lhs.trace.source_id,
                    lhs.trace.source_path) < std::tie(rhs.edge_id,
                                                      rhs.from_role,
                                                      rhs.from_msg_type,
                                                      rhs.to_role,
                                                      rhs.to_msg_type,
                                                      rhs.name,
                                                      rhs.trace.source_id,
                                                      rhs.trace.source_path);
  });
}

auto
ComputeContractId(const ContractSidecar& contract) -> std::uint64_t
{
  ContractSidecar canonical = contract;
  canonical.contract_id = 0U;
  CanonicalizeContract(&canonical);
  return Fnv1a64(SerializeWithoutContractId(canonical));
}

auto
RoleToString(ContractRole role) -> std::string_view
{
  switch (role) {
    case ContractRole::kInitiator:
      return "initiator";
    case ContractRole::kAcceptor:
      return "acceptor";
    case ContractRole::kAny:
      return "any";
    case ContractRole::kUnknown:
      break;
  }
  return "unknown";
}

auto
ParseRole(std::string_view token) -> base::Result<ContractRole>
{
  const auto value = Trim(token);
  if (value == "initiator") {
    return ContractRole::kInitiator;
  }
  if (value == "acceptor") {
    return ContractRole::kAcceptor;
  }
  if (value == "any") {
    return ContractRole::kAny;
  }
  if (value == "unknown") {
    return ContractRole::kUnknown;
  }
  return base::Status::FormatError("contract sidecar contains an unknown role");
}

auto
DirectionToString(ContractDirection direction) -> std::string_view
{
  switch (direction) {
    case ContractDirection::kNone:
      return "none";
    case ContractDirection::kSend:
      return "send";
    case ContractDirection::kReceive:
      return "receive";
    case ContractDirection::kBoth:
      return "both";
  }
  return "none";
}

auto
ParseDirection(std::string_view token) -> base::Result<ContractDirection>
{
  const auto value = Trim(token);
  if (value == "none") {
    return ContractDirection::kNone;
  }
  if (value == "send") {
    return ContractDirection::kSend;
  }
  if (value == "receive") {
    return ContractDirection::kReceive;
  }
  if (value == "both") {
    return ContractDirection::kBoth;
  }
  return base::Status::FormatError("contract sidecar contains an unknown direction");
}

auto
ConditionKindToString(ContractConditionKind kind) -> std::string_view
{
  switch (kind) {
    case ContractConditionKind::kRequired:
      return "required";
    case ContractConditionKind::kForbidden:
      return "forbidden";
  }
  return "required";
}

auto
ParseConditionKind(std::string_view token) -> base::Result<ContractConditionKind>
{
  const auto value = Trim(token);
  if (value == "required") {
    return ContractConditionKind::kRequired;
  }
  if (value == "forbidden") {
    return ContractConditionKind::kForbidden;
  }
  return base::Status::FormatError("contract sidecar contains an unknown conditional rule kind");
}

template<typename Integer>
auto
ParseInteger(std::string_view token, const char* label) -> base::Result<Integer>
{
  try {
    if constexpr (std::is_same_v<Integer, std::uint64_t>) {
      return static_cast<Integer>(std::stoull(std::string(token), nullptr, 0));
    }
    return static_cast<Integer>(std::stoul(std::string(token), nullptr, 0));
  } catch (...) {
    return base::Status::FormatError(std::string("contract sidecar has an invalid ") + label);
  }
}

auto
AppendRecord(std::ostringstream* stream, std::initializer_list<std::string_view> fields) -> void
{
  std::size_t index = 0U;
  for (const auto field : fields) {
    if (index != 0U) {
      *stream << kRecordSeparator;
    }
    *stream << EscapeToken(field);
    ++index;
  }
  *stream << '\n';
}

auto
SerializeWithoutContractId(const ContractSidecar& contract) -> std::string
{
  std::ostringstream stream;
  stream << "contract_format=" << kContractSidecarFormat << '\n';
  stream << "profile_id=" << contract.profile_id << '\n';
  stream << "schema_hash=0x" << std::hex << contract.schema_hash << std::dec << '\n';
  stream << "source_kind=" << EscapeToken(contract.source_kind) << '\n';
  stream << "source_name=" << EscapeToken(contract.source_name) << '\n';
  stream << "supported_semantics=" << SerializeSemantics(contract.supported_semantics) << '\n';
  stream << '\n';

  for (const auto& warning : contract.warnings) {
    AppendRecord(&stream, { "warning", warning.code, warning.message });
  }
  for (const auto& message : contract.messages) {
    AppendRecord(&stream,
                 { "message",
                   message.msg_type,
                   message.name,
                   message.admin ? "admin" : "app",
                   DirectionToString(message.initiator_direction),
                   DirectionToString(message.acceptor_direction),
                   message.trace.source_id,
                   message.trace.source_path });
  }
  for (const auto& rule : contract.conditional_rules) {
    AppendRecord(&stream,
                 { "conditional",
                   rule.rule_id,
                   rule.msg_type,
                   std::to_string(rule.field_tag),
                   ConditionKindToString(rule.condition),
                   std::to_string(rule.when_tag),
                   rule.when_value,
                   rule.trace.source_id,
                   rule.trace.source_path });
  }
  for (const auto& rule : contract.enum_rules) {
    for (const auto& value : rule.allowed_values) {
      AppendRecord(&stream,
                   { "enum",
                     rule.rule_id,
                     rule.msg_type,
                     std::to_string(rule.field_tag),
                     value,
                     rule.trace.source_id,
                     rule.trace.source_path });
    }
  }
  for (const auto& service_message : contract.service_messages) {
    AppendRecord(&stream,
                 { "service",
                   service_message.service_name,
                   RoleToString(service_message.role),
                   DirectionToString(service_message.direction),
                   service_message.msg_type,
                   service_message.trace.source_id,
                   service_message.trace.source_path });
  }
  for (const auto& edge : contract.flow_edges) {
    AppendRecord(&stream,
                 { "flow",
                   edge.edge_id,
                   RoleToString(edge.from_role),
                   edge.from_msg_type,
                   RoleToString(edge.to_role),
                   edge.to_msg_type,
                   edge.name,
                   edge.trace.source_id,
                   edge.trace.source_path });
  }

  return stream.str();
}

auto
BuildScenarioBody(std::string_view msg_type, std::span<const ContractConditionalFieldRule> rules) -> std::string
{
  if (msg_type == "D") {
    bool has_price_rule = false;
    for (const auto& rule : rules) {
      if (rule.msg_type == msg_type && rule.field_tag == 44U && rule.when_tag == 40U && rule.when_value == "2") {
        has_price_rule = true;
        break;
      }
    }
    if (has_price_rule) {
      return "body=35=D|11=CL1|55=IBM|54=1|60=20260426-12:00:00.000|38=100|40=2";
    }
    return "body=35=D|11=CL1|55=IBM|54=1|60=20260426-12:00:00.000|38=100|40=1";
  }
  return std::string("body=35=") + std::string(msg_type);
}

} // namespace

auto
SerializeContractSidecar(const ContractSidecar& contract) -> std::string
{
  ContractSidecar copy = contract;
  CanonicalizeContract(&copy);
  copy.contract_id = ComputeContractId(copy);

  std::ostringstream stream;
  stream << "contract_id=0x" << std::hex << copy.contract_id << std::dec << '\n';
  stream << SerializeWithoutContractId(copy);
  return stream.str();
}

auto
LoadContractSidecarText(std::string_view text) -> base::Result<ContractSidecar>
{
  ContractSidecar contract;
  std::vector<std::string> lines;
  std::string current;
  for (const auto ch : text) {
    if (ch == '\r') {
      continue;
    }
    if (ch == '\n') {
      lines.push_back(current);
      current.clear();
      continue;
    }
    current.push_back(ch);
  }
  if (!current.empty() || text.empty()) {
    lines.push_back(std::move(current));
  }

  bool saw_format = false;
  bool saw_contract_id = false;
  bool saw_profile_id = false;
  for (const auto& raw_line : lines) {
    const auto trimmed = Trim(raw_line);
    if (trimmed.empty() || trimmed.starts_with('#')) {
      continue;
    }

    if (trimmed.find(kRecordSeparator) == std::string_view::npos) {
      const auto eq = trimmed.find(kHeaderAssignmentSeparator);
      if (eq == std::string_view::npos) {
        return base::Status::FormatError("contract sidecar contains an invalid header line");
      }
      const auto key = Trim(trimmed.substr(0, eq));
      const auto value = Trim(trimmed.substr(eq + 1U));
      if (key == "contract_format") {
        if (value != kContractSidecarFormat) {
          return base::Status::VersionMismatch("contract sidecar has an unsupported format version");
        }
        saw_format = true;
      } else if (key == "contract_id") {
        auto parsed = ParseInteger<std::uint64_t>(value, "contract_id");
        if (!parsed.ok()) {
          return parsed.status();
        }
        contract.contract_id = parsed.value();
        saw_contract_id = true;
      } else if (key == "profile_id") {
        auto parsed = ParseInteger<std::uint64_t>(value, "profile_id");
        if (!parsed.ok()) {
          return parsed.status();
        }
        contract.profile_id = parsed.value();
        saw_profile_id = true;
      } else if (key == "schema_hash") {
        auto parsed = ParseInteger<std::uint64_t>(value, "schema_hash");
        if (!parsed.ok()) {
          return parsed.status();
        }
        contract.schema_hash = parsed.value();
      } else if (key == "source_kind") {
        auto unescaped = UnescapeToken(value);
        if (!unescaped.ok()) {
          return unescaped.status();
        }
        contract.source_kind = std::move(unescaped).value();
      } else if (key == "source_name") {
        auto unescaped = UnescapeToken(value);
        if (!unescaped.ok()) {
          return unescaped.status();
        }
        contract.source_name = std::move(unescaped).value();
      } else if (key == "supported_semantics") {
        auto semantics = ParseSemantics(value);
        if (!semantics.ok()) {
          return semantics.status();
        }
        contract.supported_semantics = std::move(semantics).value();
      } else {
        return base::Status::FormatError("contract sidecar contains an unknown header key");
      }
      continue;
    }

    auto parts = SplitEscaped(trimmed, kRecordSeparator);
    if (!parts.ok()) {
      return parts.status();
    }
    if (parts.value().empty()) {
      continue;
    }

    const auto& record = parts.value();
    if (record[0] == "warning") {
      if (record.size() != 3U) {
        return base::Status::FormatError("contract sidecar warning record has an invalid field count");
      }
      contract.warnings.push_back(ContractWarning{ .code = record[1], .message = record[2] });
      continue;
    }
    if (record[0] == "message") {
      if (record.size() != 8U) {
        return base::Status::FormatError("contract sidecar message record has an invalid field count");
      }
      auto initiator_direction = ParseDirection(record[4]);
      if (!initiator_direction.ok()) {
        return initiator_direction.status();
      }
      auto acceptor_direction = ParseDirection(record[5]);
      if (!acceptor_direction.ok()) {
        return acceptor_direction.status();
      }
      contract.messages.push_back(ContractMessage{
        .msg_type = record[1],
        .name = record[2],
        .admin = record[3] == "admin",
        .initiator_direction = initiator_direction.value(),
        .acceptor_direction = acceptor_direction.value(),
        .trace = ContractTrace{ .source_id = record[6], .source_path = record[7] },
      });
      continue;
    }
    if (record[0] == "conditional") {
      if (record.size() != 9U) {
        return base::Status::FormatError("contract sidecar conditional record has an invalid field count");
      }
      auto field_tag = ParseInteger<std::uint32_t>(record[3], "conditional field tag");
      if (!field_tag.ok()) {
        return field_tag.status();
      }
      auto condition = ParseConditionKind(record[4]);
      if (!condition.ok()) {
        return condition.status();
      }
      auto when_tag = ParseInteger<std::uint32_t>(record[5], "conditional when_tag");
      if (!when_tag.ok()) {
        return when_tag.status();
      }
      contract.conditional_rules.push_back(ContractConditionalFieldRule{
        .rule_id = record[1],
        .msg_type = record[2],
        .field_tag = field_tag.value(),
        .condition = condition.value(),
        .when_tag = when_tag.value(),
        .when_value = record[6],
        .trace = ContractTrace{ .source_id = record[7], .source_path = record[8] },
      });
      continue;
    }
    if (record[0] == "enum") {
      if (record.size() != 7U) {
        return base::Status::FormatError("contract sidecar enum record has an invalid field count");
      }
      auto field_tag = ParseInteger<std::uint32_t>(record[3], "enum field tag");
      if (!field_tag.ok()) {
        return field_tag.status();
      }

      auto it = std::find_if(contract.enum_rules.begin(), contract.enum_rules.end(), [&](const auto& rule) {
        return rule.rule_id == record[1] && rule.msg_type == record[2] && rule.field_tag == field_tag.value();
      });
      if (it == contract.enum_rules.end()) {
        contract.enum_rules.push_back(ContractEnumRule{
          .rule_id = record[1],
          .msg_type = record[2],
          .field_tag = field_tag.value(),
          .allowed_values = { record[4] },
          .trace = ContractTrace{ .source_id = record[5], .source_path = record[6] },
        });
      } else {
        it->allowed_values.push_back(record[4]);
      }
      continue;
    }
    if (record[0] == "service") {
      if (record.size() != 7U) {
        return base::Status::FormatError("contract sidecar service record has an invalid field count");
      }
      auto role = ParseRole(record[2]);
      if (!role.ok()) {
        return role.status();
      }
      auto direction = ParseDirection(record[3]);
      if (!direction.ok()) {
        return direction.status();
      }
      contract.service_messages.push_back(ContractServiceMessage{
        .service_name = record[1],
        .role = role.value(),
        .direction = direction.value(),
        .msg_type = record[4],
        .trace = ContractTrace{ .source_id = record[5], .source_path = record[6] },
      });
      continue;
    }
    if (record[0] == "flow") {
      if (record.size() != 9U) {
        return base::Status::FormatError("contract sidecar flow record has an invalid field count");
      }
      auto from_role = ParseRole(record[2]);
      if (!from_role.ok()) {
        return from_role.status();
      }
      auto to_role = ParseRole(record[4]);
      if (!to_role.ok()) {
        return to_role.status();
      }
      contract.flow_edges.push_back(ContractFlowEdge{
        .edge_id = record[1],
        .from_role = from_role.value(),
        .from_msg_type = record[3],
        .to_role = to_role.value(),
        .to_msg_type = record[5],
        .name = record[6],
        .trace = ContractTrace{ .source_id = record[7], .source_path = record[8] },
      });
      continue;
    }

    return base::Status::FormatError("contract sidecar contains an unknown record kind");
  }

  if (!saw_format || !saw_contract_id || !saw_profile_id) {
    return base::Status::FormatError("contract sidecar is missing required headers");
  }

  CanonicalizeContract(&contract);
  const auto expected_contract_id = ComputeContractId(contract);
  if (contract.contract_id != expected_contract_id) {
    return base::Status::FormatError("contract sidecar contract_id does not match canonical content");
  }

  return contract;
}

auto
LoadContractSidecarFile(const std::filesystem::path& path) -> base::Result<ContractSidecar>
{
  std::ifstream stream(path);
  if (!stream.is_open()) {
    return base::Status::IoError("unable to open contract sidecar: '" + path.string() + "'");
  }

  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return LoadContractSidecarText(buffer.str());
}

auto
WriteContractSidecar(const std::filesystem::path& path, const ContractSidecar& contract) -> base::Status
{
  if (const auto parent = path.parent_path(); !parent.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      return base::Status::IoError("unable to create contract sidecar directory: '" + parent.string() + "'");
    }
  }

  std::ofstream stream(path, std::ios::trunc);
  if (!stream.is_open()) {
    return base::Status::IoError("unable to write contract sidecar: '" + path.string() + "'");
  }
  stream << SerializeContractSidecar(contract);
  return base::Status::Ok();
}

auto
DirectionIncludes(ContractDirection value, ContractDirection needle) -> bool
{
  const auto value_bits = static_cast<std::uint32_t>(value);
  const auto needle_bits = static_cast<std::uint32_t>(needle);
  return (value_bits & needle_bits) == needle_bits;
}

auto
MessageDirectionForRole(const ContractMessage& message, ContractRole role) -> ContractDirection
{
  switch (role) {
    case ContractRole::kInitiator:
      return message.initiator_direction;
    case ContractRole::kAcceptor:
      return message.acceptor_direction;
    case ContractRole::kAny:
      if (message.initiator_direction == message.acceptor_direction) {
        return message.initiator_direction;
      }
      return ContractDirection::kBoth;
    case ContractRole::kUnknown:
      break;
  }
  return ContractDirection::kNone;
}

auto
ValidateContractServiceSelection(const ContractSidecar& contract, std::span<const std::string> selected_services)
  -> base::Status
{
  if (selected_services.empty()) {
    return base::Status::Ok();
  }

  std::unordered_set<std::string> available;
  for (const auto& service_message : contract.service_messages) {
    available.insert(service_message.service_name);
  }

  for (const auto& service : selected_services) {
    if (!available.contains(service)) {
      return base::Status::InvalidArgument("contract sidecar does not define service subset '" + service + "'");
    }
  }

  return base::Status::Ok();
}

auto
CollectContractMessageTypes(const ContractSidecar& contract,
                            ContractRole role,
                            ContractDirection direction,
                            std::span<const std::string> selected_services,
                            bool application_only) -> std::vector<std::string>
{
  std::set<std::string> collected;
  std::unordered_set<std::string> selected(selected_services.begin(), selected_services.end());

  if (!selected.empty()) {
    for (const auto& service_message : contract.service_messages) {
      if (!selected.contains(service_message.service_name)) {
        continue;
      }
      if (service_message.role != ContractRole::kAny && service_message.role != role) {
        continue;
      }
      if (!DirectionIncludes(service_message.direction, direction)) {
        continue;
      }

      const auto message_it = std::find_if(contract.messages.begin(), contract.messages.end(), [&](const auto& msg) {
        return msg.msg_type == service_message.msg_type;
      });
      if (message_it == contract.messages.end()) {
        continue;
      }
      if (application_only && message_it->admin) {
        continue;
      }
      collected.insert(message_it->msg_type);
    }
  } else {
    for (const auto& message : contract.messages) {
      if (application_only && message.admin) {
        continue;
      }
      if (!DirectionIncludes(MessageDirectionForRole(message, role), direction)) {
        continue;
      }
      collected.insert(message.msg_type);
    }
  }

  return { collected.begin(), collected.end() };
}

auto
RenderContractSummary(const ContractSidecar& contract) -> std::string
{
  std::ostringstream stream;
  stream << "contract_id=0x" << std::hex << contract.contract_id << std::dec << '\n';
  stream << "profile_id=" << contract.profile_id << '\n';
  stream << "schema_hash=0x" << std::hex << contract.schema_hash << std::dec << '\n';
  stream << "source=" << contract.source_kind << ':' << contract.source_name << '\n';
  stream << "messages=" << contract.messages.size() << '\n';
  stream << "conditional_rules=" << contract.conditional_rules.size() << '\n';
  stream << "enum_rules=" << contract.enum_rules.size() << '\n';
  stream << "service_messages=" << contract.service_messages.size() << '\n';
  stream << "flow_edges=" << contract.flow_edges.size() << '\n';
  stream << "warnings=" << contract.warnings.size() << '\n';
  for (const auto& warning : contract.warnings) {
    stream << "warning: " << warning.code << " - " << warning.message << '\n';
  }
  return stream.str();
}

auto
RenderContractMarkdown(const ContractSidecar& contract) -> std::string
{
  std::ostringstream stream;
  stream << "# Contract Sidecar\n\n";
  stream << "- Profile ID: `" << contract.profile_id << "`\n";
  stream << "- Contract ID: `0x" << std::hex << contract.contract_id << std::dec << "`\n";
  stream << "- Structural schema hash: `0x" << std::hex << contract.schema_hash << std::dec << "`\n";
  stream << "- Source: `" << contract.source_kind << ':' << contract.source_name << "`\n\n";

  stream << "## Message Roles\n\n";
  for (const auto& message : contract.messages) {
    stream << "- `" << message.msg_type << "` " << message.name
           << ": initiator=" << DirectionToString(message.initiator_direction)
           << ", acceptor=" << DirectionToString(message.acceptor_direction) << '\n';
  }

  stream << "\n## Conditional Rules\n\n";
  for (const auto& rule : contract.conditional_rules) {
    stream << "- `" << rule.rule_id << "`: `" << rule.msg_type << "` field `" << rule.field_tag << "` becomes `"
           << ConditionKindToString(rule.condition) << "` when `" << rule.when_tag << '=' << rule.when_value << "` (`"
           << rule.trace.source_id << "`)\n";
  }

  stream << "\n## Enum Rules\n\n";
  for (const auto& rule : contract.enum_rules) {
    stream << "- `" << rule.rule_id << "`: `" << rule.msg_type << "` field `" << rule.field_tag << "` allows ";
    for (std::size_t index = 0; index < rule.allowed_values.size(); ++index) {
      if (index != 0U) {
        stream << ", ";
      }
      stream << '`' << rule.allowed_values[index] << '`';
    }
    stream << "\n";
  }

  stream << "\n## Service Subsets\n\n";
  for (const auto& service_message : contract.service_messages) {
    stream << "- `" << service_message.service_name << "`: " << RoleToString(service_message.role) << ' '
           << DirectionToString(service_message.direction) << " `" << service_message.msg_type << "`\n";
  }

  stream << "\n## Flow Edges\n\n";
  for (const auto& edge : contract.flow_edges) {
    stream << "- `" << edge.edge_id << "`: " << RoleToString(edge.from_role) << " sends `" << edge.from_msg_type
           << "`, then " << RoleToString(edge.to_role) << " sends `" << edge.to_msg_type << "`\n";
  }

  if (!contract.warnings.empty()) {
    stream << "\n## Import Warnings\n\n";
    for (const auto& warning : contract.warnings) {
      stream << "- `" << warning.code << "`: " << warning.message << '\n';
    }
  }

  return stream.str();
}

auto
GenerateInteropScenarioAugmentations(const ContractSidecar& contract) -> std::vector<GeneratedContractScenario>
{
  std::vector<GeneratedContractScenario> generated;

  for (const auto& rule : contract.conditional_rules) {
    std::ostringstream text;
    text << "config=REPLACE_ME.nfcfg\n";
    text << "# generated from contract rule " << rule.rule_id << "\n";
    text << "action|1|protocol-inbound|seq=1|ts=0|" << BuildScenarioBody(rule.msg_type, contract.conditional_rules)
         << "\n";
    text << "expect-action|1|outbound=1\n";
    text << "expect-outbound|1|1|msg-type=j|business-reject-reason=5|text-contains=";
    text << (rule.condition == ContractConditionKind::kRequired ? "require" : "forbid");
    text << "\n";
    generated.push_back(GeneratedContractScenario{
      .file_name = rule.rule_id + ".nfscenario",
      .description = "Conditional contract rule " + rule.rule_id,
      .text = text.str(),
    });
  }

  for (const auto& edge : contract.flow_edges) {
    std::ostringstream text;
    text << "config=REPLACE_ME.nfcfg\n";
    text << "# generated from contract flow edge " << edge.edge_id << "\n";
    text << "action|1|protocol-connect|ts=0\n";
    text << "action|1|protocol-inbound|seq=1|ts=0|body=35=A|98=0|108=30\n";
    text << "action|1|protocol-inbound|seq=2|ts=1|body=35=" << edge.from_msg_type << "\n";
    text << "# expected peer follow-up message: " << edge.to_msg_type << " from " << RoleToString(edge.to_role) << "\n";
    generated.push_back(GeneratedContractScenario{
      .file_name = edge.edge_id + ".nfscenario",
      .description = "Flow edge " + edge.edge_id,
      .text = text.str(),
    });
  }

  return generated;
}

} // namespace nimble::profile