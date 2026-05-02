#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "nimblefix/runtime/config.h"

namespace nimble::runtime {

class Engine;

/// Kind of change detected between two EngineConfig snapshots.
enum class ConfigChangeKind : std::uint32_t
{
  /// A new counterparty was added.
  kAddCounterparty = 0,
  /// An existing counterparty was removed.
  kRemoveCounterparty,
  /// An existing counterparty was modified.
  kModifyCounterparty,
  /// A new listener was added.
  kAddListener,
  /// An existing listener was removed.
  kRemoveListener,
  /// An existing listener was modified.
  kModifyListener,
  /// An engine-level scalar field was changed.
  kEngineFieldChanged,
};

/// One detected change between two EngineConfig snapshots.
struct ConfigChange
{
  ConfigChangeKind kind{};
  /// session_id for counterparty changes, 0 for listener/engine changes.
  std::uint64_t session_id{ 0 };
  /// Listener name for listener changes, field description for engine changes.
  std::string name;
  /// Human-readable description of what changed.
  std::string description;
};

/// Complete delta between two EngineConfig snapshots.
struct ConfigDelta
{
  std::vector<ConfigChange> changes;

  /// True when at least one change would require a full engine restart.
  bool requires_restart{ false };
  /// Human-readable explanation of why restart is required (if applicable).
  std::string restart_reason;

  /// True when no changes were detected.
  [[nodiscard]] auto empty() const -> bool { return changes.empty(); }
};

/// Compute the delta between current and proposed EngineConfig.
///
/// This does NOT apply any changes. Use the result to inspect what would
/// change, then pass the new config to Engine::ApplyConfig() to apply.
///
/// Fields that require restart if changed:
///   - worker_count
///   - io_backend
///   - poll_mode (changing from blocking to busy or vice versa while running)
///   - queue_app_mode
///
/// Fields that can be applied live:
///   - counterparties (add/remove/modify)
///   - listeners (add/remove/modify)
///   - trace_mode, trace_capacity
///   - enable_metrics
///   - profile_artifacts, profile_dictionaries (new profiles can be loaded)
///
/// Counterparty modifications: some fields can be changed live, others require
/// remove + re-add:
///   - Live-changeable: name, application_messages_available, supported_app_msg_types,
///     reconnect_enabled, reconnect_initial_ms, reconnect_max_ms, reconnect_max_retries,
///     session_schedule, sending_time_threshold_seconds, timestamp_resolution,
///     reset_seq_num_on_logon, reset_seq_num_on_logout, reset_seq_num_on_disconnect,
///     refresh_on_logon, send_next_expected_msg_seq_num, contract_service_subsets
///   - Require remove+add: store_mode, store_path, recovery_mode, dispatch_mode,
///     session.profile_id, session.key, transport_profile, validation_policy
[[nodiscard]] auto
ComputeConfigDelta(const EngineConfig& current, const EngineConfig& proposed) -> ConfigDelta;

/// Result of applying a config change.
struct ApplyConfigResult
{
  /// Changes that were successfully applied.
  std::vector<ConfigChange> applied;
  /// Changes that were skipped because they require restart.
  std::vector<ConfigChange> skipped;
  /// True when all requested changes were applied (no skipped).
  [[nodiscard]] auto fully_applied() const -> bool { return skipped.empty(); }
  /// Human-readable summary.
  [[nodiscard]] auto summary() const -> std::string;
};

} // namespace nimble::runtime
