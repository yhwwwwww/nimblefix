#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "nimblefix/base/result.h"
#include "nimblefix/base/status.h"
#include "nimblefix/codec/fix_codec.h"
#include "nimblefix/profile/normalized_dictionary.h"
#include "nimblefix/runtime/config.h"
#include "nimblefix/runtime/diagnostics.h"
#include "nimblefix/runtime/dynamic_config.h"
#include "nimblefix/runtime/ha.h"
#include "nimblefix/runtime/metrics.h"
#include "nimblefix/runtime/profile_binding.h"
#include "nimblefix/runtime/profile_registry.h"
#include "nimblefix/runtime/session_schedule.h"
#include "nimblefix/runtime/sharded_runtime.h"
#include "nimblefix/runtime/trace.h"
#include "nimblefix/session/session_key.h"
#include "nimblefix/session/session_snapshot.h"

namespace nimble::runtime {

class ApplicationCallbacks;
struct ManagedQueueApplicationRunnerOptions;

inline constexpr std::uint64_t kFirstDynamicSessionId = 0x8000'0000'0000'0000ULL;

// Unknown inbound acceptor Logons consult SessionFactory only after static
// counterparties fail to match and only when EngineConfig::accept_unknown_sessions
// is true.
//
// The SessionKey is normalized to the local engine's perspective:
// - key.begin_string is the inbound BeginString(8)
// - key.sender_comp_id is the local acceptor CompID from inbound TargetCompID(56)
// - key.target_comp_id is the remote initiator CompID from inbound SenderCompID(49)
//
// Listener name, local port, and remote address are not exposed here. If your
// dynamic onboarding policy depends on those dimensions, use static
// counterparties or an out-of-band routing layer.
//
// Return session.session_id == 0 to let Engine auto-assign a dynamic session id
// from kFirstDynamicSessionId upward.
using SessionFactory = std::function<base::Result<CounterpartyConfig>(const session::SessionKey& key)>;

/// Provider function that returns all live session snapshots.
/// Installed by runtime components (Initiator/Acceptor) that own LiveSessionRegistry.
using SessionSnapshotProvider = std::function<std::vector<session::SessionSnapshot>()>;

class WhitelistSessionFactory
{
public:
  // Matches the local-perspective SessionKey passed to SessionFactory. The
  // second parameter is the local acceptor CompID, not the remote initiator
  // CompID from the wire.
  void Allow(std::string_view begin_string,
             std::string_view local_sender_comp_id,
             const CounterpartyConfig& config_template);
  void AllowAny(const CounterpartyConfig& config_template);

  auto operator()(const session::SessionKey& key) const -> base::Result<CounterpartyConfig>;

private:
  struct Entry
  {
    std::string begin_string;
    std::string sender_comp_id;
    CounterpartyConfig config_template;
  };

  std::unordered_map<std::string, Entry> entries_;
  std::optional<CounterpartyConfig> allow_any_template_;
};

struct ResolvedCounterparty
{
  CounterpartyConfig counterparty;
  profile::NormalizedDictionaryView dictionary;
};

class Engine
{
public:
  Engine();
  ~Engine();

  Engine(const Engine&) = delete;
  auto operator=(const Engine&) -> Engine& = delete;
  Engine(Engine&&) = delete;
  auto operator=(Engine&&) -> Engine& = delete;

  // Boot() calls this automatically. Use it directly only when tests or tooling
  // need profile loading without registering runtime sessions.
  auto LoadProfiles(const EngineConfig& config) -> base::Status;
  // Validates config, clears any previous boot state, loads profiles from
  // config.profile_artifacts/profile_dictionaries, registers static
  // counterparties, and makes config()/profiles()/runtime()/Find* queries
  // available on success.
  auto Boot(const EngineConfig& config) -> base::Status;
  /// Apply a new configuration without full engine restart.
  ///
  /// Computes the delta between the current config and new_config, then:
  /// - Validates the new config
  /// - Loads any new profiles
  /// - Adds new counterparties (registers with ShardedRuntime, updates bookkeeping)
  /// - Removes absent counterparties (unregisters from ShardedRuntime)
  /// - Updates modifiable counterparty fields in-place
  /// - Adds/removes/modifies listeners
  /// - Updates engine-level live-changeable fields (trace, metrics)
  ///
  /// Returns ApplyConfigResult describing what was applied and what requires restart.
  /// Skipped changes do NOT prevent other changes from being applied.
  ///
  /// Precondition: Engine must be booted (Boot() returned Ok).
  /// Thread safety: call from the control thread only, not from worker threads.
  auto ApplyConfig(const EngineConfig& new_config) -> base::Result<ApplyConfigResult>;

