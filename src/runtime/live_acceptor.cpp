#include "nimblefix/runtime/live_acceptor.h"

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <sys/epoll.h>
#include <unistd.h>

#include "nimblefix/base/spsc_queue.h"
#include "nimblefix/codec/fix_tags.h"
#include "nimblefix/runtime/thread_affinity.h"
#include "nimblefix/store/durable_batch_store.h"
#include "nimblefix/store/memory_store.h"
#include "nimblefix/store/mmap_store.h"

namespace nimble::runtime {

namespace {

auto
NowNs() -> std::uint64_t
{
  return static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

auto
WallClockNowNs() -> std::uint64_t
{
  return static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

auto
IsAdminMessage(std::string_view msg_type) -> bool
{
  return msg_type == "0" || msg_type == "1" || msg_type == "2" || msg_type == "3" || msg_type == "4" ||
         msg_type == "5" || msg_type == "A";
}

auto
SessionKeyText(const session::SessionKey& key) -> std::string
{
  return key.begin_string + ':' + key.sender_comp_id + "->" + key.target_comp_id;
}

auto
IsChecksumFailure(const base::Status& status) -> bool
{
  return status.code() == base::ErrorCode::kFormatError &&
         status.message().find("CheckSum mismatch") != std::string::npos;
}

auto
MakeProtocolConfig(const CounterpartyConfig& counterparty) -> session::AdminProtocolConfig
{
  return session::AdminProtocolConfig{
    .session = counterparty.session,
    .transport_profile = counterparty.transport_profile,
    .begin_string = counterparty.session.key.begin_string,
    .sender_comp_id = counterparty.session.key.sender_comp_id,
    .target_comp_id = counterparty.session.key.target_comp_id,
    .default_appl_ver_id = counterparty.default_appl_ver_id,
    .heartbeat_interval_seconds = counterparty.session.heartbeat_interval_seconds,
    .reset_seq_num_on_logon = counterparty.reset_seq_num_on_logon,
    .reset_seq_num_on_logout = counterparty.reset_seq_num_on_logout,
    .reset_seq_num_on_disconnect = counterparty.reset_seq_num_on_disconnect,
    .refresh_on_logon = counterparty.refresh_on_logon,
    .send_next_expected_msg_seq_num = counterparty.send_next_expected_msg_seq_num,
    .validation_policy = counterparty.validation_policy,
  };
}

auto
WorkerCpuAffinity(const Engine* engine, std::uint32_t worker_id) -> std::optional<std::uint32_t>
{
  if (engine == nullptr || engine->config() == nullptr) {
    return std::nullopt;
  }

  const auto& worker_cpu_affinity = engine->config()->worker_cpu_affinity;
  if (worker_id >= worker_cpu_affinity.size()) {
    return std::nullopt;
  }
  return worker_cpu_affinity[worker_id];
}

auto
FrontDoorCpuAffinity(const Engine* engine) -> std::optional<std::uint32_t>
{
  if (engine == nullptr || engine->config() == nullptr) {
    return std::nullopt;
  }
  return engine->config()->front_door_cpu;
}

auto
ApplyAcceptorFrontDoorThreadSetup(const Engine* engine, bool single_worker) -> base::Status
{
  if (single_worker) {
    SetCurrentThreadName("ff-acc-w0");
    if (const auto worker_cpu = WorkerCpuAffinity(engine, 0U)) {
      return ApplyCurrentThreadAffinity(*worker_cpu, "acceptor worker 0");
    }
    if (const auto front_door_cpu = FrontDoorCpuAffinity(engine)) {
      return ApplyCurrentThreadAffinity(*front_door_cpu, "acceptor front door");
    }
    return base::Status::Ok();
  }

  SetCurrentThreadName("ff-acc-main");
  if (const auto front_door_cpu = FrontDoorCpuAffinity(engine)) {
    return ApplyCurrentThreadAffinity(*front_door_cpu, "acceptor front door");
  }
  return base::Status::Ok();
}

auto
ApplyAcceptorWorkerThreadSetup(const Engine* engine, std::uint32_t worker_id) -> base::Status
{
  SetCurrentThreadName("ff-acc-w" + std::to_string(worker_id));
  if (const auto worker_cpu = WorkerCpuAffinity(engine, worker_id)) {
    return ApplyCurrentThreadAffinity(*worker_cpu, "acceptor worker " + std::to_string(worker_id));
  }
  return base::Status::Ok();
}

thread_local const void* g_inline_borrow_send_sink = nullptr;

class BorrowedSendScope
{
public:
  explicit BorrowedSendScope(const void* sink)
    : previous_(g_inline_borrow_send_sink)
  {
    g_inline_borrow_send_sink = sink;
  }

  BorrowedSendScope(const BorrowedSendScope&) = delete;
  auto operator=(const BorrowedSendScope&) -> BorrowedSendScope& = delete;

  ~BorrowedSendScope() { g_inline_borrow_send_sink = previous_; }

private:
  const void* previous_{ nullptr };
};

} // namespace

enum class OutboundCommandKind : std::uint32_t
{
  kSendApplication = 0,
  kSendEncodedApplication,
};

struct OutboundCommand
{
  OutboundCommandKind kind{ OutboundCommandKind::kSendApplication };
  std::uint64_t session_id{ 0 };
  message::MessageRef message;
  session::EncodedApplicationMessageRef encoded_message;
  session::SessionSendEnvelopeRef envelope;
};

class SubscriberStream final : public session::SessionSubscriptionStream
{
public:
  explicit SubscriberStream(std::size_t queue_capacity)
    : queue_(queue_capacity)
  {
  }

  auto TryPop() -> base::Result<std::optional<session::SessionNotification>> override { return queue_.TryPop(); }

  auto TryPush(const session::SessionNotification& notification) -> bool { return queue_.TryPush(notification); }

private:
  base::SpscQueue<session::SessionNotification> queue_;
};

class LiveAcceptor::CommandSink final : public session::SessionCommandSink
{
public:
  CommandSink(LiveAcceptor* owner, std::uint32_t worker_id, std::size_t queue_capacity)
    : owner_(owner)
    , worker_id_(worker_id)
    , queue_(queue_capacity)
  {
  }

  auto EnqueueSend(std::uint64_t session_id, message::MessageRef message) -> base::Status override
  {
    return EnqueueSendWithEnvelope(session_id, std::move(message), {});
  }

  auto EnqueueSendWithEnvelope(std::uint64_t session_id,
                               message::MessageRef message,
                               session::SessionSendEnvelopeRef envelope) -> base::Status override
  {
    if (!queue_.TryPush(OutboundCommand{
          .kind = OutboundCommandKind::kSendApplication,
          .session_id = session_id,
          .message = std::move(message),
          .encoded_message = {},
          .envelope = std::move(envelope),
        })) {
      return base::Status::IoError("runtime outbound command queue is full");
    }
    owner_->SignalWorkerWakeup(worker_id_);
    return base::Status::Ok();
  }

  auto EnqueueSendBorrowed(std::uint64_t session_id, const message::MessageRef& message) -> base::Result<bool> override
  {
    return EnqueueSendBorrowedWithEnvelope(session_id, message, {});
  }

  auto EnqueueSendBorrowedWithEnvelope(std::uint64_t session_id,
                                       const message::MessageRef& message,
                                       session::SessionSendEnvelopeView envelope) -> base::Result<bool> override
  {
    if (g_inline_borrow_send_sink != this) {
      return false;
    }
    if (!queue_.TryPush(OutboundCommand{
          .kind = OutboundCommandKind::kSendApplication,
          .session_id = session_id,
          .message = message,
          .encoded_message = {},
          .envelope = session::SessionSendEnvelopeRef(envelope),
        })) {
      return base::Status::IoError("runtime outbound command queue is full");
    }
    owner_->SignalWorkerWakeup(worker_id_);
    return true;
  }

  auto EnqueueSendEncoded(std::uint64_t session_id, session::EncodedApplicationMessageRef message)
    -> base::Status override
  {
    return EnqueueSendEncodedWithEnvelope(session_id, std::move(message), {});
  }

  auto EnqueueSendEncodedWithEnvelope(std::uint64_t session_id,
                                      session::EncodedApplicationMessageRef message,
                                      session::SessionSendEnvelopeRef envelope) -> base::Status override
  {
    if (!queue_.TryPush(OutboundCommand{
          .kind = OutboundCommandKind::kSendEncodedApplication,
          .session_id = session_id,
          .message = {},
          .encoded_message = std::move(message),
          .envelope = std::move(envelope),
        })) {
      return base::Status::IoError("runtime outbound command queue is full");
    }
    owner_->SignalWorkerWakeup(worker_id_);
    return base::Status::Ok();
  }

  auto EnqueueSendEncodedBorrowed(std::uint64_t session_id, const session::EncodedApplicationMessageRef& message)
    -> base::Result<bool> override
  {
    return EnqueueSendEncodedBorrowedWithEnvelope(session_id, message, {});
  }

  auto EnqueueSendEncodedBorrowedWithEnvelope(std::uint64_t session_id,
                                              const session::EncodedApplicationMessageRef& message,
                                              session::SessionSendEnvelopeView envelope) -> base::Result<bool> override
  {
    if (g_inline_borrow_send_sink != this) {
      return false;
    }
    if (!queue_.TryPush(OutboundCommand{
          .kind = OutboundCommandKind::kSendEncodedApplication,
          .session_id = session_id,
          .message = {},
          .encoded_message = message,
          .envelope = session::SessionSendEnvelopeRef(envelope),
        })) {
      return base::Status::IoError("runtime outbound command queue is full");
    }
    owner_->SignalWorkerWakeup(worker_id_);
    return true;
  }

  auto LoadSnapshot(std::uint64_t session_id) const -> base::Result<session::SessionSnapshot> override
  {
    return owner_->LoadSessionSnapshot(session_id);
  }

  auto Subscribe(std::uint64_t session_id, std::size_t queue_capacity)
    -> base::Result<session::SessionSubscription> override
  {
    return owner_->RegisterSessionSubscriber(session_id, queue_capacity);
  }

  auto TryPop() -> std::optional<OutboundCommand> { return queue_.TryPop(); }

private:
  LiveAcceptor* owner_{ nullptr };
  std::uint32_t worker_id_{ 0 };
  base::SpscQueue<OutboundCommand> queue_;
};

LiveAcceptor::LiveAcceptor(Engine* engine)
  : LiveAcceptor(engine, Options{})
{
}

LiveAcceptor::LiveAcceptor(Engine* engine, Options options)
  : engine_(engine)
  , options_(std::move(options))
{
}

LiveAcceptor::~LiveAcceptor()
{
  Stop();
  if (engine_ != nullptr) {
    static_cast<void>(engine_->ReleaseManagedQueueRunner(this));
  }
}

auto
LiveAcceptor::EnsureManagedQueueRunnerStarted() -> base::Status
{
  if (!options_.managed_queue_runner.has_value()) {
    return base::Status::Ok();
  }

  if (engine_ == nullptr) {
    return base::Status::InvalidArgument("live acceptor requires a booted engine");
  }

  return engine_->EnsureManagedQueueRunnerStarted(this, options_.application.get(), &options_.managed_queue_runner);
}

auto
LiveAcceptor::StopManagedQueueRunner() -> base::Status
{
  if (!options_.managed_queue_runner.has_value() || engine_ == nullptr) {
    return base::Status::Ok();
  }
  return engine_->StopManagedQueueRunner(this);
}

auto
LiveAcceptor::ResetWorkerShards(std::uint32_t worker_count) -> base::Status
{
  if (worker_count == 0U) {
    worker_count = 1U;
  }

  worker_shards_.clear();
  worker_shards_.reserve(worker_count);
  for (std::uint32_t worker_id = 0; worker_id < worker_count; ++worker_id) {
    worker_shards_.emplace_back();
    auto& shard = worker_shards_.back();
    shard.worker_id = worker_id;
    auto status = shard.poller.OpenWakeup();
    if (!status.ok()) {
      worker_shards_.clear();
      return status;
    }
    {
      auto backend = IoBackend::kEpoll;
      if (engine_ != nullptr && engine_->config() != nullptr) {
        backend = engine_->config()->io_backend;
      }
      status = shard.poller.InitBackend(backend);
      if (!status.ok()) {
        worker_shards_.clear();
        return status;
      }
    }
    shard.command_sink = std::make_shared<CommandSink>(this, worker_id, options_.command_queue_capacity);
    shard.inbox = std::make_unique<WorkerInbox>();
  }

  return base::Status::Ok();
}

auto
LiveAcceptor::HasActiveSession(std::uint64_t session_id) const -> bool
{
  std::lock_guard lock(control_mutex_);
  return active_session_ids_.contains(session_id);
}

auto
LiveAcceptor::RegisterActiveSession(std::uint64_t session_id) -> bool
{
  std::lock_guard lock(control_mutex_);
  return active_session_ids_.emplace(session_id).second;
}

auto
LiveAcceptor::UnregisterActiveSession(std::uint64_t session_id) -> void
{
  std::lock_guard lock(control_mutex_);
  active_session_ids_.erase(session_id);
}

auto
LiveAcceptor::SetTerminalStatus(base::Status status) -> void
{
  std::lock_guard lock(control_mutex_);
  if (!terminal_status_.has_value()) {
    terminal_status_ = std::move(status);
  }
}

auto
LiveAcceptor::LoadTerminalStatus() const -> std::optional<base::Status>
{
  std::lock_guard lock(control_mutex_);
  return terminal_status_;
}

auto
LiveAcceptor::AdoptPendingConnections(WorkerShardState& shard) -> void
{
  if (shard.inbox == nullptr) {
    return;
  }

  std::vector<ConnectionState> pending_connections;
  {
    std::lock_guard lock(shard.inbox->mutex);
    if (shard.inbox->pending_connections.empty()) {
      return;
    }
    pending_connections = std::move(shard.inbox->pending_connections);
    shard.inbox->pending_connections.clear();
  }

  const auto timestamp_ns = NowNs();
  for (auto& connection : pending_connections) {
    shard.connections.push_back(std::move(connection));
    IndexConnection(shard, shard.connections.size() - 1U);
    RefreshConnectionTimer(shard, shard.connections.back(), timestamp_ns);
  }
}

auto
LiveAcceptor::EnqueuePendingConnection(std::uint32_t worker_id, ConnectionState connection) -> base::Status
{
  auto* shard = FindWorkerShard(worker_id);
  if (shard == nullptr || shard->inbox == nullptr) {
    return base::Status::NotFound("acceptor worker inbox was not found");
  }

  {
    std::lock_guard lock(shard->inbox->mutex);
    shard->inbox->pending_connections.push_back(std::move(connection));
  }
  SignalWorkerWakeup(worker_id);
  return base::Status::Ok();
}

auto
LiveAcceptor::StartWorkerThreads() -> base::Status
{
  if (!worker_threads_.empty() || worker_shards_.size() <= 1U) {
    return base::Status::Ok();
  }

  try {
    worker_threads_.reserve(worker_shards_.size());
    for (const auto& shard : worker_shards_) {
      worker_threads_.emplace_back(
        [this, worker_id = shard.worker_id](std::stop_token stop_token) { WorkerLoop(worker_id, stop_token); });
    }
  } catch (...) {
    StopWorkerThreads();
    return base::Status::IoError("failed to start acceptor worker threads");
  }

  return base::Status::Ok();
}

auto
LiveAcceptor::StopWorkerThreads() -> void
{
  if (worker_threads_.empty()) {
    return;
  }

  for (auto& thread : worker_threads_) {
    thread.request_stop();
  }
  for (const auto& shard : worker_shards_) {
    SignalWorkerWakeup(shard.worker_id);
  }
  worker_threads_.clear();
}

auto
LiveAcceptor::WorkerLoop(std::uint32_t worker_id, std::stop_token stop_token) -> void
{
  auto* shard = FindWorkerShard(worker_id);
  if (shard == nullptr) {
    SetTerminalStatus(base::Status::NotFound("acceptor worker shard was not found"));
    stop_requested_.store(true);
    return;
  }

  auto status = ApplyAcceptorWorkerThreadSetup(engine_, worker_id);
  if (!status.ok()) {
    SetTerminalStatus(status);
    stop_requested_.store(true);
    return;
  }

  while (!stop_requested_.load() && !stop_token.stop_requested()) {
    AdoptPendingConnections(*shard);

    status = PollWorkerOnce(*shard, options_.poll_timeout);
    if (status.ok()) {
      continue;
    }

    SetTerminalStatus(status);
    stop_requested_.store(true);
    for (const auto& candidate : worker_shards_) {
      SignalWorkerWakeup(candidate.worker_id);
    }
    return;
  }
}

auto
LiveAcceptor::SignalWorkerWakeup(std::uint32_t worker_id) -> void
{
  auto* shard = FindWorkerShard(worker_id);
  if (shard == nullptr) {
    return;
  }
  shard->poller.SignalWakeup();
}

auto
LiveAcceptor::ResolveWorkerId(std::uint32_t worker_id) const -> std::uint32_t
{
  if (worker_shards_.empty()) {
    return 0U;
  }
  return std::min<std::uint32_t>(worker_id, static_cast<std::uint32_t>(worker_shards_.size() - 1U));
}

auto
LiveAcceptor::SelectAcceptWorkerId(std::uint32_t worker_hint) const -> std::uint32_t
{
  if (worker_shards_.empty()) {
    return 0U;
  }

  const auto start = ResolveWorkerId(worker_hint);
  auto best_index = start;
  auto best_load = worker_shards_[start].active_connections.load(std::memory_order_relaxed);

  for (std::size_t offset = 1U; offset < worker_shards_.size(); ++offset) {
    const auto index = static_cast<std::uint32_t>((start + offset) % worker_shards_.size());
    const auto load = worker_shards_[index].active_connections.load(std::memory_order_relaxed);
    if (load < best_load) {
      best_index = index;
      best_load = load;
    }
  }

  return worker_shards_[best_index].worker_id;
}

auto
LiveAcceptor::FindWorkerShard(std::uint32_t worker_id) -> WorkerShardState*
{
  if (worker_shards_.empty()) {
    return nullptr;
  }
  return &worker_shards_[ResolveWorkerId(worker_id)];
}

auto
LiveAcceptor::FindWorkerShard(std::uint32_t worker_id) const -> const WorkerShardState*
{
  if (worker_shards_.empty()) {
    return nullptr;
  }
  return &worker_shards_[ResolveWorkerId(worker_id)];
}

auto
LiveAcceptor::OpenListeners(std::string_view listener_name) -> base::Status
{
  if (engine_ == nullptr || engine_->config() == nullptr) {
    return base::Status::InvalidArgument("live acceptor requires a booted engine");
  }
  if (opened_) {
    return base::Status::AlreadyExists("live acceptor listeners are already open");
  }

  const auto* config = engine_->config();
  if (config->listeners.empty()) {
    return base::Status::InvalidArgument("engine config did not provide any listeners");
  }

  std::vector<const ListenerConfig*> selected;
  if (!listener_name.empty()) {
    const auto* listener = engine_->FindListenerConfig(listener_name);
    if (listener == nullptr) {
      return base::Status::NotFound("listener was not found in engine config");
    }
    selected.push_back(listener);
  } else {
    for (const auto& listener : config->listeners) {
      selected.push_back(&listener);
    }
  }

  listeners_.clear();
  listeners_.reserve(selected.size());
  auto reset_status = ResetWorkerShards(config->worker_count);
  if (!reset_status.ok()) {
    return reset_status;
  }
  for (const auto* listener : selected) {
    auto acceptor = transport::TcpAcceptor::Listen(listener->host, listener->port, 16);
    if (!acceptor.ok()) {
      Stop();
      return acceptor.status();
    }

    listeners_.push_back(ListenerState{
      .name = listener->name,
      .worker_hint = listener->worker_hint,
      .acceptor = std::move(acceptor).value(),
    });
    RecordTrace(TraceEventKind::kSessionEvent,
                0U,
                listener->worker_hint,
                NowNs(),
                listeners_.back().acceptor.port(),
                0U,
                "listener open");
  }

  opened_ = true;
  stop_requested_.store(false);
  completed_sessions_.store(0U);
  active_connection_count_.store(0U);
  for (auto& shard : worker_shards_) {
    shard.active_connections.store(0U);
    shard.connections.clear();
    shard.poller.ClearTimers();
    shard.connection_indices.clear();
    shard.session_connection_indices.clear();
    if (shard.inbox != nullptr) {
      std::lock_guard lock(shard.inbox->mutex);
      shard.inbox->pending_connections.clear();
    }
  }
  {
    std::lock_guard lock(control_mutex_);
    active_session_ids_.clear();
    terminal_status_.reset();
    session_snapshots_.clear();
    session_subscribers_.clear();
  }
  last_progress_ns_.store(NowNs());
  return base::Status::Ok();
}

auto
LiveAcceptor::Run(std::size_t max_completed_sessions, std::chrono::milliseconds idle_timeout) -> base::Status
{
  if (!opened_) {
    auto status = OpenListeners();
    if (!status.ok()) {
      return status;
    }
  }

  {
    std::lock_guard lock(run_state_mutex_);
    run_active_ = true;
    run_thread_id_ = std::this_thread::get_id();
  }
  struct RunStateGuard
  {
    LiveAcceptor* owner;

    ~RunStateGuard()
    {
      {
        std::lock_guard lock(owner->run_state_mutex_);
        owner->run_active_ = false;
        owner->run_thread_id_ = std::thread::id{};
      }
      owner->run_state_cv_.notify_all();
    }
  } run_state_guard{ this };

  stop_requested_.store(false);

  auto status = EnsureManagedQueueRunnerStarted();
  if (!status.ok()) {
    return status;
  }

  const auto idle_timeout_ns =
    static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(idle_timeout).count());

  auto run_status = base::Status::Ok();
  if (worker_shards_.size() <= 1U) {
    status = ApplyAcceptorFrontDoorThreadSetup(engine_, true);
    if (!status.ok()) {
      static_cast<void>(StopManagedQueueRunner());
      return status;
    }
    while (!stop_requested_.load()) {
      const auto terminal_status = LoadTerminalStatus();
      if (terminal_status.has_value()) {
        run_status = *terminal_status;
        break;
      }
      if (max_completed_sessions != 0U && completed_sessions_.load() >= max_completed_sessions) {
        break;
      }

      const auto now = NowNs();
      const auto last_progress_ns = last_progress_ns_.load();
      if (idle_timeout_ns != 0U && now > last_progress_ns + idle_timeout_ns) {
        run_status = base::Status::IoError("live acceptor timed out while waiting for session progress");
        break;
      }

      status = PollOnce(options_.poll_timeout);
      if (!status.ok()) {
        run_status = status;
        break;
      }
    }
  } else {
    status = StartWorkerThreads();
    if (!status.ok()) {
      static_cast<void>(StopManagedQueueRunner());
      return status;
    }

    status = ApplyAcceptorFrontDoorThreadSetup(engine_, false);
    if (!status.ok()) {
      StopWorkerThreads();
      static_cast<void>(StopManagedQueueRunner());
      return status;
    }

    const int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
      run_status = base::Status::IoError(std::string("epoll_create1 failed: ") + std::strerror(errno));
    } else {
      for (std::size_t index = 0; index < listeners_.size(); ++index) {
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.u64 = static_cast<std::uint64_t>(index);
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, listeners_[index].acceptor.fd(), &ev) != 0) {
          run_status = base::Status::IoError(std::string("epoll_ctl ADD failed: ") + std::strerror(errno));
          break;
        }
      }

