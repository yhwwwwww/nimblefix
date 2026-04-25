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

/// Default initiator reconnect backoff floor used by config helpers.
inline constexpr std::uint32_t kDefaultReconnectInitialMs = 1'000U;
/// Default initiator reconnect backoff ceiling used by config helpers.
inline constexpr std::uint32_t kDefaultReconnectMaxMs = 30'000U;
/// Sentinel meaning reconnect forever until the session is explicitly stopped.
inline constexpr std::uint32_t kUnlimitedReconnectRetries = 0U;

/// Persistence backend selected for one counterparty session.
///
/// Design intent: let integrators choose between the lowest-latency ephemeral
/// mode and two persistent replay/recovery backends without changing the rest
/// of the runtime API.
enum class StoreMode : std::uint32_t
{
  /// In-memory only. Lowest latency, no crash recovery.
  kMemory = 0,
  /// File-backed mmap store. Good warm-restart behavior and view-friendly replay.
  kMmap,
  /// Batched durable log store. Higher write batching, explicit flush cadence.
  kDurableBatch,
};

/// Application delivery mode for one session.
///
/// Inline mode runs callbacks on the runtime worker thread. Queue-decoupled mode
/// hands events to QueueApplication and shifts application work onto a separate
/// consumer path.
enum class AppDispatchMode : std::uint32_t
{
  kInline = 0,
  kQueueDecoupled,
};

/// Trace recorder mode for the engine.
enum class TraceMode : std::uint32_t
{
  kDisabled = 0,
  kRing,
};

/// How queue-decoupled application handlers are scheduled.
enum class QueueAppThreadingMode : std::uint32_t
{
  /// Runtime workers co-schedule queue draining themselves.
  kCoScheduled = 0,
  /// Dedicated application threads drain worker-local queues.
  kThreaded,
};

/// Polling strategy for transport I/O loops.
enum class PollMode : std::uint32_t
{
  /// Wait in the kernel for readiness. Lower CPU cost, higher wake latency.
  kBlocking = 0,
  /// Spin for readiness. Lower latency, higher CPU burn.
  kBusy = 1,
};

/// Lower/upper bound selectors for TLS protocol negotiation.
enum class TlsProtocolVersion : std::uint32_t
{
  /// Leave the bound to the TLS library default policy.
  kSystemDefault = 0,
  kTls10,
  kTls11,
  kTls12,
  kTls13,
};

/// Accept-side transport security policy for one FIX session.
enum class TransportSecurityRequirement : std::uint32_t
{
  /// Session may bind over either plain TCP or TLS.
  kAny = 0,
  /// Session must bind only over plain TCP listeners.
  kPlainOnly,
  /// Session must bind only over TLS-protected listeners.
  kTlsOnly,
};

/// Calendar day values used by SessionScheduleConfig.
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

/// Wall-clock time used by session and logon window configuration.
///
/// Boundary condition: validation rejects out-of-range values. This struct does
/// not normalize overflow on its own.
struct SessionTimeOfDay
{
  std::uint8_t hour{ 0 };
  std::uint8_t minute{ 0 };
  std::uint8_t second{ 0 };
};

/// Optional session and logon window rules for one counterparty.
///
/// Design intent: keep the public schedule surface close to traditional FIX
/// engine configuration while making the runtime contract explicit.
///
/// Boundary conditions:
/// - non_stop_session is mutually exclusive with all other window fields
/// - start/end days must be provided as pairs when used
/// - logon window is independent from session-active window and may be omitted
/// - use_local_time selects local wall-clock interpretation; otherwise UTC is used
struct SessionScheduleConfig
{
  // Interpret configured times in local time instead of UTC.
  bool use_local_time{ false };
  // Session is always considered active; no open/close window enforcement.
  bool non_stop_session{ false };
  // Session-active window start.
  std::optional<SessionTimeOfDay> start_time;
  // Session-active window end.
  std::optional<SessionTimeOfDay> end_time;
  std::optional<SessionDayOfWeek> start_day;
  std::optional<SessionDayOfWeek> end_day;
  // Optional logon-allowed window start.
  std::optional<SessionTimeOfDay> logon_time;
  // Optional logon-allowed window end.
  std::optional<SessionTimeOfDay> logout_time;
  std::optional<SessionDayOfWeek> logon_day;
  std::optional<SessionDayOfWeek> logout_day;
};

