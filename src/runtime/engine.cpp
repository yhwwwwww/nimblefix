#include "nimblefix/runtime/engine.h"

#include "nimblefix/advanced/engine.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "nimblefix/codec/fix_codec.h"
#include "nimblefix/profile/profile_loader.h"
#include "nimblefix/runtime/contract_binding.h"
#include "nimblefix/runtime/diagnostics.h"
#include "nimblefix/runtime/dynamic_config.h"
#include "nimblefix/runtime/metrics.h"
#include "nimblefix/runtime/profile_registry.h"
#include "nimblefix/runtime/sharded_runtime.h"
#include "nimblefix/runtime/trace.h"

namespace nimble::runtime {

namespace {

auto
ResolveQueueApplication(ApplicationCallbacks* application) -> base::Result<QueueApplication*>
{
  if (application == nullptr) {
    return base::Status::InvalidArgument("managed queue runner requires an application");
  }

  auto* provider = dynamic_cast<QueueApplicationProvider*>(application);
  if (provider == nullptr) {
    return base::Status::InvalidArgument("managed queue runner requires application to implement "
                                         "QueueApplicationProvider");
  }
  return &provider->queue_application();
}

auto
ValidateManagedQueueRunnerOptions(const EngineConfig* config,
                                  QueueApplication* application,
                                  const ManagedQueueApplicationRunnerOptions& options) -> base::Status
{
  if (application == nullptr) {
    return base::Status::InvalidArgument("queue application runner requires an application");
  }
  if (config == nullptr) {
    return base::Status::InvalidArgument("queue application runner requires a booted engine config");
  }

  const auto expected_worker_count = config->worker_count == 0U ? 1U : config->worker_count;
  if (application->worker_count() != expected_worker_count) {
    return base::Status::InvalidArgument("queue application worker_count must match engine worker_count");
  }
  if (options.handlers.size() != expected_worker_count) {
    return base::Status::InvalidArgument("queue application runner requires one handler per runtime worker");
  }
  if (options.mode != ManagedQueueApplicationRunnerMode::kThreaded && !options.thread_options.cpu_affinity.empty()) {
    return base::Status::InvalidArgument("queue application runner cpu_affinity requires threaded mode");
  }
  if (options.thread_options.cpu_affinity.size() > expected_worker_count) {
    return base::Status::InvalidArgument("queue application runner cpu_affinity must not contain more entries "
                                         "than runtime worker_count");
  }
  for (const auto& handler : options.handlers) {
    if (handler == nullptr) {
      return base::Status::InvalidArgument("queue application runner handlers must not be null");
    }
  }
  return base::Status::Ok();
}

auto
FindCounterpartyBySessionId(const EngineConfig& config, std::uint64_t session_id) -> const CounterpartyConfig*
{
  const auto it = std::find_if(config.counterparties.begin(),
                               config.counterparties.end(),
                               [&](const auto& counterparty) { return counterparty.session.session_id == session_id; });
  return it == config.counterparties.end() ? nullptr : &*it;
}

auto
ReplaceCounterpartyBySessionId(EngineConfig& config, const CounterpartyConfig& replacement) -> void
{
  const auto it =
    std::find_if(config.counterparties.begin(), config.counterparties.end(), [&](const auto& counterparty) {
      return counterparty.session.session_id == replacement.session.session_id;
    });
  if (it != config.counterparties.end()) {
    *it = replacement;
  }
}

auto
HasProfileLoadChange(const EngineConfig& current, const EngineConfig& proposed) -> bool
{
  return current.profile_artifacts != proposed.profile_artifacts ||
         current.profile_dictionaries != proposed.profile_dictionaries ||
         current.profile_contracts != proposed.profile_contracts ||
         current.profile_madvise != proposed.profile_madvise || current.profile_mlock != proposed.profile_mlock;
}

auto
ChangeIsRestartRequired(const ConfigChange& change) -> bool
{
  if (change.kind != ConfigChangeKind::kEngineFieldChanged) {
    return false;
  }
  return change.name == "worker_count" || change.name == "io_backend" || change.name == "poll_mode" ||
         change.name == "queue_app_mode";
}

auto
ChangeRequiresRemoveAdd(const ConfigChange& change) -> bool
{
  return change.kind == ConfigChangeKind::kModifyCounterparty &&
         change.description.find("requires remove+add") != std::string::npos;
}

auto
ApplyStoredEngineConfig(const EngineConfig& current, const EngineConfig& proposed) -> EngineConfig
{
  auto stored = proposed;
  stored.worker_count = current.worker_count;
  stored.io_backend = current.io_backend;
  stored.poll_mode = current.poll_mode;
  stored.queue_app_mode = current.queue_app_mode;
  return stored;
}

auto
ReloadProfilesForConfig(const EngineConfig& config) -> base::Result<std::pair<ProfileRegistry, LoadedContractMap>>
{
  ProfileRegistry loaded_profiles;
  const profile::ProfileLoadOptions load_options{
    .madvise = config.profile_madvise,
    .mlock = config.profile_mlock,
  };

  for (const auto& artifact_path : config.profile_artifacts) {
    auto loaded = profile::LoadProfileArtifact(artifact_path, load_options);
    if (!loaded.ok()) {
      return loaded.status();
    }
    auto status = loaded_profiles.Register(std::move(loaded).value());
    if (!status.ok()) {
      return status;
    }
  }

  for (const auto& dict_paths : config.profile_dictionaries) {
    auto loaded = profile::LoadProfileFromDictionaryFiles(dict_paths);
    if (!loaded.ok()) {
      return loaded.status();
    }
    auto status = loaded_profiles.Register(std::move(loaded).value());
    if (!status.ok()) {
      return status;
    }
  }

  auto loaded_contracts = LoadContractMap(config.profile_contracts);
  if (!loaded_contracts.ok()) {
    return loaded_contracts.status();
  }
  for (const auto& [profile_id, contract] : loaded_contracts.value()) {
    const auto* loaded_profile = loaded_profiles.Find(profile_id);
    if (loaded_profile == nullptr) {
      return base::Status::NotFound("contract sidecar references an unloaded profile_id");
    }
    if (contract.schema_hash != 0U && contract.schema_hash != loaded_profile->schema_hash()) {
      return base::Status::VersionMismatch("contract sidecar schema_hash does not match the loaded profile");
    }
  }

  return std::pair<ProfileRegistry, LoadedContractMap>{ std::move(loaded_profiles),
                                                        std::move(loaded_contracts).value() };
}

} // namespace

struct Engine::Impl
{
  struct ManagedQueueRunnerSlot
  {
    ManagedQueueApplicationRunnerMode mode{ ManagedQueueApplicationRunnerMode::kCoScheduled };
    QueueApplication* application{ nullptr };
    std::vector<std::unique_ptr<QueueApplicationEventHandler>> handlers;
    QueueApplicationPollerOptions poller_options{};
    std::unique_ptr<QueueApplicationRunner> runner;
    bool active{ false };
  };