      std::vector<epoll_event> epoll_events(listeners_.size());
      while (run_status.ok() && !stop_requested_.load()) {
        const auto terminal_status = LoadTerminalStatus();
        if (terminal_status.has_value()) {
          run_status = *terminal_status;
          break;
        }
        if (max_completed_sessions != 0U && completed_sessions_.load() >= max_completed_sessions) {
          break;
        }

        const auto now = NowNs();
        const auto last_progress_ns = last_progress_ns_.load();
        if (idle_timeout_ns != 0U && now > last_progress_ns + idle_timeout_ns) {
          run_status = base::Status::IoError("live acceptor timed out while waiting for session progress");
          break;
        }

        const int timeout_ms = options_.poll_timeout.count() < 0 ? -1 : static_cast<int>(options_.poll_timeout.count());
        const int n = epoll_wait(epfd, epoll_events.data(), static_cast<int>(epoll_events.size()), timeout_ms);
        if (n < 0) {
          if (errno == EINTR) {
            continue;
          }
          run_status = base::Status::IoError(std::string("epoll_wait failed: ") + std::strerror(errno));
          break;
        }

        const auto timestamp_ns = NowNs();
        for (int i = 0; i < n; ++i) {
          const auto index = static_cast<std::size_t>(epoll_events[i].data.u64);
          status = AcceptReadyListener(index, timestamp_ns);
          if (!status.ok()) {
            run_status = status;
            break;
          }
        }

        if (run_status.ok()) {
          const auto terminal_status = LoadTerminalStatus();
          if (terminal_status.has_value()) {
            run_status = *terminal_status;
          }
        }
      }
      ::close(epfd);
    }
    stop_requested_.store(true);
    StopWorkerThreads();
  }

  status = StopManagedQueueRunner();
  if (!run_status.ok()) {
    return run_status;
  }
  return status;
}

