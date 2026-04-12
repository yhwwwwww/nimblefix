#include "fastfix/runtime/live_session_worker.h"

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

#include "fastfix/base/spsc_queue.h"
#include "fastfix/runtime/thread_affinity.h"
#include "fastfix/store/durable_batch_store.h"
#include "fastfix/store/memory_store.h"
#include "fastfix/store/mmap_store.h"

namespace fastfix::runtime {

namespace {

auto NowNs() -> std::uint64_t {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now().time_since_epoch())
                                          .count());
}

auto IsAdminMessage(std::string_view msg_type) -> bool {
    return msg_type == "0" || msg_type == "1" || msg_type == "2" || msg_type == "3" ||
           msg_type == "4" || msg_type == "5" || msg_type == "A";
}

auto IsChecksumFailure(const base::Status& status) -> bool {
    return status.code() == base::ErrorCode::kFormatError &&
           status.message().find("CheckSum mismatch") != std::string::npos;
}

auto MakeProtocolConfig(const CounterpartyConfig& counterparty) -> session::AdminProtocolConfig {
    return session::AdminProtocolConfig{
        .session = counterparty.session,
        .begin_string = counterparty.session.key.begin_string,
        .sender_comp_id = counterparty.session.key.sender_comp_id,
        .target_comp_id = counterparty.session.key.target_comp_id,
        .default_appl_ver_id = counterparty.default_appl_ver_id,
        .heartbeat_interval_seconds = counterparty.session.heartbeat_interval_seconds,
        .validation_policy = counterparty.validation_policy,
    };
}

auto WorkerCpuAffinity(const Engine* engine, std::uint32_t worker_id) -> std::optional<std::uint32_t> {
    if (engine == nullptr || engine->config() == nullptr) {
        return std::nullopt;
    }
    const auto& worker_cpu_affinity = engine->config()->worker_cpu_affinity;
    if (worker_id >= worker_cpu_affinity.size()) {
        return std::nullopt;
    }
    return worker_cpu_affinity[worker_id];
}

thread_local const void* g_inline_borrow_send_sink = nullptr;

class BorrowedSendScope {
  public:
    explicit BorrowedSendScope(const void* sink) : previous_(g_inline_borrow_send_sink) {
        g_inline_borrow_send_sink = sink;
    }
    BorrowedSendScope(const BorrowedSendScope&) = delete;
    auto operator=(const BorrowedSendScope&) -> BorrowedSendScope& = delete;
    ~BorrowedSendScope() { g_inline_borrow_send_sink = previous_; }

  private:
    const void* previous_{nullptr};
};

class SubscriberStream final : public session::SessionSubscriptionStream {
  public:
    explicit SubscriberStream(std::size_t queue_capacity) : queue_(queue_capacity) {}

    auto TryPop() -> base::Result<std::optional<session::SessionNotification>> override {
        return queue_.TryPop();
    }

    auto TryPush(const session::SessionNotification& notification) -> bool {
        return queue_.TryPush(notification);
    }

  private:
    base::SpscQueue<session::SessionNotification> queue_;
};

}  // namespace

// --------------------------------------------------------------------------
// CommandSink
// --------------------------------------------------------------------------

auto LiveSessionWorker::CommandSink::EnqueueSendBorrowed(
    std::uint64_t session_id,
    const message::MessageRef& message) -> base::Result<bool> {
    if (g_inline_borrow_send_sink != this) {
        return false;
    }
    if (!queue_.TryPush(OutboundCommand{
            .kind = OutboundCommandKind::kSendApplication,
            .session_id = session_id,
            .message = message,
            .text = {},
        })) {
        return base::Status::IoError("runtime outbound command queue is full");
    }
    owner_->SignalWorkerWakeup(worker_id_);
    return true;
}

// --------------------------------------------------------------------------
// LiveSessionWorker
// --------------------------------------------------------------------------

LiveSessionWorker::LiveSessionWorker(Engine* engine, Options options)
    : engine_(engine), options_(std::move(options)) {}

LiveSessionWorker::~LiveSessionWorker() = default;

// Default no-op for acceptor.
auto LiveSessionWorker::ProcessPendingReconnects(WorkerShardState& /*shard*/, std::uint64_t /*ts*/)
    -> base::Status {
    return base::Status::Ok();
}

