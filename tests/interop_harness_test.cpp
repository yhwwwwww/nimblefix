#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

#include "nimblefix/profile/artifact_builder.h"
#include "nimblefix/profile/dictgen_input.h"
#include "nimblefix/runtime/interop_harness.h"

#include "test_support.h"

namespace {

auto
BuildInteropArtifact(const std::filesystem::path& artifact_path) -> nimble::base::Status
{
  const auto ffd_path = std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.ffd";
  auto dictionary = nimble::profile::LoadNormalizedDictionaryFile(ffd_path);
  if (!dictionary.ok()) {
    return dictionary.status();
  }
  dictionary.value().profile_id = 4400U;
  auto artifact = nimble::profile::BuildProfileArtifact(dictionary.value());
  if (!artifact.ok()) {
    return artifact.status();
  }
  return nimble::profile::WriteProfileArtifact(artifact_path, artifact.value());
}

} // namespace

TEST_CASE("interop-harness", "[interop-harness]")
{
  const auto root = std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / "tests" / "data" / "interop";
  const auto artifact_path = root / "loopback-profile.art";
  const auto store_path = root / "loopback-a.store";

  std::filesystem::remove(artifact_path);
  std::filesystem::remove(store_path);
  REQUIRE(BuildInteropArtifact(artifact_path).ok());

  auto scenario = nimble::runtime::LoadInteropScenarioFile(root / "loopback-basic.ffscenario");
  REQUIRE(scenario.ok());

  auto report = nimble::runtime::RunInteropScenario(scenario.value());
  REQUIRE(report.ok());
  REQUIRE(report.value().sessions.size() == 2U);
  REQUIRE(report.value().metrics.sessions.size() == 2U);
  REQUIRE(report.value().trace_events.size() >= 8U);

  std::filesystem::remove(artifact_path);
  std::filesystem::remove(store_path);

  const auto durable_root = std::filesystem::temp_directory_path() / "nimblefix-interop-durable-test";
  const auto durable_artifact_path = durable_root / "loopback-profile.art";
  const auto durable_config_path = durable_root / "loopback-runtime.ffcfg";
  const auto durable_scenario_path = durable_root / "loopback-basic.ffscenario";
  const auto durable_store_path = durable_root / "loopback-a.store";

  std::filesystem::remove_all(durable_root);
  std::filesystem::create_directories(durable_root);
  REQUIRE(BuildInteropArtifact(durable_artifact_path).ok());

  {
    std::ofstream config_out(durable_config_path, std::ios::trunc);
    config_out << "engine.worker_count=1\n";
    config_out << "engine.enable_metrics=true\n";
    config_out << "engine.trace_mode=ring\n";
    config_out << "engine.trace_capacity=32\n";
    config_out << "profile=" << durable_artifact_path.filename().string() << "\n";
    config_out << "counterparty|loop-a|3001|4400|FIX.4.4|BUY1|SELL1|durable|" << durable_store_path.filename().string()
               << "|warm|inline|30|true||strict|2|utc-day|2\n";
    config_out << "counterparty|loop-b|3002|4400|FIX.4.4|BUY2|SELL2|memory||"
                  "memory|inline|30|false\n";
  }

  {
    std::ofstream scenario_out(durable_scenario_path, std::ios::trunc);
    scenario_out << "config=" << durable_config_path.filename().string() << "\n";
    scenario_out << "action|connect|3001\n";
    scenario_out << "action|logon|3001\n";
    scenario_out << "action|connect|3002\n";
    scenario_out << "action|logon|3002\n";
    scenario_out << "action|outbound|3001|1|admin|100|logon-out\n";
    scenario_out << "action|inbound|3002|1|admin|101|logon-in\n";
    scenario_out << "action|outbound|3002|1|app|102|exec-out\n";
    scenario_out << "action|inbound|3001|1|app|103|exec-in\n";
    scenario_out << "action|gap|3001|3|app|104|gap\n";
    scenario_out << "action|complete-resend|3001\n";
    scenario_out << "action|save-recovery|3001\n";
    scenario_out << "action|recover-warm|3001\n";
    scenario_out << "expect-session|3001|disconnected|2|2|0\n";
    scenario_out << "expect-session|3002|active|2|2|0\n";
    scenario_out << "expect-metric|3001|1|1|1|1|0\n";
    scenario_out << "expect-metric|3002|1|1|1|0|0\n";
    scenario_out << "expect-trace-min|8\n";
  }

  auto durable_scenario = nimble::runtime::LoadInteropScenarioFile(durable_scenario_path);
  REQUIRE(durable_scenario.ok());

  auto durable_report = nimble::runtime::RunInteropScenario(durable_scenario.value());
  REQUIRE(durable_report.ok());
  REQUIRE(durable_report.value().sessions.size() == 2U);
  REQUIRE(durable_report.value().trace_events.size() >= 8U);
  REQUIRE(std::filesystem::exists(durable_store_path / "active.log"));

  std::filesystem::remove_all(durable_root);
}