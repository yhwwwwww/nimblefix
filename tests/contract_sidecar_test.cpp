#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "nimblefix/profile/artifact_builder.h"
#include "nimblefix/profile/contract_sidecar.h"
#include "nimblefix/runtime/contract_binding.h"
#include "nimblefix/runtime/engine.h"
#include "nimblefix/runtime/internal_config_parser.h"

namespace {

auto
Contains(std::string_view haystack, std::string_view needle) -> bool
{
  return haystack.find(needle) != std::string_view::npos;
}

auto
MakeRule(std::uint32_t tag, bool required) -> nimble::profile::FieldRule
{
  return nimble::profile::FieldRule{
    .tag = tag,
    .flags = required ? static_cast<std::uint32_t>(nimble::profile::FieldRuleFlags::kRequired) : 0U,
  };
}

auto
MakeDictionary(std::uint64_t profile_id, std::uint64_t schema_hash) -> nimble::profile::NormalizedDictionary
{
  using nimble::profile::EnumEntry;
  using nimble::profile::FieldDef;
  using nimble::profile::MessageDef;
  using nimble::profile::MessageFlags;
  using nimble::profile::NormalizedDictionary;
  using nimble::profile::ValueType;

  return NormalizedDictionary{
    .schema_hash = schema_hash,
    .profile_id = profile_id,
    .fields = {
      FieldDef{ .tag = 11U, .name = "ClOrdID", .value_type = ValueType::kString },
      FieldDef{ .tag = 35U, .name = "MsgType", .value_type = ValueType::kString },
      FieldDef{ .tag = 38U, .name = "OrderQty", .value_type = ValueType::kInt },
      FieldDef{ .tag = 40U,
                .name = "OrdType",
                .value_type = ValueType::kChar,
                .enum_values = { EnumEntry{ .value = "1", .name = "Market" },
                                 EnumEntry{ .value = "2", .name = "Limit" } } },
      FieldDef{ .tag = 44U, .name = "Price", .value_type = ValueType::kFloat },
      FieldDef{ .tag = 54U,
                .name = "Side",
                .value_type = ValueType::kChar,
                .enum_values = { EnumEntry{ .value = "1", .name = "Buy" }, EnumEntry{ .value = "2", .name = "Sell" } } },
      FieldDef{ .tag = 55U, .name = "Symbol", .value_type = ValueType::kString },
      FieldDef{ .tag = 98U, .name = "EncryptMethod", .value_type = ValueType::kInt },
      FieldDef{ .tag = 108U, .name = "HeartBtInt", .value_type = ValueType::kInt },
      FieldDef{ .tag = 150U,
                .name = "ExecType",
                .value_type = ValueType::kChar,
                .enum_values = { EnumEntry{ .value = "0", .name = "New" } } },
    },
    .messages = {
      MessageDef{ .msg_type = "A",
                  .name = "Logon",
                  .field_rules = { MakeRule(98U, true), MakeRule(108U, true) },
                  .flags = static_cast<std::uint32_t>(MessageFlags::kAdmin) },
      MessageDef{ .msg_type = "D",
                  .name = "NewOrderSingle",
                  .field_rules = { MakeRule(11U, true),
                                   MakeRule(55U, true),
                                   MakeRule(54U, true),
                                   MakeRule(38U, true),
                                   MakeRule(40U, true),
                                   MakeRule(44U, false) } },
      MessageDef{ .msg_type = "8",
                  .name = "ExecutionReport",
                  .field_rules = { MakeRule(150U, true), MakeRule(11U, false) } },
    },
  };
}

auto
MakeContract(std::uint64_t profile_id, std::uint64_t schema_hash) -> nimble::profile::ContractSidecar
{
  using nimble::profile::ContractConditionKind;
  using nimble::profile::ContractDirection;
  using nimble::profile::ContractEnumRule;
  using nimble::profile::ContractFlowEdge;
  using nimble::profile::ContractMessage;
  using nimble::profile::ContractRole;
  using nimble::profile::ContractServiceMessage;
  using nimble::profile::ContractSidecar;
  using nimble::profile::ContractTrace;
  using nimble::profile::ContractWarning;

  return ContractSidecar{
    .profile_id = profile_id,
    .schema_hash = schema_hash,
    .source_kind = "fix-orchestra",
    .source_name = "contract-fixture.xml",
    .supported_semantics = { "service-subsets", "conditional-field-rules", "enum-code-constraints", "service-subsets" },
    .warnings = { ContractWarning{ .code = "warning-b", .message = "second" },
                  ContractWarning{ .code = "warning-a", .message = "first" } },
    .messages = {
      ContractMessage{ .msg_type = "8",
                       .name = "ExecutionReport",
                       .admin = false,
                       .initiator_direction = ContractDirection::kReceive,
                       .acceptor_direction = ContractDirection::kSend,
                       .trace = ContractTrace{ .source_id = "msg-8", .source_path = "messages[8]" } },
      ContractMessage{ .msg_type = "A",
                       .name = "Logon",
                       .admin = true,
                       .initiator_direction = ContractDirection::kReceive,
                       .acceptor_direction = ContractDirection::kSend,
                       .trace = ContractTrace{ .source_id = "msg-a", .source_path = "messages[A]" } },
      ContractMessage{ .msg_type = "D",
                       .name = "NewOrderSingle",
                       .admin = false,
                       .initiator_direction = ContractDirection::kSend,
                       .acceptor_direction = ContractDirection::kReceive,
                       .trace = ContractTrace{ .source_id = "msg-d", .source_path = "messages[D]" } },
    },
    .conditional_rules = {
      nimble::profile::ContractConditionalFieldRule{ .rule_id = "price-required",
                                                     .msg_type = "D",
                                                     .field_tag = 44U,
                                                     .condition = ContractConditionKind::kRequired,
                                                     .when_tag = 40U,
                                                     .when_value = "2",
                                                     .trace = ContractTrace{ .source_id = "rule-price",
                                                                             .source_path = "rules[price-required]" } },
    },
    .enum_rules = {
      ContractEnumRule{ .rule_id = "ord-type",
                        .msg_type = "D",
                        .field_tag = 40U,
                        .allowed_values = { "2", "1", "2" },
                        .trace = ContractTrace{ .source_id = "codeset-ordtype",
                                                .source_path = "codesets[OrdType]" } },
    },
    .service_messages = {
      ContractServiceMessage{ .service_name = "order-entry",
                              .role = ContractRole::kInitiator,
                              .direction = ContractDirection::kReceive,
                              .msg_type = "8",
                              .trace = ContractTrace{ .source_id = "svc-order-entry",
                                                      .source_path = "services[order-entry]" } },
      ContractServiceMessage{ .service_name = "admin-only",
                              .role = ContractRole::kInitiator,
                              .direction = ContractDirection::kReceive,
                              .msg_type = "A",
                              .trace = ContractTrace{ .source_id = "svc-admin-only",
                                                      .source_path = "services[admin-only]" } },
      ContractServiceMessage{ .service_name = "order-entry",
                              .role = ContractRole::kInitiator,
                              .direction = ContractDirection::kSend,
                              .msg_type = "D",
                              .trace = ContractTrace{ .source_id = "svc-order-entry",
                                                      .source_path = "services[order-entry]" } },
    },
    .flow_edges = {
      ContractFlowEdge{ .edge_id = "order-flow",
                        .from_role = ContractRole::kInitiator,
                        .from_msg_type = "D",
                        .to_role = ContractRole::kAcceptor,
                        .to_msg_type = "8",
                        .name = "order-lifecycle",
                        .trace = ContractTrace{ .source_id = "flow-order",
                                                .source_path = "flows[order-flow]" } },
    },
  };
}

auto
WriteArtifact(const std::filesystem::path& path, std::uint64_t profile_id, std::uint64_t schema_hash)
  -> nimble::base::Status
{
  auto artifact = nimble::profile::BuildProfileArtifact(MakeDictionary(profile_id, schema_hash));
  if (!artifact.ok()) {
    return artifact.status();
  }
  return nimble::profile::WriteProfileArtifact(path, artifact.value());
}

auto
Join(std::span<const std::string> parts, char delimiter) -> std::string
{
  std::string out;
  for (std::size_t index = 0; index < parts.size(); ++index) {
    if (index != 0U) {
      out.push_back(delimiter);
    }
    out.append(parts[index]);
  }
  return out;
}

} // namespace

