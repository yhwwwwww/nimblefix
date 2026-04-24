#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "nimblefix/base/status.h"
#include "nimblefix/runtime/io_backend.h"
#include "nimblefix/session/resend_recovery.h"
#include "nimblefix/session/session_core.h"
#include "nimblefix/session/transport_profile.h"
#include "nimblefix/session/validation_policy.h"
#include "nimblefix/store/durable_batch_store.h"

namespace nimble::runtime {

inline constexpr std::uint32_t kDefaultReconnectInitialMs = 1'000U;
inline constexpr std::uint32_t kDefaultReconnectMaxMs = 30'000U;
inline constexpr std::uint32_t kUnlimitedReconnectRetries = 0U;

enum class StoreMode : std::uint32_t
{
  kMemory = 0,
  kMmap,
  kDurableBatch,
};

enum class AppDispatchMode : std::uint32_t
{
  kInline = 0,
  kQueueDecoupled,
};

enum class TraceMode : std::uint32_t
{
  kDisabled = 0,
  kRing,
};

enum class QueueAppThreadingMode : std::uint32_t
{
  kCoScheduled = 0,
  kThreaded,
};

enum class PollMode : std::uint32_t
{
  kBlocking = 0,
  kBusy = 1,
};

enum class SessionDayOfWeek : std::uint32_t
{
  kSunday = 0,
  kMonday,
  kTuesday,
  kWednesday,
  kThursday,
  kFriday,
  kSaturday,
};

struct SessionTimeOfDay
{
  std::uint8_t hour{ 0 };
  std::uint8_t minute{ 0 };
  std::uint8_t second{ 0 };
};

struct SessionScheduleConfig
{
  bool use_local_time{ false };
  bool non_stop_session{ false };
  std::optional<SessionTimeOfDay> start_time;
  std::optional<SessionTimeOfDay> end_time;
  std::optional<SessionDayOfWeek> start_day;
  std::optional<SessionDayOfWeek> end_day;
  std::optional<SessionTimeOfDay> logon_time;
  std::optional<SessionTimeOfDay> logout_time;
  std::optional<SessionDayOfWeek> logon_day;
  std::optional<SessionDayOfWeek> logout_day;
};

struct ListenerConfig
{
  // Required. Unique listener name referenced by LiveAcceptor::OpenListeners().
  std::string name;
  // Optional bind address. Use 0.0.0.0 to listen on all interfaces.
  std::string host{ "0.0.0.0" };
  // Required for a real listener. Use 0 to request an ephemeral test port.
  std::uint16_t port{ 0 };
  // Optional accept-side routing hint. After Logon, the bound session worker
  // owns steady-state protocol and application work.
  std::uint32_t worker_hint{ 0 };
};

struct CounterpartyConfig
{
  // Required. Human-readable counterparty label used in logs and metrics.
  std::string name;
  // Required. Session identity is always expressed from the local engine's
  // perspective, even when matching inbound acceptor Logons.
  session::SessionConfig session;
  // Required. Keep this aligned with session.key.begin_string.
  session::TransportSessionProfile transport_profile;
  // Conditionally required. Persistent store modes require a non-empty path.
  std::filesystem::path store_path;
  // Conditionally required for FIXT.1.1 transport sessions.
  std::string default_appl_ver_id;
  // Memory is the simplest default. Mmap and durable batch require store_path.
  StoreMode store_mode{ StoreMode::kMemory };
  std::uint32_t durable_flush_threshold{ 0 };
  store::DurableStoreRolloverMode durable_rollover_mode{ store::DurableStoreRolloverMode::kUtcDay };
  std::uint32_t durable_archive_limit{ 0 };
  std::int32_t durable_local_utc_offset_seconds{ 0 };
  bool durable_use_system_timezone{ true };
  session::RecoveryMode recovery_mode{ session::RecoveryMode::kMemoryOnly };
  AppDispatchMode dispatch_mode{ AppDispatchMode::kInline };
  session::ValidationPolicy validation_policy{ session::ValidationPolicy::Strict() };
  bool reset_seq_num_on_logon{ false };
  bool reset_seq_num_on_logout{ false };
  bool reset_seq_num_on_disconnect{ false };
  bool refresh_on_logon{ false };
  bool send_next_expected_msg_seq_num{ false };
  SessionScheduleConfig session_schedule;
  // Initiator reconnect defaults. Leave disabled for acceptors.
  bool reconnect_enabled = false;
  std::uint32_t reconnect_initial_ms = kDefaultReconnectInitialMs;
  std::uint32_t reconnect_max_ms = kDefaultReconnectMaxMs;
  std::uint32_t reconnect_max_retries = kUnlimitedReconnectRetries;
  session::DayCutConfig day_cut;
};

struct EngineConfig
{
  // Recommended default until you need multi-session parallelism.
  std::uint32_t worker_count{ 1 };
  bool enable_metrics{ true };
  TraceMode trace_mode{ TraceMode::kDisabled };
  std::uint32_t trace_capacity{ 0 };
  std::optional<std::uint32_t> front_door_cpu;
  std::vector<std::uint32_t> worker_cpu_affinity;
  QueueAppThreadingMode queue_app_mode{ QueueAppThreadingMode::kCoScheduled };
  PollMode poll_mode{ PollMode::kBlocking };
  IoBackend io_backend{ IoBackend::kEpoll };
  std::vector<std::uint32_t> app_cpu_affinity;
  std::vector<std::filesystem::path> profile_artifacts;
  std::vector<std::vector<std::filesystem::path>> profile_dictionaries;
  bool profile_madvise{ false };
  bool profile_mlock{ false };
  // Acceptor-only. At least one listener is required before OpenListeners().
  std::vector<ListenerConfig> listeners;
  // Static session inventory. Initiators usually configure one entry per
  // outbound session.
  std::vector<CounterpartyConfig> counterparties;
  // Unknown inbound Logons consult SessionFactory only when this flag is true.
  // Static counterparties still win first. If true but no factory is
  // installed, unknown Logons are rejected.
  bool accept_unknown_sessions{ false };
};

class ListenerConfigBuilder
{
public:
  static auto Named(std::string name) -> ListenerConfigBuilder;

