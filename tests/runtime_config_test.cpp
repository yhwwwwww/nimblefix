#include <catch2/catch_test_macros.hpp>

#include <ctime>
#include <filesystem>
#include <fstream>

#include "nimblefix/codec/fix_codec.h"
#include "nimblefix/message/message.h"
#include "nimblefix/profile/artifact_builder.h"
#include "nimblefix/profile/dictgen_input.h"
#include "nimblefix/runtime/config.h"
#include "nimblefix/runtime/config_io.h"
#include "nimblefix/runtime/engine.h"

namespace {

auto
BuildSampleArtifact(const std::filesystem::path& artifact_path, std::uint64_t profile_id) -> nimble::base::Status
{
  const auto ffd_path = std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.ffd";
  auto dictionary = nimble::profile::LoadNormalizedDictionaryFile(ffd_path);
  if (!dictionary.ok()) {
    return dictionary.status();
  }
  dictionary.value().profile_id = profile_id;
  auto artifact = nimble::profile::BuildProfileArtifact(dictionary.value());
  if (!artifact.ok()) {
    return artifact.status();
  }
  return nimble::profile::WriteProfileArtifact(artifact_path, artifact.value());
}

auto
MakeUtcNs(int year, int month, int day, int hour, int minute, int second) -> std::uint64_t
{
  std::tm value{};
  value.tm_year = year - 1900;
  value.tm_mon = month - 1;
  value.tm_mday = day;
  value.tm_hour = hour;
  value.tm_min = minute;
  value.tm_sec = second;
  return static_cast<std::uint64_t>(timegm(&value)) * 1'000'000'000ULL;
}

} // namespace