  [[nodiscard]] auto profiles() const -> const ProfileRegistry&;
  [[nodiscard]] auto runtime() const -> const ShardedRuntime*;
  [[nodiscard]] auto mutable_runtime() -> ShardedRuntime*;
  [[nodiscard]] auto metrics() const -> const MetricsRegistry&;
  [[nodiscard]] auto mutable_metrics() -> MetricsRegistry*;
  [[nodiscard]] auto trace() const -> const TraceRecorder&;
  [[nodiscard]] auto mutable_trace() -> TraceRecorder*;
  [[nodiscard]] auto diagnostics() -> DiagnosticsMonitor&;
  [[nodiscard]] auto diagnostics() const -> const DiagnosticsMonitor&;
  [[nodiscard]] auto config() const -> const EngineConfig*;

  [[nodiscard]] auto FindCounterpartyConfig(std::uint64_t session_id) const -> const CounterpartyConfig*;
  /// Query schedule status for a registered counterparty session.
  [[nodiscard]] auto QueryScheduleStatus(std::uint64_t session_id, std::uint64_t unix_time_ns) const
    -> base::Result<SessionScheduleStatus>;
  [[nodiscard]] auto FindListenerConfig(std::string_view name) const -> const ListenerConfig*;
  template<class Profile>
  auto Bind() const -> base::Result<ProfileBinding<Profile>>;
  auto LoadDictionaryView(std::uint64_t profile_id) const -> base::Result<profile::NormalizedDictionaryView>;
  auto ResolveInboundSession(const codec::SessionHeader& header) const -> base::Result<ResolvedCounterparty>;
  auto ResolveInboundSession(const codec::SessionHeaderView& header) const -> base::Result<ResolvedCounterparty>;

  // Install or replace the dynamic factory used for unknown inbound acceptor
  // Logons. Static counterparties always match first.
  void SetSessionFactory(SessionFactory factory);

  /// Install or replace the runtime-owned live session snapshot provider.
  void SetSessionSnapshotProvider(SessionSnapshotProvider provider);
  /// Query live session snapshots if a runtime component has installed a provider.
  [[nodiscard]] auto QuerySessionSnapshots() const -> std::vector<session::SessionSnapshot>;
  /// Store an HA snapshot that runtime sessions should apply when they boot.
  void SetLastAppliedHaSnapshot(HaStateSnapshot snapshot);
  /// Return the last HA snapshot staged for runtime session boot, if any.
  [[nodiscard]] auto last_applied_ha_snapshot() const -> const HaStateSnapshot*;

private:
  friend auto EnsureManagedQueueRunnerStarted(Engine& engine,
                                              const void* owner,
                                              ApplicationCallbacks* application,
                                              std::optional<ManagedQueueApplicationRunnerOptions>* options)
    -> base::Status;
  friend auto StopManagedQueueRunner(Engine& engine, const void* owner) -> base::Status;
  friend auto ReleaseManagedQueueRunner(Engine& engine, const void* owner) -> base::Status;
  friend auto PollManagedQueueWorkerOnce(Engine& engine, const void* owner, std::uint32_t worker_id)
    -> base::Result<std::size_t>;

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

template<class Profile>
auto
Engine::Bind() const -> base::Result<ProfileBinding<Profile>>
{
  auto dictionary = LoadDictionaryView(Profile::kProfileId);
  if (!dictionary.ok()) {
    return dictionary.status();
  }

  const auto& loaded_profile = dictionary.value().profile();
  if (loaded_profile.profile_id() != Profile::kProfileId) {
    return base::Status::VersionMismatch("profile_id mismatch between generated API and runtime profile");
  }
  if (loaded_profile.schema_hash() != Profile::kSchemaHash) {
    return base::Status::VersionMismatch("schema_hash mismatch between generated API and runtime profile");
  }

  return ProfileBinding<Profile>(std::move(dictionary).value());
}

} // namespace nimble::runtime