TEST_CASE("contract sidecar serialization is canonical and self-validating", "[contract-sidecar]")
{
  const auto contract = MakeContract(9901U, 0xC0FFEEU);

  const auto serialized = nimble::profile::SerializeContractSidecar(contract);
  auto loaded = nimble::profile::LoadContractSidecarText(serialized);
  REQUIRE(loaded.ok());
  REQUIRE(loaded.value().contract_id != 0U);
  REQUIRE(loaded.value().supported_semantics ==
          std::vector<std::string>{ "conditional-field-rules", "enum-code-constraints", "service-subsets" });
  REQUIRE(loaded.value().warnings.front().code == "warning-a");
  REQUIRE(loaded.value().enum_rules.front().allowed_values == std::vector<std::string>{ "1", "2" });
  REQUIRE(nimble::profile::SerializeContractSidecar(loaded.value()) == serialized);

  auto tampered = serialized;
  const auto line_end = tampered.find('\n');
  REQUIRE(line_end != std::string::npos);
  tampered.replace(0U, line_end, "contract_id=0x1");

  auto tampered_load = nimble::profile::LoadContractSidecarText(tampered);
  REQUIRE(!tampered_load.ok());
  REQUIRE(Contains(tampered_load.status().message(), "contract_id"));
}

TEST_CASE("contract binding derives effective inbound subsets", "[contract-sidecar][runtime-config]")
{
  const auto profile_id = 9902U;
  const auto schema_hash = 0xA11CEU;

  nimble::runtime::LoadedContractMap contracts;
  contracts.emplace(profile_id, MakeContract(profile_id, schema_hash));

  const auto base_counterparty = nimble::runtime::CounterpartyConfigBuilder::Initiator(
                                   "contract-bound",
                                   7001U,
                                   nimble::session::SessionKey{ .sender_comp_id = "BUY1", .target_comp_id = "SELL1" },
                                   profile_id)
                                   .build();

  {
    auto counterparty = base_counterparty;
    counterparty.contract_service_subsets = { "order-entry" };

    auto effective = nimble::runtime::ResolveEffectiveCounterpartyConfig(counterparty, contracts);
    REQUIRE(effective.ok());
    REQUIRE(effective.value().supported_app_msg_types == std::vector<std::string>{ "8" });
    REQUIRE(effective.value().application_messages_available);
  }

  {
    auto counterparty = base_counterparty;
    counterparty.contract_service_subsets = { "admin-only" };

    auto effective = nimble::runtime::ResolveEffectiveCounterpartyConfig(counterparty, contracts);
    REQUIRE(effective.ok());
    REQUIRE(effective.value().supported_app_msg_types.empty());
    REQUIRE_FALSE(effective.value().application_messages_available);
  }

  {
    auto counterparty = base_counterparty;
    counterparty.contract_service_subsets = { "order-entry" };
    counterparty.supported_app_msg_types = { "D" };

    auto effective = nimble::runtime::ResolveEffectiveCounterpartyConfig(counterparty, contracts);
    REQUIRE(!effective.ok());
    REQUIRE(Contains(effective.status().message(), "supported_app_msg_types"));
    REQUIRE(Contains(effective.status().message(), "contract-bound"));
  }

  {
    auto counterparty = base_counterparty;
    counterparty.contract_service_subsets = { "missing-service" };

    auto effective = nimble::runtime::ResolveEffectiveCounterpartyConfig(counterparty, contracts);
    REQUIRE(!effective.ok());
    REQUIRE(Contains(effective.status().message(), "missing-service"));
    REQUIRE(Contains(effective.status().message(), "profile_id="));
  }

  {
    auto counterparty = base_counterparty;
    counterparty.contract_service_subsets = { "order-entry" };

    nimble::runtime::LoadedContractMap no_contracts;
    auto effective = nimble::runtime::ResolveEffectiveCounterpartyConfig(counterparty, no_contracts);
    REQUIRE(!effective.ok());
    REQUIRE(Contains(effective.status().message(), "no contract sidecar"));
  }
}

