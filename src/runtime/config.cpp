#include "nimblefix/runtime/config.h"

#include <ctime>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace nimble::runtime {

namespace {

constexpr int kSecondsPerDay = 24 * 60 * 60;

struct SessionWindowSpec
{
  bool use_local_time{ false };
  int start_second{ 0 };
  int end_second{ 0 };
  std::optional<int> start_day;
  std::optional<int> end_day;
};

struct CalendarPoint
{
  std::tm civil_time{};
  int weekday{ 0 };
  int second_of_day{ 0 };
};

auto
TransportProfileForVersion(session::TransportVersion version) -> session::TransportSessionProfile
{
  switch (version) {
    case session::TransportVersion::kFix40:
      return session::TransportSessionProfile::Fix40();
    case session::TransportVersion::kFix41:
      return session::TransportSessionProfile::Fix41();
    case session::TransportVersion::kFix42:
      return session::TransportSessionProfile::Fix42();
    case session::TransportVersion::kFix43:
      return session::TransportSessionProfile::Fix43();
    case session::TransportVersion::kFix44:
      return session::TransportSessionProfile::Fix44();
    case session::TransportVersion::kFixT11:
      return session::TransportSessionProfile::FixT11();
  }

  return session::TransportSessionProfile::Fix44();
}

auto
IsTransportSession(std::string_view begin_string) -> bool
{
  return begin_string == "FIXT.1.1";
}

auto
IsValidTimeOfDay(const SessionTimeOfDay& time) -> bool
{
  return time.hour < 24U && time.minute < 60U && time.second < 60U;
}

auto
HasSessionWindowFields(const SessionScheduleConfig& schedule) -> bool
{
  return schedule.start_time.has_value() || schedule.end_time.has_value() || schedule.start_day.has_value() ||
         schedule.end_day.has_value();
}

auto
HasLogonWindowFields(const SessionScheduleConfig& schedule) -> bool
{
  return schedule.logon_time.has_value() || schedule.logout_time.has_value() || schedule.logon_day.has_value() ||
         schedule.logout_day.has_value();
}

auto
SecondsOfDay(const SessionTimeOfDay& time) -> int
{
  return static_cast<int>(time.hour) * 3600 + static_cast<int>(time.minute) * 60 + static_cast<int>(time.second);
}

auto
BuildWindowSpec(const SessionScheduleConfig& schedule, bool logon_window) -> std::optional<SessionWindowSpec>
{
  if (schedule.non_stop_session) {
    return std::nullopt;
  }

  if (logon_window && HasLogonWindowFields(schedule)) {
    return SessionWindowSpec{
      .use_local_time = schedule.use_local_time,
      .start_second = SecondsOfDay(*schedule.logon_time),
      .end_second = SecondsOfDay(*schedule.logout_time),
      .start_day =
        schedule.logon_day.has_value() ? std::optional<int>(static_cast<int>(*schedule.logon_day)) : std::nullopt,
      .end_day =
        schedule.logout_day.has_value() ? std::optional<int>(static_cast<int>(*schedule.logout_day)) : std::nullopt,
    };
  }

  if (!HasSessionWindowFields(schedule)) {
    return std::nullopt;
  }

  return SessionWindowSpec{
    .use_local_time = schedule.use_local_time,
    .start_second = SecondsOfDay(*schedule.start_time),
    .end_second = SecondsOfDay(*schedule.end_time),
    .start_day =
      schedule.start_day.has_value() ? std::optional<int>(static_cast<int>(*schedule.start_day)) : std::nullopt,
    .end_day = schedule.end_day.has_value() ? std::optional<int>(static_cast<int>(*schedule.end_day)) : std::nullopt,
  };
}

auto
BuildCalendarPoint(std::uint64_t unix_time_ns, bool use_local_time) -> CalendarPoint
{
  const auto unix_seconds = static_cast<std::time_t>(unix_time_ns / 1'000'000'000ULL);
  std::tm civil_time{};
  if (use_local_time) {
    localtime_r(&unix_seconds, &civil_time);
  } else {
    gmtime_r(&unix_seconds, &civil_time);
  }
  return CalendarPoint{
    .civil_time = civil_time,
    .weekday = civil_time.tm_wday,
    .second_of_day = civil_time.tm_hour * 3600 + civil_time.tm_min * 60 + civil_time.tm_sec,
  };
}

auto
MakeUnixTimeNs(std::tm civil_time, bool use_local_time) -> std::optional<std::uint64_t>
{
  const auto unix_seconds = use_local_time ? mktime(&civil_time) : timegm(&civil_time);
  if (unix_seconds < 0) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(unix_seconds) * 1'000'000'000ULL;
}

auto
IsWithinWindow(const SessionWindowSpec& window, std::uint64_t unix_time_ns) -> bool
{
  const auto point = BuildCalendarPoint(unix_time_ns, window.use_local_time);
  const auto current =
    window.start_day.has_value() ? point.weekday * kSecondsPerDay + point.second_of_day : point.second_of_day;
  const auto start =
    window.start_day.has_value() ? *window.start_day * kSecondsPerDay + window.start_second : window.start_second;
  const auto end =
    window.end_day.has_value() ? *window.end_day * kSecondsPerDay + window.end_second : window.end_second;
  if (start == end) {
    return true;
  }
  if (start < end) {
    return current >= start && current < end;
  }
  return current >= start || current < end;
}

auto
NextWindowStart(const SessionWindowSpec& window, std::uint64_t unix_time_ns) -> std::optional<std::uint64_t>
{
  if (IsWithinWindow(window, unix_time_ns)) {
    return unix_time_ns;
  }

  const auto point = BuildCalendarPoint(unix_time_ns, window.use_local_time);
  auto candidate = point.civil_time;
  candidate.tm_hour = window.start_second / 3600;
  candidate.tm_min = (window.start_second / 60) % 60;
  candidate.tm_sec = window.start_second % 60;

  if (window.start_day.has_value()) {
    int delta_days = *window.start_day - point.weekday;
    if (delta_days < 0) {
      delta_days += 7;
    }
    candidate.tm_mday += delta_days;
    auto candidate_ns = MakeUnixTimeNs(candidate, window.use_local_time);
    if (!candidate_ns.has_value()) {
      return std::nullopt;
    }
    if (*candidate_ns <= unix_time_ns) {
      candidate.tm_mday += 7;
      return MakeUnixTimeNs(candidate, window.use_local_time);
    }
    return candidate_ns;
  }

  auto candidate_ns = MakeUnixTimeNs(candidate, window.use_local_time);
  if (!candidate_ns.has_value()) {
    return std::nullopt;
  }
  if (*candidate_ns <= unix_time_ns) {
    candidate.tm_mday += 1;
    return MakeUnixTimeNs(candidate, window.use_local_time);
  }
  return candidate_ns;
}

} // namespace

auto
ValidateSessionSchedule(const SessionScheduleConfig& schedule) -> base::Status
{
  const auto validate_time_pair = [](std::string_view label,
                                     const std::optional<SessionTimeOfDay>& start_time,
                                     const std::optional<SessionTimeOfDay>& end_time,
                                     const std::optional<SessionDayOfWeek>& start_day,
                                     const std::optional<SessionDayOfWeek>& end_day) -> base::Status {
    if (start_time.has_value() != end_time.has_value()) {
      return base::Status::InvalidArgument(std::string(label) + " requires both start and end times");
    }
    if (start_day.has_value() != end_day.has_value()) {
      return base::Status::InvalidArgument(std::string(label) + " requires both start and end days");
    }
    if ((start_day.has_value() || end_day.has_value()) && !start_time.has_value()) {
      return base::Status::InvalidArgument(std::string(label) + " days require matching times");
    }
    if (start_time.has_value() && !IsValidTimeOfDay(*start_time)) {
      return base::Status::InvalidArgument(std::string(label) + " start time is out of range");
    }
    if (end_time.has_value() && !IsValidTimeOfDay(*end_time)) {
      return base::Status::InvalidArgument(std::string(label) + " end time is out of range");
    }
    return base::Status::Ok();
  };

  if (schedule.non_stop_session && (HasSessionWindowFields(schedule) || HasLogonWindowFields(schedule))) {
    return base::Status::InvalidArgument("non_stop_session cannot be combined with session or logon window "
                                         "settings");
  }

  auto session_window_status =
    validate_time_pair("session window", schedule.start_time, schedule.end_time, schedule.start_day, schedule.end_day);
  if (!session_window_status.ok()) {
    return session_window_status;
  }

  auto logon_window_status = validate_time_pair(
    "logon window", schedule.logon_time, schedule.logout_time, schedule.logon_day, schedule.logout_day);
  if (!logon_window_status.ok()) {
    return logon_window_status;
  }

  return base::Status::Ok();
}

auto
IsWithinSessionWindow(const SessionScheduleConfig& schedule, std::uint64_t unix_time_ns) -> bool
{
  const auto window = BuildWindowSpec(schedule, false);
  return !window.has_value() || IsWithinWindow(*window, unix_time_ns);
}

auto
IsWithinLogonWindow(const SessionScheduleConfig& schedule, std::uint64_t unix_time_ns) -> bool
{
  const auto window = BuildWindowSpec(schedule, true);
  return !window.has_value() || IsWithinWindow(*window, unix_time_ns);
}

auto
NextLogonWindowStart(const SessionScheduleConfig& schedule, std::uint64_t unix_time_ns) -> std::optional<std::uint64_t>
{
  const auto window = BuildWindowSpec(schedule, true);
  if (!window.has_value()) {
    return unix_time_ns;
  }
  return NextWindowStart(*window, unix_time_ns);
}

namespace {

auto
TlsVersionRank(TlsProtocolVersion version) -> int
{
  switch (version) {
    case TlsProtocolVersion::kSystemDefault:
      return 0;
    case TlsProtocolVersion::kTls10:
      return 10;
    case TlsProtocolVersion::kTls11:
      return 11;
    case TlsProtocolVersion::kTls12:
      return 12;
    case TlsProtocolVersion::kTls13:
      return 13;
  }
  return 0;
}

auto
PathExists(const std::filesystem::path& path) -> bool
{
  return path.empty() || std::filesystem::exists(path);
}

auto
ValidateTlsVersionRange(TlsProtocolVersion min_version, TlsProtocolVersion max_version, std::string_view owner_label)
  -> base::Status
{
  const auto min_rank = TlsVersionRank(min_version);
  const auto max_rank = TlsVersionRank(max_version);
  if (min_rank != 0 && max_rank != 0 && min_rank > max_rank) {
    return base::Status::InvalidArgument(std::string(owner_label) + " config has min_version above max_version");
  }
  return base::Status::Ok();
}

auto
ValidateTlsClientConfig(std::string_view counterparty_name, const TlsClientConfig& config) -> base::Status
{
  if (!config.enabled) {
    return base::Status::Ok();
  }

  if (!TlsTransportEnabledAtBuild()) {
    return base::Status::InvalidArgument("counterparty '" + std::string(counterparty_name) +
                                         "' sets tls_client.enabled=true but this build was compiled without "
                                         "optional TLS support");
  }
  if (config.certificate_chain_file.empty() != config.private_key_file.empty()) {
    return base::Status::InvalidArgument("counterparty '" + std::string(counterparty_name) +
                                         "' TLS client certificate_chain_file and private_key_file must be configured "
                                         "together");
  }
  if (!PathExists(config.certificate_chain_file)) {
    return base::Status::InvalidArgument("counterparty '" + std::string(counterparty_name) +
                                         "' TLS client certificate_chain_file does not exist");
  }
  if (!PathExists(config.private_key_file)) {
    return base::Status::InvalidArgument("counterparty '" + std::string(counterparty_name) +
                                         "' TLS client private_key_file does not exist");
  }
  if (!PathExists(config.ca_file)) {
    return base::Status::InvalidArgument("counterparty '" + std::string(counterparty_name) +
                                         "' TLS client ca_file does not exist");
  }
  if (!PathExists(config.ca_path)) {
    return base::Status::InvalidArgument("counterparty '" + std::string(counterparty_name) +
                                         "' TLS client ca_path does not exist");
  }
  if (!config.expected_peer_name.empty() && !config.verify_peer) {
    return base::Status::InvalidArgument("counterparty '" + std::string(counterparty_name) +
                                         "' sets expected_peer_name while verify_peer is disabled");
  }
  return ValidateTlsVersionRange(
    config.min_version, config.max_version, "counterparty '" + std::string(counterparty_name) + "' TLS client");
}

auto
ValidateTlsServerConfig(std::string_view listener_name, const TlsServerConfig& config) -> base::Status
{
  if (!config.enabled) {
    return base::Status::Ok();
  }

  if (!TlsTransportEnabledAtBuild()) {
    return base::Status::InvalidArgument("listener '" + std::string(listener_name) +
                                         "' sets tls_server.enabled=true but this build was compiled without "
                                         "optional TLS support");
  }
  if (config.certificate_chain_file.empty() || config.private_key_file.empty()) {
    return base::Status::InvalidArgument("listener '" + std::string(listener_name) +
                                         "' TLS server requires certificate_chain_file and private_key_file");
  }
  if (!PathExists(config.certificate_chain_file)) {
    return base::Status::InvalidArgument("listener '" + std::string(listener_name) +
                                         "' TLS server certificate_chain_file does not exist");
  }
  if (!PathExists(config.private_key_file)) {
    return base::Status::InvalidArgument("listener '" + std::string(listener_name) +
                                         "' TLS server private_key_file does not exist");
  }
  if (!PathExists(config.ca_file)) {
    return base::Status::InvalidArgument("listener '" + std::string(listener_name) +
                                         "' TLS server ca_file does not exist");
  }
  if (!PathExists(config.ca_path)) {
    return base::Status::InvalidArgument("listener '" + std::string(listener_name) +
                                         "' TLS server ca_path does not exist");
  }
  if (config.require_client_certificate && !config.verify_peer) {
    return base::Status::InvalidArgument("listener '" + std::string(listener_name) +
                                         "' TLS server require_client_certificate implies verify_peer=true");
  }
  if (config.require_client_certificate && config.ca_file.empty() && config.ca_path.empty()) {
    return base::Status::InvalidArgument("listener '" + std::string(listener_name) +
                                         "' TLS server require_client_certificate requires ca_file or ca_path");
  }
  return ValidateTlsVersionRange(
    config.min_version, config.max_version, "listener '" + std::string(listener_name) + "' TLS server");
}

} // namespace

auto
TlsTransportEnabledAtBuild() noexcept -> bool
{
#if defined(NIMBLEFIX_ENABLE_TLS)
  return true;
#else
  return false;
#endif
}

auto
ValidateEngineConfig(const EngineConfig& config) -> base::Status
{
  if (config.worker_count == 0) {
    return base::Status::InvalidArgument("engine worker_count must be positive");
  }
  if (config.trace_mode == TraceMode::kRing && config.trace_capacity == 0) {
    return base::Status::InvalidArgument("ring trace mode requires a positive trace_capacity");
  }
  if (config.worker_cpu_affinity.size() > config.worker_count) {
    return base::Status::InvalidArgument("worker_cpu_affinity must not contain more entries than worker_count");
  }
  if (config.app_cpu_affinity.size() > config.worker_count) {
    return base::Status::InvalidArgument("app_cpu_affinity must not contain more entries than worker_count");
  }
  if (config.queue_app_mode == QueueAppThreadingMode::kCoScheduled && !config.app_cpu_affinity.empty()) {
    return base::Status::InvalidArgument("app_cpu_affinity requires engine.queue_app_mode=threaded");
  }
  if (!config.counterparties.empty() && config.profile_artifacts.empty() && config.profile_dictionaries.empty()) {
    return base::Status::InvalidArgument("counterparty configs require at least one profile artifact");
  }

  std::unordered_set<std::string> listener_names;
  bool has_plain_listener = false;
  bool has_tls_listener = false;
  for (const auto& listener : config.listeners) {
    if (listener.name.empty()) {
      return base::Status::InvalidArgument("listener name must not be empty");
    }
    if (!listener_names.emplace(listener.name).second) {
      return base::Status::AlreadyExists("duplicate listener name in runtime config");
    }
    if (listener.worker_hint >= config.worker_count) {
      return base::Status::InvalidArgument("listener worker_hint must be less than worker_count");
    }
    auto tls_status = ValidateTlsServerConfig(listener.name, listener.tls_server);
    if (!tls_status.ok()) {
      return tls_status;
    }
    if (listener.tls_server.enabled) {
      has_tls_listener = true;
    } else {
      has_plain_listener = true;
    }
  }

  std::unordered_set<std::uint64_t> session_ids;
  for (const auto& counterparty : config.counterparties) {
    if (counterparty.name.empty()) {
      return base::Status::InvalidArgument("counterparty name must not be empty");
    }
    if (counterparty.session.session_id == 0) {
      return base::Status::InvalidArgument("counterparty session_id must be positive");
    }
    if (!session_ids.emplace(counterparty.session.session_id).second) {
      return base::Status::AlreadyExists("duplicate session_id in runtime config");
    }
    if (counterparty.session.profile_id == 0) {
      return base::Status::InvalidArgument("counterparty profile_id must be positive");
    }
    if (counterparty.session.key.begin_string.empty() || counterparty.session.key.sender_comp_id.empty() ||
        counterparty.session.key.target_comp_id.empty()) {
      return base::Status::InvalidArgument("counterparty session key fields must not be empty");
    }
    if (IsTransportSession(counterparty.session.key.begin_string) && counterparty.default_appl_ver_id.empty()) {
      return base::Status::InvalidArgument("FIXT counterparties require default_appl_ver_id");
    }
    if ((counterparty.store_mode == StoreMode::kMmap || counterparty.store_mode == StoreMode::kDurableBatch) &&
        counterparty.store_path.empty()) {
      return base::Status::InvalidArgument("persistent store modes require a store_path");
    }
    if (counterparty.recovery_mode == session::RecoveryMode::kWarmRestart &&
        counterparty.store_mode == StoreMode::kMemory) {
      return base::Status::InvalidArgument("warm restart recovery requires a persistent store mode");
    }
    if (counterparty.reset_seq_num_on_logon && !counterparty.transport_profile.supports_reset_on_logon) {
      return base::Status::InvalidArgument("reset_seq_num_on_logon is not supported by the configured transport "
                                           "version");
    }
    if (counterparty.send_next_expected_msg_seq_num &&
        !counterparty.transport_profile.supports_next_expected_msg_seq_num) {
      return base::Status::InvalidArgument("send_next_expected_msg_seq_num is not supported by the configured "
                                           "transport version");
    }
    auto schedule_status = ValidateSessionSchedule(counterparty.session_schedule);
    if (!schedule_status.ok()) {
      return schedule_status;
    }
    auto tls_status = ValidateTlsClientConfig(counterparty.name, counterparty.tls_client);
    if (!tls_status.ok()) {
      return tls_status;
    }
    if (!counterparty.session.is_initiator && counterparty.tls_client.enabled) {
      return base::Status::InvalidArgument("counterparty '" + counterparty.name +
                                           "' is acceptor-mode but configures initiator TLS client settings");
    }
    if (counterparty.session.is_initiator &&
        counterparty.acceptor_transport_security != TransportSecurityRequirement::kAny) {
      return base::Status::InvalidArgument("counterparty '" + counterparty.name +
                                           "' is initiator-mode and must not set acceptor_transport_security");
    }
    if (!counterparty.session.is_initiator) {
      if (counterparty.acceptor_transport_security == TransportSecurityRequirement::kTlsOnly && !has_tls_listener) {
        return base::Status::InvalidArgument("counterparty '" + counterparty.name +
                                             "' requires TLS-only acceptor transport but no TLS listener is "
                                             "configured");
      }
      if (counterparty.acceptor_transport_security == TransportSecurityRequirement::kPlainOnly && !has_plain_listener) {
        return base::Status::InvalidArgument("counterparty '" + counterparty.name +
                                             "' requires plain-only acceptor transport but no plain listener is "
                                             "configured");
      }
    }
  }

  return base::Status::Ok();
}

ListenerConfigBuilder::ListenerConfigBuilder(ListenerConfig config)
  : config_(std::move(config))
{
}

auto
ListenerConfigBuilder::Named(std::string name) -> ListenerConfigBuilder
{
  return ListenerConfigBuilder(ListenerConfig{ .name = std::move(name) });
}

auto
ListenerConfigBuilder::bind(std::string host, std::uint16_t port) -> ListenerConfigBuilder&
{
  config_.host = std::move(host);
  config_.port = port;
  return *this;
}

auto
ListenerConfigBuilder::worker_hint(std::uint32_t worker_id) -> ListenerConfigBuilder&
{
  config_.worker_hint = worker_id;
  return *this;
}

auto
ListenerConfigBuilder::tls_server(TlsServerConfig config) -> ListenerConfigBuilder&
{
  config_.tls_server = std::move(config);
  return *this;
}

auto
ListenerConfigBuilder::build() const -> ListenerConfig
{
  return config_;
}

CounterpartyConfigBuilder::CounterpartyConfigBuilder(CounterpartyConfig config)
  : config_(std::move(config))
{
}

auto
CounterpartyConfigBuilder::Initiator(std::string name,
                                     std::uint64_t session_id,
                                     session::SessionKey key,
                                     std::uint64_t profile_id,
                                     session::TransportVersion transport_version) -> CounterpartyConfigBuilder
{
  CounterpartyConfig config;
  config.name = std::move(name);
  config.session.session_id = session_id;
  config.session.key = std::move(key);
  config.session.profile_id = profile_id;
  config.session.heartbeat_interval_seconds =
    TransportProfileForVersion(transport_version).default_heartbeat_interval_seconds;
  config.session.is_initiator = true;
  return CounterpartyConfigBuilder(std::move(config)).transport_version(transport_version).disable_reconnect();
}

auto
CounterpartyConfigBuilder::Acceptor(std::string name,
                                    std::uint64_t session_id,
                                    session::SessionKey key,
                                    std::uint64_t profile_id,
                                    session::TransportVersion transport_version) -> CounterpartyConfigBuilder
{
  CounterpartyConfig config;
  config.name = std::move(name);
  config.session.session_id = session_id;
  config.session.key = std::move(key);
  config.session.profile_id = profile_id;
  config.session.heartbeat_interval_seconds =
    TransportProfileForVersion(transport_version).default_heartbeat_interval_seconds;
  config.session.is_initiator = false;
  return CounterpartyConfigBuilder(std::move(config)).transport_version(transport_version).disable_reconnect();
}

auto
CounterpartyConfigBuilder::transport_version(session::TransportVersion version) -> CounterpartyConfigBuilder&
{
  config_.transport_profile = TransportProfileForVersion(version);
  config_.session.key.begin_string = config_.transport_profile.begin_string;
  return *this;
}

auto
CounterpartyConfigBuilder::default_appl_ver_id(std::string value) -> CounterpartyConfigBuilder&
{
  config_.default_appl_ver_id = value;
  config_.session.default_appl_ver_id = std::move(value);
  return *this;
}

auto
CounterpartyConfigBuilder::heartbeat_interval_seconds(std::uint32_t seconds) -> CounterpartyConfigBuilder&
{
  config_.session.heartbeat_interval_seconds = seconds;
  return *this;
}

auto
CounterpartyConfigBuilder::store(StoreMode mode, std::filesystem::path path) -> CounterpartyConfigBuilder&
{
  config_.store_mode = mode;
  config_.store_path = std::move(path);
  return *this;
}

auto
CounterpartyConfigBuilder::recovery_mode(session::RecoveryMode mode) -> CounterpartyConfigBuilder&
{
  config_.recovery_mode = mode;
  return *this;
}

auto
CounterpartyConfigBuilder::dispatch_mode(AppDispatchMode mode) -> CounterpartyConfigBuilder&
{
  config_.dispatch_mode = mode;
  return *this;
}

auto
CounterpartyConfigBuilder::validation_policy(session::ValidationPolicy policy) -> CounterpartyConfigBuilder&
{
  config_.validation_policy = std::move(policy);
  return *this;
}

auto
CounterpartyConfigBuilder::tls_client(TlsClientConfig config) -> CounterpartyConfigBuilder&
{
  config_.tls_client = std::move(config);
  return *this;
}

auto
CounterpartyConfigBuilder::acceptor_transport_security(TransportSecurityRequirement requirement)
  -> CounterpartyConfigBuilder&
{
  config_.acceptor_transport_security = requirement;
  return *this;
}

auto
CounterpartyConfigBuilder::reconnect(std::uint32_t initial_ms, std::uint32_t max_ms, std::uint32_t max_retries)
  -> CounterpartyConfigBuilder&
{
  config_.reconnect_enabled = true;
  config_.reconnect_initial_ms = initial_ms;
  config_.reconnect_max_ms = max_ms;
  config_.reconnect_max_retries = max_retries;
  return *this;
}

auto
CounterpartyConfigBuilder::disable_reconnect() -> CounterpartyConfigBuilder&
{
  config_.reconnect_enabled = false;
  config_.reconnect_initial_ms = kDefaultReconnectInitialMs;
  config_.reconnect_max_ms = kDefaultReconnectMaxMs;
  config_.reconnect_max_retries = kUnlimitedReconnectRetries;
  return *this;
}

auto
CounterpartyConfigBuilder::build() const -> CounterpartyConfig
{
  return config_;
}

} // namespace nimble::runtime