auto
LiveAcceptor::Stop() -> void
{
  stop_requested_.store(true);

  {
    std::unique_lock lock(run_state_mutex_);
    if (run_active_ && run_thread_id_ == std::this_thread::get_id()) {
      return;
    }
  }

  for (const auto& shard : worker_shards_) {
    SignalWorkerWakeup(shard.worker_id);
  }

  {
    std::unique_lock lock(run_state_mutex_);
    if (run_active_) {
      run_state_cv_.wait(lock, [this]() { return !run_active_; });
    }
  }

  StopWorkerThreads();
  static_cast<void>(StopManagedQueueRunner());
  for (auto& shard : worker_shards_) {
    for (auto& connection : shard.connections) {
      connection.connection.Close();
    }
    shard.connections.clear();
    shard.poller.ClearTimers();
    shard.poller.CloseWakeup();
    shard.connection_indices.clear();
    shard.session_connection_indices.clear();
    shard.command_sink.reset();
    shard.inbox.reset();
  }
  active_connection_count_.store(0U);
  worker_shards_.clear();
  {
    std::lock_guard lock(control_mutex_);
    active_session_ids_.clear();
    terminal_status_.reset();
    session_snapshots_.clear();
    session_subscribers_.clear();
  }
  for (auto& listener : listeners_) {
    listener.acceptor.Close();
  }
  listeners_.clear();
  opened_ = false;
}

auto
LiveAcceptor::listener_port(std::string_view name) const -> base::Result<std::uint16_t>
{
  for (const auto& listener : listeners_) {
    if (listener.name == name) {
      return listener.acceptor.port();
    }
  }
  return base::Status::NotFound("listener is not open");
}

auto
LiveAcceptor::PollOnce(std::chrono::milliseconds timeout) -> base::Status
{
  if (listeners_.empty()) {
    return base::Status::InvalidArgument("live acceptor has no open listeners");
  }

  const auto poll_started_ns = NowNs();
  const auto effective_timeout = ComputePollTimeout(timeout, poll_started_ns);

  // In single-worker mode we combine listener fds and connection fds into the
  // shard's IoPoller so that a single epoll_wait sees both incoming connections
  // AND data on existing connections (just like the old poll() call did).
  auto& shard = worker_shards_.front();

  // Temporarily register listener fds on the shard poller using sentinel tags.
  // Tag = kListenerTagBase + index to distinguish from connection fd tags.
  static constexpr std::size_t kListenerTagBase = ~std::size_t{ 0 } - 0x10000;
  for (std::size_t i = 0; i < listeners_.size(); ++i) {
    const int fd = listeners_[i].acceptor.fd();
    if (fd >= 0) {
      auto status = shard.poller.io_poller()->AddFd(fd, kListenerTagBase + i);
      if (!status.ok()) {
        // Cleanup already-added listener fds
        for (std::size_t j = 0; j < i; ++j) {
          shard.poller.io_poller()->RemoveFd(listeners_[j].acceptor.fd());
        }
        return status;
      }
    }
  }

  // Combined SyncAndWait: polls both connections and listener fds.
  auto status = shard.poller.SyncAndWait(
    shard.connections.size(),
    [&](std::size_t index) { return shard.connections[index].connection.fd(); },
    effective_timeout,
    shard.io_ready_state);

  // Remove listener fds from the shard poller (they are managed separately).
  for (std::size_t i = 0; i < listeners_.size(); ++i) {
    shard.poller.io_poller()->RemoveFd(listeners_[i].acceptor.fd());
  }

  if (!status.ok()) {
    return status;
  }

  const auto now = NowNs();

  // Check if any listener tags became ready (they appear in io_poller's
  // ready list via ReadyTag, but SyncAndWait maps tags to connection indices
  // so listener events won't appear in ready_indices — we need to check the
  // poller's raw result).  Since SyncAndWait filters by fd_to_index_ and
  // listener fds are NOT in fd_to_index_, their events are silently dropped.
  //
  // Workaround: after the combined wait, do a non-blocking accept attempt on
  // all listeners.  accept4(SOCK_NONBLOCK) returns EAGAIN if no connection.
  for (std::size_t i = 0; i < listeners_.size(); ++i) {
    auto accept_status = AcceptReadyListener(i, now);
    if (!accept_status.ok())
      return accept_status;
  }

  // Process ready connections.
  for (auto it = shard.io_ready_state.ready_indices.rbegin(); it != shard.io_ready_state.ready_indices.rend(); ++it) {
    const auto connection_index = *it;
    if (connection_index < shard.connections.size()) {
      auto conn_status = ProcessConnection(shard, connection_index, true, now);
      if (!conn_status.ok())
        return conn_status;
    }
  }

  {
    auto app_status = PollManagedApplicationWorker(shard.worker_id);
    if (!app_status.ok())
      return app_status;
  }
  {
    auto retry_status = RetryPendingAppEvents(shard, now);
    if (!retry_status.ok())
      return retry_status;
  }
  {
    auto drain_status = DrainWorkerCommands(shard.worker_id, now);
    if (!drain_status.ok())
      return drain_status;
  }

  const auto timers_now = NowNs();
  {
    auto timer_status = ProcessDueTimers(shard, timers_now);
    if (!timer_status.ok())
      return timer_status;
  }
  {
    auto app_status = PollManagedApplicationWorker(shard.worker_id);
    if (!app_status.ok())
      return app_status;
  }
  {
    auto retry_status = RetryPendingAppEvents(shard, timers_now);
    if (!retry_status.ok())
      return retry_status;
  }

  const auto final_now = NowNs();
  {
    auto drain_status = DrainWorkerCommands(shard.worker_id, final_now);
    if (!drain_status.ok())
      return drain_status;
  }

  return base::Status::Ok();
}