/// Optional TLS client policy for initiator connections.
///
/// Runtime TLS remains disabled unless enabled is set true.
struct TlsClientConfig
{
  // Runtime TLS is disabled unless this is set true.
  bool enabled{ false };
  std::string server_name;
  std::string expected_peer_name;
  std::filesystem::path ca_file;
  std::filesystem::path ca_path;
  std::filesystem::path certificate_chain_file;
  std::filesystem::path private_key_file;
  bool verify_peer{ true };
  TlsProtocolVersion min_version{ TlsProtocolVersion::kSystemDefault };
  TlsProtocolVersion max_version{ TlsProtocolVersion::kSystemDefault };
  std::string cipher_list;
  std::string cipher_suites;
  bool session_resumption{ true };
};

/// Optional TLS server policy for acceptor listeners.
///
/// Runtime TLS remains disabled unless enabled is set true.
struct TlsServerConfig
{
  // Runtime TLS is disabled unless this is set true.
  bool enabled{ false };
  std::filesystem::path certificate_chain_file;
  std::filesystem::path private_key_file;
  std::filesystem::path ca_file;
  std::filesystem::path ca_path;
  bool verify_peer{ false };
  bool require_client_certificate{ false };
  TlsProtocolVersion min_version{ TlsProtocolVersion::kSystemDefault };
  TlsProtocolVersion max_version{ TlsProtocolVersion::kSystemDefault };
  std::string cipher_list;
  std::string cipher_suites;
  bool session_cache{ true };
};

/// Listener definition for one acceptor front door.
///
/// Design intent: separate transport listener identity from session matching so
/// callers can reason about which front door they are opening before any FIX
/// session binds.
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
  // Optional TLS server policy for this listener. Runtime TLS remains off
  // unless tls_server.enabled is true.
  TlsServerConfig tls_server;
};

/// Static runtime configuration for one counterparty session.
///
/// Design intent: gather all session-specific transport, persistence,
/// validation, scheduling, and reconnect settings into one public struct that
/// can be validated before boot.
///
/// Performance/lifecycle notes:
/// - dispatch_mode decides whether application callbacks run inline or through
///   a queue handoff
/// - store_mode and recovery_mode together determine replay durability and
///   restart cost
/// - reconnect settings are meaningful only for initiator sessions
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
  std::vector<std::string> supported_app_msg_types;
  std::uint32_t sending_time_threshold_seconds{ 0 };
  bool application_messages_available{ true };
  // Memory is the simplest default. Mmap and durable batch require store_path.
  StoreMode store_mode{ StoreMode::kMemory };
  // Durable-batch tuning. Ignored unless store_mode == kDurableBatch.
  std::uint32_t durable_flush_threshold{ 0 };
  store::DurableStoreRolloverMode durable_rollover_mode{ store::DurableStoreRolloverMode::kUtcDay };
  std::uint32_t durable_archive_limit{ 0 };
  std::int32_t durable_local_utc_offset_seconds{ 0 };
  bool durable_use_system_timezone{ true };
  // Recovery and application delivery policy.
  session::RecoveryMode recovery_mode{ session::RecoveryMode::kMemoryOnly };
  AppDispatchMode dispatch_mode{ AppDispatchMode::kInline };
  session::ValidationPolicy validation_policy{ session::ValidationPolicy::Strict() };
  // Session reset and refresh behavior.
  bool reset_seq_num_on_logon{ false };
  bool reset_seq_num_on_logout{ false };
  bool reset_seq_num_on_disconnect{ false };
  bool refresh_on_logon{ false };
  bool send_next_expected_msg_seq_num{ false };
  // Session-active and logon window policy.
  SessionScheduleConfig session_schedule;
  // Optional initiator TLS policy. Runtime TLS remains off unless
  // tls_client.enabled is true. Acceptor sessions ignore this field.
  TlsClientConfig tls_client;
  // Acceptor-side transport security requirement. Initiator sessions should
  // leave this at kAny.
  TransportSecurityRequirement acceptor_transport_security{ TransportSecurityRequirement::kAny };
  // Initiator reconnect defaults. Leave disabled for acceptors.
  bool reconnect_enabled = false;
  std::uint32_t reconnect_initial_ms = kDefaultReconnectInitialMs;
  std::uint32_t reconnect_max_ms = kDefaultReconnectMaxMs;
  std::uint32_t reconnect_max_retries = kUnlimitedReconnectRetries;
  // Sequence-reset day-cut policy.
  session::DayCutConfig day_cut;
};