  std::optional<EngineConfig> config_;
  std::optional<ShardedRuntime> runtime_;
  std::unordered_map<std::uint64_t, CounterpartyConfig> counterparties_;
  LoadedContractMap contracts_;
  std::optional<SessionFactory> session_factory_;
  std::optional<SessionSnapshotProvider> session_snapshot_provider_;
  std::optional<HaStateSnapshot> last_applied_ha_snapshot_;
  mutable std::mutex managed_queue_runner_mutex_;
  std::unordered_map<const void*, ManagedQueueRunnerSlot> managed_queue_runners_;
  ProfileRegistry profiles_;
  MetricsRegistry metrics_;
  TraceRecorder trace_;
  DiagnosticsMonitor diagnostics_;
  std::atomic<std::uint64_t> next_dynamic_session_id_{ kFirstDynamicSessionId };
};

#define config_ impl_->config_
#define runtime_ impl_->runtime_
#define counterparties_ impl_->counterparties_
#define contracts_ impl_->contracts_
#define session_factory_ impl_->session_factory_
#define session_snapshot_provider_ impl_->session_snapshot_provider_
#define last_applied_ha_snapshot_ impl_->last_applied_ha_snapshot_
#define managed_queue_runner_mutex_ impl_->managed_queue_runner_mutex_
#define managed_queue_runners_ impl_->managed_queue_runners_
#define profiles_ impl_->profiles_
#define metrics_ impl_->metrics_
#define trace_ impl_->trace_
#define diagnostics_ impl_->diagnostics_
#define next_dynamic_session_id_ impl_->next_dynamic_session_id_

Engine::Engine()
  : impl_(std::make_unique<Impl>())
{
}

Engine::~Engine()
{
  diagnostics_.Stop();
}

auto
Engine::profiles() const -> const ProfileRegistry&
{
  return profiles_;
}

auto
Engine::runtime() const -> const ShardedRuntime*
{
  return runtime_.has_value() ? &*runtime_ : nullptr;
}

auto
Engine::mutable_runtime() -> ShardedRuntime*
{
  return runtime_.has_value() ? &*runtime_ : nullptr;
}

auto
Engine::metrics() const -> const MetricsRegistry&
{
  return metrics_;
}

auto
Engine::mutable_metrics() -> MetricsRegistry*
{
  return &metrics_;
}

auto
Engine::trace() const -> const TraceRecorder&
{
  return trace_;
}

auto
Engine::mutable_trace() -> TraceRecorder*
{
  return &trace_;
}

auto
Engine::diagnostics() -> DiagnosticsMonitor&
{
  return diagnostics_;
}

auto
Engine::diagnostics() const -> const DiagnosticsMonitor&
{
  return diagnostics_;
}

auto
Engine::config() const -> const EngineConfig*
{
  return config_.has_value() ? &*config_ : nullptr;
}

auto
Engine::LoadProfiles(const EngineConfig& config) -> base::Status
{
  profiles_.Clear();
  contracts_.clear();
  ProfileRegistry loaded_profiles;
  const profile::ProfileLoadOptions load_options{
    .madvise = config.profile_madvise,
    .mlock = config.profile_mlock,
  };
  for (const auto& artifact_path : config.profile_artifacts) {
    auto loaded = profile::LoadProfileArtifact(artifact_path, load_options);
    if (!loaded.ok()) {
      return loaded.status();
    }

    const auto profile_id = loaded.value().profile_id();

    auto status = loaded_profiles.Register(std::move(loaded).value());
    if (!status.ok()) {
      return status;
    }

    trace_.Record(TraceEventKind::kProfileLoaded, profile_id, 0U, 0U, profile_id, 0U, artifact_path.string());
  }

  for (const auto& dict_paths : config.profile_dictionaries) {
    auto loaded = profile::LoadProfileFromDictionaryFiles(dict_paths);
    if (!loaded.ok()) {
      return loaded.status();
    }

    const auto profile_id = loaded.value().profile_id();

    auto status = loaded_profiles.Register(std::move(loaded).value());
    if (!status.ok()) {
      return status;
    }

    trace_.Record(TraceEventKind::kProfileLoaded, profile_id, 0U, 0U, profile_id, 0U, dict_paths.front().string());
  }

  auto loaded_contracts = LoadContractMap(config.profile_contracts);
  if (!loaded_contracts.ok()) {
    return loaded_contracts.status();
  }
  for (const auto& [profile_id, contract] : loaded_contracts.value()) {
    const auto* loaded_profile = loaded_profiles.Find(profile_id);
    if (loaded_profile == nullptr) {
      return base::Status::NotFound("contract sidecar references an unloaded profile_id");
    }
    if (contract.schema_hash != 0U && contract.schema_hash != loaded_profile->schema_hash()) {
      return base::Status::VersionMismatch("contract sidecar schema_hash does not match the loaded profile");
    }
  }
  profiles_ = std::move(loaded_profiles);
  contracts_ = std::move(loaded_contracts).value();

  return base::Status::Ok();
}

auto
Engine::Boot(const EngineConfig& config) -> base::Status
{
  auto validation = ValidateEngineConfig(config);
  if (!validation.ok()) {
    return validation;
  }

  {
    std::lock_guard lock(managed_queue_runner_mutex_);
    managed_queue_runners_.clear();
  }

  config_ = config;
  counterparties_.clear();
  runtime_.emplace(config.worker_count);
  metrics_.Reset(config.worker_count);
  trace_.Configure(config.trace_mode, config.trace_capacity, config.worker_count);
  diagnostics_.Bind(&metrics_, &trace_);
  trace_.Record(
    TraceEventKind::kConfigLoaded, 0U, 0U, 0U, config.worker_count, config.counterparties.size(), "engine boot");

  auto status = LoadProfiles(config);
  if (!status.ok()) {
    return status;
  }

  for (auto& counterparty : config_->counterparties) {
    auto effective = ResolveEffectiveCounterpartyConfig(counterparty, contracts_);
    if (!effective.ok()) {
      return effective.status();
    }
    counterparty = std::move(effective).value();
  }

  for (const auto& counterparty : config_->counterparties) {
    if (profiles_.Find(counterparty.session.profile_id) == nullptr) {
      return base::Status::NotFound("counterparty references an unloaded profile");
    }

    session::SessionCore session(counterparty.session);
    status = runtime_->RegisterSession(session);
    if (!status.ok()) {
      return status;
    }

    const auto worker_id = runtime_->RouteSession(session.key());
    if (config.enable_metrics) {
      status = metrics_.RegisterSession(session.session_id(), worker_id);
      if (!status.ok()) {
        return status;
      }
    }

    counterparties_.emplace(session.session_id(), counterparty);
    trace_.Record(TraceEventKind::kSessionRegistered,
                  session.session_id(),
                  worker_id,
                  0U,
                  session.profile_id(),
                  static_cast<std::uint64_t>(counterparty.store_mode),
                  counterparty.name);
  }

  return base::Status::Ok();
}

auto
Engine::ApplyConfig(const EngineConfig& new_config) -> base::Result<ApplyConfigResult>
{
  if (!config_.has_value() || !runtime_.has_value()) {
    return base::Status::InvalidArgument("engine must be booted before ApplyConfig");
  }

  auto validation = ValidateEngineConfig(new_config);
  if (!validation.ok()) {
    return validation;
  }

  const auto current_config = *config_;
  const auto delta = ComputeConfigDelta(current_config, new_config);
  ApplyConfigResult result;
  if (delta.empty()) {
    return result;
  }

  auto stored_config = ApplyStoredEngineConfig(current_config, new_config);

  if (HasProfileLoadChange(current_config, new_config)) {
    auto loaded = ReloadProfilesForConfig(new_config);
    if (!loaded.ok()) {
      return loaded.status();
    }
    profiles_ = std::move(loaded.value().first);
    contracts_ = std::move(loaded.value().second);
  }

  for (const auto& change : delta.changes) {
    if (ChangeIsRestartRequired(change)) {
      result.skipped.push_back(change);
      continue;
    }

    if (ChangeRequiresRemoveAdd(change)) {
      if (const auto* current = FindCounterpartyBySessionId(current_config, change.session_id); current != nullptr) {
        ReplaceCounterpartyBySessionId(stored_config, *current);
      }
      result.skipped.push_back(change);
      continue;
    }

    switch (change.kind) {
      case ConfigChangeKind::kAddCounterparty: {
        const auto* proposed = FindCounterpartyBySessionId(new_config, change.session_id);
        if (proposed == nullptr) {
          return base::Status::NotFound("added counterparty was not found in proposed config");
        }
        auto effective = ResolveEffectiveCounterpartyConfig(*proposed, contracts_);
        if (!effective.ok()) {
          return effective.status();
        }
        if (profiles_.Find(effective.value().session.profile_id) == nullptr) {
          return base::Status::NotFound("counterparty references an unloaded profile");
        }

        session::SessionCore session(effective.value().session);
        auto status = runtime_->RegisterSession(session);
        if (!status.ok()) {
          return status;
        }

        const auto worker_id = runtime_->RouteSession(session.key());
        if (new_config.enable_metrics) {
          status = metrics_.RegisterSession(session.session_id(), worker_id);
          if (!status.ok() && metrics_.FindSession(session.session_id()) == nullptr) {
            (void)runtime_->UnregisterSession(session.session_id());
            return status;
          }
        }

        auto counterparty = std::move(effective).value();
        ReplaceCounterpartyBySessionId(stored_config, counterparty);
        counterparties_[session.session_id()] = counterparty;
        trace_.Record(TraceEventKind::kSessionRegistered,
                      session.session_id(),
                      worker_id,
                      0U,
                      session.profile_id(),
                      static_cast<std::uint64_t>(counterparty.store_mode),
                      counterparty.name);
        result.applied.push_back(change);
        break;
      }
      case ConfigChangeKind::kRemoveCounterparty: {
        const auto* shard = runtime_->FindSessionShard(change.session_id);
        const auto worker_id = shard == nullptr ? 0U : shard->worker_id;
        auto status = runtime_->UnregisterSession(change.session_id);
        if (!status.ok()) {
          return status;
        }
        counterparties_.erase(change.session_id);
        trace_.Record(TraceEventKind::kSessionEvent, change.session_id, worker_id, 0U, 0U, 0U, "session unregistered");
        result.applied.push_back(change);
        break;
      }
      case ConfigChangeKind::kModifyCounterparty: {
        const auto* proposed = FindCounterpartyBySessionId(new_config, change.session_id);
        if (proposed == nullptr) {
          return base::Status::NotFound("modified counterparty was not found in proposed config");
        }
        auto effective = ResolveEffectiveCounterpartyConfig(*proposed, contracts_);
        if (!effective.ok()) {
          return effective.status();
        }
        if (profiles_.Find(effective.value().session.profile_id) == nullptr) {
          return base::Status::NotFound("counterparty references an unloaded profile");
        }
        auto counterparty = std::move(effective).value();
        ReplaceCounterpartyBySessionId(stored_config, counterparty);
        counterparties_[change.session_id] = counterparty;
        const auto* shard = runtime_->FindSessionShard(change.session_id);
        trace_.Record(TraceEventKind::kSessionEvent,
                      change.session_id,
                      shard == nullptr ? 0U : shard->worker_id,
                      0U,
                      counterparty.session.profile_id,
                      static_cast<std::uint64_t>(counterparty.store_mode),
                      counterparty.name);
        result.applied.push_back(change);
        break;
      }
      case ConfigChangeKind::kAddListener:
      case ConfigChangeKind::kRemoveListener:
      case ConfigChangeKind::kModifyListener:
        trace_.Record(TraceEventKind::kConfigLoaded, 0U, 0U, 0U, 0U, 0U, change.name);
        result.applied.push_back(change);
        break;
      case ConfigChangeKind::kEngineFieldChanged:
        if (change.name == "trace") {
          trace_.Configure(new_config.trace_mode, new_config.trace_capacity, current_config.worker_count);
        }
        trace_.Record(TraceEventKind::kConfigLoaded, 0U, 0U, 0U, current_config.worker_count, 0U, change.name);
        result.applied.push_back(change);
        break;
    }
  }

  config_ = std::move(stored_config);
  return result;
}

#undef managed_queue_runner_mutex_
#undef managed_queue_runners_

auto
EnsureManagedQueueRunnerStarted(Engine& engine,
                                const void* owner,
                                ApplicationCallbacks* application,
                                std::optional<ManagedQueueApplicationRunnerOptions>* options) -> base::Status
{
  if (owner == nullptr) {
    return base::Status::InvalidArgument("managed queue runner requires an owner token");
  }

  auto* engine_impl = engine.impl_.get();
  std::lock_guard lock(engine_impl->managed_queue_runner_mutex_);
  const auto existing = engine_impl->managed_queue_runners_.find(owner);
  if (existing != engine_impl->managed_queue_runners_.end()) {
    if (existing->second.mode == ManagedQueueApplicationRunnerMode::kThreaded) {
      if (existing->second.runner->running()) {
        return base::Status::Ok();
      }
      return existing->second.runner->Start();
    }

    existing->second.active = true;
    return base::Status::Ok();
  }

  if (options == nullptr || !options->has_value()) {
    return base::Status::InvalidArgument("managed queue runner requires runner options before first start");
  }

  auto queue_application = ResolveQueueApplication(application);
  if (!queue_application.ok()) {
    return queue_application.status();
  }

  auto status = ValidateManagedQueueRunnerOptions(engine.config(), queue_application.value(), options->value());
  if (!status.ok()) {
    return status;
  }

  Engine::Impl::ManagedQueueRunnerSlot slot{
    .mode = options->value().mode,
    .application = queue_application.value(),
    .handlers = {},
    .poller_options = options->value().poller_options,
    .runner = nullptr,
    .active = true,
  };

  if (options->value().mode == ManagedQueueApplicationRunnerMode::kThreaded) {
    auto runner = std::make_unique<QueueApplicationRunner>(queue_application.value(),
                                                           std::move(options->value().handlers),
                                                           options->value().poller_options,
                                                           std::move(options->value().thread_options));
    status = runner->Start();
    if (!status.ok()) {
      return status;
    }
    slot.runner = std::move(runner);
    engine_impl->managed_queue_runners_.emplace(owner, std::move(slot));
    return base::Status::Ok();
  }

  slot.handlers = std::move(options->value().handlers);
  engine_impl->managed_queue_runners_.emplace(owner, std::move(slot));
  return base::Status::Ok();
}

auto
PollManagedQueueWorkerOnce(Engine& engine, const void* owner, std::uint32_t worker_id) -> base::Result<std::size_t>
{
  if (owner == nullptr) {
    return base::Status::InvalidArgument("managed queue runner requires an owner token");
  }

  auto* engine_impl = engine.impl_.get();
  std::lock_guard lock(engine_impl->managed_queue_runner_mutex_);
  const auto it = engine_impl->managed_queue_runners_.find(owner);
  if (it == engine_impl->managed_queue_runners_.end()) {
    return std::size_t{ 0U };
  }

  auto& slot = it->second;
  if (!slot.active || slot.mode != ManagedQueueApplicationRunnerMode::kCoScheduled) {
    return std::size_t{ 0U };
  }
  if (worker_id >= slot.handlers.size()) {
    return base::Status::NotFound("queue application runner worker was not found");
  }

  QueueApplicationPoller poller(slot.application, slot.handlers[worker_id].get(), slot.poller_options);
  std::size_t drained_total = 0U;
  while (true) {
    auto drained = poller.PollWorkerOnce(worker_id);
    if (!drained.ok()) {
      return drained.status();
    }

    drained_total += drained.value();
    if (drained.value() == 0U) {
      return drained_total;
    }

    const auto limit = slot.poller_options.max_events_per_poll;
    if (limit != 0U && drained.value() < limit) {
      return drained_total;
    }
  }
}

auto
StopManagedQueueRunner(Engine& engine, const void* owner) -> base::Status
{
  if (owner == nullptr) {
    return base::Status::InvalidArgument("managed queue runner requires an owner token");
  }

  auto* engine_impl = engine.impl_.get();
  std::lock_guard lock(engine_impl->managed_queue_runner_mutex_);
  const auto it = engine_impl->managed_queue_runners_.find(owner);
  if (it == engine_impl->managed_queue_runners_.end()) {
    return base::Status::Ok();
  }
  if (it->second.mode != ManagedQueueApplicationRunnerMode::kThreaded) {
    it->second.active = false;
    return base::Status::Ok();
  }
  return it->second.runner->Stop();
}

auto
ReleaseManagedQueueRunner(Engine& engine, const void* owner) -> base::Status
{
  if (owner == nullptr) {
    return base::Status::InvalidArgument("managed queue runner requires an owner token");
  }

  std::unique_ptr<QueueApplicationRunner> runner;
  {
    auto* engine_impl = engine.impl_.get();
    std::lock_guard lock(engine_impl->managed_queue_runner_mutex_);
    const auto it = engine_impl->managed_queue_runners_.find(owner);
    if (it == engine_impl->managed_queue_runners_.end()) {
      return base::Status::Ok();
    }
    if (it->second.mode == ManagedQueueApplicationRunnerMode::kThreaded) {
      runner = std::move(it->second.runner);
    }
    engine_impl->managed_queue_runners_.erase(it);
  }
  return runner == nullptr ? base::Status::Ok() : runner->Stop();
}

auto
Engine::FindCounterpartyConfig(std::uint64_t session_id) const -> const CounterpartyConfig*
{
  const auto it = counterparties_.find(session_id);
  if (it == counterparties_.end()) {
    return nullptr;
  }
  return &it->second;
}

auto
Engine::QueryScheduleStatus(std::uint64_t session_id, std::uint64_t unix_time_ns) const
  -> base::Result<SessionScheduleStatus>
{
  const auto* counterparty = FindCounterpartyConfig(session_id);
  if (counterparty == nullptr) {
    return base::Status::NotFound("counterparty session not found");
  }
  auto status = runtime::QueryScheduleStatus(*counterparty, unix_time_ns);
  trace_.Record(TraceEventKind::kScheduleEvent,
                session_id,
                0U,
                unix_time_ns,
                status.in_session_window ? 1U : 0U,
                status.in_logon_window ? 1U : 0U,
                "schedule status queried");
  return status;
}

auto
Engine::FindListenerConfig(std::string_view name) const -> const ListenerConfig*
{
  if (!config_.has_value()) {
    return nullptr;
  }
  for (const auto& listener : config_->listeners) {
    if (listener.name == name) {
      return &listener;
    }
  }
  return nullptr;
}

auto
Engine::LoadDictionaryView(std::uint64_t profile_id) const -> base::Result<profile::NormalizedDictionaryView>
{
  const auto* loaded = profiles_.Find(profile_id);
  if (loaded == nullptr) {
    return base::Status::NotFound("profile dictionary not found");
  }
  return profile::NormalizedDictionaryView::FromProfile(*loaded);
}

auto
Engine::ResolveInboundSession(const codec::SessionHeader& header) const -> base::Result<ResolvedCounterparty>
{
  return ResolveInboundSession(codec::SessionHeaderView{
    .begin_string = header.begin_string,
    .msg_type = header.msg_type,
    .sender_comp_id = header.sender_comp_id,
    .sender_sub_id = header.sender_sub_id,
    .target_comp_id = header.target_comp_id,
    .target_sub_id = header.target_sub_id,
    .default_appl_ver_id = header.default_appl_ver_id,
    .sending_time = header.sending_time,
    .orig_sending_time = header.orig_sending_time,
    .body_length = header.body_length,
    .msg_seq_num = header.msg_seq_num,
    .checksum = header.checksum,
    .poss_dup = header.poss_dup,
  });
}

auto
Engine::ResolveInboundSession(const codec::SessionHeaderView& header) const -> base::Result<ResolvedCounterparty>
{
  if (!config_.has_value()) {
    return base::Status::InvalidArgument("engine is not booted");
  }
  if (header.begin_string.empty() || header.sender_comp_id.empty() || header.target_comp_id.empty()) {
    return base::Status::InvalidArgument("inbound FIX header is missing session identity fields");
  }

  for (const auto& counterparty : config_->counterparties) {
    const auto& key = counterparty.session.key;
    if (key.begin_string != header.begin_string) {
      continue;
    }
    if (key.sender_comp_id != header.target_comp_id) {
      continue;
    }
    if (key.target_comp_id != header.sender_comp_id) {
      continue;
    }
    if (!counterparty.default_appl_ver_id.empty() && counterparty.default_appl_ver_id != header.default_appl_ver_id) {
      continue;
    }

    auto dictionary = LoadDictionaryView(counterparty.session.profile_id);
    if (!dictionary.ok()) {
      return dictionary.status();
    }

    return ResolvedCounterparty{
      .counterparty = counterparty,
      .dictionary = std::move(dictionary).value(),
    };
  }

  if (!config_->accept_unknown_sessions) {
    return base::Status::NotFound("no counterparty matched inbound FIX session header");
  }

  if (session_factory_.has_value()) {
    session::SessionKey key{
      std::string(header.begin_string),
      std::string(header.target_comp_id),
      std::string(header.sender_comp_id),
    };
    auto factory_result = (*session_factory_)(key);
    if (!factory_result.ok()) {
      return factory_result.status();
    }

    auto& counterparty = factory_result.value();
    if (counterparty.session.session_id == 0) {
      counterparty.session.session_id = next_dynamic_session_id_.fetch_add(1, std::memory_order_relaxed);
    }
    auto dictionary = LoadDictionaryView(counterparty.session.profile_id);
    if (!dictionary.ok()) {
      return dictionary.status();
    }

    return ResolvedCounterparty{
      .counterparty = std::move(counterparty),
      .dictionary = std::move(dictionary).value(),
    };
  }

  return base::Status::NotFound("accept_unknown_sessions is enabled but no session factory matched the inbound FIX "
                                "session header");
}

void
Engine::SetSessionFactory(SessionFactory factory)
{
  session_factory_ = std::move(factory);
}

void
Engine::SetSessionSnapshotProvider(SessionSnapshotProvider provider)
{
  session_snapshot_provider_ = std::move(provider);
}

auto
Engine::QuerySessionSnapshots() const -> std::vector<session::SessionSnapshot>
{
  if (session_snapshot_provider_.has_value()) {
    return (*session_snapshot_provider_)();
  }
  return {};
}

void
Engine::SetLastAppliedHaSnapshot(HaStateSnapshot snapshot)
{
  last_applied_ha_snapshot_ = std::move(snapshot);
}

auto
Engine::last_applied_ha_snapshot() const -> const HaStateSnapshot*
{
  return last_applied_ha_snapshot_.has_value() ? &*last_applied_ha_snapshot_ : nullptr;
}

void
WhitelistSessionFactory::Allow(std::string_view begin_string,
                               std::string_view sender_comp_id,
                               const CounterpartyConfig& config_template)
{
  std::string key;
  key.reserve(begin_string.size() + 1 + sender_comp_id.size());
  key.append(begin_string);
  key.push_back('\x01');
  key.append(sender_comp_id);
  entries_.emplace(std::move(key),
                   Entry{
                     .begin_string = std::string(begin_string),
                     .sender_comp_id = std::string(sender_comp_id),
                     .config_template = config_template,
                   });
}

void
WhitelistSessionFactory::AllowAny(const CounterpartyConfig& config_template)
{
  allow_any_template_ = config_template;
}

auto
WhitelistSessionFactory::operator()(const session::SessionKey& key) const -> base::Result<CounterpartyConfig>
{
  // Try exact match first: begin_string + \x01 + sender_comp_id
  {
    std::string lookup;
    lookup.reserve(key.begin_string.size() + 1 + key.sender_comp_id.size());
    lookup.append(key.begin_string);
    lookup.push_back('\x01');
    lookup.append(key.sender_comp_id);
    const auto it = entries_.find(lookup);
    if (it != entries_.end()) {
      auto config = it->second.config_template;
      config.session.key = key;
      return config;
    }
  }

  // Try wildcard sender match: begin_string + \x01 + ""
  {
    std::string lookup;
    lookup.reserve(key.begin_string.size() + 1);
    lookup.append(key.begin_string);
    lookup.push_back('\x01');
    const auto it = entries_.find(lookup);
    if (it != entries_.end()) {
      auto config = it->second.config_template;
      config.session.key = key;
      return config;
    }
  }

  if (allow_any_template_.has_value()) {
    auto config = *allow_any_template_;
    config.session.key = key;
    return config;
  }

  return base::Status::NotFound("session not in whitelist");
}

#undef config_
#undef runtime_
#undef counterparties_
#undef session_factory_
#undef session_snapshot_provider_
#undef last_applied_ha_snapshot_
#undef managed_queue_runner_mutex_
#undef managed_queue_runners_
#undef profiles_
#undef metrics_
#undef trace_
#undef diagnostics_
#undef next_dynamic_session_id_

} // namespace nimble::runtime