auto
LiveAcceptor::PollWorkerOnce(WorkerShardState& shard, std::chrono::milliseconds timeout) -> base::Status
{
  WorkerMetrics* wm = nullptr;
  {
    const auto* cfg = engine_->config();
    if (cfg != nullptr && cfg->enable_metrics) {
      wm = engine_->mutable_metrics()->FindWorker(shard.worker_id);
    }
  }

  const auto poll_started_ns = NowNs();
  const auto effective_timeout = ComputePollTimeout(shard, timeout, poll_started_ns);

  const auto t_poll_start = NowNs();

  {
    auto status = shard.poller.SyncAndWait(
      shard.connections.size(),
      [&](std::size_t index) { return shard.connections[index].connection.fd(); },
      effective_timeout,
      shard.io_ready_state);
    if (!status.ok())
      return status;
  }

  const auto t_poll_end = NowNs();
  const auto now = NowNs();

  base::Status status;
  for (auto it = shard.io_ready_state.ready_indices.rbegin(); it != shard.io_ready_state.ready_indices.rend(); ++it) {
    const auto connection_index = *it;
    if (connection_index < shard.connections.size()) {
      status = ProcessConnection(shard, connection_index, true, now);
      if (!status.ok())
        return status;
    }
  }
  const auto t_recv_done = NowNs();

  status = PollManagedApplicationWorker(shard.worker_id);
  if (!status.ok()) {
    return status;
  }
  status = RetryPendingAppEvents(shard, now);
  if (!status.ok()) {
    return status;
  }
  const auto t_app_done = NowNs();

  status = DrainWorkerCommands(shard.worker_id, now);
  if (!status.ok()) {
    return status;
  }
  const auto t_send_done = NowNs();

  const auto timers_now = t_send_done;
  status = ProcessDueTimers(shard, timers_now);
  if (!status.ok()) {
    return status;
  }
  const auto t_timer_done = NowNs();

  status = PollManagedApplicationWorker(shard.worker_id);
  if (!status.ok()) {
    return status;
  }
  status = RetryPendingAppEvents(shard, timers_now);
  if (!status.ok()) {
    return status;
  }
  const auto t_app2_done = NowNs();

  const auto final_now = t_app2_done;
  status = DrainWorkerCommands(shard.worker_id, final_now);
  if (!status.ok()) {
    return status;
  }
  const auto t_send2_done = NowNs();

  if (wm != nullptr) {
    wm->poll_wait_ns.fetch_add(t_poll_end - t_poll_start, std::memory_order_relaxed);
    wm->recv_dispatch_ns.fetch_add(t_recv_done - t_poll_end, std::memory_order_relaxed);
    wm->app_callback_ns.fetch_add((t_app_done - t_recv_done) + (t_app2_done - t_timer_done), std::memory_order_relaxed);
    wm->send_ns.fetch_add((t_send_done - t_app_done) + (t_send2_done - t_app2_done), std::memory_order_relaxed);
    wm->timer_process_ns.fetch_add(t_timer_done - t_send_done, std::memory_order_relaxed);
    wm->poll_iterations.fetch_add(1, std::memory_order_relaxed);
  }

  AdoptPendingConnections(shard);
  return base::Status::Ok();
}

auto
LiveAcceptor::ComputePollTimeout(std::chrono::milliseconds timeout, std::uint64_t timestamp_ns) const
  -> std::chrono::milliseconds
{
  if (engine_ != nullptr && engine_->config() != nullptr && engine_->config()->poll_mode == PollMode::kBusy) {
    return std::chrono::milliseconds(0);
  }
  std::optional<std::uint64_t> deadline;
  for (const auto& shard : worker_shards_) {
    const auto shard_deadline = shard.poller.NextDeadline();
    if (!shard_deadline.has_value()) {
      continue;
    }
    if (!deadline.has_value() || *shard_deadline < *deadline) {
      deadline = shard_deadline;
    }
  }
  if (!deadline.has_value()) {
    return timeout;
  }
  if (*deadline <= timestamp_ns) {
    return std::chrono::milliseconds(0);
  }

  const auto timer_timeout =
    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::nanoseconds(*deadline - timestamp_ns));
  if (timeout.count() < 0) {
    return timer_timeout;
  }
  return std::min(timeout, timer_timeout);
}

auto
LiveAcceptor::ComputePollTimeout(const WorkerShardState& shard,
                                 std::chrono::milliseconds timeout,
                                 std::uint64_t timestamp_ns) const -> std::chrono::milliseconds
{
  if (engine_ != nullptr && engine_->config() != nullptr && engine_->config()->poll_mode == PollMode::kBusy) {
    return std::chrono::milliseconds(0);
  }
  const auto deadline = shard.poller.NextDeadline();
  if (!deadline.has_value()) {
    return timeout;
  }
  if (*deadline <= timestamp_ns) {
    return std::chrono::milliseconds(0);
  }

  const auto timer_timeout =
    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::nanoseconds(*deadline - timestamp_ns));
  if (timeout.count() < 0) {
    return timer_timeout;
  }
  return std::min(timeout, timer_timeout);
}

auto
LiveAcceptor::AcceptReadyListener(std::size_t listener_index, std::uint64_t timestamp_ns) -> base::Status
{
  while (true) {
    auto accepted = listeners_[listener_index].acceptor.TryAccept();
    if (!accepted.ok()) {
      return accepted.status();
    }
    if (!accepted.value().has_value()) {
      return base::Status::Ok();
    }

    auto* shard = FindWorkerShard(SelectAcceptWorkerId(listeners_[listener_index].worker_hint));
    if (shard == nullptr) {
      return base::Status::NotFound("live acceptor worker shard was not found");
    }

    const auto connection_id = next_connection_id_++;
    ConnectionState connection{
      .connection_id = connection_id,
      .connection = std::move(*accepted.value()),
      .last_progress_ns = timestamp_ns,
    };
    if (worker_threads_.empty()) {
      shard->connections.push_back(std::move(connection));
      IndexConnection(*shard, shard->connections.size() - 1U);
      shard->active_connections.fetch_add(1U, std::memory_order_relaxed);
    } else {
      auto status = EnqueuePendingConnection(shard->worker_id, std::move(connection));
      if (!status.ok()) {
        return status;
      }
      shard->active_connections.fetch_add(1U, std::memory_order_relaxed);
    }
    active_connection_count_.fetch_add(1U);
    last_progress_ns_.store(timestamp_ns);
    RecordTrace(TraceEventKind::kSessionEvent,
                0U,
                shard->worker_id,
                timestamp_ns,
                connection_id,
                listeners_[listener_index].acceptor.port(),
                "tcp accept");
  }
}