/// Engine-wide runtime configuration shared by all sessions.
///
/// Design intent: keep worker topology, profile loading, metrics, tracing, and
/// static session inventory in one value that can be validated before boot.
///
/// Boundary conditions:
/// - worker_count must be positive
/// - affinity vectors must not exceed worker_count
/// - app_cpu_affinity is only valid in threaded queue mode
/// - accept_unknown_sessions gates SessionFactory use for unknown acceptor Logons
struct EngineConfig
{
  // Recommended default until you need multi-session parallelism.
  std::uint32_t worker_count{ 1 };
  // Enable metrics counters and gauges owned by the engine.
  bool enable_metrics{ true };
  // Trace recorder policy; ring mode requires trace_capacity > 0.
  TraceMode trace_mode{ TraceMode::kDisabled };
  std::uint32_t trace_capacity{ 0 };
  // Optional CPU affinity for the acceptor front door thread.
  std::optional<std::uint32_t> front_door_cpu;
  // Optional per-worker CPU affinity.
  std::vector<std::uint32_t> worker_cpu_affinity;
  // Queue-drain threading model when sessions use queue dispatch.
  QueueAppThreadingMode queue_app_mode{ QueueAppThreadingMode::kCoScheduled };
  // Transport readiness polling policy.
  PollMode poll_mode{ PollMode::kBlocking };
  // Kernel I/O backend for network polling.
  IoBackend io_backend{ IoBackend::kEpoll };
  // Optional per-application-thread CPU affinity in threaded queue mode.
  std::vector<std::uint32_t> app_cpu_affinity;
  // Artifacts and dictionaries loaded before runtime boot completes.
  std::vector<std::filesystem::path> profile_artifacts;
  std::vector<std::vector<std::filesystem::path>> profile_dictionaries;
  // Profile loading memory hints.
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

/// Small builder for ListenerConfig.
///
/// Design intent: cover the common acceptor bring-up path without making users
/// spell out aggregate initialization details.
class ListenerConfigBuilder
{
public:
  /// Start building a listener with a required name.
  ///
  /// \param name Unique listener name.
  /// \return Builder seeded with the supplied name.
  static auto Named(std::string name) -> ListenerConfigBuilder;

  /// Set bind host and port.
  ///
  /// \param host Bind address such as 0.0.0.0 or 127.0.0.1.
  /// \param port Bind port. Use 0 for an ephemeral test port.
  /// \return This builder.
  auto bind(std::string host, std::uint16_t port) -> ListenerConfigBuilder&;

  /// Hint which runtime worker should adopt sessions accepted here.
  ///
  /// \param worker_id Preferred worker id.
  /// \return This builder.
  auto worker_hint(std::uint32_t worker_id) -> ListenerConfigBuilder&;

  /// Configure optional TLS server support on this listener.
  ///
  /// Runtime TLS uses this policy only when config.enabled is true.
  ///
  /// \param config TLS server configuration.
  /// \return This builder.
  auto tls_server(TlsServerConfig config) -> ListenerConfigBuilder&;

  /// Materialize the current builder state.
  ///
  /// This does not perform validation; use ValidateEngineConfig for that.
  ///
  /// \return Listener config by value.
  [[nodiscard]] auto build() const -> ListenerConfig;

private:
  explicit ListenerConfigBuilder(ListenerConfig config);

