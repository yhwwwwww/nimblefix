#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "nimblefix/base/status.h"
#include "nimblefix/runtime/io_poller.h"
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
  // Inline callbacks execute on the session owner runtime worker thread.
  kInline = 0,
  // Queue-decoupled mode keeps one worker-local queue per runtime worker.
  kQueueDecoupled,
};

enum class TraceMode : std::uint32_t
{
  kDisabled = 0,
  kRing,
};

enum class QueueAppThreadingMode : std::uint32_t
{
  // Queue-decoupled handlers drain on the owning FIX worker thread.
  kCoScheduled = 0,
  // Queue-decoupled handlers run on one paired application thread per worker.
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
  std::string name;
  std::string host{ "0.0.0.0" };
  std::uint16_t port{ 0 };
  // Seeds accept-side routing into the worker pool; it does not create a
  // dedicated listener thread.
  std::uint32_t worker_hint{ 0 };
};

struct CounterpartyConfig
{
  std::string name;
  session::SessionConfig session;
  session::TransportSessionProfile transport_profile;
  std::filesystem::path store_path;
  std::string default_appl_ver_id;
  std::vector<std::string> supported_app_msg_types;
  std::uint32_t sending_time_threshold_seconds{ 0 };
  bool application_messages_available{ true };
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
  // Reconnect backoff (initiator only)
  bool reconnect_enabled = false;
  std::uint32_t reconnect_initial_ms = kDefaultReconnectInitialMs;
  std::uint32_t reconnect_max_ms = kDefaultReconnectMaxMs;
  std::uint32_t reconnect_max_retries = kUnlimitedReconnectRetries; // 0 = unlimited
  session::DayCutConfig day_cut;
};

struct EngineConfig
{
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
  std::vector<ListenerConfig> listeners;
  std::vector<CounterpartyConfig> counterparties;
  bool accept_unknown_sessions{ false };
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