auto
LiveAcceptor::ProcessConnection(WorkerShardState& shard,
                                std::size_t connection_index,
                                bool readable,
                                std::uint64_t timestamp_ns) -> base::Status
{
  auto* connection_shard = &shard;
  auto* connection = &connection_shard->connections[connection_index];

  if (connection->session != nullptr && connection->pending_app_event.has_value()) {
    auto status = RetryPendingAppEvent(*connection, timestamp_ns);
    if (!status.ok()) {
      MarkConnectionForClose(*connection, status.message(), false);
    }
    if (connection->pending_app_event.has_value()) {
      // Backpressure is still active.  We must NOT read application-level
      // frames because the previous app message hasn't been dispatched
      // yet and reading would either drop or double-dispatch it.
      // However, we still need to drain admin frames (heartbeats, test
      // requests) so that the session's last_inbound_ns stays current
      // and the heartbeat watchdog doesn't disconnect us.
      if (readable) {
        while (!connection->close_requested) {
          auto frame = connection->connection.TryReceiveFrameView();
          if (!frame.ok()) {
            MarkConnectionForClose(*connection, frame.status().message(), false);
            break;
          }
          if (!frame.value().has_value()) {
            break;
          }
          const auto frame_bytes = frame.value().value();
          auto header = codec::PeekSessionHeaderView(frame_bytes);
          if (!header.ok()) {
            MarkConnectionForClose(*connection, header.status().message(), false);
            break;
          }
          if (!IsAdminMessage(header.value().msg_type)) {
            // Application frame — cannot process while backpressured.
            // Stash the frame bytes so the next ProcessConnection
            // iteration can reprocess them once backpressure clears.
            connection->stashed_app_frame.assign(frame_bytes.begin(), frame_bytes.end());
            break;
          }
          // Admin frame — safe to process; keeps the session alive.
          connection->last_progress_ns = timestamp_ns;
          last_progress_ns_.store(timestamp_ns);
          const auto conn_id = connection->connection_id;
          status = HandleInboundFrame(*connection_shard,
                                      connection_shard->connection_indices[conn_id],
                                      frame_bytes,
                                      header.value(),
                                      timestamp_ns);
          if (!status.ok()) {
            MarkConnectionForClose(*connection, status.message(), false);
            break;
          }
        }
      }
      readable = false;
    }
  }

  if (readable) {
    // If a previous admin-drain loop stashed an application frame,
    // reprocess it now that backpressure has cleared.
    if (!connection->stashed_app_frame.empty()) {
      // Copy the stashed frame so the original vector can be safely moved
      // if HandleInboundFrame triggers connection migration.
      auto stashed_copy = std::move(connection->stashed_app_frame);
      connection->stashed_app_frame.clear();
      const auto frame_bytes = std::span<const std::byte>(stashed_copy.data(), stashed_copy.size());
      auto header = codec::PeekSessionHeaderView(frame_bytes);
      if (!header.ok()) {
        if (connection->session != nullptr) {
          RecordProtocolFailure(*connection->session, header.status());
        }
        MarkConnectionForClose(*connection, header.status().message(), false);
      } else {
        connection->last_progress_ns = timestamp_ns;
        last_progress_ns_.store(timestamp_ns);

        const auto connection_id = connection->connection_id;
        auto status = HandleInboundFrame(*connection_shard,
                                         connection_shard->connection_indices[connection_id],
                                         frame_bytes,
                                         header.value(),
                                         timestamp_ns);
        if (!status.ok()) {
          if (connection->session != nullptr) {
            RecordProtocolFailure(*connection->session, status);
          }
          MarkConnectionForClose(*connection, status.message(), false);
        } else {
          // Re-find connection after HandleInboundFrame (may have migrated).
          connection = nullptr;
          connection_shard = nullptr;
          for (auto& candidate_shard : worker_shards_) {
            auto* candidate = FindConnectionById(candidate_shard, connection_id);
            if (candidate != nullptr) {
              connection = candidate;
              connection_shard = &candidate_shard;
              break;
            }
          }
          if (connection == nullptr || connection_shard == nullptr) {
            if (!worker_threads_.empty()) {
              return base::Status::Ok();
            }
            return base::Status::NotFound("live acceptor connection was not found after stashed frame "
                                          "processing");
          }
        }
      }
    }

    while (!connection->close_requested && !connection->pending_app_event.has_value()) {
      auto frame = connection->connection.TryReceiveFrameView();
      if (!frame.ok()) {
        if (connection->session != nullptr) {
          RecordProtocolFailure(*connection->session, frame.status());
        }
        MarkConnectionForClose(*connection, frame.status().message(), false);
        break;
      }
      if (!frame.value().has_value()) {
        break;
      }

      const auto frame_bytes = frame.value().value();
      auto header = codec::PeekSessionHeaderView(frame_bytes);
      if (!header.ok()) {
        if (connection->session != nullptr) {
          RecordProtocolFailure(*connection->session, header.status());
        }
        MarkConnectionForClose(*connection, header.status().message(), false);
        break;
      }

      connection->last_progress_ns = timestamp_ns;
      last_progress_ns_.store(timestamp_ns);

      const auto connection_id = connection->connection_id;
      auto status = HandleInboundFrame(*connection_shard,
                                       connection_shard->connection_indices[connection_id],
                                       frame_bytes,
                                       header.value(),
                                       timestamp_ns);
      if (!status.ok()) {
        if (connection->session != nullptr) {
          RecordProtocolFailure(*connection->session, status);
        }
        MarkConnectionForClose(*connection, status.message(), false);
        break;
      }

      connection = nullptr;
      connection_shard = nullptr;
      for (auto& candidate_shard : worker_shards_) {
        auto* candidate = FindConnectionById(candidate_shard, connection_id);
        if (candidate != nullptr) {
          connection = candidate;
          connection_shard = &candidate_shard;
          break;
        }
      }
      if (connection == nullptr || connection_shard == nullptr) {
        if (!worker_threads_.empty()) {
          return base::Status::Ok();
        }
        return base::Status::NotFound("live acceptor connection was not found after inbound processing");
      }
      if (connection->pending_app_event.has_value()) {
        break;
      }
    }
  }

  if (connection->close_requested) {
    const auto it = connection_shard->connection_indices.find(connection->connection_id);
    if (it != connection_shard->connection_indices.end()) {
      CloseConnection(*connection_shard, it->second, timestamp_ns);
    }
  }

  return base::Status::Ok();
}

auto
LiveAcceptor::MigrateConnectionToRoutedWorker(WorkerShardState& shard, std::size_t connection_index) -> base::Status
{
  if (connection_index >= shard.connections.size()) {
    return base::Status::NotFound("acceptor connection was not found for worker migration");
  }

  auto& current = shard.connections[connection_index];
  if (current.session == nullptr || current.session->routed_worker_id == shard.worker_id) {
    return base::Status::Ok();
  }

  auto* target_shard = FindWorkerShard(current.session->routed_worker_id);
  if (target_shard == nullptr || target_shard->command_sink == nullptr) {
    return base::Status::NotFound("acceptor target worker shard was not found for worker migration");
  }

  auto migrated = std::move(current);
  const auto session_id = migrated.session->counterparty.session.session_id;
  const auto target_worker_id = migrated.session->routed_worker_id;
  migrated.session->worker_id = target_worker_id;
  migrated.session->handle = session::SessionHandle(session_id, target_worker_id, target_shard->command_sink);

  shard.poller.timer_wheel().Cancel(migrated.connection_id);
  shard.connection_indices.erase(migrated.connection_id);
  shard.session_connection_indices.erase(session_id);

  const auto last_index = shard.connections.size() - 1U;
  if (connection_index != last_index) {
    shard.connections[connection_index] = std::move(shard.connections.back());
    IndexConnection(shard, connection_index);
  }
  shard.connections.pop_back();
  shard.active_connections.fetch_sub(1U, std::memory_order_relaxed);
  target_shard->active_connections.fetch_add(1U, std::memory_order_relaxed);

  RecordTrace(TraceEventKind::kSessionEvent,
              session_id,
              target_worker_id,
              NowNs(),
              migrated.connection_id,
              shard.worker_id,
              "worker migrate");

  return EnqueuePendingConnection(target_worker_id, std::move(migrated));
}

auto
LiveAcceptor::ProcessDueTimers(WorkerShardState& shard, std::uint64_t timestamp_ns) -> base::Status
{
  std::vector<std::uint64_t> due_connection_ids;
  shard.poller.timer_wheel().PopExpired(timestamp_ns, &due_connection_ids);
  for (const auto connection_id : due_connection_ids) {
    auto* connection = FindConnectionById(shard, connection_id);
    if (connection == nullptr || connection->session == nullptr || connection->close_requested) {
      continue;
    }

    auto status = ServiceTimer(shard, *connection, timestamp_ns);
    if (!status.ok()) {
      MarkConnectionForClose(*connection, status.message(), false);
    }

    if (!connection->close_requested) {
      continue;
    }

    const auto it = shard.connection_indices.find(connection_id);
    if (it != shard.connection_indices.end()) {
      CloseConnection(shard, it->second, timestamp_ns);
    }
  }
  return base::Status::Ok();
}

auto
LiveAcceptor::RetryPendingAppEvent(ConnectionState& connection, std::uint64_t timestamp_ns) -> base::Status
{
  if (!connection.pending_app_event.has_value() || options_.application == nullptr || connection.session == nullptr) {
    return base::Status::Ok();
  }

  auto status = options_.application->OnAppMessage(*connection.pending_app_event);
  if (status.code() == base::ErrorCode::kBusy) {
    return base::Status::Ok();
  }
  if (!status.ok()) {
    return status;
  }

  if (connection.session->counterparty.dispatch_mode == AppDispatchMode::kInline) {
    status = DrainWorkerCommands(connection.session->worker_id, timestamp_ns);
    if (!status.ok()) {
      return status;
    }
  }

  connection.pending_app_event.reset();
  connection.last_progress_ns = timestamp_ns;
  last_progress_ns_.store(timestamp_ns);
  return base::Status::Ok();
}

auto
LiveAcceptor::RetryPendingAppEvents(WorkerShardState& shard, std::uint64_t timestamp_ns) -> base::Status
{
  for (auto& connection : shard.connections) {
    if (connection.close_requested || !connection.pending_app_event.has_value()) {
      continue;
    }
    auto status = RetryPendingAppEvent(connection, timestamp_ns);
    if (!status.ok()) {
      MarkConnectionForClose(connection, status.message(), false);
    }
  }
  return base::Status::Ok();
}