auto LiveSessionWorker::PollWorkerOnce(WorkerShardState& shard,
                                       std::chrono::milliseconds timeout) -> base::Status {
    WorkerMetrics* wm = nullptr;
    {
        const auto* cfg = engine_->config();
        if (cfg != nullptr && cfg->enable_metrics) {
            wm = engine_->mutable_metrics()->FindWorker(shard.worker_id);
        }
    }

    const auto poll_started_ns = NowNs();
    const auto effective_timeout = ComputePollTimeout(shard, timeout, poll_started_ns);
    const auto t_poll_start = poll_started_ns;

    if (shard.poller.has_io_poller()) {
        auto status = shard.poller.SyncAndWait(
            shard.connections.size(),
            [&](std::size_t index) { return shard.connections[index].connection.fd(); },
            effective_timeout,
            shard.io_ready_state);
        if (!status.ok()) {
            return status;
        }
    } else {
        return base::Status::InvalidArgument("poll backend has been removed, use epoll or io_uring");
    }

    const auto t_poll_end = NowNs();
    const auto now = t_poll_end;

    base::Status status;
    for (auto it = shard.io_ready_state.ready_indices.rbegin();
         it != shard.io_ready_state.ready_indices.rend(); ++it) {
        const auto connection_index = *it;
        if (connection_index < shard.connections.size()) {
            status = ProcessConnection(shard, connection_index, true, now);
            if (!status.ok()) {
                return status;
            }
        }
    }
    const auto t_recv_done = NowNs();

    status = PollManagedApplicationWorker(shard.worker_id);
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

    status = ProcessPendingReconnects(shard, timers_now);
    if (!status.ok()) {
        return status;
    }
    const auto t_timer_done = NowNs();

    status = PollManagedApplicationWorker(shard.worker_id);
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
        wm->app_callback_ns.fetch_add(
            (t_app_done - t_recv_done) + (t_app2_done - t_timer_done), std::memory_order_relaxed);
        wm->send_ns.fetch_add(
            (t_send_done - t_app_done) + (t_send2_done - t_app2_done), std::memory_order_relaxed);
        wm->timer_process_ns.fetch_add(t_timer_done - t_send_done, std::memory_order_relaxed);
        wm->poll_iterations.fetch_add(1, std::memory_order_relaxed);
    }

    AdoptPendingConnections(shard);
    return base::Status::Ok();
}

auto LiveSessionWorker::ComputePollTimeout(const WorkerShardState& shard,
                                           std::chrono::milliseconds timeout,
                                           std::uint64_t timestamp_ns) const
    -> std::chrono::milliseconds {
    if (engine_ != nullptr && engine_->config() != nullptr &&
        engine_->config()->poll_mode == PollMode::kBusy) {
        return std::chrono::milliseconds(0);
    }
    std::optional<std::uint64_t> deadline = shard.poller.NextDeadline();
    for (const auto& pending : shard.pending_reconnects) {
        if (!deadline.has_value() || pending.next_attempt_ns < *deadline) {
            deadline = pending.next_attempt_ns;
        }
    }
    if (!deadline.has_value()) {
        return timeout;
    }
    if (*deadline <= timestamp_ns) {
        return std::chrono::milliseconds(0);
    }
    const auto timer_timeout = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::nanoseconds(*deadline - timestamp_ns));
    if (timeout.count() < 0) {
        return timer_timeout;
    }
    return std::min(timeout, timer_timeout);
}