TEST_CASE("runtime-config", "[runtime-config]")
{
  const auto temp_root = std::filesystem::temp_directory_path() / "nimblefix-runtime-config-test";
  std::filesystem::create_directories(temp_root);

  const auto artifact_path = temp_root / "sample-profile.art";
  const auto transport_artifact_path = temp_root / "sample-transport-profile.art";
  REQUIRE(BuildSampleArtifact(artifact_path, 4400U).ok());
  REQUIRE(BuildSampleArtifact(transport_artifact_path, 4401U).ok());

  const auto config_path = temp_root / "engine.ffcfg";
  const auto store_path = temp_root / "session-2002.store";
  const auto durable_store_path = temp_root / "session-2004.store";
  const auto durable_local_store_path = temp_root / "session-2005.store";
  std::ofstream out(config_path, std::ios::trunc);
  out << "engine.worker_count=2\n";
  out << "engine.enable_metrics=true\n";
  out << "engine.trace_mode=ring\n";
  out << "engine.trace_capacity=16\n";
  out << "engine.front_door_cpu=7\n";
  out << "engine.worker_cpu_affinity=3,5\n";
  out << "engine.queue_app_mode=threaded\n";
  out << "engine.app_cpu_affinity=11,13\n";
  out << "profile=sample-profile.art\n";
  out << "profile=sample-transport-profile.art\n";
  out << "listener|main|127.0.0.1|9878|0\n";
  out << "counterparty|buy-sell-a|2001|4400|FIX.4.4|BUY1|SELL1|memory||memory|"
         "inline|30|true||compatible\n";
  out << "counterparty|buy-sell-b|2002|4400|FIX.4.4|BUY2|SELL2|mmap|" << store_path.filename().string()
      << "|warm|queue|20|false\n";
  out << "counterparty|transport-fixt|2003|4401|FIXT.1.1|SELLT|BUYT|memory||"
         "memory|inline|30|false|9\n";
  out << "counterparty|buy-sell-d|2004|4400|FIX.4.4|BUYD|SELLD|durable|" << durable_store_path.filename().string()
      << "|warm|queue|25|false||strict|2|external|3\n";
  out << "counterparty|buy-sell-e|2005|4400|FIX.4.4|BUYE|SELLE|durable|" << durable_local_store_path.filename().string()
      << "|warm|inline|15|false||strict|0|local-time|4|true|100|1000|0|3600|"
         "false\n";
  out.close();

  auto config = nimble::runtime::LoadEngineConfigFile(config_path);
  REQUIRE(config.ok());
  REQUIRE(config.value().worker_count == 2U);
  REQUIRE(config.value().front_door_cpu.has_value());
  REQUIRE(config.value().front_door_cpu.value() == 7U);
  REQUIRE(config.value().worker_cpu_affinity.size() == 2U);
  REQUIRE(config.value().worker_cpu_affinity[0] == 3U);
  REQUIRE(config.value().worker_cpu_affinity[1] == 5U);
  REQUIRE(config.value().queue_app_mode == nimble::runtime::QueueAppThreadingMode::kThreaded);
  REQUIRE(config.value().app_cpu_affinity.size() == 2U);
  REQUIRE(config.value().app_cpu_affinity[0] == 11U);
  REQUIRE(config.value().app_cpu_affinity[1] == 13U);
  REQUIRE(config.value().profile_artifacts.size() == 2U);
  REQUIRE(config.value().counterparties.size() == 5U);
  REQUIRE(config.value().counterparties[0].validation_policy.mode == nimble::session::ValidationMode::kCompatible);
  REQUIRE(config.value().counterparties[1].store_mode == nimble::runtime::StoreMode::kMmap);
  REQUIRE(config.value().counterparties[1].store_path == store_path);
  REQUIRE(config.value().counterparties[2].default_appl_ver_id == "9");
  REQUIRE(config.value().counterparties[2].session.default_appl_ver_id == "9");
  REQUIRE(config.value().counterparties[3].store_mode == nimble::runtime::StoreMode::kDurableBatch);
  REQUIRE(config.value().counterparties[3].store_path == durable_store_path);
  REQUIRE(config.value().counterparties[3].durable_flush_threshold == 2U);
  REQUIRE(config.value().counterparties[3].durable_rollover_mode == nimble::store::DurableStoreRolloverMode::kExternal);
  REQUIRE(config.value().counterparties[3].durable_archive_limit == 3U);
  REQUIRE(config.value().counterparties[4].store_path == durable_local_store_path);
  REQUIRE(config.value().counterparties[4].durable_rollover_mode ==
          nimble::store::DurableStoreRolloverMode::kLocalTime);
  REQUIRE(config.value().counterparties[4].durable_archive_limit == 4U);
  REQUIRE(config.value().counterparties[4].reconnect_enabled);
  REQUIRE(config.value().counterparties[4].reconnect_initial_ms == 100U);
  REQUIRE(config.value().counterparties[4].reconnect_max_ms == 1000U);
  REQUIRE(config.value().counterparties[4].reconnect_max_retries == 0U);
  REQUIRE(config.value().counterparties[4].durable_local_utc_offset_seconds == 3600);
  REQUIRE(!config.value().counterparties[4].durable_use_system_timezone);

  nimble::runtime::Engine engine;
  REQUIRE(engine.Boot(config.value()).ok());
  REQUIRE(engine.runtime() != nullptr);
  REQUIRE(engine.runtime()->worker_count() == 2U);
  REQUIRE(engine.runtime()->session_count() == 5U);
  REQUIRE(engine.profiles().Find(4400U) != nullptr);
  REQUIRE(engine.profiles().Find(4401U) != nullptr);
  REQUIRE(engine.FindCounterpartyConfig(2002U) != nullptr);
  REQUIRE(engine.FindCounterpartyConfig(2002U)->dispatch_mode == nimble::runtime::AppDispatchMode::kQueueDecoupled);
  REQUIRE(engine.FindListenerConfig("main") != nullptr);

  auto transport_dictionary = engine.LoadDictionaryView(4401U);
  REQUIRE(transport_dictionary.ok());

  nimble::message::MessageBuilder logon_builder("A");
  logon_builder.set_string(35U, "A").set_int(98U, 0).set_int(108U, 30);

  nimble::codec::EncodeOptions options;
  options.begin_string = "FIXT.1.1";
  options.sender_comp_id = "BUYT";
  options.target_comp_id = "SELLT";
  options.default_appl_ver_id = "9";
  options.msg_seq_num = 1U;

  auto encoded =
    nimble::codec::EncodeFixMessage(std::move(logon_builder).build(), transport_dictionary.value(), options);
  REQUIRE(encoded.ok());

  auto peeked = nimble::codec::PeekSessionHeader(encoded.value());
  REQUIRE(peeked.ok());
  REQUIRE(peeked.value().begin_string == "FIXT.1.1");
  REQUIRE(peeked.value().default_appl_ver_id == "9");
  REQUIRE(peeked.value().sender_comp_id == "BUYT");
  REQUIRE(peeked.value().target_comp_id == "SELLT");

  nimble::message::MessageBuilder heartbeat_builder("0");
  heartbeat_builder.set_string(35U, "0");
  auto encoded_heartbeat =
    nimble::codec::EncodeFixMessage(std::move(heartbeat_builder).build(), transport_dictionary.value(), options);
  REQUIRE(encoded_heartbeat.ok());
  auto heartbeat_header = nimble::codec::PeekSessionHeader(encoded_heartbeat.value());
  REQUIRE(heartbeat_header.ok());
  REQUIRE(heartbeat_header.value().msg_type == "0");
  REQUIRE(heartbeat_header.value().default_appl_ver_id.empty());

  auto resolved = engine.ResolveInboundSession(peeked.value());
  REQUIRE(resolved.ok());
  REQUIRE(resolved.value().counterparty.session.session_id == 2003U);
  REQUIRE(resolved.value().counterparty.default_appl_ver_id == "9");
  REQUIRE(resolved.value().dictionary.profile().header().profile_id == 4401U);

  const auto metrics = engine.metrics().Snapshot();
  REQUIRE(metrics.sessions.size() == 5U);
  REQUIRE(metrics.workers.size() == 2U);

  const auto traces = engine.trace().Snapshot();
  REQUIRE(traces.size() >= 5U);
  REQUIRE(traces[0].kind == nimble::runtime::TraceEventKind::kConfigLoaded);

  const auto invalid_config_text = std::string("engine.worker_count=1\n"
                                               "engine.worker_cpu_affinity=1,2\n"
                                               "profile=sample-transport-profile.art\n"
                                               "counterparty|bad-fixt|3001|4401|FIXT.1.1|SELLX|BUYX|memory||"
                                               "memory|inline|30|false\n");
  auto invalid = nimble::runtime::LoadEngineConfigText(invalid_config_text, temp_root);
  REQUIRE(!invalid.ok());

  const auto invalid_listener_text = std::string("engine.worker_count=2\n"
                                                 "profile=sample-profile.art\n"
                                                 "listener|bad|127.0.0.1|9878|2\n");
  auto invalid_listener = nimble::runtime::LoadEngineConfigText(invalid_listener_text, temp_root);
  REQUIRE(!invalid_listener.ok());

  const auto invalid_app_cpu_text = std::string("engine.worker_count=2\n"
                                                "engine.app_cpu_affinity=7\n"
                                                "profile=sample-profile.art\n");
  auto invalid_app_cpu = nimble::runtime::LoadEngineConfigText(invalid_app_cpu_text, temp_root);
  REQUIRE(!invalid_app_cpu.ok());

  const auto advanced_config_text =
    std::string("engine.worker_count=1\n"
                "profile=sample-profile.art\n"
                "counterparty|scheduled|3001|4400|FIX.4.4|BUY|SELL|memory||memory|inline|"
                "30|true||strict|0|utc-day|0|false|1000|5000|3|0|true|no-auto-reset|0|0|"
                "0|true|true|true|true|true|false|false|09:30:00|16:00:00|mon|fri|08:45:"
                "00|16:15:00|mon|fri\n");
  auto advanced = nimble::runtime::LoadEngineConfigText(advanced_config_text, temp_root);
  REQUIRE(advanced.ok());
  REQUIRE(advanced.value().counterparties.size() == 1U);
  const auto& scheduled = advanced.value().counterparties.front();
  REQUIRE(scheduled.reset_seq_num_on_logon);
  REQUIRE(scheduled.reset_seq_num_on_logout);
  REQUIRE(scheduled.reset_seq_num_on_disconnect);
  REQUIRE(scheduled.refresh_on_logon);
  REQUIRE(scheduled.send_next_expected_msg_seq_num);
  REQUIRE(!scheduled.session_schedule.use_local_time);
  REQUIRE(!scheduled.session_schedule.non_stop_session);
  REQUIRE(scheduled.session_schedule.start_time.has_value());
  REQUIRE(scheduled.session_schedule.start_time->hour == 9U);
  REQUIRE(scheduled.session_schedule.end_time->hour == 16U);
  REQUIRE(scheduled.session_schedule.start_day == nimble::runtime::SessionDayOfWeek::kMonday);
  REQUIRE(scheduled.session_schedule.end_day == nimble::runtime::SessionDayOfWeek::kFriday);
  REQUIRE(scheduled.session_schedule.logon_time.has_value());
  REQUIRE(scheduled.session_schedule.logon_time->hour == 8U);
  REQUIRE(scheduled.session_schedule.logout_time->minute == 15U);

  nimble::runtime::SessionScheduleConfig session_schedule;
  session_schedule.start_time = nimble::runtime::SessionTimeOfDay{ 9U, 0U, 0U };
  session_schedule.end_time = nimble::runtime::SessionTimeOfDay{ 17U, 0U, 0U };
  REQUIRE(nimble::runtime::ValidateSessionSchedule(session_schedule).ok());
  REQUIRE(nimble::runtime::IsWithinSessionWindow(session_schedule, MakeUtcNs(2026, 4, 6, 10, 0, 0)));
  REQUIRE(!nimble::runtime::IsWithinSessionWindow(session_schedule, MakeUtcNs(2026, 4, 6, 8, 0, 0)));
  const auto next_logon = nimble::runtime::NextLogonWindowStart(session_schedule, MakeUtcNs(2026, 4, 6, 8, 0, 0));
  REQUIRE(next_logon.has_value());
  REQUIRE(next_logon.value() == MakeUtcNs(2026, 4, 6, 9, 0, 0));

  std::filesystem::remove(config_path);
  std::filesystem::remove(artifact_path);
  std::filesystem::remove(transport_artifact_path);
  std::filesystem::remove(store_path);
  std::filesystem::remove_all(durable_store_path);
  std::filesystem::remove_all(durable_local_store_path);
  std::filesystem::remove_all(temp_root);
}