auto
LiveAcceptor::BindConnectionFromLogon(WorkerShardState& shard,
                                      std::size_t connection_index,
                                      const codec::SessionHeaderView& header,
                                      std::uint64_t timestamp_ns) -> base::Result<ConnectionState*>
{
  auto* connection = &shard.connections[connection_index];
  if (header.msg_type != "A") {
    return base::Status::InvalidArgument("acceptor requires Logon as the first inbound frame");
  }

  session::SessionKey pending_key{
    std::string(header.begin_string),
    std::string(header.target_comp_id),
    std::string(header.sender_comp_id),
  };

  auto* runtime = engine_->mutable_runtime();
  bool pending_registered = false;
  if (runtime != nullptr) {
    auto pending_status = runtime->RegisterPendingConnection(PendingConnection{
      .connection_id = connection->connection_id,
      .session_key = pending_key,
      .profile_id = 0U,
    });
    if (!pending_status.ok()) {
      return pending_status;
    }
    pending_registered = true;
    RecordTrace(TraceEventKind::kPendingConnectionRegistered,
                0U,
                runtime->RouteSession(pending_key),
                timestamp_ns,
                connection->connection_id,
                0U,
                SessionKeyText(pending_key));
  }

  auto resolved = engine_->ResolveInboundSession(header);
  if (pending_registered) {
    auto unregister_status = runtime->UnregisterPendingConnection(connection->connection_id);
    if (!unregister_status.ok()) {
      return unregister_status;
    }
  }
  if (!resolved.ok()) {
    return resolved.status();
  }

  if (!IsWithinLogonWindow(resolved.value().counterparty.session_schedule, WallClockNowNs())) {
    return base::Status::InvalidArgument("inbound Logon arrived outside the configured logon window");
  }

  if (HasActiveSession(resolved.value().counterparty.session.session_id)) {
    return base::Status::AlreadyExists("session already has an active transport connection");
  }

  auto matched = std::move(resolved).value();
  auto active = MakeActiveSession(matched.counterparty, std::move(matched.dictionary));
  if (!active.ok()) {
    return active.status();
  }

  active.value().routed_worker_id = ResolveWorkerId(active.value().worker_id);

  auto* target_shard = FindWorkerShard(active.value().routed_worker_id);
  if (target_shard == nullptr) {
    return base::Status::NotFound("acceptor worker shard was not found for bound session");
  }

  const bool threaded_handoff = !worker_threads_.empty() && target_shard != &shard;
  active.value().worker_id = threaded_handoff ? shard.worker_id : active.value().routed_worker_id;

  std::shared_ptr<session::SessionCommandSink> command_sink;
  auto* bound_command_shard = threaded_handoff ? &shard : target_shard;
  if (bound_command_shard->command_sink != nullptr) {
    command_sink = bound_command_shard->command_sink;
  }
  active.value().handle = session::SessionHandle(
    active.value().counterparty.session.session_id, active.value().worker_id, std::move(command_sink));

  auto start = active.value().protocol->OnTransportConnected(timestamp_ns);
  if (!start.ok()) {
    return start.status();
  }

  if (!RegisterActiveSession(active.value().counterparty.session.session_id)) {
    return base::Status::AlreadyExists("session already has an active transport connection");
  }
  connection->session = std::make_unique<ActiveSession>(std::move(active).value());

  ConnectionState* bound_connection = connection;
  if (target_shard != &shard && worker_threads_.empty()) {
    bound_connection = ReassignConnectionToWorker(shard, connection_index, target_shard->worker_id);
    if (bound_connection == nullptr) {
      UnregisterActiveSession(matched.counterparty.session.session_id);
      return base::Status::IoError("failed to migrate bound acceptor connection to worker shard");
    }
  } else {
    IndexConnection(shard, connection_index);
  }

  RecordTrace(TraceEventKind::kSessionEvent,
              bound_connection->session->counterparty.session.session_id,
              bound_connection->session->worker_id,
              timestamp_ns,
              bound_connection->connection_id,
              bound_connection->session->counterparty.session.profile_id,
              "logon matched");

  auto status = SendFrames(*bound_connection, start.value().outbound_frames, timestamp_ns);
  if (!status.ok()) {
    auto* bound_shard = FindWorkerShard(bound_connection->session->worker_id);
    if (bound_shard != nullptr) {
      bound_shard->session_connection_indices.erase(bound_connection->session->counterparty.session.session_id);
    }
    UnregisterActiveSession(bound_connection->session->counterparty.session.session_id);
    bound_connection->session.reset();
    return status;
  }

  UpdateSessionSnapshot(*bound_connection->session);

  status =
    DispatchSessionEvent(*bound_connection->session, SessionEventKind::kBound, timestamp_ns, "logon matched", true);
  if (!status.ok()) {
    auto* bound_shard = FindWorkerShard(bound_connection->session->worker_id);
    if (bound_shard != nullptr) {
      bound_shard->session_connection_indices.erase(bound_connection->session->counterparty.session.session_id);
    }
    UnregisterActiveSession(bound_connection->session->counterparty.session.session_id);
    bound_connection->session.reset();
    return status;
  }

  return bound_connection;
}

auto
LiveAcceptor::HandleInboundFrame(WorkerShardState& shard,
                                 std::size_t connection_index,
                                 std::span<const std::byte> frame,
                                 const codec::SessionHeaderView& header,
                                 std::uint64_t timestamp_ns) -> base::Status
{
  auto* connection = &shard.connections[connection_index];
  if (connection->session == nullptr) {
    auto bound = BindConnectionFromLogon(shard, connection_index, header, timestamp_ns);
    if (!bound.ok()) {
      return bound.status();
    }
    connection = bound.value();
  }

  auto* connection_shard = FindWorkerShard(connection->session->worker_id);
  if (connection_shard == nullptr) {
    return base::Status::NotFound("acceptor worker shard was not found for inbound session");
  }

  std::optional<codec::DecodedMessageView> decoded_message;
  std::optional<message::MessageView> admin_message;
  session::ProtocolEvent event;
  if (options_.application != nullptr) {
    auto decoded = codec::DecodeFixMessageView(frame, *connection->session->dictionary);
    if (!decoded.ok()) {
      return decoded.status();
    }
    decoded_message = std::move(decoded).value();
    auto protocol_event = connection->session->protocol->OnInbound(*decoded_message, timestamp_ns);
    if (!protocol_event.ok()) {
      return protocol_event.status();
    }
    if (IsAdminMessage(header.msg_type)) {
      admin_message = decoded_message->message.view();
    }
    event = std::move(protocol_event).value();
  } else {
    auto decoded = codec::DecodeFixMessageView(frame, *connection->session->dictionary);
    if (!decoded.ok()) {
      return decoded.status();
    }
    auto protocol_event = connection->session->protocol->OnInbound(decoded.value(), timestamp_ns);
    if (!protocol_event.ok()) {
      return protocol_event.status();
    }
    event = std::move(protocol_event).value();
  }

  RecordInboundMetrics(*connection->session, header.msg_type);

  if (admin_message.has_value()) {
    auto status = DispatchAdminMessage(*connection->session, *admin_message, timestamp_ns);
    if (!status.ok()) {
      return status;
    }
  }

  if (event.session_active) {
    auto status =
      DispatchSessionEvent(*connection->session, SessionEventKind::kActive, timestamp_ns, "session active", true);
    if (!status.ok()) {
      return status;
    }
  }

  auto status = HandleProtocolEvent(*connection_shard, *connection, event, timestamp_ns);
  if (!status.ok()) {
    return status;
  }

  if (!worker_threads_.empty() && connection->session != nullptr &&
      connection->session->routed_worker_id != connection_shard->worker_id && !connection->close_requested &&
      !connection->pending_app_event.has_value()) {
    const auto migrated_index = connection_shard->connection_indices.find(connection->connection_id);
    if (migrated_index != connection_shard->connection_indices.end()) {
      return MigrateConnectionToRoutedWorker(*connection_shard, migrated_index->second);
    }
  }

  return base::Status::Ok();
}

auto
LiveAcceptor::HandleProtocolEvent(WorkerShardState& shard,
                                  ConnectionState& connection,
                                  const session::ProtocolEvent& event,
                                  std::uint64_t timestamp_ns) -> base::Status
{
  auto status = SendFramesBatch(connection, event.outbound_frames, timestamp_ns);
  if (!status.ok()) {
    return status;
  }

  for (const auto& application_message : event.application_messages) {
    if (options_.application == nullptr) {
      auto outbound = connection.session->protocol->SendApplication(application_message, timestamp_ns);
      if (!outbound.ok()) {
        return outbound.status();
      }

      status = SendFrame(connection, outbound.value(), timestamp_ns);
    } else {
      status = DispatchAppMessage(connection, application_message.view(), timestamp_ns);
    }
    if (!status.ok()) {
      return status;
    }
    if (connection.pending_app_event.has_value()) {
      break;
    }
  }

  if (event.disconnect) {
    MarkConnectionForClose(connection, "session disconnect", true);
  }

  UpdateSessionSnapshot(*connection.session);
  RefreshConnectionTimer(shard, connection, timestamp_ns);

  return base::Status::Ok();
}

auto
LiveAcceptor::DispatchSessionEvent(const ActiveSession& session,
                                   SessionEventKind kind,
                                   std::uint64_t timestamp_ns,
                                   std::string text,
                                   bool drain_inline) -> base::Status
{
  if (options_.application == nullptr) {
    return base::Status::Ok();
  }

  const auto notification_text = text;
  auto event = RuntimeEvent{
    .kind = RuntimeEventKind::kSession,
    .session_event = kind,
    .handle = session.handle,
    .session_key = session.counterparty.session.key,
    .message = {},
    .text = std::move(text),
    .timestamp_ns = timestamp_ns,
  };
  base::Status status;
  auto* worker_shard = FindWorkerShard(session.worker_id);
  if (session.counterparty.dispatch_mode == AppDispatchMode::kInline && worker_shard != nullptr &&
      worker_shard->command_sink != nullptr) {
    BorrowedSendScope scope(worker_shard->command_sink.get());
    status = options_.application->OnSessionEvent(event);
  } else {
    status = options_.application->OnSessionEvent(event);
  }
  if (HasSessionSubscribers(session.counterparty.session.session_id)) {
    PublishNotification(session::SessionNotification{
      .kind = kind == SessionEventKind::kBound
                ? session::SessionNotificationKind::kSessionBound
                : (kind == SessionEventKind::kActive ? session::SessionNotificationKind::kSessionActive
                                                     : session::SessionNotificationKind::kSessionClosed),
      .snapshot = session.protocol->session().Snapshot(),
      .session_key = session.counterparty.session.key,
      .message = {},
      .text = notification_text,
      .timestamp_ns = timestamp_ns,
    });
  } else {
    UpdateSessionSnapshot(session);
  }
  if (!status.ok() || !drain_inline || session.counterparty.dispatch_mode != AppDispatchMode::kInline) {
    return status;
  }
  return DrainWorkerCommands(session.worker_id, timestamp_ns);
}

auto
LiveAcceptor::DispatchAdminMessage(const ActiveSession& session,
                                   message::MessageView message,
                                   std::uint64_t timestamp_ns) -> base::Status
{
  if (options_.application == nullptr) {
    return base::Status::Ok();
  }

  const bool inline_mode = session.counterparty.dispatch_mode == AppDispatchMode::kInline;
  message::MessageRef owned_message;
  if (!inline_mode) {
    owned_message = message::MessageRef::Own(message);
  }

  auto event = RuntimeEvent{
    .kind = RuntimeEventKind::kAdminMessage,
    .session_event = SessionEventKind::kBound,
    .handle = session.handle,
    .session_key = session.counterparty.session.key,
    .message = inline_mode ? message::MessageRef(message) : owned_message,
    .text = {},
    .timestamp_ns = timestamp_ns,
  };
  base::Status status;
  auto* worker_shard = FindWorkerShard(session.worker_id);
  if (inline_mode && worker_shard != nullptr && worker_shard->command_sink != nullptr) {
    BorrowedSendScope scope(worker_shard->command_sink.get());
    status = options_.application->OnAdminMessage(event);
  } else {
    status = options_.application->OnAdminMessage(event);
  }
  if (HasSessionSubscribers(session.counterparty.session.session_id)) {
    if (!owned_message.valid()) {
      owned_message = message::MessageRef::Own(message);
    }
    PublishNotification(session::SessionNotification{
      .kind = session::SessionNotificationKind::kAdminMessage,
      .snapshot = session.protocol->session().Snapshot(),
      .session_key = session.counterparty.session.key,
      .message = owned_message,
      .text = {},
      .timestamp_ns = timestamp_ns,
    });
  }
  if (!status.ok() || !inline_mode) {
    return status;
  }
  return DrainWorkerCommands(session.worker_id, timestamp_ns);
}