  ListenerConfig config_{};
};

/// Builder for common initiator and acceptor counterparty setups.
///
/// Design intent: make the public bring-up path explicit about session role,
/// transport version, persistence choice, and reconnect behavior.
class CounterpartyConfigBuilder
{
public:
  /// Create a minimal initiator session config.
  ///
  /// The builder seeds heartbeat interval from the selected transport version
  /// and disables reconnect until reconnect() is called.
  ///
  /// \param name Human-readable counterparty label.
  /// \param session_id Stable runtime session id.
  /// \param key Local-perspective FIX session key.
  /// \param profile_id Loaded dictionary/profile id.
  /// \param transport_version Transport version used to normalize BeginString.
  /// \return Builder seeded for an initiator session.
  static auto Initiator(std::string name,
                        std::uint64_t session_id,
                        session::SessionKey key,
                        std::uint64_t profile_id,
                        session::TransportVersion transport_version = session::TransportVersion::kFix44)
    -> CounterpartyConfigBuilder;

  /// Create a minimal acceptor session config.
  ///
  /// \param name Human-readable counterparty label.
  /// \param session_id Stable runtime session id.
  /// \param key Local-perspective FIX session key.
  /// \param profile_id Loaded dictionary/profile id.
  /// \param transport_version Transport version used to normalize BeginString.
  /// \return Builder seeded for an acceptor session.
  static auto Acceptor(std::string name,
                       std::uint64_t session_id,
                       session::SessionKey key,
                       std::uint64_t profile_id,
                       session::TransportVersion transport_version = session::TransportVersion::kFix44)
    -> CounterpartyConfigBuilder;

  // Re-normalizes session.key.begin_string to the selected transport version.
  /// \param version Transport version to normalize into transport_profile and session key.
  /// \return This builder.
  auto transport_version(session::TransportVersion version) -> CounterpartyConfigBuilder&;

  /// Set DefaultApplVerID for FIXT sessions.
  ///
  /// \param value Application version identifier.
  /// \return This builder.
  auto default_appl_ver_id(std::string value) -> CounterpartyConfigBuilder&;

  /// Override heartbeat interval in seconds.
  ///
  /// \param seconds Heartbeat interval in whole seconds.
  /// \return This builder.
  auto heartbeat_interval_seconds(std::uint32_t seconds) -> CounterpartyConfigBuilder&;

  /// Override the maximum allowed absolute SendingTime drift in seconds.
  ///
  /// Zero disables SendingTime accuracy rejection.
  ///
  /// \param seconds Maximum tolerated absolute drift from wall clock.
  /// \return This builder.
  auto sending_time_threshold_seconds(std::uint32_t seconds) -> CounterpartyConfigBuilder&;

  /// Restrict this session to a known subset of application message types.
  ///
  /// Empty means any application message type present in the bound dictionary is
  /// considered supported.
  ///
  /// \param values Supported application MsgType values.
  /// \return This builder.
  auto supported_app_msg_types(std::vector<std::string> values) -> CounterpartyConfigBuilder&;

  /// Toggle whether application handling is currently available for the session.
  ///
  /// When false, known application messages are answered with
  /// BusinessMessageReject reason 4.
  ///
  /// \param available True when the application service is available.
  /// \return This builder.
  auto application_messages_available(bool available) -> CounterpartyConfigBuilder&;

  /// Select persistence backend and optional path.
  ///
  /// \param mode Store backend.
  /// \param path Store path for persistent backends.
  /// \return This builder.
  auto store(StoreMode mode, std::filesystem::path path = {}) -> CounterpartyConfigBuilder&;

  /// Select restart recovery policy.
  ///
  /// \param mode Recovery mode.
  /// \return This builder.
  auto recovery_mode(session::RecoveryMode mode) -> CounterpartyConfigBuilder&;

  /// Select application dispatch mode.
  ///
  /// \param mode Inline or queue-decoupled delivery.
  /// \return This builder.
  auto dispatch_mode(AppDispatchMode mode) -> CounterpartyConfigBuilder&;

