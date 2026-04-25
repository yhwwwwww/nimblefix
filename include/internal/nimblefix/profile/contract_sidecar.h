#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "nimblefix/base/result.h"
#include "nimblefix/base/status.h"

namespace nimble::profile {

inline constexpr std::string_view kContractSidecarFormat = "nimblefix-contract-v1";

enum class ContractRole : std::uint32_t
{
  kUnknown = 0,
  kInitiator,
  kAcceptor,
  kAny,
};

enum class ContractDirection : std::uint32_t
{
  kNone = 0,
  kSend = 1U << 0,
  kReceive = 1U << 1,
  kBoth = kSend | kReceive,
};

enum class ContractConditionKind : std::uint32_t
{
  kRequired = 0,
  kForbidden,
};

struct ContractTrace
{
  std::string source_id;
  std::string source_path;
};

struct ContractWarning
{
  std::string code;
  std::string message;
};

struct ContractMessage
{
  std::string msg_type;
  std::string name;
  bool admin{ false };
  ContractDirection initiator_direction{ ContractDirection::kNone };
  ContractDirection acceptor_direction{ ContractDirection::kNone };
  ContractTrace trace;
};

struct ContractConditionalFieldRule
{
  std::string rule_id;
  std::string msg_type;
  std::uint32_t field_tag{ 0 };
  ContractConditionKind condition{ ContractConditionKind::kRequired };
  std::uint32_t when_tag{ 0 };
  std::string when_value;
  ContractTrace trace;
};

struct ContractEnumRule
{
  std::string rule_id;
  std::string msg_type;
  std::uint32_t field_tag{ 0 };
  std::vector<std::string> allowed_values;
  ContractTrace trace;
};

struct ContractServiceMessage
{
  std::string service_name;
  ContractRole role{ ContractRole::kUnknown };
  ContractDirection direction{ ContractDirection::kNone };
  std::string msg_type;
  ContractTrace trace;
};

struct ContractFlowEdge
{
  std::string edge_id;
  ContractRole from_role{ ContractRole::kUnknown };
  std::string from_msg_type;
  ContractRole to_role{ ContractRole::kUnknown };
  std::string to_msg_type;
  std::string name;
  ContractTrace trace;
};

struct ContractSidecar
{
  std::uint64_t profile_id{ 0 };
  std::uint64_t contract_id{ 0 };
  std::uint64_t schema_hash{ 0 };
  std::string source_kind;
  std::string source_name;
  std::vector<std::string> supported_semantics;
  std::vector<ContractWarning> warnings;
  std::vector<ContractMessage> messages;
  std::vector<ContractConditionalFieldRule> conditional_rules;
  std::vector<ContractEnumRule> enum_rules;
  std::vector<ContractServiceMessage> service_messages;
  std::vector<ContractFlowEdge> flow_edges;
};

struct GeneratedContractScenario
{
  std::string file_name;
  std::string description;
  std::string text;
};

auto
SerializeContractSidecar(const ContractSidecar& contract) -> std::string;

auto
LoadContractSidecarText(std::string_view text) -> base::Result<ContractSidecar>;

auto
LoadContractSidecarFile(const std::filesystem::path& path) -> base::Result<ContractSidecar>;

auto
WriteContractSidecar(const std::filesystem::path& path, const ContractSidecar& contract) -> base::Status;

[[nodiscard]] auto
DirectionIncludes(ContractDirection value, ContractDirection needle) -> bool;

[[nodiscard]] auto
MessageDirectionForRole(const ContractMessage& message, ContractRole role) -> ContractDirection;

auto
ValidateContractServiceSelection(const ContractSidecar& contract, std::span<const std::string> selected_services)
  -> base::Status;

auto
CollectContractMessageTypes(const ContractSidecar& contract,
                            ContractRole role,
                            ContractDirection direction,
                            std::span<const std::string> selected_services,
                            bool application_only) -> std::vector<std::string>;

auto
RenderContractSummary(const ContractSidecar& contract) -> std::string;

auto
RenderContractMarkdown(const ContractSidecar& contract) -> std::string;

auto
GenerateInteropScenarioAugmentations(const ContractSidecar& contract) -> std::vector<GeneratedContractScenario>;

} // namespace nimble::profile