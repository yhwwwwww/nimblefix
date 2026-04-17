#include "fastfix/runtime/live_initiator.h"

#include <chrono>
#include <cstddef>
#include <cstring>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "fastfix/codec/fix_tags.h"
#include "fastfix/base/spsc_queue.h"
#include "fastfix/runtime/thread_affinity.h"
#include "fastfix/store/durable_batch_store.h"
#include "fastfix/store/memory_store.h"
#include "fastfix/store/mmap_store.h"

namespace fastfix::runtime {

namespace {

constexpr std::uint64_t kReconnectJitterSeedMask = 0xFFFF'FFFFULL;
constexpr std::uint32_t kReconnectJitterDivisor = 4U;
constexpr std::uint64_t kNanosPerMillisecond = static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::milliseconds{1}).count());
constexpr auto kMultiWorkerIdlePause = std::chrono::milliseconds{1};

auto NowNs() -> std::uint64_t {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
                                              .count());
}

auto WallClockNowNs() -> std::uint64_t {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch())
                                              .count());
}

auto MillisToNanos(std::uint64_t millis) -> std::uint64_t {
    return millis * kNanosPerMillisecond;
}

auto ComputeScheduledAttemptNs(
    const CounterpartyConfig& counterparty,
    std::uint64_t steady_now_ns,
    std::uint64_t wall_now_ns,
    std::uint64_t fallback_delay_ns) -> std::uint64_t {
    const auto next_logon_wall_ns = NextLogonWindowStart(counterparty.session_schedule, wall_now_ns);
    if (!next_logon_wall_ns.has_value() || *next_logon_wall_ns <= wall_now_ns) {
        return steady_now_ns + fallback_delay_ns;
    }
    return steady_now_ns + (*next_logon_wall_ns - wall_now_ns);
}

auto ReconnectJitterLimit(std::uint32_t backoff_ms) -> std::uint32_t {
    return backoff_ms / kReconnectJitterDivisor;
}

thread_local const void* g_inline_borrow_send_sink = nullptr;

class BorrowedSendScope {
    public:
        explicit BorrowedSendScope(const void* sink)
                : previous_(g_inline_borrow_send_sink) {
                g_inline_borrow_send_sink = sink;
        }

        BorrowedSendScope(const BorrowedSendScope&) = delete;
        auto operator=(const BorrowedSendScope&) -> BorrowedSendScope& = delete;

        ~BorrowedSendScope() {
                g_inline_borrow_send_sink = previous_;
        }

    private:
        const void* previous_{nullptr};
};
auto IsAdminMessage(std::string_view msg_type) -> bool {
    return msg_type == "0" || msg_type == "1" || msg_type == "2" || msg_type == "3" ||
           msg_type == "4" || msg_type == "5" || msg_type == "A";
}

auto IsChecksumFailure(const base::Status& status) -> bool {
    return status.code() == base::ErrorCode::kFormatError && status.message().find("CheckSum mismatch") != std::string::npos;
}

auto RandomJitter(std::uint32_t max_jitter_ms) -> std::uint32_t {
    if (max_jitter_ms == 0U) {
        return 0U;
    }
    thread_local std::mt19937 rng(static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) &
        kReconnectJitterSeedMask));
    return std::uniform_int_distribution<std::uint32_t>(0, max_jitter_ms)(rng);
}