  /// Override inbound validation policy.
  ///
  /// \param policy Validation rules for this session.
  /// \return This builder.
  auto validation_policy(session::ValidationPolicy policy) -> CounterpartyConfigBuilder&;

  /// Configure optional initiator TLS client support.
  ///
  /// Runtime TLS uses this policy only when config.enabled is true.
  ///
  /// \param config TLS client configuration.
  /// \return This builder.
  auto tls_client(TlsClientConfig config) -> CounterpartyConfigBuilder&;

  /// Restrict which accept-side transport security levels may bind this session.
  ///
  /// \param requirement Plain/TLS binding requirement for acceptors.
  /// \return This builder.
  auto acceptor_transport_security(TransportSecurityRequirement requirement) -> CounterpartyConfigBuilder&;

  /// Enable initiator reconnect with backoff limits.
  ///
  /// \param initial_ms Initial reconnect backoff in milliseconds.
  /// \param max_ms Maximum reconnect backoff in milliseconds.
  /// \param max_retries Retry count, or kUnlimitedReconnectRetries for forever.
  /// \return This builder.
  auto reconnect(std::uint32_t initial_ms = kDefaultReconnectInitialMs,
                 std::uint32_t max_ms = kDefaultReconnectMaxMs,
                 std::uint32_t max_retries = kUnlimitedReconnectRetries) -> CounterpartyConfigBuilder&;

  /// Disable initiator reconnect and restore helper defaults.
  ///
  /// \return This builder.
  auto disable_reconnect() -> CounterpartyConfigBuilder&;

  /// Materialize the current builder state.
  ///
  /// This does not perform validation; use ValidateEngineConfig before boot.
  ///
  /// \return Counterparty config by value.
  [[nodiscard]] auto build() const -> CounterpartyConfig;

private:
  explicit CounterpartyConfigBuilder(CounterpartyConfig config);

  CounterpartyConfig config_{};
};

/// Validate one session schedule.
///
/// \param schedule Schedule to validate.
/// \return Ok on success, otherwise an InvalidArgument status describing the broken rule.
[[nodiscard]] auto
ValidateSessionSchedule(const SessionScheduleConfig& schedule) -> base::Status;

/// Check whether a timestamp lies inside the configured session-active window.
///
/// If no session window is configured, this returns true.
///
/// \param schedule Schedule to evaluate.
/// \param unix_time_ns Wall-clock time in nanoseconds since Unix epoch.
/// \return True when the session is considered active.
[[nodiscard]] auto
IsWithinSessionWindow(const SessionScheduleConfig& schedule, std::uint64_t unix_time_ns) -> bool;

/// Check whether a timestamp lies inside the configured logon-allowed window.
///
/// If no logon window is configured, this returns true.
///
/// \param schedule Schedule to evaluate.
/// \param unix_time_ns Wall-clock time in nanoseconds since Unix epoch.
/// \return True when logon is allowed at that instant.
[[nodiscard]] auto
IsWithinLogonWindow(const SessionScheduleConfig& schedule, std::uint64_t unix_time_ns) -> bool;

/// Find the next logon-window opening time.
///
/// Boundary condition: when no logon window is configured, the current
/// timestamp is returned unchanged.
///
/// \param schedule Schedule to evaluate.
/// \param unix_time_ns Wall-clock time in nanoseconds since Unix epoch.
/// \return Next opening timestamp, or nullopt when no future opening can be derived.
[[nodiscard]] auto
NextLogonWindowStart(const SessionScheduleConfig& schedule, std::uint64_t unix_time_ns) -> std::optional<std::uint64_t>;

/// Report whether this build was compiled with optional TLS support.
[[nodiscard]] auto
TlsTransportEnabledAtBuild() noexcept -> bool;

/// Validate a full engine configuration before boot.
///
/// \param config Engine config to validate.
/// \return Ok on success, otherwise an error describing the first violated public contract.
auto
ValidateEngineConfig(const EngineConfig& config) -> base::Status;

} // namespace nimble::runtime