auto
LiveAcceptor::DispatchAppMessage(ConnectionState& connection, message::MessageView message, std::uint64_t timestamp_ns)
  -> base::Status
{
  const bool inline_mode = connection.session->counterparty.dispatch_mode == AppDispatchMode::kInline;
  message::MessageRef owned_message;
  if (!inline_mode) {
    owned_message = message::MessageRef::Own(message);
  }
  auto event = RuntimeEvent{
    .kind = RuntimeEventKind::kApplicationMessage,
    .session_event = SessionEventKind::kBound,
    .handle = connection.session->handle,
    .session_key = connection.session->counterparty.session.key,
    .message = inline_mode ? message::MessageRef(message) : owned_message,
    .text = {},
    .timestamp_ns = timestamp_ns,
    .poss_resend = message.get_boolean(codec::tags::kPossResend).value_or(false),
  };
  base::Status status;
  auto* worker_shard = FindWorkerShard(connection.session->worker_id);
  if (inline_mode && worker_shard != nullptr && worker_shard->command_sink != nullptr) {
    BorrowedSendScope scope(worker_shard->command_sink.get());
    status = options_.application->OnAppMessage(event);
  } else {
    status = options_.application->OnAppMessage(event);
  }
  if (HasSessionSubscribers(connection.session->counterparty.session.session_id)) {
    if (!owned_message.valid()) {
      owned_message = message::MessageRef::Own(message);
    }
    PublishNotification(session::SessionNotification{
      .kind = session::SessionNotificationKind::kApplicationMessage,
      .snapshot = connection.session->protocol->session().Snapshot(),
      .session_key = connection.session->counterparty.session.key,
      .message = owned_message,
      .text = {},
      .timestamp_ns = timestamp_ns,
    });
  }
  if (status.code() == base::ErrorCode::kBusy &&
      connection.session->counterparty.dispatch_mode == AppDispatchMode::kQueueDecoupled) {
    connection.pending_app_event = std::move(event);
    return base::Status::Ok();
  }
  if (!status.ok() || !inline_mode) {
    return status;
  }
  return DrainWorkerCommands(connection.session->worker_id, timestamp_ns);
}

auto
LiveAcceptor::ServiceTimer(WorkerShardState& shard, ConnectionState& connection, std::uint64_t timestamp_ns)
  -> base::Status
{
  const auto wall_now_ns = WallClockNowNs();
  if (!IsWithinSessionWindow(connection.session->counterparty.session_schedule, wall_now_ns)) {
    const auto state = connection.session->protocol->session().state();
    if (state == session::SessionState::kActive || state == session::SessionState::kResendProcessing) {
      auto logout = connection.session->protocol->BeginLogout("session window closed", timestamp_ns);
      if (!logout.ok()) {
        return logout.status();
      }
      auto status = SendFrame(connection, logout.value(), timestamp_ns);
      if (!status.ok()) {
        return status;
      }
      UpdateSessionSnapshot(*connection.session);
      RefreshConnectionTimer(shard, connection, timestamp_ns);
      return base::Status::Ok();
    }

    MarkConnectionForClose(connection, "session window closed", false);
    return base::Status::Ok();
  }

  auto event = connection.session->protocol->OnTimer(timestamp_ns);
  if (!event.ok()) {
    return event.status();
  }
  return HandleProtocolEvent(shard, connection, event.value(), timestamp_ns);
}

auto
LiveAcceptor::SendFrame(ConnectionState& connection, const session::EncodedFrame& frame, std::uint64_t timestamp_ns)
  -> base::Status
{
  auto status = connection.connection.Send(frame.bytes, options_.io_timeout);
  if (!status.ok()) {
    return status;
  }

  connection.last_progress_ns = timestamp_ns;
  last_progress_ns_.store(timestamp_ns);
  RecordOutboundMetrics(*connection.session, frame);
  return base::Status::Ok();
}

auto
LiveAcceptor::SendFrames(ConnectionState& connection,
                         const session::ProtocolFrameList& frames,
                         std::uint64_t timestamp_ns) -> base::Status
{
  for (const auto& frame : frames) {
    auto status = SendFrame(connection, frame, timestamp_ns);
    if (!status.ok()) {
      return status;
    }
  }
  return base::Status::Ok();
}

auto
LiveAcceptor::SendFramesBatch(ConnectionState& connection,
                              const session::ProtocolFrameList& frames,
                              std::uint64_t timestamp_ns) -> base::Status
{
  if (frames.size() <= 1U) {
    if (frames.empty() || frames[0].bytes.external_body.empty()) {
      return SendFrames(connection, frames, timestamp_ns);
    }
  }

  std::vector<std::span<const std::byte>> segments;
  segments.reserve(frames.size() * 3U);
  for (const auto& frame : frames) {
    auto full = frame.bytes.view();
    if (full.empty()) {
      continue;
    }
    if (!frame.bytes.external_body.empty()) {
      const auto splice = frame.bytes.body_splice_offset;
      segments.push_back(full.subspan(0, splice));
      segments.push_back(frame.bytes.external_body);
      if (splice < full.size()) {
        segments.push_back(full.subspan(splice));
      }
    } else {
      segments.push_back(full);
    }
  }

  bool has_external = false;
  for (const auto& frame : frames) {
    if (!frame.bytes.external_body.empty()) {
      has_external = true;
      break;
    }
  }

  auto status = has_external ? connection.connection.SendZeroCopyGather(segments, options_.io_timeout)
                             : connection.connection.SendGather(segments, options_.io_timeout);
  if (!status.ok()) {
    return status;
  }

  for (const auto& frame : frames) {
    RecordOutboundMetrics(*connection.session, frame);
  }
  connection.last_progress_ns = timestamp_ns;
  last_progress_ns_.store(timestamp_ns);
  return base::Status::Ok();
}

auto
LiveAcceptor::LoadSessionSnapshot(std::uint64_t session_id) const -> base::Result<session::SessionSnapshot>
{
  std::lock_guard lock(control_mutex_);
  const auto it = session_snapshots_.find(session_id);
  if (it == session_snapshots_.end()) {
    return base::Status::NotFound("session snapshot was not found");
  }
  return it->second;
}

auto
LiveAcceptor::RegisterSessionSubscriber(std::uint64_t session_id, std::size_t queue_capacity)
  -> base::Result<session::SessionSubscription>
{
  auto stream = std::make_shared<SubscriberStream>(queue_capacity);

  std::lock_guard lock(control_mutex_);
  if (!session_snapshots_.contains(session_id)) {
    return base::Status::NotFound("session subscription target was not found");
  }
  session_subscribers_[session_id].push_back(stream);
  return session::SessionSubscription(std::move(stream));
}

auto
LiveAcceptor::HasSessionSubscribers(std::uint64_t session_id) -> bool
{
  std::lock_guard lock(control_mutex_);
  const auto it = session_subscribers_.find(session_id);
  if (it == session_subscribers_.end()) {
    return false;
  }

  auto& weak_subscribers = it->second;
  for (auto weak_it = weak_subscribers.begin(); weak_it != weak_subscribers.end();) {
    if (weak_it->expired()) {
      weak_it = weak_subscribers.erase(weak_it);
      continue;
    }
    return true;
  }

  session_subscribers_.erase(it);
  return false;
}

auto
LiveAcceptor::UpdateSessionSnapshot(const ActiveSession& session) -> void
{
  std::lock_guard lock(control_mutex_);
  session_snapshots_[session.counterparty.session.session_id] = session.protocol->session().Snapshot();
}

auto
LiveAcceptor::PublishNotification(const session::SessionNotification& notification) -> void
{
  std::vector<std::shared_ptr<session::SessionSubscriptionStream>> subscribers;
  {
    std::lock_guard lock(control_mutex_);
    session_snapshots_[notification.snapshot.session_id] = notification.snapshot;

    auto it = session_subscribers_.find(notification.snapshot.session_id);
    if (it == session_subscribers_.end()) {
      return;
    }

    auto& weak_subscribers = it->second;
    for (auto weak_it = weak_subscribers.begin(); weak_it != weak_subscribers.end();) {
      auto subscriber = weak_it->lock();
      if (subscriber == nullptr) {
        weak_it = weak_subscribers.erase(weak_it);
        continue;
      }
      subscribers.push_back(std::move(subscriber));
      ++weak_it;
    }
  }

  for (const auto& subscriber : subscribers) {
    auto* queue = dynamic_cast<SubscriberStream*>(subscriber.get());
    if (queue != nullptr) {
      static_cast<void>(queue->TryPush(notification));
    }
  }
}

auto
LiveAcceptor::PollManagedApplicationWorker(std::uint32_t worker_id) -> base::Status
{
  if (engine_ == nullptr || !options_.managed_queue_runner.has_value()) {
    return base::Status::Ok();
  }

  auto drained = engine_->PollManagedQueueWorkerOnce(this, worker_id);
  if (!drained.ok()) {
    return drained.status();
  }

  return base::Status::Ok();
}