auto MakeProtocolConfig(const CounterpartyConfig& counterparty) -> session::AdminProtocolConfig {
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

auto ApplyInitiatorMainThreadSetup(const Engine* engine, bool single_worker) -> base::Status {
    if (single_worker) {
        SetCurrentThreadName("ff-ini-w0");
        if (const auto worker_cpu = WorkerCpuAffinity(engine, 0U)) {
            return ApplyCurrentThreadAffinity(*worker_cpu, "initiator worker 0");
        }
        return base::Status::Ok();
    }

    SetCurrentThreadName("ff-ini-main");
    return base::Status::Ok();
}

auto ApplyInitiatorWorkerThreadSetup(const Engine* engine, std::uint32_t worker_id) -> base::Status {
    SetCurrentThreadName("ff-ini-w" + std::to_string(worker_id));
    if (const auto worker_cpu = WorkerCpuAffinity(engine, worker_id)) {
        return ApplyCurrentThreadAffinity(*worker_cpu, "initiator worker " + std::to_string(worker_id));
    }
    return base::Status::Ok();
}

enum class OutboundCommandKind : std::uint32_t {
    kSendApplication = 0,
    kSendEncodedApplication,
    kBeginLogout,
};

struct OutboundCommand {
    OutboundCommandKind kind{OutboundCommandKind::kSendApplication};
    std::uint64_t session_id{0};
    message::MessageRef message;
    session::EncodedApplicationMessageRef encoded_message;
    session::SessionSendEnvelopeRef envelope;
    std::string text;
};

class SubscriberStream final : public session::SessionSubscriptionStream {
  public:
    explicit SubscriberStream(std::size_t queue_capacity)
        : queue_(queue_capacity) {
    }

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

class LiveInitiator::CommandSink final : public session::SessionCommandSink {
  public:
        CommandSink(LiveInitiator* owner, std::uint32_t worker_id, std::size_t queue_capacity)
        : owner_(owner),
                    worker_id_(worker_id),
          queue_(queue_capacity) {
    }

    auto EnqueueSend(std::uint64_t session_id, message::MessageRef message) -> base::Status override {
        return EnqueueSendWithEnvelope(session_id, std::move(message), {});
    }

    auto EnqueueSendWithEnvelope(
        std::uint64_t session_id,
        message::MessageRef message,
        session::SessionSendEnvelopeRef envelope) -> base::Status override {
        if (!queue_.TryPush(OutboundCommand{
                .kind = OutboundCommandKind::kSendApplication,
                .session_id = session_id,
                .message = std::move(message),
                .encoded_message = {},
                .envelope = std::move(envelope),
                .text = {},
            })) {
            return base::Status::IoError("runtime outbound command queue is full");
        }
        owner_->SignalWorkerWakeup(worker_id_);
        return base::Status::Ok();
    }

    auto EnqueueSendBorrowed(std::uint64_t session_id, const message::MessageRef& message)
        -> base::Result<bool> override {
        return EnqueueSendBorrowedWithEnvelope(session_id, message, {});
    }

    auto EnqueueSendBorrowedWithEnvelope(
        std::uint64_t session_id,
        const message::MessageRef& message,
        session::SessionSendEnvelopeView envelope) -> base::Result<bool> override {
        if (g_inline_borrow_send_sink != this) {
            return false;
        }
        if (!queue_.TryPush(OutboundCommand{
                .kind = OutboundCommandKind::kSendApplication,
                .session_id = session_id,
                .message = message,
                .encoded_message = {},
                .envelope = session::SessionSendEnvelopeRef(envelope),
                .text = {},
            })) {
            return base::Status::IoError("runtime outbound command queue is full");
        }
        owner_->SignalWorkerWakeup(worker_id_);
        return true;
    }

    auto EnqueueSendEncoded(
        std::uint64_t session_id,
        session::EncodedApplicationMessageRef message) -> base::Status override {
        return EnqueueSendEncodedWithEnvelope(session_id, std::move(message), {});
    }

    auto EnqueueSendEncodedWithEnvelope(
        std::uint64_t session_id,
        session::EncodedApplicationMessageRef message,
        session::SessionSendEnvelopeRef envelope) -> base::Status override {
        if (!queue_.TryPush(OutboundCommand{
                .kind = OutboundCommandKind::kSendEncodedApplication,
                .session_id = session_id,
                .message = {},
                .encoded_message = std::move(message),
                .envelope = std::move(envelope),
                .text = {},
            })) {
            return base::Status::IoError("runtime outbound command queue is full");
        }
        owner_->SignalWorkerWakeup(worker_id_);
        return base::Status::Ok();
    }

    auto EnqueueSendEncodedBorrowed(
        std::uint64_t session_id,
        const session::EncodedApplicationMessageRef& message) -> base::Result<bool> override {
        return EnqueueSendEncodedBorrowedWithEnvelope(session_id, message, {});
    }

    auto EnqueueSendEncodedBorrowedWithEnvelope(
        std::uint64_t session_id,
        const session::EncodedApplicationMessageRef& message,
        session::SessionSendEnvelopeView envelope) -> base::Result<bool> override {
        if (g_inline_borrow_send_sink != this) {
            return false;
        }
        if (!queue_.TryPush(OutboundCommand{
                .kind = OutboundCommandKind::kSendEncodedApplication,
                .session_id = session_id,
                .message = {},
                .encoded_message = message,
                .envelope = session::SessionSendEnvelopeRef(envelope),
                .text = {},
            })) {
            return base::Status::IoError("runtime outbound command queue is full");
        }
        owner_->SignalWorkerWakeup(worker_id_);
        return true;
    }

    auto LoadSnapshot(std::uint64_t session_id) const -> base::Result<session::SessionSnapshot> override {
        return owner_->LoadSessionSnapshot(session_id);
    }

    auto Subscribe(std::uint64_t session_id, std::size_t queue_capacity) -> base::Result<session::SessionSubscription> override {
        return owner_->RegisterSessionSubscriber(session_id, queue_capacity);
    }

    auto EnqueueLogout(std::uint64_t session_id, std::string text) -> base::Status {
        if (!queue_.TryPush(OutboundCommand{
                .kind = OutboundCommandKind::kBeginLogout,
                .session_id = session_id,
                .message = {},
                .envelope = {},
                .text = std::move(text),
            })) {
            return base::Status::IoError("runtime outbound command queue is full");
        }
        owner_->SignalWorkerWakeup(worker_id_);
        return base::Status::Ok();
    }

    auto TryPop() -> std::optional<OutboundCommand> {
        return queue_.TryPop();
    }

  private:
    LiveInitiator* owner_{nullptr};
        std::uint32_t worker_id_{0};
    base::SpscQueue<OutboundCommand> queue_;
};

LiveInitiator::LiveInitiator(Engine* engine)
    : LiveInitiator(engine, Options{}) {
}

LiveInitiator::LiveInitiator(Engine* engine, Options options)
    : engine_(engine),
      options_(std::move(options)) {
}

LiveInitiator::~LiveInitiator() {
    Stop();
    if (engine_ != nullptr) {
        static_cast<void>(engine_->ReleaseManagedQueueRunner(this));
    }
}

auto LiveInitiator::EnsureManagedQueueRunnerStarted() -> base::Status {
    if (!options_.managed_queue_runner.has_value()) {
        return base::Status::Ok();
    }

    if (engine_ == nullptr) {
        return base::Status::InvalidArgument("live initiator requires a booted engine");
    }

    return engine_->EnsureManagedQueueRunnerStarted(this, options_.application.get(), &options_.managed_queue_runner);
}

auto LiveInitiator::StopManagedQueueRunner() -> base::Status {
    if (!options_.managed_queue_runner.has_value() || engine_ == nullptr) {
        return base::Status::Ok();
    }
    return engine_->StopManagedQueueRunner(this);
}

auto LiveInitiator::ResetWorkerShards(std::uint32_t worker_count) -> base::Status {
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

auto LiveInitiator::HasActiveSession(std::uint64_t session_id) const -> bool {
    std::lock_guard lock(control_mutex_);
    return active_session_ids_.contains(session_id);
}

auto LiveInitiator::RegisterActiveSession(std::uint64_t session_id) -> bool {
    std::lock_guard lock(control_mutex_);
    return active_session_ids_.emplace(session_id).second;
}

auto LiveInitiator::UnregisterActiveSession(std::uint64_t session_id) -> void {
    std::lock_guard lock(control_mutex_);
    active_session_ids_.erase(session_id);
}

auto LiveInitiator::SetTerminalStatus(base::Status status) -> void {
    std::lock_guard lock(control_mutex_);
    if (!terminal_status_.has_value()) {
        terminal_status_ = std::move(status);
    }
}

auto LiveInitiator::LoadTerminalStatus() const -> std::optional<base::Status> {
    std::lock_guard lock(control_mutex_);
    return terminal_status_;
}

auto LiveInitiator::AdoptPendingConnections(WorkerShardState& shard) -> void {
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

    for (auto& connection : pending_connections) {
        shard.connections.push_back(std::move(connection));
        IndexConnection(shard, shard.connections.size() - 1U);
    }
}

auto LiveInitiator::EnqueuePendingConnection(std::uint32_t worker_id, ConnectionState connection) -> base::Status {
    auto* shard = FindWorkerShard(worker_id);
    if (shard == nullptr || shard->inbox == nullptr) {
        return base::Status::NotFound("initiator worker inbox was not found");
    }

    {
        std::lock_guard lock(shard->inbox->mutex);
        shard->inbox->pending_connections.push_back(std::move(connection));
    }
    SignalWorkerWakeup(worker_id);
    return base::Status::Ok();
}

auto LiveInitiator::StartWorkerThreads() -> base::Status {
    if (!worker_threads_.empty() || worker_shards_.size() <= 1U) {
        return base::Status::Ok();
    }

    try {
        worker_threads_.reserve(worker_shards_.size());
        for (const auto& shard : worker_shards_) {
            worker_threads_.emplace_back([this, worker_id = shard.worker_id](std::stop_token stop_token) {
                WorkerLoop(worker_id, stop_token);
            });
        }
    } catch (...) {
        StopWorkerThreads();
        return base::Status::IoError("failed to start initiator worker threads");
    }

    return base::Status::Ok();
}

auto LiveInitiator::StopWorkerThreads() -> void {
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

auto LiveInitiator::WorkerLoop(std::uint32_t worker_id, std::stop_token stop_token) -> void {
    auto* shard = FindWorkerShard(worker_id);
    if (shard == nullptr) {
        SetTerminalStatus(base::Status::NotFound("initiator worker shard was not found"));
        stop_requested_.store(true);
        return;
    }

    auto status = ApplyInitiatorWorkerThreadSetup(engine_, worker_id);
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

auto LiveInitiator::SignalWorkerWakeup(std::uint32_t worker_id) -> void {
    auto* shard = FindWorkerShard(worker_id);
    if (shard == nullptr) {
        return;
    }
    shard->poller.SignalWakeup();
}

auto LiveInitiator::ResolveWorkerId(std::uint32_t worker_id) const -> std::uint32_t {
    if (worker_shards_.empty()) {
        return 0U;
    }
    return std::min<std::uint32_t>(worker_id, static_cast<std::uint32_t>(worker_shards_.size() - 1U));
}

auto LiveInitiator::FindWorkerShard(std::uint32_t worker_id) -> WorkerShardState* {
    if (worker_shards_.empty()) {
        return nullptr;
    }
    return &worker_shards_[ResolveWorkerId(worker_id)];
}

auto LiveInitiator::FindWorkerShard(std::uint32_t worker_id) const -> const WorkerShardState* {
    if (worker_shards_.empty()) {
        return nullptr;
    }
    return &worker_shards_[ResolveWorkerId(worker_id)];
}

auto LiveInitiator::OpenSession(std::uint64_t session_id, std::string host, std::uint16_t port) -> base::Status {
    if (engine_ == nullptr || engine_->config() == nullptr) {
        return base::Status::InvalidArgument("live initiator requires a booted engine");
    }
    if (host.empty() || port == 0U) {
        return base::Status::InvalidArgument("live initiator requires a remote host and port");
    }
    if (HasActiveSession(session_id)) {
        return base::Status::AlreadyExists("initiator session already has an active transport connection");
    }
    if (!worker_threads_.empty()) {
        return base::Status::InvalidArgument("initiator does not support opening new sessions while multi-worker run is active");
    }

    const auto* counterparty = engine_->FindCounterpartyConfig(session_id);
    if (counterparty == nullptr) {
        return base::Status::NotFound("initiator session was not found in engine config");
    }
    if (!counterparty->session.is_initiator) {
        return base::Status::InvalidArgument("live initiator requires an initiator-mode counterparty");
    }

    if (worker_shards_.empty()) {
        auto status = ResetWorkerShards(engine_->config()->worker_count);
        if (!status.ok()) {
            return status;
        }
    }

    auto dictionary = engine_->LoadDictionaryView(counterparty->session.profile_id);
    if (!dictionary.ok()) {
        return dictionary.status();
    }

    auto active = MakeActiveSession(*counterparty, std::move(dictionary).value(), std::move(host), port);
    if (!active.ok()) {
        return active.status();
    }

    auto session_state = std::move(active).value();
    session_state.worker_id = ResolveWorkerId(session_state.worker_id);

    auto* shard = FindWorkerShard(session_state.worker_id);
    if (shard == nullptr) {
        return base::Status::NotFound("initiator worker shard was not found");
    }

    std::shared_ptr<session::SessionCommandSink> command_sink;
    if (shard->command_sink != nullptr) {
        command_sink = shard->command_sink;
    }
    session_state.handle = session::SessionHandle(
        session_state.counterparty.session.session_id,
        session_state.worker_id,
        std::move(command_sink));

    const auto timestamp_ns = NowNs();
    const auto wall_now_ns = WallClockNowNs();
    if (!IsWithinLogonWindow(session_state.counterparty.session_schedule, wall_now_ns)) {
        if (!RegisterActiveSession(session_state.counterparty.session.session_id)) {
            return base::Status::AlreadyExists("initiator session already has an active transport connection");
        }
        {
            std::lock_guard lock(control_mutex_);
            session_worker_ids_[session_state.counterparty.session.session_id] = session_state.worker_id;
            terminal_status_.reset();
        }
        UpdateSessionSnapshot(session_state);

        shard->pending_reconnects.push_back(PendingReconnect{
            .session_id = session_state.counterparty.session.session_id,
            .host = session_state.host,
            .port = session_state.port,
            .retry_count = 0,
            .current_backoff_ms = session_state.counterparty.reconnect_initial_ms,
            .next_attempt_ns = ComputeScheduledAttemptNs(session_state.counterparty, timestamp_ns, wall_now_ns, 0U),
            .session_registered = true,
            .session = std::make_unique<ActiveSession>(std::move(session_state)),
        });
        pending_reconnect_count_.fetch_add(1U);
        last_progress_ns_.store(timestamp_ns);
        stop_requested_.store(false);
        RecordTrace(
            TraceEventKind::kSessionEvent,
            shard->pending_reconnects.back().session_id,
            shard->pending_reconnects.back().session->worker_id,
            timestamp_ns,
            0U,
            shard->pending_reconnects.back().port,
            "initiator waiting for logon window");
        return base::Status::Ok();
    }

    auto connection = transport::TcpConnection::Connect(session_state.host, session_state.port, options_.io_timeout);
    if (!connection.ok()) {
        return connection.status();
    }

    auto start = session_state.protocol->OnTransportConnected(timestamp_ns);
    if (!start.ok()) {
        return start.status();
    }

    ConnectionState state{
        .connection_id = next_connection_id_++,
        .connection = std::move(connection).value(),
        .session = std::make_unique<ActiveSession>(std::move(session_state)),
        .last_progress_ns = timestamp_ns,
    };

    auto status = SendFrames(state, start.value().outbound_frames, timestamp_ns);
    if (!status.ok()) {
        state.connection.Close();
        return status;
    }

    if (!RegisterActiveSession(state.session->counterparty.session.session_id)) {
        state.connection.Close();
        return base::Status::AlreadyExists("initiator session already has an active transport connection");
    }
    {
        std::lock_guard lock(control_mutex_);
        session_worker_ids_[state.session->counterparty.session.session_id] = state.session->worker_id;
        terminal_status_.reset();
    }
    UpdateSessionSnapshot(*state.session);
    active_connection_count_.fetch_add(1U);

    RecordTrace(
        TraceEventKind::kSessionEvent,
        state.session->counterparty.session.session_id,
        state.session->worker_id,
        timestamp_ns,
        state.connection_id,
        state.session->port,
        "initiator connect");

    shard->connections.push_back(std::move(state));
    IndexConnection(*shard, shard->connections.size() - 1U);
    last_progress_ns_.store(timestamp_ns);
    stop_requested_.store(false);

    status = DispatchSessionEvent(*shard->connections.back().session, SessionEventKind::kBound, timestamp_ns, "transport connected", true);
    if (!status.ok()) {
        MarkConnectionForClose(shard->connections.back(), status.message(), false);
        CloseConnection(*shard, shard->connections.size() - 1U, timestamp_ns);
        return status;
    }

    return base::Status::Ok();
}

auto LiveInitiator::Run(std::size_t max_completed_sessions, std::chrono::milliseconds idle_timeout) -> base::Status {
    if (active_connection_count() == 0U && pending_reconnect_count() == 0U) {
        return base::Status::InvalidArgument("live initiator has no open sessions");
    }

    auto status = EnsureManagedQueueRunnerStarted();
    if (!status.ok()) {
        return status;
    }

    const auto idle_timeout_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(idle_timeout).count());

    auto run_status = base::Status::Ok();
    if (worker_shards_.size() <= 1U) {
        status = ApplyInitiatorMainThreadSetup(engine_, true);
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
            if (active_connection_count() == 0U && pending_reconnect_count() == 0U) {
                break;
            }
            if (max_completed_sessions != 0U && completed_sessions_.load() >= max_completed_sessions) {
                break;
            }

            const auto now = NowNs();
            const auto last_progress_ns = last_progress_ns_.load();
            if (idle_timeout_ns != 0U && now > last_progress_ns + idle_timeout_ns) {
                run_status = base::Status::IoError("live initiator timed out while waiting for session progress");
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

        status = ApplyInitiatorMainThreadSetup(engine_, false);
        if (!status.ok()) {
            StopWorkerThreads();
            static_cast<void>(StopManagedQueueRunner());
            return status;
        }

        while (!stop_requested_.load()) {
            const auto terminal_status = LoadTerminalStatus();
            if (terminal_status.has_value()) {
                run_status = *terminal_status;
                break;
            }
            if (active_connection_count() == 0U && pending_reconnect_count() == 0U) {
                break;
            }
            if (max_completed_sessions != 0U && completed_sessions_.load() >= max_completed_sessions) {
                break;
            }

            const auto now = NowNs();
            const auto last_progress_ns = last_progress_ns_.load();
            if (idle_timeout_ns != 0U && now > last_progress_ns + idle_timeout_ns) {
                run_status = base::Status::IoError("live initiator timed out while waiting for session progress");
                break;
            }

            std::this_thread::sleep_for(kMultiWorkerIdlePause);
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

auto LiveInitiator::Stop() -> void {
    stop_requested_.store(true);
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
        shard.pending_reconnects.clear();
    }
    worker_shards_.clear();
    active_connection_count_.store(0U);
    pending_reconnect_count_.store(0U);
    {
        std::lock_guard lock(control_mutex_);
        active_session_ids_.clear();
        terminal_status_.reset();
        session_snapshots_.clear();
        session_worker_ids_.clear();
        session_subscribers_.clear();
    }
}

auto LiveInitiator::RequestLogout(std::uint64_t session_id, std::string text) -> base::Status {
    std::uint32_t worker_id = 0U;
    {
        std::lock_guard lock(control_mutex_);
        const auto it = session_worker_ids_.find(session_id);
        if (it == session_worker_ids_.end()) {
            return base::Status::NotFound("initiator session worker was not found");
        }
        worker_id = it->second;
    }

    auto* shard = FindWorkerShard(worker_id);
    if (shard == nullptr || shard->command_sink == nullptr) {
        return base::Status::NotFound("initiator outbound command worker was not found");
    }
    return shard->command_sink->EnqueueLogout(session_id, std::move(text));
}

auto LiveInitiator::PollOnce(std::chrono::milliseconds timeout) -> base::Status {
    if (active_connection_count() == 0U && pending_reconnect_count() == 0U) {
        return base::Status::InvalidArgument("live initiator has no open sessions");
    }

    const auto poll_started_ns = NowNs();
    const auto effective_timeout = ComputePollTimeout(timeout, poll_started_ns);

    // SyncAndWait on each shard's connections via their IoPoller.
    for (auto& shard : worker_shards_) {
        auto status = shard.poller.SyncAndWait(
            shard.connections.size(),
            [&](std::size_t index) {
                return shard.connections[index].connection.fd();
            },
            effective_timeout,
            shard.io_ready_state);
        if (!status.ok()) {
            return status;
        }
    }

    const auto now = NowNs();
    for (auto& shard : worker_shards_) {
        for (auto it = shard.io_ready_state.ready_indices.rbegin();
             it != shard.io_ready_state.ready_indices.rend(); ++it) {
            const auto connection_index = *it;
            if (connection_index < shard.connections.size()) {
                auto status = ProcessConnection(shard, connection_index, true, now);
                if (!status.ok()) return status;
            }
        }
    }

    for (const auto& shard : worker_shards_) {
        auto status = PollManagedApplicationWorker(shard.worker_id);
        if (!status.ok()) {
            return status;
        }
    }

    for (auto& shard : worker_shards_) {
        auto status = DrainWorkerCommands(shard.worker_id, now);
        if (!status.ok()) {
            return status;
        }
    }

    const auto timers_now = NowNs();
    for (auto& shard : worker_shards_) {
        auto status = ProcessDueTimers(shard, timers_now);
        if (!status.ok()) {
            return status;
        }
    }

    for (auto& shard : worker_shards_) {
        auto status = ProcessPendingReconnects(shard, timers_now);
        if (!status.ok()) {
            return status;
        }
    }

    for (const auto& shard : worker_shards_) {
        auto status = PollManagedApplicationWorker(shard.worker_id);
        if (!status.ok()) {
            return status;
        }
    }

    const auto final_now = NowNs();
    for (auto& shard : worker_shards_) {
        auto status = DrainWorkerCommands(shard.worker_id, final_now);
        if (!status.ok()) {
            return status;
        }
    }

    return base::Status::Ok();
}

auto LiveInitiator::PollWorkerOnce(WorkerShardState& shard, std::chrono::milliseconds timeout) -> base::Status {
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
        if (!status.ok()) return status;
    }

    const auto t_poll_end = NowNs();
    const auto now = NowNs();

    base::Status status;
    for (auto it = shard.io_ready_state.ready_indices.rbegin();
         it != shard.io_ready_state.ready_indices.rend(); ++it) {
        const auto connection_index = *it;
        if (connection_index < shard.connections.size()) {
            status = ProcessConnection(shard, connection_index, true, now);
            if (!status.ok()) return status;
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
        wm->app_callback_ns.fetch_add((t_app_done - t_recv_done) + (t_app2_done - t_timer_done), std::memory_order_relaxed);
        wm->send_ns.fetch_add((t_send_done - t_app_done) + (t_send2_done - t_app2_done), std::memory_order_relaxed);
        wm->timer_process_ns.fetch_add(t_timer_done - t_send_done, std::memory_order_relaxed);
        wm->poll_iterations.fetch_add(1, std::memory_order_relaxed);
    }

    AdoptPendingConnections(shard);
    return base::Status::Ok();
}

auto LiveInitiator::ComputePollTimeout(std::chrono::milliseconds timeout, std::uint64_t timestamp_ns) const
    -> std::chrono::milliseconds {
    if (engine_ != nullptr && engine_->config() != nullptr &&
        engine_->config()->poll_mode == PollMode::kBusy) {
        return std::chrono::milliseconds(0);
    }
    std::optional<std::uint64_t> deadline;
    for (const auto& shard : worker_shards_) {
        const auto shard_deadline = shard.poller.NextDeadline();
        if (shard_deadline.has_value() && (!deadline.has_value() || *shard_deadline < *deadline)) {
            deadline = shard_deadline;
        }
        for (const auto& pending : shard.pending_reconnects) {
            if (!deadline.has_value() || pending.next_attempt_ns < *deadline) {
                deadline = pending.next_attempt_ns;
            }
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

auto LiveInitiator::ComputePollTimeout(
    const WorkerShardState& shard,
    std::chrono::milliseconds timeout,
    std::uint64_t timestamp_ns) const -> std::chrono::milliseconds {
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

auto LiveInitiator::ProcessConnection(
    WorkerShardState& shard,
    std::size_t connection_index,
    bool readable,
    std::uint64_t timestamp_ns)
    -> base::Status {
    auto* connection = &shard.connections[connection_index];

    if (connection->session != nullptr && connection->pending_app_event.has_value()) {
        auto status = RetryPendingAppEvent(*connection, timestamp_ns);
        if (!status.ok()) {
            MarkConnectionForClose(*connection, status.message(), false);
        }
        if (connection->pending_app_event.has_value()) {
            readable = false;
        }
    }

    if (readable) {
        while (!connection->close_requested) {
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

            auto status = HandleInboundFrame(shard, *connection, frame_bytes, header.value(), timestamp_ns);
            if (!status.ok()) {
                if (connection->session != nullptr) {
                    RecordProtocolFailure(*connection->session, status);
                }
                MarkConnectionForClose(*connection, status.message(), false);
                break;
            }
            if (connection->pending_app_event.has_value()) {
                break;
            }
        }
    }

    if (connection->close_requested) {
        CloseConnection(shard, connection_index, timestamp_ns);
    }

    return base::Status::Ok();
}

auto LiveInitiator::ProcessDueTimers(WorkerShardState& shard, std::uint64_t timestamp_ns) -> base::Status {
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

auto LiveInitiator::ProcessPendingReconnects(WorkerShardState& shard, std::uint64_t timestamp_ns) -> base::Status {
    auto it = shard.pending_reconnects.begin();
    while (it != shard.pending_reconnects.end()) {
        if (it->next_attempt_ns > timestamp_ns) {
            ++it;
            continue;
        }

        if (it->session == nullptr) {
            pending_reconnect_count_.fetch_sub(1U);
            it = shard.pending_reconnects.erase(it);
            continue;
        }

        const auto wall_now_ns = WallClockNowNs();
        if (!IsWithinLogonWindow(it->session->counterparty.session_schedule, wall_now_ns)) {
            it->next_attempt_ns = ComputeScheduledAttemptNs(it->session->counterparty, timestamp_ns, wall_now_ns, 0U);
            last_progress_ns_.store(timestamp_ns);
            ++it;
            continue;
        }

        auto connection_result = transport::TcpConnection::Connect(it->host, it->port, options_.io_timeout);

        if (!connection_result.ok()) {
            it->retry_count++;
            const auto max_retries = it->session->counterparty.reconnect_max_retries;
            if (max_retries > 0 && it->retry_count >= max_retries) {
                RecordTrace(
                    TraceEventKind::kSessionEvent,
                    it->session_id,
                    it->session->worker_id,
                    timestamp_ns,
                    0,
                    it->retry_count,
                    "reconnect gave up");
                SetTerminalStatus(base::Status::IoError(
                    "reconnect gave up after " + std::to_string(it->retry_count) + " attempts"));
                if (it->session_registered) {
                    UnregisterActiveSession(it->session_id);
                    std::lock_guard lock(control_mutex_);
                    session_worker_ids_.erase(it->session_id);
                }
                pending_reconnect_count_.fetch_sub(1U);
                it = shard.pending_reconnects.erase(it);
                continue;
            }

            const auto max_ms = it->session->counterparty.reconnect_max_ms;
            const auto next_backoff = std::min(it->current_backoff_ms * 2, max_ms);
            const auto jitter = RandomJitter(ReconnectJitterLimit(it->current_backoff_ms));
            it->current_backoff_ms = next_backoff;
            it->next_attempt_ns = ComputeScheduledAttemptNs(
                it->session->counterparty,
                timestamp_ns,
                wall_now_ns,
                MillisToNanos(static_cast<std::uint64_t>(next_backoff) + jitter));
            last_progress_ns_.store(timestamp_ns);
            ++it;
            continue;
        }

        auto connected = it->session->protocol->OnTransportConnected(timestamp_ns);
        if (!connected.ok()) {
            if (it->session_registered) {
                UnregisterActiveSession(it->session_id);
                std::lock_guard lock(control_mutex_);
                session_worker_ids_.erase(it->session_id);
            }
            pending_reconnect_count_.fetch_sub(1U);
            it = shard.pending_reconnects.erase(it);
            SetTerminalStatus(connected.status());
            continue;
        }

        if (!it->session_registered) {
            if (!RegisterActiveSession(it->session_id)) {
                pending_reconnect_count_.fetch_sub(1U);
                it = shard.pending_reconnects.erase(it);
                continue;
            }
            it->session_registered = true;
        }
        {
            std::lock_guard lock(control_mutex_);
            session_worker_ids_[it->session_id] = it->session->worker_id;
            terminal_status_.reset();
        }

        ConnectionState state{
            .connection_id = next_connection_id_++,
            .connection = std::move(connection_result).value(),
            .session = std::move(it->session),
            .last_progress_ns = timestamp_ns,
        };

        auto send_status = SendFrames(state, connected.value().outbound_frames, timestamp_ns);
        if (!send_status.ok()) {
            state.connection.Close();
            if (state.session->protocol.has_value()) {
                static_cast<void>(state.session->protocol->OnTransportClosed());
            }
            it->session = std::move(state.session);
            it->retry_count++;
            const auto max_retries = it->session->counterparty.reconnect_max_retries;
            if (max_retries > 0 && it->retry_count >= max_retries) {
                if (it->session_registered) {
                    UnregisterActiveSession(it->session_id);
                    std::lock_guard lock(control_mutex_);
                    session_worker_ids_.erase(it->session_id);
                }
                pending_reconnect_count_.fetch_sub(1U);
                it = shard.pending_reconnects.erase(it);
                continue;
            }
            const auto max_ms = it->session->counterparty.reconnect_max_ms;
            const auto next_backoff = std::min(it->current_backoff_ms * 2, max_ms);
            const auto jitter_val = RandomJitter(ReconnectJitterLimit(it->current_backoff_ms));
            it->current_backoff_ms = next_backoff;
            it->next_attempt_ns = ComputeScheduledAttemptNs(
                it->session->counterparty,
                timestamp_ns,
                wall_now_ns,
                MillisToNanos(static_cast<std::uint64_t>(next_backoff) + jitter_val));
            ++it;
            continue;
        }

        UpdateSessionSnapshot(*state.session);
        active_connection_count_.fetch_add(1U);

        RecordTrace(
            TraceEventKind::kSessionEvent,
            state.session->counterparty.session.session_id,
            state.session->worker_id,
            timestamp_ns,
            state.connection_id,
            state.session->port,
            "initiator reconnect");

        shard.connections.push_back(std::move(state));
        IndexConnection(shard, shard.connections.size() - 1U);
        last_progress_ns_.store(timestamp_ns);

        auto dispatch_status = DispatchSessionEvent(
            *shard.connections.back().session,
            SessionEventKind::kBound,
            timestamp_ns,
            "transport reconnected",
            true);
        if (!dispatch_status.ok()) {
            MarkConnectionForClose(shard.connections.back(), dispatch_status.message(), false);
        }

        pending_reconnect_count_.fetch_sub(1U);
        it = shard.pending_reconnects.erase(it);
    }
    return base::Status::Ok();
}

auto LiveInitiator::RetryPendingAppEvent(ConnectionState& connection, std::uint64_t timestamp_ns) -> base::Status {
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

auto LiveInitiator::HandleInboundFrame(
    WorkerShardState& shard,
    ConnectionState& connection,
    std::span<const std::byte> frame,
    const codec::SessionHeaderView& header,
    std::uint64_t timestamp_ns) -> base::Status {
    std::optional<codec::DecodedMessageView> decoded_message;
    std::optional<message::MessageView> admin_message;
    session::ProtocolEvent event;
    if (options_.application != nullptr) {
        auto decoded = codec::DecodeFixMessageView(frame, *connection.session->dictionary);
        if (!decoded.ok()) {
            return decoded.status();
        }
        decoded_message = std::move(decoded).value();
        auto protocol_event = connection.session->protocol->OnInbound(*decoded_message, timestamp_ns);
        if (!protocol_event.ok()) {
            return protocol_event.status();
        }
        if (IsAdminMessage(header.msg_type)) {
            admin_message = decoded_message->message.view();
        }
        event = std::move(protocol_event).value();
    } else {
        auto decoded = codec::DecodeFixMessageView(frame, *connection.session->dictionary);
        if (!decoded.ok()) {
            return decoded.status();
        }
        auto protocol_event = connection.session->protocol->OnInbound(decoded.value(), timestamp_ns);
        if (!protocol_event.ok()) {
            return protocol_event.status();
        }
        event = std::move(protocol_event).value();
    }

    RecordInboundMetrics(*connection.session, header.msg_type);

    if (admin_message.has_value()) {
        auto status = DispatchAdminMessage(*connection.session, *admin_message, timestamp_ns);
        if (!status.ok()) {
            return status;
        }
    }

    if (event.session_active) {
        auto status = DispatchSessionEvent(*connection.session, SessionEventKind::kActive, timestamp_ns, "session active", true);
        if (!status.ok()) {
            return status;
        }
    }

    return HandleProtocolEvent(shard, connection, event, timestamp_ns);
}

auto LiveInitiator::HandleProtocolEvent(
    WorkerShardState& shard,
    ConnectionState& connection,
    const session::ProtocolEvent& event,
    std::uint64_t timestamp_ns) -> base::Status {
    auto status = SendFramesBatch(connection, event.outbound_frames, timestamp_ns);
    if (!status.ok()) {
        return status;
    }

    for (const auto& application_message : event.application_messages) {
        if (options_.application == nullptr) {
            continue;
        }
        status = DispatchAppMessage(connection, application_message.view(), timestamp_ns);
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

auto LiveInitiator::DispatchSessionEvent(
    const ActiveSession& session,
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
    if (session.counterparty.dispatch_mode == AppDispatchMode::kInline && worker_shard != nullptr && worker_shard->command_sink != nullptr) {
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
    if (!status.ok() || !drain_inline || session.counterparty.dispatch_mode != AppDispatchMode::kInline) {
        return status;
    }
    return DrainWorkerCommands(session.worker_id, timestamp_ns);
}

auto LiveInitiator::DispatchAdminMessage(
    const ActiveSession& session,
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

auto LiveInitiator::DispatchAppMessage(
    ConnectionState& connection,
    message::MessageView message,
    std::uint64_t timestamp_ns) -> base::Status {
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

auto LiveInitiator::ServiceTimer(WorkerShardState& shard, ConnectionState& connection, std::uint64_t timestamp_ns) -> base::Status {
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

auto LiveInitiator::SendFrame(
    ConnectionState& connection,
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

auto LiveInitiator::SendFrames(
    ConnectionState& connection,
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

auto LiveInitiator::SendFramesBatch(
    ConnectionState& connection,
    const session::ProtocolFrameList& frames,
    std::uint64_t timestamp_ns) -> base::Status {
    if (frames.size() <= 1U) {
        // Check if single frame uses external_body — if so, fall through to gather path.
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
            // Scatter-gather: [header] [external_body] [trailer]
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

    // Use zero-copy send when any frame has external_body (mmap-backed scatter-gather).
    bool has_external = false;
    for (const auto& frame : frames) {
        if (!frame.bytes.external_body.empty()) {
            has_external = true;
            break;
        }
    }

    auto status = has_external
        ? connection.connection.SendZeroCopyGather(segments, options_.io_timeout)
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

auto LiveInitiator::LoadSessionSnapshot(std::uint64_t session_id) const -> base::Result<session::SessionSnapshot> {
    std::lock_guard lock(control_mutex_);
    const auto it = session_snapshots_.find(session_id);
    if (it == session_snapshots_.end()) {
        return base::Status::NotFound("session snapshot was not found");
    }
    return it->second;
}

auto LiveInitiator::RegisterSessionSubscriber(std::uint64_t session_id, std::size_t queue_capacity)
    -> base::Result<session::SessionSubscription> {
    auto stream = std::make_shared<SubscriberStream>(queue_capacity);

    std::lock_guard lock(control_mutex_);
    if (!session_snapshots_.contains(session_id)) {
        return base::Status::NotFound("session subscription target was not found");
    }
    session_subscribers_[session_id].push_back(stream);
    return session::SessionSubscription(std::move(stream));
}

auto LiveInitiator::HasSessionSubscribers(std::uint64_t session_id) -> bool {
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

auto LiveInitiator::UpdateSessionSnapshot(const ActiveSession& session) -> void {
    std::lock_guard lock(control_mutex_);
    session_snapshots_[session.counterparty.session.session_id] = session.protocol->session().Snapshot();
}

auto LiveInitiator::PublishNotification(const session::SessionNotification& notification) -> void {
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

auto LiveInitiator::PollManagedApplicationWorker(std::uint32_t worker_id) -> base::Status {
    if (engine_ == nullptr || !options_.managed_queue_runner.has_value()) {
        return base::Status::Ok();
    }

    auto drained = engine_->PollManagedQueueWorkerOnce(this, worker_id);
    if (!drained.ok()) {
        return drained.status();
    }

    return base::Status::Ok();
}

auto LiveInitiator::DrainWorkerCommands(std::uint32_t worker_id, std::uint64_t timestamp_ns) -> base::Status {
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

        auto outbound = command->kind == OutboundCommandKind::kSendEncodedApplication
            ? connection->session->protocol->SendEncodedApplication(
                  command->encoded_message,
                  timestamp_ns,
                  command->envelope.view())
            : connection->session->protocol->SendApplication(
                  command->message,
                  timestamp_ns,
                  command->envelope.view());
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

auto LiveInitiator::RefreshConnectionTimer(WorkerShardState& shard, ConnectionState& connection, std::uint64_t timestamp_ns) -> void {
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

auto LiveInitiator::IndexConnection(WorkerShardState& shard, std::size_t connection_index) -> void {
    if (connection_index >= shard.connections.size()) {
        return;
    }

    auto& connection = shard.connections[connection_index];
    shard.connection_indices[connection.connection_id] = connection_index;
    if (connection.session != nullptr) {
        shard.session_connection_indices[connection.session->counterparty.session.session_id] = connection_index;
    }
}

auto LiveInitiator::FindConnectionById(WorkerShardState& shard, std::uint64_t connection_id) -> ConnectionState* {
    const auto it = shard.connection_indices.find(connection_id);
    if (it == shard.connection_indices.end() || it->second >= shard.connections.size()) {
        return nullptr;
    }
    return &shard.connections[it->second];
}

auto LiveInitiator::FindConnectionBySessionId(WorkerShardState& shard, std::uint64_t session_id) -> ConnectionState* {
    const auto it = shard.session_connection_indices.find(session_id);
    if (it == shard.session_connection_indices.end() || it->second >= shard.connections.size()) {
        return nullptr;
    }
    return &shard.connections[it->second];
}

auto LiveInitiator::CloseConnection(WorkerShardState& shard, std::size_t connection_index, std::uint64_t timestamp_ns) -> void {
    auto connection = std::move(shard.connections[connection_index]);
    std::uint64_t session_id = 0U;
    std::uint32_t worker_id = 0U;
    bool schedule_reconnect = false;
    bool release_session_registration = true;
    if (connection.session != nullptr) {
        session_id = connection.session->counterparty.session.session_id;
        worker_id = connection.session->worker_id;
        if (connection.session->protocol.has_value()) {
            connection.session->protocol->OnTransportClosed();
        }
        UpdateSessionSnapshot(*connection.session);
        static_cast<void>(DispatchSessionEvent(
            *connection.session,
            SessionEventKind::kClosed,
            timestamp_ns,
            connection.close_reason,
            false));
        if (connection.count_completion) {
            completed_sessions_.fetch_add(1U);
        } else if (connection.session->counterparty.reconnect_enabled &&
                   connection.session->counterparty.session.is_initiator) {
            schedule_reconnect = true;
            release_session_registration = false;
        } else if (!connection.close_reason.empty()) {
            SetTerminalStatus(base::Status::IoError(connection.close_reason));
        }

        if (release_session_registration) {
            UnregisterActiveSession(session_id);
        }
        {
            std::lock_guard lock(control_mutex_);
            session_worker_ids_.erase(session_id);
        }
    }

    shard.poller.timer_wheel().Cancel(connection.connection_id);
    shard.connection_indices.erase(connection.connection_id);
    if (session_id != 0U) {
        shard.session_connection_indices.erase(session_id);
    }

    RecordTrace(
        TraceEventKind::kSessionEvent,
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
    active_connection_count_.fetch_sub(1U);
    last_progress_ns_.store(timestamp_ns);

    if (schedule_reconnect && connection.session != nullptr) {
        const auto initial_ms = connection.session->counterparty.reconnect_initial_ms;
        const auto jitter = RandomJitter(ReconnectJitterLimit(initial_ms));
        const auto wall_now_ns = WallClockNowNs();
        shard.pending_reconnects.push_back(PendingReconnect{
            .session_id = session_id,
            .host = connection.session->host,
            .port = connection.session->port,
            .retry_count = 0,
            .current_backoff_ms = initial_ms,
            .next_attempt_ns = ComputeScheduledAttemptNs(
                connection.session->counterparty,
                timestamp_ns,
                wall_now_ns,
                MillisToNanos(static_cast<std::uint64_t>(initial_ms) + jitter)),
            .session_registered = true,
            .session = std::move(connection.session),
        });
        pending_reconnect_count_.fetch_add(1U);
    }
}

auto LiveInitiator::MarkConnectionForClose(ConnectionState& connection, std::string_view reason, bool count_completion)
    -> void {
    connection.close_requested = true;
    connection.count_completion = connection.count_completion || count_completion;
    connection.close_reason = std::string(reason);
}

auto LiveInitiator::MakeStore(const CounterpartyConfig& counterparty) const
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

auto LiveInitiator::MakeActiveSession(
    const CounterpartyConfig& counterparty,
    profile::NormalizedDictionaryView dictionary,
    std::string host,
    std::uint16_t port) const -> base::Result<ActiveSession> {
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
        .host = std::move(host),
        .port = port,
    };
    session_state.protocol.emplace(MakeProtocolConfig(counterparty), *session_state.dictionary, session_state.store.get());
    return session_state;
}

auto LiveInitiator::RecordInboundMetrics(const ActiveSession& session, std::string_view msg_type) -> void {
    const auto* config = engine_->config();
    if (config == nullptr || !config->enable_metrics) {
        return;
    }
    static_cast<void>(engine_->mutable_metrics()->RecordInbound(
        session.counterparty.session.session_id,
        IsAdminMessage(msg_type)));
}

auto LiveInitiator::RecordOutboundMetrics(const ActiveSession& session, const session::EncodedFrame& frame) -> void {
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

auto LiveInitiator::RecordProtocolFailure(const ActiveSession& session, const base::Status& status) -> void {
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

auto LiveInitiator::RecordTrace(
    TraceEventKind kind,
    std::uint64_t session_id,
    std::uint32_t worker_id,
    std::uint64_t timestamp_ns,
    std::uint64_t arg0,
    std::uint64_t arg1,
    std::string_view text) -> void {
    engine_->mutable_trace()->Record(kind, session_id, worker_id, timestamp_ns, arg0, arg1, text);
}

}  // namespace fastfix::runtime