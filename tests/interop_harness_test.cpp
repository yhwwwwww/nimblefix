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
  const auto ffd_path = std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.nfd";
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
  const auto artifact_path = root / "loopback-profile.nfa";
  const auto store_path = root / "loopback-a.store";

  std::filesystem::remove(artifact_path);
  std::filesystem::remove(store_path);
  REQUIRE(BuildInteropArtifact(artifact_path).ok());

  auto scenario = nimble::runtime::LoadInteropScenarioFile(root / "loopback-basic.nfscenario");
  REQUIRE(scenario.ok());

  auto report = nimble::runtime::RunInteropScenario(scenario.value());
  REQUIRE(report.ok());
  REQUIRE(report.value().sessions.size() == 2U);
  REQUIRE(report.value().metrics.sessions.size() == 2U);
  REQUIRE(report.value().trace_events.size() >= 8U);

  std::filesystem::remove(artifact_path);
  std::filesystem::remove(store_path);

  const auto durable_root = std::filesystem::temp_directory_path() / "nimblefix-interop-durable-test";
  const auto durable_artifact_path = durable_root / "loopback-profile.nfa";
  const auto durable_config_path = durable_root / "loopback-runtime.nfcfg";
  const auto durable_scenario_path = durable_root / "loopback-basic.nfscenario";
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

  const auto official_root = std::filesystem::temp_directory_path() / "nimblefix-interop-official-test";
  const auto official_artifact_path = official_root / "official-profile.nfa";
  const auto official_config_path = official_root / "official-runtime.nfcfg";
  const auto official_scenario_path = official_root / "official-case-2s.nfscenario";

  std::filesystem::remove_all(official_root);
  std::filesystem::create_directories(official_root);
  REQUIRE(BuildInteropArtifact(official_artifact_path).ok());

  {
    std::ofstream config_out(official_config_path, std::ios::trunc);
    config_out << "engine.worker_count=1\n";
    config_out << "engine.enable_metrics=true\n";
    config_out << "engine.trace_mode=ring\n";
    config_out << "engine.trace_capacity=32\n";
    config_out << "profile=" << official_artifact_path.filename().string() << "\n";
    config_out << "counterparty|official-acceptor|4101|4400|FIX.4.4|SELL|BUY|memory||memory|inline|30|false\n";
  }

  {
    std::ofstream scenario_out(official_scenario_path, std::ios::trunc);
    scenario_out << "config=" << official_config_path.filename().string() << "\n";
    scenario_out << "action|protocol-connect|4101|ts=1\n";
    scenario_out << "action|protocol-inbound|4101|seq=1|ts=10|body=35=0\n";
    scenario_out << "expect-action|1|outbound=0|app=0|disconnect=0\n";
    scenario_out << "expect-action|2|outbound=1|app=0|disconnect=1\n";
    scenario_out << "expect-outbound|2|1|msg-type=5|text-contains=received 0 before Logon completed\n";
    scenario_out << "expect-session|4101|disconnected|2|2|0\n";
  }

  auto official_scenario = nimble::runtime::LoadInteropScenarioFile(official_scenario_path);
  REQUIRE(official_scenario.ok());

  auto official_report = nimble::runtime::RunInteropScenario(official_scenario.value());
  REQUIRE(official_report.ok());
  REQUIRE(official_report.value().action_reports.size() == 2U);
  REQUIRE(official_report.value().action_reports[1].outbound_frame_summaries.size() == 1U);
  REQUIRE(official_report.value().action_reports[1].outbound_frame_summaries.front().msg_type == "5");

  const auto poss_resend_scenario_path = official_root / "official-case-19.nfscenario";
  {
    std::ofstream scenario_out(poss_resend_scenario_path, std::ios::trunc);
    scenario_out << "config=" << official_config_path.filename().string() << "\n";
    scenario_out << "action|protocol-connect|4101|ts=1\n";
    scenario_out << "action|protocol-inbound|4101|seq=1|ts=10|body=35=A^98=0^108=30\n";
    scenario_out
      << "action|protocol-inbound|4101|seq=2|ts=20|body=35=D^11=ORD-SEEN^55=AAPL^54=1^60=20260425-12:00:00.000^40=1\n";
    scenario_out << "action|protocol-inbound|4101|seq=3|ts=30|possdup=1|orig-sending-time=20260425-11:59:00.000|body="
                    "35=D^97=Y^11=ORD-SEEN^55=AAPL^54=1^60=20260425-12:00:00.000^40=1\n";
    scenario_out << "action|protocol-inbound|4101|seq=4|ts=40|possdup=1|orig-sending-time=20260425-11:58:00.000|body="
                    "35=D^97=Y^11=ORD-NEW^55=MSFT^54=1^60=20260425-12:00:01.000^40=1\n";
    scenario_out << "expect-action|3|app=1|processed-app=1|ignored-app=0|poss-resend-app=0|disconnect=0\n";
    scenario_out << "expect-action|4|app=1|processed-app=0|ignored-app=1|poss-resend-app=1|disconnect=0\n";
    scenario_out << "expect-action|5|app=1|processed-app=1|ignored-app=0|poss-resend-app=1|disconnect=0\n";
    scenario_out << "expect-session|4101|active|5|2|0\n";
  }

  auto poss_resend_scenario = nimble::runtime::LoadInteropScenarioFile(poss_resend_scenario_path);
  REQUIRE(poss_resend_scenario.ok());

  auto poss_resend_report = nimble::runtime::RunInteropScenario(poss_resend_scenario.value());
  REQUIRE(poss_resend_report.ok());
  REQUIRE(poss_resend_report.value().action_reports.size() == 5U);
  REQUIRE(poss_resend_report.value().action_reports[2].processed_application_messages == 1U);
  REQUIRE(poss_resend_report.value().action_reports[2].ignored_application_messages == 0U);
  REQUIRE(poss_resend_report.value().action_reports[3].processed_application_messages == 0U);
  REQUIRE(poss_resend_report.value().action_reports[3].ignored_application_messages == 1U);
  REQUIRE(poss_resend_report.value().action_reports[3].poss_resend_application_messages == 1U);
  REQUIRE(poss_resend_report.value().action_reports[4].processed_application_messages == 1U);
  REQUIRE(poss_resend_report.value().action_reports[4].ignored_application_messages == 0U);
  REQUIRE(poss_resend_report.value().action_reports[4].poss_resend_application_messages == 1U);

  const auto offline_queue_scenario_path = official_root / "official-case-16.nfscenario";
  {
    std::ofstream scenario_out(offline_queue_scenario_path, std::ios::trunc);
    scenario_out << "config=" << official_config_path.filename().string() << "\n";
    scenario_out << "action|protocol-queue-application|4101|ts=5|body=35=D^11=ORD-OFFLINE^55=AAPL^54=1^60=20260425-12:"
                    "00:00.000^40=1\n";
    scenario_out << "action|protocol-connect|4101|ts=10\n";
    scenario_out << "action|protocol-inbound|4101|seq=1|ts=20|body=35=A^98=0^108=30\n";
    scenario_out << "expect-action|1|queued-app=1|disconnect=0\n";
    scenario_out << "expect-action|3|outbound=2|active=1|disconnect=0\n";
    scenario_out << "expect-outbound|3|1|msg-type=A|msg-seq-num=1\n";
    scenario_out << "expect-outbound|3|2|msg-type=D|msg-seq-num=2\n";
    scenario_out << "expect-session|4101|active|2|3|0\n";
  }

  auto offline_queue_scenario = nimble::runtime::LoadInteropScenarioFile(offline_queue_scenario_path);
  REQUIRE(offline_queue_scenario.ok());

  auto offline_queue_report = nimble::runtime::RunInteropScenario(offline_queue_scenario.value());
  REQUIRE(offline_queue_report.ok());
  REQUIRE(offline_queue_report.value().action_reports.size() == 3U);
  REQUIRE(offline_queue_report.value().action_reports[0].queued_application_messages == 1U);
  REQUIRE(offline_queue_report.value().action_reports[2].outbound_frame_summaries.size() == 2U);
  REQUIRE(offline_queue_report.value().action_reports[2].outbound_frame_summaries[1].msg_type == "D");

  std::filesystem::remove_all(official_root);
}