TEST_CASE("engine loads contract sidecars and clears state on failure", "[contract-sidecar][engine]")
{
  const auto temp_root = std::filesystem::temp_directory_path() / "nimblefix-contract-sidecar-engine-test";
  const auto artifact_path = temp_root / "profile.nfa";
  const auto contract_path = temp_root / "profile.nfct";
  const auto bad_contract_path = temp_root / "profile-bad.nfct";
  const auto profile_id = 9903U;
  const auto schema_hash = 0xBEEFU;

  std::filesystem::remove_all(temp_root);
  std::filesystem::create_directories(temp_root);

  REQUIRE(WriteArtifact(artifact_path, profile_id, schema_hash).ok());
  REQUIRE(nimble::profile::WriteContractSidecar(contract_path, MakeContract(profile_id, schema_hash)).ok());
  REQUIRE(nimble::profile::WriteContractSidecar(bad_contract_path, MakeContract(profile_id, schema_hash + 1U)).ok());

  nimble::runtime::Engine engine;

  nimble::runtime::EngineConfig load_only_config;
  load_only_config.profile_artifacts.push_back(artifact_path);
  REQUIRE(engine.LoadProfiles(load_only_config).ok());
  REQUIRE(engine.profiles().size() == 1U);

  nimble::runtime::EngineConfig bad_load_config;
  bad_load_config.profile_artifacts.push_back(artifact_path);
  bad_load_config.profile_contracts.push_back(bad_contract_path);
  auto bad_load = engine.LoadProfiles(bad_load_config);
  REQUIRE(!bad_load.ok());
  REQUIRE(engine.profiles().size() == 0U);

  nimble::runtime::EngineConfig boot_config;
  boot_config.profile_artifacts.push_back(artifact_path);
  boot_config.profile_contracts.push_back(contract_path);
  boot_config.counterparties.push_back(
    nimble::runtime::CounterpartyConfigBuilder::Initiator(
      "engine-contract-bound",
      7002U,
      nimble::session::SessionKey{ .sender_comp_id = "BUY2", .target_comp_id = "SELL2" },
      profile_id)
      .contract_service_subsets({ "order-entry" })
      .build());

  REQUIRE(engine.Boot(boot_config).ok());
  const auto* effective = engine.FindCounterpartyConfig(7002U);
  REQUIRE(effective != nullptr);
  REQUIRE(effective->supported_app_msg_types == std::vector<std::string>{ "8" });

  std::filesystem::remove_all(temp_root);
}