auto LiveSessionWorker::ProcessDueTimers(WorkerShardState& shard,
                                         std::uint64_t timestamp_ns) -> base::Status {
    shard.expired_timer_ids.clear();
    shard.poller.timer_wheel().PopExpired(timestamp_ns, &shard.expired_timer_ids);
    for (const auto connection_id : shard.expired_timer_ids) {
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

auto LiveSessionWorker::RetryPendingAppEvent(ConnectionState& connection,
                                             std::uint64_t timestamp_ns) -> base::Status {
    if (!connection.pending_app_event.has_value() || options_.application == nullptr ||
        connection.session == nullptr) {
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

auto LiveSessionWorker::DispatchSessionEvent(const ActiveSession& session,
                                             SessionEventKind kind,
                                             std::uint64_t timestamp_ns,
                                             std::string text,
                                             bool drain_inline) -> base::Status {
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
    if (session.counterparty.dispatch_mode == AppDispatchMode::kInline &&
        worker_shard != nullptr && worker_shard->command_sink != nullptr) {
        BorrowedSendScope scope(worker_shard->command_sink.get());
        status = options_.application->OnSessionEvent(event);
    } else {
        status = options_.application->OnSessionEvent(event);
    }
    if (HasSessionSubscribers(session.counterparty.session.session_id)) {
        PublishNotification(session::SessionNotification{
            .kind = kind == SessionEventKind::kBound
                        ? session::SessionNotificationKind::kSessionBound
                        : (kind == SessionEventKind::kActive
                               ? session::SessionNotificationKind::kSessionActive
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
    if (!status.ok() || !drain_inline ||
        session.counterparty.dispatch_mode != AppDispatchMode::kInline) {
        return status;
    }
    return DrainWorkerCommands(session.worker_id, timestamp_ns);
}

auto LiveSessionWorker::DispatchAdminMessage(const ActiveSession& session,
                                             message::MessageView message,
                                             std::uint64_t timestamp_ns) -> base::Status {
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

auto LiveSessionWorker::DispatchAppMessage(ConnectionState& connection,
                                           message::MessageView message,
                                           std::uint64_t timestamp_ns) -> base::Status {
    const bool inline_mode =
        connection.session->counterparty.dispatch_mode == AppDispatchMode::kInline;
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
        .poss_resend = message.get_boolean(97U).value_or(false),
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

auto LiveSessionWorker::ServiceTimer(WorkerShardState& shard,
                                     ConnectionState& connection,
                                     std::uint64_t timestamp_ns) -> base::Status {
    auto event = connection.session->protocol->OnTimer(timestamp_ns);
    if (!event.ok()) {
        return event.status();
    }
    return HandleProtocolEvent(shard, connection, event.value(), timestamp_ns);
}

auto LiveSessionWorker::SendFrame(ConnectionState& connection,
                                  const session::EncodedFrame& frame,
                                  std::uint64_t timestamp_ns) -> base::Status {
    auto status = connection.connection.Send(frame.bytes, options_.io_timeout);
    if (!status.ok()) {
        return status;
    }
    connection.last_progress_ns = timestamp_ns;
    last_progress_ns_.store(timestamp_ns);
    RecordOutboundMetrics(*connection.session, frame);
    return base::Status::Ok();
}

auto LiveSessionWorker::SendFrames(ConnectionState& connection,
                                   const session::ProtocolFrameList& frames,
                                   std::uint64_t timestamp_ns) -> base::Status {
    for (const auto& frame : frames) {
        auto status = SendFrame(connection, frame, timestamp_ns);
        if (!status.ok()) {
            return status;
        }
    }
    return base::Status::Ok();
}

auto LiveSessionWorker::LoadSessionSnapshot(std::uint64_t session_id) const
    -> base::Result<session::SessionSnapshot> {
    std::lock_guard lock(control_mutex_);
    const auto it = session_snapshots_.find(session_id);
    if (it == session_snapshots_.end()) {
        return base::Status::NotFound("session snapshot was not found");
    }
    return it->second;
}

auto LiveSessionWorker::RegisterSessionSubscriber(std::uint64_t session_id,
                                                  std::size_t queue_capacity)
    -> base::Result<session::SessionSubscription> {
    auto stream = std::make_shared<SubscriberStream>(queue_capacity);
    std::lock_guard lock(control_mutex_);
    if (!session_snapshots_.contains(session_id)) {
        return base::Status::NotFound("session subscription target was not found");
    }
    session_subscribers_[session_id].push_back(stream);
    return session::SessionSubscription(std::move(stream));
}

auto LiveSessionWorker::HasSessionSubscribers(std::uint64_t session_id) -> bool {
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

auto LiveSessionWorker::UpdateSessionSnapshot(const ActiveSession& session) -> void {
    std::lock_guard lock(control_mutex_);
    session_snapshots_[session.counterparty.session.session_id] =
        session.protocol->session().Snapshot();
}

auto LiveSessionWorker::PublishNotification(
    const session::SessionNotification& notification) -> void {
    publish_scratch_.clear();
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
            publish_scratch_.push_back(std::move(subscriber));
            ++weak_it;
        }
    }
    for (const auto& subscriber : publish_scratch_) {
        auto* queue = static_cast<SubscriberStream*>(subscriber.get());
        static_cast<void>(queue->TryPush(notification));
    }
    publish_scratch_.clear();
}

auto LiveSessionWorker::PollManagedApplicationWorker(std::uint32_t worker_id) -> base::Status {
    if (engine_ == nullptr || !options_.managed_queue_runner.has_value()) {
        return base::Status::Ok();
    }
    auto drained = engine_->PollManagedQueueWorkerOnce(this, worker_id);
    if (!drained.ok()) {
        return drained.status();
    }
    return base::Status::Ok();
}

auto LiveSessionWorker::DrainWorkerCommands(std::uint32_t worker_id,
                                            std::uint64_t timestamp_ns) -> base::Status {
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
        if (command->kind == OutboundCommandKind::kBeginLogout) {
            auto outbound = connection->session->protocol->BeginLogout(command->text, timestamp_ns);
            if (!outbound.ok()) {
                return outbound.status();
            }
            auto status = SendFrame(*connection, outbound.value(), timestamp_ns);
            if (!status.ok()) {
                return status;
            }
            UpdateSessionSnapshot(*connection->session);
            RefreshConnectionTimer(*shard, *connection, timestamp_ns);
            continue;
        }
        auto outbound = connection->session->protocol->SendApplication(command->message, timestamp_ns);
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

auto LiveSessionWorker::RefreshConnectionTimer(WorkerShardState& shard,
                                               ConnectionState& connection,
                                               std::uint64_t timestamp_ns) -> void {
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

auto LiveSessionWorker::IndexConnection(WorkerShardState& shard,
                                        std::size_t connection_index) -> void {
    if (connection_index >= shard.connections.size()) {
        return;
    }
    auto& connection = shard.connections[connection_index];
    shard.connection_indices[connection.connection_id] = connection_index;
    if (connection.session != nullptr) {
        shard.session_connection_indices[connection.session->counterparty.session.session_id] =
            connection_index;
    }
}

auto LiveSessionWorker::FindConnectionById(WorkerShardState& shard,
                                           std::uint64_t connection_id) -> ConnectionState* {
    const auto it = shard.connection_indices.find(connection_id);
    if (it == shard.connection_indices.end() || it->second >= shard.connections.size()) {
        return nullptr;
    }
    return &shard.connections[it->second];
}

auto LiveSessionWorker::FindConnectionBySessionId(WorkerShardState& shard,
                                                  std::uint64_t session_id) -> ConnectionState* {
    const auto it = shard.session_connection_indices.find(session_id);
    if (it == shard.session_connection_indices.end() || it->second >= shard.connections.size()) {
        return nullptr;
    }
    return &shard.connections[it->second];
}

auto LiveSessionWorker::MarkConnectionForClose(ConnectionState& connection,
                                               std::string reason,
                                               bool count_completion) -> void {
    connection.close_requested = true;
    connection.count_completion = connection.count_completion || count_completion;
    connection.close_reason = std::move(reason);
}

auto LiveSessionWorker::EnsureManagedQueueRunnerStarted() -> base::Status {
    if (!options_.managed_queue_runner.has_value()) {
        return base::Status::Ok();
    }
    if (engine_ == nullptr) {
        return base::Status::InvalidArgument("live session worker requires a booted engine");
    }
    return engine_->EnsureManagedQueueRunnerStarted(
        this, options_.application.get(), &options_.managed_queue_runner);
}

auto LiveSessionWorker::StopManagedQueueRunner() -> base::Status {
    if (!options_.managed_queue_runner.has_value() || engine_ == nullptr) {
        return base::Status::Ok();
    }
    return engine_->StopManagedQueueRunner(this);
}

auto LiveSessionWorker::ResetWorkerShards(std::uint32_t worker_count) -> base::Status {
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
        shard.command_sink =
            std::make_shared<CommandSink>(this, worker_id, options_.command_queue_capacity);
        shard.inbox = std::make_unique<WorkerInbox>();
    }
    return base::Status::Ok();
}

auto LiveSessionWorker::StartWorkerThreads() -> base::Status {
    if (!worker_threads_.empty() || worker_shards_.size() <= 1U) {
        return base::Status::Ok();
    }
    try {
        worker_threads_.reserve(worker_shards_.size());
        for (const auto& shard : worker_shards_) {
            worker_threads_.emplace_back(
                [this, worker_id = shard.worker_id](std::stop_token stop_token) {
                    WorkerLoop(worker_id, stop_token);
                });
        }
    } catch (...) {
        StopWorkerThreads();
        return base::Status::IoError("failed to start session worker threads");
    }
    return base::Status::Ok();
}

auto LiveSessionWorker::StopWorkerThreads() -> void {
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

auto LiveSessionWorker::WorkerLoop(std::uint32_t worker_id,
                                   std::stop_token stop_token) -> void {
    auto* shard = FindWorkerShard(worker_id);
    if (shard == nullptr) {
        SetTerminalStatus(base::Status::NotFound("session worker shard was not found"));
        stop_requested_.store(true);
        return;
    }
    auto status = ApplyWorkerThreadSetup(engine_, worker_id);
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

auto LiveSessionWorker::AdoptPendingConnections(WorkerShardState& shard) -> void {
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

auto LiveSessionWorker::EnqueuePendingConnection(std::uint32_t worker_id,
                                                 ConnectionState connection) -> base::Status {
    auto* shard = FindWorkerShard(worker_id);
    if (shard == nullptr || shard->inbox == nullptr) {
        return base::Status::NotFound("session worker inbox was not found");
    }
    {
        std::lock_guard lock(shard->inbox->mutex);
        shard->inbox->pending_connections.push_back(std::move(connection));
    }
    SignalWorkerWakeup(worker_id);
    return base::Status::Ok();
}

auto LiveSessionWorker::SignalWorkerWakeup(std::uint32_t worker_id) -> void {
    auto* shard = FindWorkerShard(worker_id);
    if (shard == nullptr) {
        return;
    }
    shard->poller.SignalWakeup();
}

auto LiveSessionWorker::ResolveWorkerId(std::uint32_t worker_id) const -> std::uint32_t {
    if (worker_shards_.empty()) {
        return 0U;
    }
    return std::min<std::uint32_t>(worker_id,
                                   static_cast<std::uint32_t>(worker_shards_.size() - 1U));
}

auto LiveSessionWorker::FindWorkerShard(std::uint32_t worker_id) -> WorkerShardState* {
    if (worker_shards_.empty()) {
        return nullptr;
    }
    return &worker_shards_[ResolveWorkerId(worker_id)];
}

auto LiveSessionWorker::FindWorkerShard(std::uint32_t worker_id) const
    -> const WorkerShardState* {
    if (worker_shards_.empty()) {
        return nullptr;
    }
    return &worker_shards_[ResolveWorkerId(worker_id)];
}

auto LiveSessionWorker::SetTerminalStatus(base::Status status) -> void {
    std::lock_guard lock(control_mutex_);
    if (!terminal_status_.has_value()) {
        terminal_status_ = std::move(status);
        terminal_status_set_.store(true, std::memory_order_release);
    }
}

auto LiveSessionWorker::LoadTerminalStatus() const -> std::optional<base::Status> {
    if (!terminal_status_set_.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    std::lock_guard lock(control_mutex_);
    return terminal_status_;
}

auto LiveSessionWorker::HasActiveSession(std::uint64_t session_id) const -> bool {
    std::lock_guard lock(control_mutex_);
    return active_session_ids_.contains(session_id);
}

auto LiveSessionWorker::RegisterActiveSession(std::uint64_t session_id) -> bool {
    std::lock_guard lock(control_mutex_);
    return active_session_ids_.emplace(session_id).second;
}

auto LiveSessionWorker::UnregisterActiveSession(std::uint64_t session_id) -> void {
    std::lock_guard lock(control_mutex_);
    active_session_ids_.erase(session_id);
}

auto LiveSessionWorker::MakeStore(const CounterpartyConfig& counterparty) const
    -> base::Result<std::unique_ptr<store::SessionStore>> {
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

auto LiveSessionWorker::RecordInboundMetrics(const ActiveSession& session,
                                             std::string_view msg_type) -> void {
    const auto* config = engine_->config();
    if (config == nullptr || !config->enable_metrics) {
        return;
    }
    static_cast<void>(engine_->mutable_metrics()->RecordInbound(
        session.counterparty.session.session_id, IsAdminMessage(msg_type)));
}

auto LiveSessionWorker::RecordOutboundMetrics(const ActiveSession& session,
                                              const session::EncodedFrame& frame) -> void {
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

auto LiveSessionWorker::RecordProtocolFailure(const ActiveSession& session,
                                              const base::Status& status) -> void {
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

auto LiveSessionWorker::RecordTrace(TraceEventKind kind,
                                    std::uint64_t session_id,
                                    std::uint32_t worker_id,
                                    std::uint64_t timestamp_ns,
                                    std::uint64_t arg0,
                                    std::uint64_t arg1,
                                    std::string_view text) -> void {
    engine_->mutable_trace()->Record(kind, session_id, worker_id, timestamp_ns, arg0, arg1, text);
}

// Shared MakeActiveSession (no host/port) for acceptor.
auto LiveSessionWorker::MakeActiveSessionBase(
    const CounterpartyConfig& counterparty,
    profile::NormalizedDictionaryView dictionary) const -> base::Result<ActiveSession> {
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

}  // namespace fastfix::runtime
