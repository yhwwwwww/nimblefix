#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "nimblefix/base/result.h"
#include "nimblefix/base/status.h"
#include "nimblefix/runtime/config.h"
#include "nimblefix/session/session_snapshot.h"

namespace nimble::runtime {

class Engine;

/// Detailed status of one session from the management perspective.
struct ManagedSessionStatus
{
  std::uint64_t session_id{ 0 };
  std::string name;
  session::SessionState state{ session::SessionState::kDisconnected };
  std::uint32_t next_inbound_seq{ 0 };
  std::uint32_t next_outbound_seq{ 0 };
  std::uint64_t last_activity_ns{ 0 };
  bool is_initiator{ false };
  std::uint64_t profile_id{ 0 };
  std::string begin_string;
  std::string sender_comp_id;
  std::string target_comp_id;
  StoreMode store_mode{ StoreMode::kMemory };
  AppDispatchMode dispatch_mode{ AppDispatchMode::kInline };
  bool reconnect_enabled{ false };
};

/// Engine-wide status snapshot for the management plane.
struct EngineManagementStatus
{
  /// Timestamp when this snapshot was taken.
  std::uint64_t timestamp_ns{ 0 };
  /// Whether the engine is booted and operational.
  bool booted{ false };
  /// Number of configured worker threads.
  std::uint32_t worker_count{ 0 };
  /// Total number of registered sessions (static + dynamic).
  std::uint32_t total_sessions{ 0 };
  /// Number of loaded profiles.
  std::uint32_t loaded_profiles{ 0 };
  /// Number of configured listeners.
  std::uint32_t listener_count{ 0 };
  /// Engine uptime since boot.
  std::chrono::nanoseconds uptime{};
  /// Per-session status.
  std::vector<ManagedSessionStatus> sessions;
};

/// Management command vocabulary.
///
/// Query helpers are implemented directly on ManagementPlane today. Control
/// commands are reserved for the command-execution surface and are not yet
/// wired to runtime sessions.
enum class ManagementCommand : std::uint32_t
{
  /// Query engine status (read-only).
  kQueryStatus = 0,
  /// Query a specific session's status.
  kQuerySession,
  /// Force disconnect a session (close transport).
  kForceDisconnect,
  /// Trigger day-cut sequence reset for a session.
  kTriggerDayCut,
  /// Reset sequence numbers for a session to 1.
  kResetSequences,
  /// Enable/disable application messages for a session.
  kToggleApplicationMessages,
};

/// Result of a management command execution.
struct ManagementCommandResult
{
  ManagementCommand command{};
  bool success{ false };
  std::string message;
  /// For query commands, the status data.
  std::optional<EngineManagementStatus> engine_status;
  std::optional<ManagedSessionStatus> session_status;
};

/// Management plane interface for querying a running engine.
///
/// This provides a single entry point for monitoring/admin tools to:
/// - Query engine and session state
/// - Check health information
///
/// Thread safety: all methods are safe to call from any thread (they read
/// Engine state which is protected by the Engine's internal synchronization).
class ManagementPlane
{
public:
  explicit ManagementPlane(Engine* engine);
  ~ManagementPlane();

  ManagementPlane(const ManagementPlane&) = delete;
  auto operator=(const ManagementPlane&) -> ManagementPlane& = delete;
  ManagementPlane(ManagementPlane&&) noexcept;
  auto operator=(ManagementPlane&&) noexcept -> ManagementPlane&;

  /// Get a full engine status snapshot.
  [[nodiscard]] auto QueryEngineStatus() const -> base::Result<EngineManagementStatus>;

  /// Get status for a specific session.
  [[nodiscard]] auto QuerySessionStatus(std::uint64_t session_id) const -> base::Result<ManagedSessionStatus>;

  /// Get status for all sessions.
  [[nodiscard]] auto QueryAllSessions() const -> base::Result<std::vector<ManagedSessionStatus>>;

  /// Check if the engine is healthy (booted, has sessions, no terminal errors).
  [[nodiscard]] auto IsHealthy() const -> bool;

  /// Get a summary string for quick health display.
  [[nodiscard]] auto HealthSummary() const -> std::string;

  /// Toggle application message availability for a session.
  ///
  /// This control path is not wired to live runtime sessions yet and currently
  /// returns kInvalidArgument after validating the engine/session.
  auto SetApplicationMessagesAvailable(std::uint64_t session_id, bool available) -> base::Status;

  /// Get the boot timestamp of the engine.
  [[nodiscard]] auto boot_timestamp_ns() const -> std::uint64_t;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace nimble::runtime