  auto bind(std::string host, std::uint16_t port) -> ListenerConfigBuilder&;
  auto worker_hint(std::uint32_t worker_id) -> ListenerConfigBuilder&;
  [[nodiscard]] auto build() const -> ListenerConfig;

private:
  explicit ListenerConfigBuilder(ListenerConfig config);

  ListenerConfig config_{};
};

class CounterpartyConfigBuilder
{
public:
  static auto Initiator(std::string name,
                        std::uint64_t session_id,
                        session::SessionKey key,
                        std::uint64_t profile_id,
                        session::TransportVersion transport_version = session::TransportVersion::kFix44)
    -> CounterpartyConfigBuilder;

  static auto Acceptor(std::string name,
                       std::uint64_t session_id,
                       session::SessionKey key,
                       std::uint64_t profile_id,
                       session::TransportVersion transport_version = session::TransportVersion::kFix44)
    -> CounterpartyConfigBuilder;

  // Re-normalizes session.key.begin_string to the selected transport version.
  auto transport_version(session::TransportVersion version) -> CounterpartyConfigBuilder&;
  auto default_appl_ver_id(std::string value) -> CounterpartyConfigBuilder&;
  auto heartbeat_interval_seconds(std::uint32_t seconds) -> CounterpartyConfigBuilder&;
  auto store(StoreMode mode, std::filesystem::path path = {}) -> CounterpartyConfigBuilder&;
  auto recovery_mode(session::RecoveryMode mode) -> CounterpartyConfigBuilder&;
  auto dispatch_mode(AppDispatchMode mode) -> CounterpartyConfigBuilder&;
  auto validation_policy(session::ValidationPolicy policy) -> CounterpartyConfigBuilder&;
  auto reconnect(std::uint32_t initial_ms = kDefaultReconnectInitialMs,
                 std::uint32_t max_ms = kDefaultReconnectMaxMs,
                 std::uint32_t max_retries = kUnlimitedReconnectRetries) -> CounterpartyConfigBuilder&;
  auto disable_reconnect() -> CounterpartyConfigBuilder&;
  [[nodiscard]] auto build() const -> CounterpartyConfig;

private:
  explicit CounterpartyConfigBuilder(CounterpartyConfig config);

  CounterpartyConfig config_{};
};

[[nodiscard]] auto
ValidateSessionSchedule(const SessionScheduleConfig& schedule) -> base::Status;
[[nodiscard]] auto
IsWithinSessionWindow(const SessionScheduleConfig& schedule, std::uint64_t unix_time_ns) -> bool;
[[nodiscard]] auto
IsWithinLogonWindow(const SessionScheduleConfig& schedule, std::uint64_t unix_time_ns) -> bool;
[[nodiscard]] auto
NextLogonWindowStart(const SessionScheduleConfig& schedule, std::uint64_t unix_time_ns) -> std::optional<std::uint64_t>;

auto
ValidateEngineConfig(const EngineConfig& config) -> base::Status;

} // namespace nimble::runtime