auto
LiveAcceptor::DrainWorkerCommands(std::uint32_t worker_id, std::uint64_t timestamp_ns) -> base::Status
{
  auto* shard = FindWorkerShard(worker_id);
  if (shard == nullptr || shard->command_sink == nullptr) {
    return base::Status::NotFound("runtime outbound command worker was not found");
  }

  shard->poller.DrainWakeup();

  while (true) {
    auto command = shard->command_sink->TryPop();
    if (!command.has_value()) {
      return base::Status::Ok();
    }

    auto* connection = FindConnectionBySessionId(*shard, command->session_id);
    if (connection == nullptr || connection->session == nullptr || connection->close_requested) {
      continue;
    }

    auto outbound =
      command->kind == OutboundCommandKind::kSendEncodedApplication
        ? connection->session->protocol->SendEncodedApplication(
            command->encoded_message, timestamp_ns, command->envelope.view())
        : connection->session->protocol->SendApplication(command->message, timestamp_ns, command->envelope.view());
    if (!outbound.ok()) {
      return outbound.status();
    }

    auto status = SendFrame(*connection, outbound.value(), timestamp_ns);
    if (!status.ok()) {
      return status;
    }

    UpdateSessionSnapshot(*connection->session);
    RefreshConnectionTimer(*shard, *connection, timestamp_ns);
  }
}

auto
LiveAcceptor::RefreshConnectionTimer(WorkerShardState& shard, ConnectionState& connection, std::uint64_t timestamp_ns)
  -> void
{
  if (connection.session == nullptr || !connection.session->protocol.has_value()) {
    shard.poller.timer_wheel().Cancel(connection.connection_id);
    return;
  }

  const auto deadline = connection.session->protocol->NextTimerDeadline(timestamp_ns);
  if (!deadline.has_value()) {
    shard.poller.timer_wheel().Cancel(connection.connection_id);
    return;
  }
  shard.poller.timer_wheel().Schedule(connection.connection_id, *deadline, timestamp_ns);
}

auto
LiveAcceptor::IndexConnection(WorkerShardState& shard, std::size_t connection_index) -> void
{
  if (connection_index >= shard.connections.size()) {
    return;
  }

  auto& connection = shard.connections[connection_index];
  shard.connection_indices[connection.connection_id] = connection_index;
  if (connection.session != nullptr) {
    shard.session_connection_indices[connection.session->counterparty.session.session_id] = connection_index;
  }
}

auto
LiveAcceptor::FindConnectionById(WorkerShardState& shard, std::uint64_t connection_id) -> ConnectionState*
{
  const auto it = shard.connection_indices.find(connection_id);
  if (it == shard.connection_indices.end() || it->second >= shard.connections.size()) {
    return nullptr;
  }
  return &shard.connections[it->second];
}

auto
LiveAcceptor::FindConnectionBySessionId(WorkerShardState& shard, std::uint64_t session_id) -> ConnectionState*
{
  const auto it = shard.session_connection_indices.find(session_id);
  if (it == shard.session_connection_indices.end() || it->second >= shard.connections.size()) {
    return nullptr;
  }
  return &shard.connections[it->second];
}

auto
LiveAcceptor::ReassignConnectionToWorker(WorkerShardState& source_shard,
                                         std::size_t connection_index,
                                         std::uint32_t target_worker_id) -> ConnectionState*
{
  auto* target_shard = FindWorkerShard(target_worker_id);
  if (target_shard == nullptr) {
    return nullptr;
  }
  if (target_shard == &source_shard) {
    return &source_shard.connections[connection_index];
  }

  auto connection = std::move(source_shard.connections[connection_index]);
  source_shard.connection_indices.erase(connection.connection_id);
  if (connection.session != nullptr) {
    source_shard.session_connection_indices.erase(connection.session->counterparty.session.session_id);
  }

  const auto last_index = source_shard.connections.size() - 1U;
  if (connection_index != last_index) {
    source_shard.connections[connection_index] = std::move(source_shard.connections.back());
    IndexConnection(source_shard, connection_index);
  }
  source_shard.connections.pop_back();
  source_shard.active_connections.fetch_sub(1U, std::memory_order_relaxed);

  target_shard->connections.push_back(std::move(connection));
  target_shard->active_connections.fetch_add(1U, std::memory_order_relaxed);
  IndexConnection(*target_shard, target_shard->connections.size() - 1U);
  return &target_shard->connections.back();
}

auto
LiveAcceptor::CloseConnection(WorkerShardState& shard, std::size_t connection_index, std::uint64_t timestamp_ns) -> void
{
  auto connection = std::move(shard.connections[connection_index]);
  std::uint64_t session_id = 0U;
  std::uint32_t worker_id = 0U;
  if (connection.session != nullptr) {
    session_id = connection.session->counterparty.session.session_id;
    worker_id = connection.session->worker_id;
    if (connection.session->protocol.has_value()) {
      connection.session->protocol->OnTransportClosed();
    }
    UpdateSessionSnapshot(*connection.session);
    static_cast<void>(DispatchSessionEvent(
      *connection.session, SessionEventKind::kClosed, timestamp_ns, connection.close_reason, false));
    UnregisterActiveSession(session_id);
    if (connection.count_completion) {
      completed_sessions_.fetch_add(1U);
    }
  }

  shard.poller.timer_wheel().Cancel(connection.connection_id);
  shard.connection_indices.erase(connection.connection_id);
  if (session_id != 0U) {
    shard.session_connection_indices.erase(session_id);
  }

  RecordTrace(TraceEventKind::kSessionEvent,
              session_id,
              worker_id,
              timestamp_ns,
              connection.connection_id,
              connection.count_completion ? 1U : 0U,
              connection.close_reason);

  connection.connection.Close();
  const auto last_index = shard.connections.size() - 1U;
  if (connection_index != last_index) {
    shard.connections[connection_index] = std::move(shard.connections.back());
    IndexConnection(shard, connection_index);
  }
  shard.connections.pop_back();
  shard.active_connections.fetch_sub(1U, std::memory_order_relaxed);
  active_connection_count_.fetch_sub(1U);
  last_progress_ns_.store(timestamp_ns);
}

auto
LiveAcceptor::MarkConnectionForClose(ConnectionState& connection, std::string_view reason, bool count_completion)
  -> void
{
  connection.close_requested = true;
  connection.count_completion = connection.count_completion || count_completion;
  connection.close_reason = std::string(reason);
}

auto
LiveAcceptor::MakeStore(const CounterpartyConfig& counterparty) const
  -> base::Result<std::unique_ptr<store::SessionStore>>
{
  if (counterparty.store_mode == StoreMode::kMmap) {
    auto store = std::make_unique<store::MmapSessionStore>(counterparty.store_path);
    auto status = store->Open();
    if (!status.ok()) {
      return status;
    }
    return std::unique_ptr<store::SessionStore>(std::move(store));
  }

  if (counterparty.store_mode == StoreMode::kDurableBatch) {
    auto store = std::make_unique<store::DurableBatchSessionStore>(
      counterparty.store_path,
      store::DurableBatchStoreOptions{
        .flush_threshold = counterparty.durable_flush_threshold,
        .rollover_mode = counterparty.durable_rollover_mode,
        .max_archived_segments = counterparty.durable_archive_limit,
        .local_utc_offset_seconds = counterparty.durable_local_utc_offset_seconds,
        .use_system_timezone = counterparty.durable_use_system_timezone,
      });
    auto status = store->Open();
    if (!status.ok()) {
      return status;
    }
    return std::unique_ptr<store::SessionStore>(std::move(store));
  }

  return std::unique_ptr<store::SessionStore>(std::make_unique<store::MemorySessionStore>());
}

auto
LiveAcceptor::MakeActiveSession(const CounterpartyConfig& counterparty,
                                profile::NormalizedDictionaryView dictionary) const -> base::Result<ActiveSession>
{
  auto store = MakeStore(counterparty);
  if (!store.ok()) {
    return store.status();
  }

  std::uint32_t worker_id = 0U;
  if (const auto* runtime = engine_->runtime(); runtime != nullptr) {
    const auto* shard = runtime->FindSessionShard(counterparty.session.session_id);
    if (shard != nullptr) {
      worker_id = shard->worker_id;
    }
  }

  ActiveSession session_state{
    .counterparty = counterparty,
    .worker_id = worker_id,
    .handle = {},
    .dictionary = std::make_unique<profile::NormalizedDictionaryView>(std::move(dictionary)),
    .store = std::move(store).value(),
    .protocol = std::nullopt,
  };
  session_state.protocol.emplace(
    MakeProtocolConfig(counterparty), *session_state.dictionary, session_state.store.get());
  return session_state;
}

auto
LiveAcceptor::RecordInboundMetrics(const ActiveSession& session, std::string_view msg_type) -> void
{
  const auto* config = engine_->config();
  if (config == nullptr || !config->enable_metrics) {
    return;
  }
  static_cast<void>(
    engine_->mutable_metrics()->RecordInbound(session.counterparty.session.session_id, IsAdminMessage(msg_type)));
}

auto
LiveAcceptor::RecordOutboundMetrics(const ActiveSession& session, const session::EncodedFrame& frame) -> void
{
  const auto* config = engine_->config();
  if (config == nullptr || !config->enable_metrics) {
    return;
  }

  auto* metrics = engine_->mutable_metrics();
  const auto session_id = session.counterparty.session.session_id;
  static_cast<void>(metrics->RecordOutbound(session_id, frame.admin));
  if (frame.msg_type == "2") {
    static_cast<void>(metrics->RecordResendRequest(session_id));
  } else if (frame.msg_type == "4") {
    static_cast<void>(metrics->RecordGapFill(session_id, 1U));
  }
}

auto
LiveAcceptor::RecordProtocolFailure(const ActiveSession& session, const base::Status& status) -> void
{
  const auto* config = engine_->config();
  if (config == nullptr || !config->enable_metrics) {
    return;
  }

  auto* metrics = engine_->mutable_metrics();
  const auto session_id = session.counterparty.session.session_id;
  if (IsChecksumFailure(status)) {
    static_cast<void>(metrics->RecordChecksumFailure(session_id));
  } else if (status.code() == base::ErrorCode::kFormatError) {
    static_cast<void>(metrics->RecordParseFailure(session_id));
  }
}

auto
LiveAcceptor::RecordTrace(TraceEventKind kind,
                          std::uint64_t session_id,
                          std::uint32_t worker_id,
                          std::uint64_t timestamp_ns,
                          std::uint64_t arg0,
                          std::uint64_t arg1,
                          std::string_view text) -> void
{
  engine_->mutable_trace()->Record(kind, session_id, worker_id, timestamp_ns, arg0, arg1, text);
}

} // namespace nimble::runtime