TEST_CASE("runtime config parser reads contract artifacts and service subsets", "[runtime-config][contract-sidecar]")
{
  const auto temp_root = std::filesystem::temp_directory_path() / "nimblefix-contract-sidecar-parser-test";
  const auto contract_dir = temp_root / "contracts";
  std::filesystem::remove_all(temp_root);
  std::filesystem::create_directories(contract_dir);

  REQUIRE(nimble::profile::WriteContractSidecar(contract_dir / "fix44.nfct", MakeContract(9904U, 0x1234U)).ok());

  std::vector<std::string> fields(47U);
  fields[0] = "counterparty";
  fields[1] = "parser-contract-bound";
  fields[2] = "7003";
  fields[3] = "9904";
  fields[4] = "FIX.4.4";
  fields[5] = "BUY3";
  fields[6] = "SELL3";
  fields[7] = "memory";
  fields[8] = "";
  fields[9] = "memory";
  fields[10] = "inline";
  fields[11] = "30";
  fields[12] = "true";
  fields[15] = "0";
  fields[17] = "0";
  fields[44] = "8";
  fields[45] = "true";
  fields[46] = "order-entry,admin-only";

  const auto text =
    std::string("profile=profiles/fix44.nfa\n") + "contract=contracts/fix44.nfct\n" + Join(fields, '|') + "\n";

  auto parsed = nimble::runtime::LoadEngineConfigText(text, temp_root);
  if (!parsed.ok()) {
    FAIL(std::string(parsed.status().message()));
  }
  REQUIRE(parsed.value().profile_artifacts ==
          std::vector<std::filesystem::path>{ temp_root / "profiles" / "fix44.nfa" });
  REQUIRE(parsed.value().profile_contracts ==
          std::vector<std::filesystem::path>{ temp_root / "contracts" / "fix44.nfct" });
  REQUIRE(parsed.value().counterparties.size() == 1U);
  REQUIRE(parsed.value().counterparties.front().supported_app_msg_types == std::vector<std::string>{ "8" });
  REQUIRE(parsed.value().counterparties.front().contract_service_subsets ==
          std::vector<std::string>{ "order-entry", "admin-only" });

  std::filesystem::remove_all(temp_root);
}