#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

#include "nimblefix/advanced/message_builder.h"
#include "nimblefix/codec/fix_codec.h"
#include "nimblefix/profile/artifact_builder.h"
#include "nimblefix/profile/dictgen_input.h"
#include "nimblefix/runtime/config.h"
#include "nimblefix/runtime/engine.h"
#include "nimblefix/runtime/internal_config_parser.h"
#include "nimblefix/transport/transport_connection.h"

namespace {

auto
BuildSampleArtifact(const std::filesystem::path& artifact_path, std::uint64_t profile_id) -> nimble::base::Status
{
  const auto ffd_path = std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.nfd";
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

auto
StatusContains(const nimble::base::Status& status, std::string_view text) -> bool
{
  return status.message().find(text) != std::string_view::npos;
}

auto
HasDiagnosticField(const nimble::runtime::ConfigValidationResult& result,
                   std::string_view field_path,
                   nimble::runtime::ConfigErrorSeverity severity = nimble::runtime::ConfigErrorSeverity::kError) -> bool
{
  return std::any_of(result.errors.begin(), result.errors.end(), [&](const auto& error) {
    return error.severity == severity && error.field_path == field_path;
  });
}

auto
OptionalTimeEquals(const std::optional<nimble::runtime::SessionTimeOfDay>& left,
                   const std::optional<nimble::runtime::SessionTimeOfDay>& right) -> bool
{
  if (left.has_value() != right.has_value()) {
    return false;
  }
  if (!left.has_value()) {
    return true;
  }
  return left->hour == right->hour && left->minute == right->minute && left->second == right->second;
}

auto
RequireSameCoreConfig(const nimble::runtime::EngineConfig& expected, const nimble::runtime::EngineConfig& actual)
  -> void
{
  REQUIRE(actual.worker_count == expected.worker_count);
  REQUIRE(actual.enable_metrics == expected.enable_metrics);
  REQUIRE(actual.trace_mode == expected.trace_mode);
  REQUIRE(actual.trace_capacity == expected.trace_capacity);
  REQUIRE(actual.front_door_cpu == expected.front_door_cpu);
  REQUIRE(actual.worker_cpu_affinity == expected.worker_cpu_affinity);
  REQUIRE(actual.queue_app_mode == expected.queue_app_mode);
  REQUIRE(actual.poll_mode == expected.poll_mode);
  REQUIRE(actual.io_backend == expected.io_backend);
  REQUIRE(actual.app_cpu_affinity == expected.app_cpu_affinity);
  REQUIRE(actual.profile_artifacts == expected.profile_artifacts);
  REQUIRE(actual.profile_dictionaries == expected.profile_dictionaries);
  REQUIRE(actual.profile_contracts == expected.profile_contracts);
  REQUIRE(actual.profile_madvise == expected.profile_madvise);
  REQUIRE(actual.profile_mlock == expected.profile_mlock);
  // .nfcfg accepts this parser flag for tools, but LoadEngineConfigText validates
  // with the default false value before returning the parsed config.
  REQUIRE(actual.accept_unknown_sessions == false);
  REQUIRE(actual.backlog_warn_threshold_ms == expected.backlog_warn_threshold_ms);
  REQUIRE(actual.backlog_warn_throttle_ms == expected.backlog_warn_throttle_ms);
  REQUIRE(actual.listeners.size() == expected.listeners.size());
  for (std::size_t index = 0; index < expected.listeners.size(); ++index) {
    REQUIRE(actual.listeners[index].name == expected.listeners[index].name);
    REQUIRE(actual.listeners[index].host == expected.listeners[index].host);
    REQUIRE(actual.listeners[index].port == expected.listeners[index].port);
    REQUIRE(actual.listeners[index].worker_hint == expected.listeners[index].worker_hint);
  }
  REQUIRE(actual.counterparties.size() == expected.counterparties.size());
  for (std::size_t index = 0; index < expected.counterparties.size(); ++index) {
    const auto& expected_counterparty = expected.counterparties[index];
    const auto& actual_counterparty = actual.counterparties[index];
    REQUIRE(actual_counterparty.name == expected_counterparty.name);
    REQUIRE(actual_counterparty.session.session_id == expected_counterparty.session.session_id);
    REQUIRE(actual_counterparty.session.profile_id == expected_counterparty.session.profile_id);
    REQUIRE(actual_counterparty.session.key.begin_string == expected_counterparty.session.key.begin_string);
    REQUIRE(actual_counterparty.session.key.sender_comp_id == expected_counterparty.session.key.sender_comp_id);
    REQUIRE(actual_counterparty.session.key.target_comp_id == expected_counterparty.session.key.target_comp_id);
    REQUIRE(actual_counterparty.session.default_appl_ver_id == expected_counterparty.session.default_appl_ver_id);
    REQUIRE(actual_counterparty.session.heartbeat_interval_seconds ==
            expected_counterparty.session.heartbeat_interval_seconds);
    REQUIRE(actual_counterparty.session.is_initiator == expected_counterparty.session.is_initiator);
    REQUIRE(actual_counterparty.transport_profile.begin_string == expected_counterparty.transport_profile.begin_string);
    REQUIRE(actual_counterparty.transport_profile.version == expected_counterparty.transport_profile.version);
    REQUIRE(actual_counterparty.store_path == expected_counterparty.store_path);
    REQUIRE(actual_counterparty.default_appl_ver_id == expected_counterparty.default_appl_ver_id);
    REQUIRE(actual_counterparty.supported_app_msg_types == expected_counterparty.supported_app_msg_types);
    REQUIRE(actual_counterparty.contract_service_subsets == expected_counterparty.contract_service_subsets);
    REQUIRE(actual_counterparty.sending_time_threshold_seconds == expected_counterparty.sending_time_threshold_seconds);
    REQUIRE(actual_counterparty.application_messages_available == expected_counterparty.application_messages_available);
    REQUIRE(actual_counterparty.store_mode == expected_counterparty.store_mode);
    REQUIRE(actual_counterparty.durable_flush_threshold == expected_counterparty.durable_flush_threshold);
    REQUIRE(actual_counterparty.durable_rollover_mode == expected_counterparty.durable_rollover_mode);
    REQUIRE(actual_counterparty.durable_archive_limit == expected_counterparty.durable_archive_limit);
    REQUIRE(actual_counterparty.durable_local_utc_offset_seconds ==
            expected_counterparty.durable_local_utc_offset_seconds);
    REQUIRE(actual_counterparty.durable_use_system_timezone == expected_counterparty.durable_use_system_timezone);
    REQUIRE(actual_counterparty.recovery_mode == expected_counterparty.recovery_mode);
    REQUIRE(actual_counterparty.dispatch_mode == expected_counterparty.dispatch_mode);
    REQUIRE(actual_counterparty.validation_policy.mode == expected_counterparty.validation_policy.mode);
    REQUIRE(actual_counterparty.validation_policy.verify_checksum ==
            expected_counterparty.validation_policy.verify_checksum);
    REQUIRE(actual_counterparty.reset_seq_num_on_logon == expected_counterparty.reset_seq_num_on_logon);
    REQUIRE(actual_counterparty.reset_seq_num_on_logout == expected_counterparty.reset_seq_num_on_logout);
    REQUIRE(actual_counterparty.reset_seq_num_on_disconnect == expected_counterparty.reset_seq_num_on_disconnect);
    REQUIRE(actual_counterparty.refresh_on_logon == expected_counterparty.refresh_on_logon);
    REQUIRE(actual_counterparty.send_next_expected_msg_seq_num == expected_counterparty.send_next_expected_msg_seq_num);
    REQUIRE(!actual_counterparty.tls_client.enabled);
    REQUIRE(!expected_counterparty.tls_client.enabled);
    REQUIRE(actual_counterparty.session_schedule.use_local_time ==
            expected_counterparty.session_schedule.use_local_time);
    REQUIRE(actual_counterparty.session_schedule.non_stop_session ==
            expected_counterparty.session_schedule.non_stop_session);
    REQUIRE(OptionalTimeEquals(actual_counterparty.session_schedule.start_time,
                               expected_counterparty.session_schedule.start_time));
    REQUIRE(OptionalTimeEquals(actual_counterparty.session_schedule.end_time,
                               expected_counterparty.session_schedule.end_time));
    REQUIRE(actual_counterparty.session_schedule.start_day == expected_counterparty.session_schedule.start_day);
    REQUIRE(actual_counterparty.session_schedule.end_day == expected_counterparty.session_schedule.end_day);
    REQUIRE(OptionalTimeEquals(actual_counterparty.session_schedule.logon_time,
                               expected_counterparty.session_schedule.logon_time));
    REQUIRE(OptionalTimeEquals(actual_counterparty.session_schedule.logout_time,
                               expected_counterparty.session_schedule.logout_time));
    REQUIRE(actual_counterparty.session_schedule.logon_day == expected_counterparty.session_schedule.logon_day);
    REQUIRE(actual_counterparty.session_schedule.logout_day == expected_counterparty.session_schedule.logout_day);
    REQUIRE(actual_counterparty.reconnect_enabled == expected_counterparty.reconnect_enabled);
    REQUIRE(actual_counterparty.reconnect_initial_ms == expected_counterparty.reconnect_initial_ms);
    REQUIRE(actual_counterparty.reconnect_max_ms == expected_counterparty.reconnect_max_ms);
    REQUIRE(actual_counterparty.reconnect_max_retries == expected_counterparty.reconnect_max_retries);
    REQUIRE(actual_counterparty.day_cut.mode == expected_counterparty.day_cut.mode);
    REQUIRE(actual_counterparty.day_cut.reset_hour == expected_counterparty.day_cut.reset_hour);
    REQUIRE(actual_counterparty.day_cut.reset_minute == expected_counterparty.day_cut.reset_minute);
    REQUIRE(actual_counterparty.day_cut.utc_offset_seconds == expected_counterparty.day_cut.utc_offset_seconds);
  }
}

auto
TouchFile(const std::filesystem::path& path) -> void
{
  std::ofstream out(path, std::ios::trunc);
  out << "test fixture\n";
}

auto
MakeTlsValidationConfig(const std::filesystem::path& artifact_path) -> nimble::runtime::EngineConfig
{
  nimble::runtime::EngineConfig config;
  config.worker_count = 1U;
  config.profile_artifacts.push_back(artifact_path);
  return config;
}

auto
MakeInitiator(std::string name, nimble::runtime::TlsClientConfig tls_client = {}) -> nimble::runtime::CounterpartyConfig
{
  return nimble::runtime::CounterpartyConfigBuilder::Initiator(
           std::move(name),
           6001U,
           nimble::session::SessionKey{ .sender_comp_id = "BUYTLS", .target_comp_id = "SELLTLS" },
           4400U)
    .tls_client(std::move(tls_client))
    .build();
}

auto
MakeAcceptor(std::string name, nimble::runtime::TransportSecurityRequirement requirement)
  -> nimble::runtime::CounterpartyConfig
{
  return nimble::runtime::CounterpartyConfigBuilder::Acceptor(
           std::move(name),
           7001U,
           nimble::session::SessionKey{ .sender_comp_id = "SELLTLS", .target_comp_id = "BUYTLS" },
           4400U)
    .acceptor_transport_security(requirement)
    .build();
}

} // namespace

TEST_CASE("runtime TLS config validation", "[runtime-config][tls]")
{
  const auto temp_root = std::filesystem::temp_directory_path() / "nimblefix-runtime-tls-config-test";
  std::filesystem::remove_all(temp_root);
  std::filesystem::create_directories(temp_root);

  const auto artifact_path = temp_root / "sample-profile.nfa";
  const auto cert_path = temp_root / "server-chain.pem";
  const auto key_path = temp_root / "server-key.pem";
  const auto ca_path = temp_root / "ca.pem";
  TouchFile(artifact_path);
  TouchFile(cert_path);
  TouchFile(key_path);
  TouchFile(ca_path);

  REQUIRE(!nimble::runtime::TlsClientConfig{}.enabled);
  REQUIRE(!nimble::runtime::TlsServerConfig{}.enabled);

  if (!nimble::runtime::TlsTransportEnabledAtBuild()) {
    auto listener_config = MakeTlsValidationConfig(artifact_path);
    listener_config.listeners.push_back(nimble::runtime::ListenerConfigBuilder::Named("tls-listener")
                                          .bind("127.0.0.1", 9101U)
                                          .tls_server(nimble::runtime::TlsServerConfig{ .enabled = true })
                                          .build());
    const auto listener_status = nimble::runtime::ValidateEngineConfig(listener_config);
    REQUIRE(!listener_status.ok());
    REQUIRE(StatusContains(listener_status, "tls_server.enabled=true"));

    auto counterparty_config = MakeTlsValidationConfig(artifact_path);
    counterparty_config.counterparties.push_back(
      MakeInitiator("tls-initiator", nimble::runtime::TlsClientConfig{ .enabled = true }));
    const auto counterparty_status = nimble::runtime::ValidateEngineConfig(counterparty_config);
    REQUIRE(!counterparty_status.ok());
    REQUIRE(StatusContains(counterparty_status, "tls_client.enabled=true"));

    auto tls_connect = nimble::transport::TransportConnection::Connect(
      "127.0.0.1", 1U, std::chrono::milliseconds(5), &counterparty_config.counterparties.front().tls_client);
    REQUIRE(!tls_connect.ok());
    REQUIRE(StatusContains(tls_connect.status(), "without optional TLS support"));

    std::filesystem::remove_all(temp_root);
    return;
  }

  {
    auto config = MakeTlsValidationConfig(artifact_path);
    config.listeners.push_back(
      nimble::runtime::ListenerConfigBuilder::Named("tls-missing-key")
        .bind("127.0.0.1", 9102U)
        .tls_server(nimble::runtime::TlsServerConfig{ .enabled = true, .certificate_chain_file = cert_path })
        .build());
    const auto status = nimble::runtime::ValidateEngineConfig(config);
    REQUIRE(!status.ok());
    REQUIRE(StatusContains(status, "requires certificate_chain_file and private_key_file"));
  }

  {
    auto config = MakeTlsValidationConfig(artifact_path);
    config.listeners.push_back(
      nimble::runtime::ListenerConfigBuilder::Named("tls-bad-ca")
        .bind("127.0.0.1", 9103U)
        .tls_server(nimble::runtime::TlsServerConfig{ .enabled = true,
                                                      .certificate_chain_file = cert_path,
                                                      .private_key_file = key_path,
                                                      .ca_file = temp_root / "missing-ca.pem" })
        .build());
    const auto status = nimble::runtime::ValidateEngineConfig(config);
    REQUIRE(!status.ok());
    REQUIRE(StatusContains(status, "ca_file does not exist"));
  }

  {
    auto config = MakeTlsValidationConfig(artifact_path);
    config.counterparties.push_back(MakeInitiator(
      "tls-client-pair", nimble::runtime::TlsClientConfig{ .enabled = true, .certificate_chain_file = cert_path }));
    const auto status = nimble::runtime::ValidateEngineConfig(config);
    REQUIRE(!status.ok());
    REQUIRE(StatusContains(status, "certificate_chain_file and private_key_file must be configured together"));
  }

  {
    auto config = MakeTlsValidationConfig(artifact_path);
    config.counterparties.push_back(
      MakeInitiator("tls-client-peer-name",
                    nimble::runtime::TlsClientConfig{
                      .enabled = true, .expected_peer_name = "fix.example.test", .verify_peer = false }));
    const auto status = nimble::runtime::ValidateEngineConfig(config);
    REQUIRE(!status.ok());
    REQUIRE(StatusContains(status, "expected_peer_name while verify_peer is disabled"));
  }

  {
    auto config = MakeTlsValidationConfig(artifact_path);
    config.counterparties.push_back(
      MakeInitiator("tls-client-version",
                    nimble::runtime::TlsClientConfig{ .enabled = true,
                                                      .ca_file = ca_path,
                                                      .min_version = nimble::runtime::TlsProtocolVersion::kTls13,
                                                      .max_version = nimble::runtime::TlsProtocolVersion::kTls12 }));
    const auto status = nimble::runtime::ValidateEngineConfig(config);
    REQUIRE(!status.ok());
    REQUIRE(StatusContains(status, "min_version above max_version"));
  }

  {
    auto config = MakeTlsValidationConfig(artifact_path);
    config.listeners.push_back(nimble::runtime::ListenerConfigBuilder::Named("tls-mtls-no-verify")
                                 .bind("127.0.0.1", 9104U)
                                 .tls_server(nimble::runtime::TlsServerConfig{ .enabled = true,
                                                                               .certificate_chain_file = cert_path,
                                                                               .private_key_file = key_path,
                                                                               .require_client_certificate = true })
                                 .build());
    const auto status = nimble::runtime::ValidateEngineConfig(config);
    REQUIRE(!status.ok());
    REQUIRE(StatusContains(status, "require_client_certificate implies verify_peer=true"));
  }

  {
    auto config = MakeTlsValidationConfig(artifact_path);
    config.listeners.push_back(nimble::runtime::ListenerConfigBuilder::Named("tls-mtls-no-ca")
                                 .bind("127.0.0.1", 9105U)
                                 .tls_server(nimble::runtime::TlsServerConfig{ .enabled = true,
                                                                               .certificate_chain_file = cert_path,
                                                                               .private_key_file = key_path,
                                                                               .verify_peer = true,
                                                                               .require_client_certificate = true })
                                 .build());
    const auto status = nimble::runtime::ValidateEngineConfig(config);
    REQUIRE(!status.ok());
    REQUIRE(StatusContains(status, "requires ca_file or ca_path"));
  }

  {
    auto config = MakeTlsValidationConfig(artifact_path);
    config.listeners.push_back(nimble::runtime::ListenerConfigBuilder::Named("plain").bind("127.0.0.1", 9106U).build());
    config.counterparties.push_back(
      MakeAcceptor("tls-only-session", nimble::runtime::TransportSecurityRequirement::kTlsOnly));
    const auto status = nimble::runtime::ValidateEngineConfig(config);
    REQUIRE(!status.ok());
    REQUIRE(StatusContains(status, "requires TLS-only acceptor transport"));
  }

  {
    auto config = MakeTlsValidationConfig(artifact_path);
    config.listeners.push_back(nimble::runtime::ListenerConfigBuilder::Named("tls")
                                 .bind("127.0.0.1", 9107U)
                                 .tls_server(nimble::runtime::TlsServerConfig{
                                   .enabled = true, .certificate_chain_file = cert_path, .private_key_file = key_path })
                                 .build());
    config.counterparties.push_back(
      MakeAcceptor("plain-only-session", nimble::runtime::TransportSecurityRequirement::kPlainOnly));
    const auto status = nimble::runtime::ValidateEngineConfig(config);
    REQUIRE(!status.ok());
    REQUIRE(StatusContains(status, "requires plain-only acceptor transport"));
  }

  {
    auto config = MakeTlsValidationConfig(artifact_path);
    config.listeners.push_back(nimble::runtime::ListenerConfigBuilder::Named("tls")
                                 .bind("127.0.0.1", 9108U)
                                 .tls_server(nimble::runtime::TlsServerConfig{ .enabled = true,
                                                                               .certificate_chain_file = cert_path,
                                                                               .private_key_file = key_path,
                                                                               .ca_file = ca_path,
                                                                               .verify_peer = true,
                                                                               .require_client_certificate = true })
                                 .build());
    config.counterparties.push_back(
      MakeAcceptor("tls-only-session", nimble::runtime::TransportSecurityRequirement::kTlsOnly));
    REQUIRE(nimble::runtime::ValidateEngineConfig(config).ok());
  }

  {
    auto config = MakeTlsValidationConfig(artifact_path);
    config.counterparties.push_back(
      MakeInitiator("tls-client-valid",
                    nimble::runtime::TlsClientConfig{ .enabled = true,
                                                      .server_name = "fix.example.test",
                                                      .ca_file = ca_path,
                                                      .certificate_chain_file = cert_path,
                                                      .private_key_file = key_path,
                                                      .min_version = nimble::runtime::TlsProtocolVersion::kTls12,
                                                      .max_version = nimble::runtime::TlsProtocolVersion::kTls13 }));
    REQUIRE(nimble::runtime::ValidateEngineConfig(config).ok());
  }

  std::filesystem::remove_all(temp_root);
}

TEST_CASE("structured config validation collects all errors", "[runtime-config]")
{
  nimble::runtime::EngineConfig config;
  config.worker_count = 1U;
  config.trace_mode = nimble::runtime::TraceMode::kRing;
  config.worker_cpu_affinity = { 0U, 1U };
  config.listeners.push_back(nimble::runtime::ListenerConfig{ .name = "", .worker_hint = 2U });

  config.counterparties.push_back(nimble::runtime::CounterpartyConfig{
    .name = "",
    .session =
      nimble::session::SessionConfig{
        .session_id = 0U,
        .key = nimble::session::SessionKey{ .begin_string = "", .sender_comp_id = "", .target_comp_id = "" },
        .profile_id = 0U,
        .heartbeat_interval_seconds = 30U,
        .is_initiator = false,
      },
    .transport_profile = nimble::session::TransportSessionProfile::Fix44(),
    .store_mode = nimble::runtime::StoreMode::kMmap,
  });

  config.counterparties.push_back(nimble::runtime::CounterpartyConfig{
    .name = "bad-fixt",
    .session =
      nimble::session::SessionConfig{
        .session_id = 2U,
        .key =
          nimble::session::SessionKey{
            .begin_string = "FIXT.1.1",
            .sender_comp_id = "SELL",
            .target_comp_id = "BUY",
          },
        .profile_id = 0U,
        .heartbeat_interval_seconds = 30U,
        .is_initiator = false,
      },
    .transport_profile = nimble::session::TransportSessionProfile::FixT11(),
    .session_schedule =
      nimble::runtime::SessionScheduleConfig{
        .start_time = nimble::runtime::SessionTimeOfDay{ 9U, 0U, 0U },
      },
  });

  const auto result = nimble::runtime::ValidateEngineConfigFull(config);
  REQUIRE(!result.ok());
  REQUIRE(result.has_errors());
  REQUIRE(HasDiagnosticField(result, "trace_capacity"));
  REQUIRE(HasDiagnosticField(result, "worker_cpu_affinity"));
  REQUIRE(HasDiagnosticField(result, "profile_artifacts"));
  REQUIRE(HasDiagnosticField(result, "listeners[0].name"));
  REQUIRE(HasDiagnosticField(result, "listeners[0].worker_hint"));
  REQUIRE(HasDiagnosticField(result, "counterparties[0].name"));
  REQUIRE(HasDiagnosticField(result, "counterparties[0].session.session_id"));
  REQUIRE(HasDiagnosticField(result, "counterparties[0].session.profile_id"));
  REQUIRE(HasDiagnosticField(result, "counterparties[0].session.key.begin_string"));
  REQUIRE(HasDiagnosticField(result, "counterparties[0].session.key.sender_comp_id"));
  REQUIRE(HasDiagnosticField(result, "counterparties[0].session.key.target_comp_id"));
  REQUIRE(HasDiagnosticField(result, "counterparties[0].store_path"));
  REQUIRE(HasDiagnosticField(result, "counterparties[1].session.profile_id"));
  REQUIRE(HasDiagnosticField(result, "counterparties[1].default_appl_ver_id"));
  REQUIRE(HasDiagnosticField(result, "counterparties[1].session_schedule.end_time"));

  const auto first_error = result.first_error_status();
  REQUIRE(!first_error.ok());
  REQUIRE(first_error.code() == nimble::base::ErrorCode::kInvalidArgument);
  REQUIRE(StatusContains(first_error, "ring trace mode requires a positive trace_capacity"));
  REQUIRE(StatusContains(nimble::runtime::ValidateEngineConfig(config),
                         "ring trace mode requires a positive trace_capacity"));
  REQUIRE(result.summary().find("counterparties[0].store_path") != std::string::npos);
}

TEST_CASE("config validation warnings", "[runtime-config]")
{
  nimble::runtime::EngineConfig config;
  config.enable_metrics = false;
  config.profile_mlock = true;
  config.counterparties.push_back(
    nimble::runtime::CounterpartyConfigBuilder::Initiator(
      "warn-initiator", 9101U, nimble::session::SessionKey{ .sender_comp_id = "BUY", .target_comp_id = "SELL" }, 4400U)
      .reconnect(5000U, 1000U, 3U)
      .build());

  const auto result = nimble::runtime::ValidateEngineConfigFull(config);
  REQUIRE(!result.ok());
  REQUIRE(result.has_errors());
  REQUIRE(result.has_warnings());
  REQUIRE(HasDiagnosticField(result, "enable_metrics", nimble::runtime::ConfigErrorSeverity::kWarning));
  REQUIRE(HasDiagnosticField(result, "profile_mlock", nimble::runtime::ConfigErrorSeverity::kWarning));
  REQUIRE(
    HasDiagnosticField(result, "counterparties[0].reconnect_enabled", nimble::runtime::ConfigErrorSeverity::kWarning));
  REQUIRE(HasDiagnosticField(result, "profile_artifacts"));
  REQUIRE(result.summary().find("3 warnings") != std::string::npos);

  nimble::runtime::EngineConfig warning_only_config;
  warning_only_config.enable_metrics = false;
  warning_only_config.profile_mlock = true;
  const auto warning_only = nimble::runtime::ValidateEngineConfigFull(warning_only_config);
  REQUIRE(warning_only.ok());
  REQUIRE(!warning_only.has_errors());
  REQUIRE(warning_only.has_warnings());
  REQUIRE(warning_only.first_error_status().ok());
}

TEST_CASE("ConfigToText round-trip", "[runtime-config]")
{
  const auto temp_root = std::filesystem::temp_directory_path() / "nimblefix-config-to-text-roundtrip-test";
  std::filesystem::remove_all(temp_root);
  std::filesystem::create_directories(temp_root);

  const auto artifact_a = temp_root / "profile-a.nfa";
  const auto artifact_b = temp_root / "profile-b.nfa";
  const auto dictionary_a = temp_root / "base.nfd";
  const auto dictionary_b = temp_root / "overlay.nfd";
  TouchFile(artifact_a);
  TouchFile(artifact_b);
  TouchFile(dictionary_a);
  TouchFile(dictionary_b);

  nimble::runtime::EngineConfig config;
  config.worker_count = 2U;
  config.enable_metrics = false;
  config.trace_mode = nimble::runtime::TraceMode::kRing;
  config.trace_capacity = 64U;
  config.front_door_cpu = 7U;
  config.worker_cpu_affinity = { 3U, 5U };
  config.queue_app_mode = nimble::runtime::QueueAppThreadingMode::kThreaded;
  config.poll_mode = nimble::runtime::PollMode::kBusy;
  config.io_backend = nimble::runtime::IoBackend::kEpoll;
  config.app_cpu_affinity = { 11U, 13U };
  config.profile_artifacts = { artifact_a, artifact_b };
  config.profile_dictionaries.push_back({ dictionary_a, dictionary_b });
  config.profile_madvise = true;
  config.profile_mlock = true;
  config.listeners.push_back(nimble::runtime::ListenerConfig{
    .name = "main",
    .host = "127.0.0.1",
    .port = 9901U,
    .worker_hint = 0U,
  });
  config.listeners.push_back(nimble::runtime::ListenerConfig{
    .name = "backup",
    .host = "0.0.0.0",
    .port = 9902U,
    .worker_hint = 1U,
  });
  config.accept_unknown_sessions = false;

  auto initiator =
    nimble::runtime::CounterpartyConfigBuilder::Initiator(
      "initiator-a", 1001U, nimble::session::SessionKey{ .sender_comp_id = "BUY1", .target_comp_id = "SELL1" }, 4400U)
      .store(nimble::runtime::StoreMode::kDurableBatch, temp_root / "initiator.store")
      .recovery_mode(nimble::session::RecoveryMode::kWarmRestart)
      .dispatch_mode(nimble::runtime::AppDispatchMode::kQueueDecoupled)
      .validation_policy(nimble::session::ValidationPolicy::Compatible())
      .reconnect(250U, 2000U, 7U)
      .build();
  initiator.durable_flush_threshold = 4U;
  initiator.durable_rollover_mode = nimble::store::DurableStoreRolloverMode::kLocalTime;
  initiator.durable_archive_limit = 9U;
  initiator.durable_local_utc_offset_seconds = 3600;
  initiator.durable_use_system_timezone = false;
  initiator.reset_seq_num_on_logon = true;
  initiator.reset_seq_num_on_logout = true;
  initiator.reset_seq_num_on_disconnect = true;
  initiator.refresh_on_logon = true;
  initiator.send_next_expected_msg_seq_num = true;
  initiator.sending_time_threshold_seconds = 60U;
  initiator.supported_app_msg_types = { "D", "8" };
  initiator.application_messages_available = false;
  initiator.day_cut = nimble::session::DayCutConfig{
    .mode = nimble::session::DayCutMode::kFixedLocalTime,
    .reset_hour = 17,
    .reset_minute = 30,
    .utc_offset_seconds = 3600,
  };
  initiator.session_schedule = nimble::runtime::SessionScheduleConfig{
    .use_local_time = true,
    .start_time = nimble::runtime::SessionTimeOfDay{ 9U, 30U, 0U },
    .end_time = nimble::runtime::SessionTimeOfDay{ 16U, 0U, 0U },
    .start_day = nimble::runtime::SessionDayOfWeek::kMonday,
    .end_day = nimble::runtime::SessionDayOfWeek::kFriday,
    .logon_time = nimble::runtime::SessionTimeOfDay{ 8U, 45U, 0U },
    .logout_time = nimble::runtime::SessionTimeOfDay{ 16U, 15U, 0U },
    .logon_day = nimble::runtime::SessionDayOfWeek::kMonday,
    .logout_day = nimble::runtime::SessionDayOfWeek::kFriday,
  };
  config.counterparties.push_back(initiator);

  auto acceptor =
    nimble::runtime::CounterpartyConfigBuilder::Acceptor(
      "acceptor-a", 1002U, nimble::session::SessionKey{ .sender_comp_id = "SELL2", .target_comp_id = "BUY2" }, 4400U)
      .store(nimble::runtime::StoreMode::kMemory)
      .validation_policy(nimble::session::ValidationPolicy::RawPassThrough())
      .build();
  acceptor.recovery_mode = nimble::session::RecoveryMode::kNoRecovery;
  acceptor.durable_rollover_mode = nimble::store::DurableStoreRolloverMode::kDisabled;
  acceptor.day_cut = nimble::session::DayCutConfig{
    .mode = nimble::session::DayCutMode::kExternalControl,
    .reset_hour = 0,
    .reset_minute = 0,
    .utc_offset_seconds = 0,
  };
  acceptor.session_schedule.non_stop_session = true;
  config.counterparties.push_back(acceptor);

  const auto text = nimble::runtime::ConfigToText(config);
  REQUIRE(text.find("listener|main|127.0.0.1|9901|0") != std::string::npos);
  REQUIRE(text.find("counterparty|initiator-a|1001|4400|FIX.4.4|BUY1|SELL1|durable|") != std::string::npos);
  REQUIRE(text.find("|D,8|false|") != std::string::npos);

  const auto parsed = nimble::runtime::LoadEngineConfigText(text);
  REQUIRE(parsed.ok());
  RequireSameCoreConfig(config, parsed.value());

  std::filesystem::remove_all(temp_root);
}

TEST_CASE("ConfigToText minimal config", "[runtime-config]")
{
  const auto temp_root = std::filesystem::temp_directory_path() / "nimblefix-config-to-text-minimal-test";
  std::filesystem::remove_all(temp_root);
  std::filesystem::create_directories(temp_root);
  const auto artifact_path = temp_root / "minimal.nfa";
  TouchFile(artifact_path);

  nimble::runtime::EngineConfig config;
  config.worker_count = 1U;
  config.profile_artifacts.push_back(artifact_path);

  const auto parsed = nimble::runtime::LoadEngineConfigText(nimble::runtime::ConfigToText(config));
  REQUIRE(parsed.ok());
  RequireSameCoreConfig(config, parsed.value());

  std::filesystem::remove_all(temp_root);
}

TEST_CASE("runtime-config", "[runtime-config]")
{
  const auto temp_root = std::filesystem::temp_directory_path() / "nimblefix-runtime-config-test";
  std::filesystem::create_directories(temp_root);

  const auto artifact_path = temp_root / "sample-profile.nfa";
  const auto transport_artifact_path = temp_root / "sample-transport-profile.nfa";
  REQUIRE(BuildSampleArtifact(artifact_path, 4400U).ok());
  REQUIRE(BuildSampleArtifact(transport_artifact_path, 4401U).ok());

  const auto config_path = temp_root / "engine.nfcfg";
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
  out << "profile=sample-profile.nfa\n";
  out << "profile=sample-transport-profile.nfa\n";
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
  REQUIRE(config.value().counterparties[0].validation_policy.verify_checksum);
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
  config.value().accept_unknown_sessions = true;

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

  std::atomic<std::uint32_t> factory_calls{ 0U };
  engine.SetSessionFactory(
    [&](const nimble::session::SessionKey& key) -> nimble::base::Result<nimble::runtime::CounterpartyConfig> {
      ++factory_calls;
      return nimble::runtime::CounterpartyConfigBuilder::Acceptor(
               "dynamic-" + key.target_comp_id, 0U, key, 4400U, nimble::session::TransportVersion::kFix44)
        .validation_policy(nimble::session::ValidationPolicy::Permissive())
        .build();
    });

  auto static_resolved = engine.ResolveInboundSession(peeked.value());
  REQUIRE(static_resolved.ok());
  REQUIRE(static_resolved.value().counterparty.session.session_id == 2003U);
  REQUIRE(factory_calls.load() == 0U);

  nimble::codec::SessionHeader dynamic_header;
  dynamic_header.begin_string = "FIX.4.4";
  dynamic_header.msg_type = "A";
  dynamic_header.sender_comp_id = "UNKNOWN-BUYER";
  dynamic_header.target_comp_id = "SELL1";
  dynamic_header.msg_seq_num = 1U;
  auto dynamic_resolved = engine.ResolveInboundSession(dynamic_header);
  REQUIRE(dynamic_resolved.ok());
  REQUIRE(dynamic_resolved.value().counterparty.session.session_id >= nimble::runtime::kFirstDynamicSessionId);
  REQUIRE(dynamic_resolved.value().counterparty.session.key.sender_comp_id == "SELL1");
  REQUIRE(dynamic_resolved.value().counterparty.session.key.target_comp_id == "UNKNOWN-BUYER");
  REQUIRE(factory_calls.load() == 1U);

  const auto metrics = engine.metrics().Snapshot();
  REQUIRE(metrics.sessions.size() == 5U);
  REQUIRE(metrics.workers.size() == 2U);

  const auto traces = engine.trace().Snapshot();
  REQUIRE(traces.size() >= 5U);
  REQUIRE(traces[0].kind == nimble::runtime::TraceEventKind::kConfigLoaded);

  const auto invalid_config_text = std::string("engine.worker_count=1\n"
                                               "engine.worker_cpu_affinity=1,2\n"
                                               "profile=sample-transport-profile.nfa\n"
                                               "counterparty|bad-fixt|3001|4401|FIXT.1.1|SELLX|BUYX|memory||"
                                               "memory|inline|30|false\n");
  auto invalid = nimble::runtime::LoadEngineConfigText(invalid_config_text, temp_root);
  REQUIRE(!invalid.ok());

  const auto invalid_listener_text = std::string("engine.worker_count=2\n"
                                                 "profile=sample-profile.nfa\n"
                                                 "listener|bad|127.0.0.1|9878|2\n");
  auto invalid_listener = nimble::runtime::LoadEngineConfigText(invalid_listener_text, temp_root);
  REQUIRE(!invalid_listener.ok());

  const auto invalid_app_cpu_text = std::string("engine.worker_count=2\n"
                                                "engine.app_cpu_affinity=7\n"
                                                "profile=sample-profile.nfa\n");
  auto invalid_app_cpu = nimble::runtime::LoadEngineConfigText(invalid_app_cpu_text, temp_root);
  REQUIRE(!invalid_app_cpu.ok());

  const auto advanced_config_text =
    std::string("engine.worker_count=1\n"
                "profile=sample-profile.nfa\n"
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

  auto builder_listener =
    nimble::runtime::ListenerConfigBuilder::Named("builder-main").bind("127.0.0.1", 9001U).worker_hint(1U).build();
  REQUIRE(builder_listener.name == "builder-main");
  REQUIRE(builder_listener.host == "127.0.0.1");
  REQUIRE(builder_listener.port == 9001U);
  REQUIRE(builder_listener.worker_hint == 1U);

  auto initiator = nimble::runtime::CounterpartyConfigBuilder::Initiator(
                     "builder-initiator",
                     3001U,
                     nimble::session::SessionKey{ .sender_comp_id = "BUY", .target_comp_id = "SELL" },
                     4400U,
                     nimble::session::TransportVersion::kFixT11)
                     .default_appl_ver_id("9")
                     .heartbeat_interval_seconds(45U)
                     .store(nimble::runtime::StoreMode::kDurableBatch, temp_root / "builder-initiator.store")
                     .recovery_mode(nimble::session::RecoveryMode::kWarmRestart)
                     .reconnect(250U, 2000U, 7U)
                     .build();
  REQUIRE(initiator.session.is_initiator);
  REQUIRE(initiator.transport_profile.version == nimble::session::TransportVersion::kFixT11);
  REQUIRE(initiator.session.key.begin_string == "FIXT.1.1");
  REQUIRE(initiator.default_appl_ver_id == "9");
  REQUIRE(initiator.session.default_appl_ver_id == "9");
  REQUIRE(initiator.reconnect_enabled);
  REQUIRE(initiator.reconnect_initial_ms == 250U);
  REQUIRE(initiator.recovery_mode == nimble::session::RecoveryMode::kWarmRestart);

  auto acceptor = nimble::runtime::CounterpartyConfigBuilder::Acceptor(
                    "builder-acceptor",
                    3002U,
                    nimble::session::SessionKey{ .sender_comp_id = "SELL", .target_comp_id = "BUY" },
                    4400U)
                    .dispatch_mode(nimble::runtime::AppDispatchMode::kQueueDecoupled)
                    .validation_policy(nimble::session::ValidationPolicy::Compatible())
                    .store(nimble::runtime::StoreMode::kMemory)
                    .build();
  REQUIRE(!acceptor.session.is_initiator);
  REQUIRE(acceptor.transport_profile.version == nimble::session::TransportVersion::kFix44);
  REQUIRE(acceptor.session.key.begin_string == "FIX.4.4");
  REQUIRE(acceptor.dispatch_mode == nimble::runtime::AppDispatchMode::kQueueDecoupled);
  REQUIRE(acceptor.validation_policy.mode == nimble::session::ValidationMode::kCompatible);
  REQUIRE(acceptor.validation_policy.verify_checksum);
  REQUIRE(!acceptor.reconnect_enabled);

  const auto strict_policy = nimble::session::ValidationPolicy::Strict();
  const auto compatible_policy = nimble::session::ValidationPolicy::Compatible();
  const auto permissive_policy = nimble::session::ValidationPolicy::Permissive();
  const auto raw_policy = nimble::session::ValidationPolicy::RawPassThrough();
  REQUIRE(strict_policy.verify_checksum);
  REQUIRE(compatible_policy.verify_checksum);
  REQUIRE(permissive_policy.verify_checksum);
  REQUIRE_FALSE(raw_policy.verify_checksum);

  REQUIRE(!nimble::runtime::TlsClientConfig{}.enabled);
  REQUIRE(!nimble::runtime::TlsServerConfig{}.enabled);

  if (!nimble::runtime::TlsTransportEnabledAtBuild()) {
    nimble::runtime::EngineConfig tls_listener_config;
    tls_listener_config.worker_count = 1U;
    tls_listener_config.profile_artifacts.push_back(artifact_path);
    tls_listener_config.listeners.push_back(nimble::runtime::ListenerConfigBuilder::Named("tls-listener")
                                              .bind("127.0.0.1", 9101U)
                                              .tls_server(nimble::runtime::TlsServerConfig{ .enabled = true })
                                              .build());

    const auto listener_status = nimble::runtime::ValidateEngineConfig(tls_listener_config);
    REQUIRE(!listener_status.ok());
    REQUIRE(listener_status.message().find("tls_server.enabled=true") != std::string::npos);

    nimble::runtime::EngineConfig tls_counterparty_config;
    tls_counterparty_config.worker_count = 1U;
    tls_counterparty_config.profile_artifacts.push_back(artifact_path);
    tls_counterparty_config.counterparties.push_back(
      nimble::runtime::CounterpartyConfigBuilder::Initiator(
        "tls-initiator",
        4001U,
        nimble::session::SessionKey{ .sender_comp_id = "BUYTLS", .target_comp_id = "SELLTLS" },
        4400U)
        .tls_client(nimble::runtime::TlsClientConfig{ .enabled = true })
        .build());

    const auto counterparty_status = nimble::runtime::ValidateEngineConfig(tls_counterparty_config);
    REQUIRE(!counterparty_status.ok());
    REQUIRE(counterparty_status.message().find("tls_client.enabled=true") != std::string::npos);

    auto tls_connect = nimble::transport::TransportConnection::Connect(
      "127.0.0.1", 1U, std::chrono::milliseconds(5), &tls_counterparty_config.counterparties.front().tls_client);
    REQUIRE(!tls_connect.ok());
    REQUIRE(tls_connect.status().message().find("without optional TLS support") != std::string::npos);
  }

  std::filesystem::remove(config_path);
  std::filesystem::remove(artifact_path);
  std::filesystem::remove(transport_artifact_path);
  std::filesystem::remove(store_path);
  std::filesystem::remove_all(durable_store_path);
  std::filesystem::remove_all(durable_local_store_path);
  std::filesystem::remove_all(temp_root);
}
