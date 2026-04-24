#include "nimblefix/runtime/engine.h"

#include <atomic>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "nimblefix/codec/fix_codec.h"
#include "nimblefix/profile/profile_loader.h"
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
  std::optional<SessionFactory> session_factory_;
  mutable std::mutex managed_queue_runner_mutex_;
  std::unordered_map<const void*, ManagedQueueRunnerSlot> managed_queue_runners_;
  ProfileRegistry profiles_;
  MetricsRegistry metrics_;
  TraceRecorder trace_;
  std::atomic<std::uint64_t> next_dynamic_session_id_{ kFirstDynamicSessionId };
};

#define config_ impl_->config_
#define runtime_ impl_->runtime_
#define counterparties_ impl_->counterparties_
#define session_factory_ impl_->session_factory_
#define managed_queue_runner_mutex_ impl_->managed_queue_runner_mutex_
#define managed_queue_runners_ impl_->managed_queue_runners_
#define profiles_ impl_->profiles_
#define metrics_ impl_->metrics_
#define trace_ impl_->trace_
#define next_dynamic_session_id_ impl_->next_dynamic_session_id_

Engine::Engine()
  : impl_(std::make_unique<Impl>())
{
}

Engine::~Engine() = default;

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
Engine::config() const -> const EngineConfig*
{
  return config_.has_value() ? &*config_ : nullptr;
}

auto
Engine::LoadProfiles(const EngineConfig& config) -> base::Status
{
  profiles_.Clear();
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

    auto status = profiles_.Register(std::move(loaded).value());
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

    auto status = profiles_.Register(std::move(loaded).value());
    if (!status.ok()) {
      return status;
    }

    trace_.Record(TraceEventKind::kProfileLoaded, profile_id, 0U, 0U, profile_id, 0U, dict_paths.front().string());
  }

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
  trace_.Record(
    TraceEventKind::kConfigLoaded, 0U, 0U, 0U, config.worker_count, config.counterparties.size(), "engine boot");

  auto status = LoadProfiles(config);
  if (!status.ok()) {
    return status;
  }

  for (const auto& counterparty : config.counterparties) {
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
Engine::EnsureManagedQueueRunnerStarted(const void* owner,
                                        ApplicationCallbacks* application,
                                        std::optional<ManagedQueueApplicationRunnerOptions>* options) -> base::Status
{
  if (owner == nullptr) {
    return base::Status::InvalidArgument("managed queue runner requires an owner token");
  }

  std::lock_guard lock(managed_queue_runner_mutex_);
  const auto existing = managed_queue_runners_.find(owner);
  if (existing != managed_queue_runners_.end()) {
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

  auto status = ValidateManagedQueueRunnerOptions(config(), queue_application.value(), options->value());
  if (!status.ok()) {
    return status;
  }

  Impl::ManagedQueueRunnerSlot slot{
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
    managed_queue_runners_.emplace(owner, std::move(slot));
    return base::Status::Ok();
  }

  slot.handlers = std::move(options->value().handlers);
  managed_queue_runners_.emplace(owner, std::move(slot));
  return base::Status::Ok();
}

auto
Engine::PollManagedQueueWorkerOnce(const void* owner, std::uint32_t worker_id) -> base::Result<std::size_t>
{
  if (owner == nullptr) {
    return base::Status::InvalidArgument("managed queue runner requires an owner token");
  }

  std::lock_guard lock(managed_queue_runner_mutex_);
  const auto it = managed_queue_runners_.find(owner);
  if (it == managed_queue_runners_.end()) {
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
Engine::StopManagedQueueRunner(const void* owner) -> base::Status
{
  if (owner == nullptr) {
    return base::Status::InvalidArgument("managed queue runner requires an owner token");
  }

  std::lock_guard lock(managed_queue_runner_mutex_);
  const auto it = managed_queue_runners_.find(owner);
  if (it == managed_queue_runners_.end()) {
    return base::Status::Ok();
  }
  if (it->second.mode != ManagedQueueApplicationRunnerMode::kThreaded) {
    it->second.active = false;
    return base::Status::Ok();
  }
  return it->second.runner->Stop();
}

auto
Engine::ReleaseManagedQueueRunner(const void* owner) -> base::Status
{
  if (owner == nullptr) {
    return base::Status::InvalidArgument("managed queue runner requires an owner token");
  }

  std::unique_ptr<QueueApplicationRunner> runner;
  {
    std::lock_guard lock(managed_queue_runner_mutex_);
    const auto it = managed_queue_runners_.find(owner);
    if (it == managed_queue_runners_.end()) {
      return base::Status::Ok();
    }
    if (it->second.mode == ManagedQueueApplicationRunnerMode::kThreaded) {
      runner = std::move(it->second.runner);
    }
    managed_queue_runners_.erase(it);
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
#undef managed_queue_runner_mutex_
#undef managed_queue_runners_
#undef profiles_
#undef metrics_
#undef trace_
#undef next_dynamic_session_id_

} // namespace nimble::runtime
