#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "nimblefix/base/result.h"
#include "nimblefix/base/status.h"

namespace nimble::runtime {

class Engine;

/// Role of this engine instance in an HA cluster.
enum class HaRole : std::uint32_t
{
  /// Standalone instance, no HA coordination.
  kSolo = 0,
  /// Primary (active) instance handling all traffic.
  kPrimary,
  /// Standby instance ready to take over.
  kStandby,
};

/// State of an HA peer.
enum class HaPeerState : std::uint32_t
{
  /// Peer state unknown (not yet heard from).
  kUnknown = 0,
  /// Peer is alive and healthy.
  kAlive,
  /// Peer missed heartbeats, may be failing.
  kSuspect,
  /// Peer is confirmed dead (fenced).
  kDead,
};

/// Sequence state for one session (used for state replication).
struct SessionSequenceState
{
  std::uint64_t session_id{ 0 };
  std::uint32_t next_inbound_seq{ 1 };
  std::uint32_t next_outbound_seq{ 1 };
  std::uint64_t last_activity_ns{ 0 };
};

/// Snapshot of all session sequence states for HA replication.
struct HaStateSnapshot
{
  std::uint64_t snapshot_timestamp_ns{ 0 };
  std::uint64_t generation{ 0 };
  std::vector<SessionSequenceState> sessions;
};

/// Callback invoked when HA role changes.
using HaRoleChangeCallback = std::function<void(HaRole old_role, HaRole new_role)>;

/// Callback for replicating state to standby.
/// The primary calls this periodically with its current state.
using HaStateReplicator = std::function<base::Status(const HaStateSnapshot& snapshot)>;

/// Callback for receiving replicated state from primary.
/// The standby calls this to apply received state.
using HaStateReceiver = std::function<base::Result<HaStateSnapshot>()>;

/// Configuration for HA coordination.
struct HaConfig
{
  /// Initial role for this instance.
  HaRole initial_role{ HaRole::kSolo };
  /// Heartbeat interval between HA peers.
  std::chrono::milliseconds heartbeat_interval{ std::chrono::milliseconds{ 1000 } };
  /// Number of missed heartbeats before peer is suspected.
  std::uint32_t suspect_threshold{ 3 };
  /// Number of missed heartbeats before peer is declared dead.
  std::uint32_t dead_threshold{ 5 };
  /// Interval for state replication from primary to standby.
  std::chrono::milliseconds replication_interval{ std::chrono::milliseconds{ 500 } };
  /// Whether to automatically promote standby to primary on peer death.
  bool auto_failover{ false };
  /// Callback when role changes.
  HaRoleChangeCallback on_role_change;
  /// State replicator (primary side).
  HaStateReplicator replicator;
  /// State receiver (standby side).
  HaStateReceiver receiver;
};

/// HA coordination controller for one engine instance.
///
/// Manages role transitions, heartbeat monitoring, state replication,
/// and failover decisions. The transport layer for peer communication
/// is abstracted via callbacks (replicator/receiver) — the controller
/// does not implement its own network protocol.
class HaController
{
public:
  HaController();
  ~HaController();

  HaController(const HaController&) = delete;
  auto operator=(const HaController&) -> HaController& = delete;
  HaController(HaController&&) noexcept;
  auto operator=(HaController&&) noexcept -> HaController&;

  /// Configure the HA controller. Must be called before Start().
  auto Configure(HaConfig config) -> base::Status;

  /// Start HA coordination (heartbeat monitoring, replication).
  auto Start() -> base::Status;

  /// Stop HA coordination.
  auto Stop() -> void;

  /// Current role.
  [[nodiscard]] auto role() const -> HaRole;

  /// Current peer state.
  [[nodiscard]] auto peer_state() const -> HaPeerState;

  /// Current generation (incremented on each role change).
  [[nodiscard]] auto generation() const -> std::uint64_t;

  /// Whether the controller is running.
  [[nodiscard]] auto running() const -> bool;

  /// Manually promote this instance to primary.
  /// Only valid when current role is kStandby.
  auto PromoteToPrimary() -> base::Status;

  /// Manually demote this instance to standby.
  /// Only valid when current role is kPrimary.
  auto DemoteToStandby() -> base::Status;

  /// Record a heartbeat from the peer (call when heartbeat received).
  auto RecordPeerHeartbeat(std::uint64_t timestamp_ns = 0) -> void;

  /// Take a state snapshot from the engine for replication.
  [[nodiscard]] auto TakeSnapshot(const Engine& engine) -> base::Result<HaStateSnapshot>;

  /// Apply a received state snapshot to the engine (standby path).
  auto ApplySnapshot(Engine& engine, const HaStateSnapshot& snapshot) -> base::Status;

  /// Last state snapshot accepted by ApplySnapshot, if any.
  [[nodiscard]] auto last_applied_snapshot() const -> const std::optional<HaStateSnapshot>&;

  /// Check heartbeat health and trigger failover if needed.
  /// Call this periodically (e.g., from a timer or monitor thread).
  auto CheckHealth(std::uint64_t current_time_ns = 0) -> void;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Reference in-memory state replicator/receiver pair for testing and
/// single-process HA simulation.
class InMemoryHaTransport
{
public:
  [[nodiscard]] auto replicator() -> HaStateReplicator;
  [[nodiscard]] auto receiver() -> HaStateReceiver;

private:
  mutable std::mutex mutex_;
  std::optional<HaStateSnapshot> latest_;
};

} // namespace nimble::runtime
