#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "fastfix/base/result.h"
#include "fastfix/base/status.h"
#include "fastfix/profile/normalized_dictionary.h"
#include "fastfix/runtime/application.h"
#include "fastfix/runtime/config.h"
#include "fastfix/runtime/engine.h"
#include "fastfix/runtime/shard_poller.h"
#include "fastfix/session/admin_protocol.h"
#include "fastfix/store/session_store.h"
#include "fastfix/transport/tcp_transport.h"

namespace fastfix::runtime {

// LiveAcceptor keeps socket accept on a front-door thread. In multi-worker mode,
// listener worker_hint only seeds initial accept-side placement; once Logon binds
// the session, the bound worker owns steady-state protocol and application work.
class LiveAcceptor {
  public:
    struct Options {
    std::chrono::milliseconds poll_timeout{kDefaultRuntimePollTimeout};
    std::chrono::milliseconds io_timeout{kDefaultRuntimeIoTimeout};
                // Inline callbacks run on the bound session owner worker thread.
        std::shared_ptr<ApplicationCallbacks> application;
                // Queue mode binds handler[i] to worker i; co-scheduled drains on worker i's
                // runtime thread, threaded runs one app thread per worker.
        std::optional<ManagedQueueApplicationRunnerOptions> managed_queue_runner;
    std::size_t command_queue_capacity{kDefaultQueueEventCapacity};
    };

    explicit LiveAcceptor(Engine* engine);
    explicit LiveAcceptor(Engine* engine, Options options);
    ~LiveAcceptor();

    auto OpenListeners(std::string_view listener_name = {}) -> base::Status;
    auto Run(
        std::size_t max_completed_sessions = 0,
        std::chrono::milliseconds idle_timeout = std::chrono::milliseconds{0}) -> base::Status;
    auto Stop() -> void;

    [[nodiscard]] auto listener_port(std::string_view name) const -> base::Result<std::uint16_t>;

    [[nodiscard]] auto active_connection_count() const -> std::size_t {
        return active_connection_count_.load();
    }

    [[nodiscard]] auto completed_session_count() const -> std::size_t {
        return completed_sessions_.load();
    }

  private:
    class CommandSink;

    struct ActiveSession {
        CounterpartyConfig counterparty;
        std::uint32_t worker_id{0};
        std::uint32_t routed_worker_id{0};
        session::SessionHandle handle;
        std::unique_ptr<profile::NormalizedDictionaryView> dictionary;
        std::unique_ptr<store::SessionStore> store;
        std::optional<session::AdminProtocol> protocol;
    };

    struct ListenerState {
        std::string name;
        std::uint32_t worker_hint{0};
        transport::TcpAcceptor acceptor;
    };

    struct ConnectionState {
        std::uint64_t connection_id{0};
        transport::TcpConnection connection;
        std::unique_ptr<ActiveSession> session;
        std::uint64_t last_progress_ns{0};
        std::optional<RuntimeEvent> pending_app_event;
        bool close_requested{false};
        bool count_completion{false};
        std::string close_reason;
    };

    struct WorkerInbox {
        std::mutex mutex;
        std::vector<ConnectionState> pending_connections;
    };

    struct WorkerShardState {
        WorkerShardState() = default;
        WorkerShardState(const WorkerShardState&) = delete;
        auto operator=(const WorkerShardState&) -> WorkerShardState& = delete;

        WorkerShardState(WorkerShardState&& other) noexcept
            : worker_id(other.worker_id),
              active_connections(other.active_connections.load(std::memory_order_relaxed)),
              connections(std::move(other.connections)),
              poller(std::move(other.poller)),
              connection_indices(std::move(other.connection_indices)),
              session_connection_indices(std::move(other.session_connection_indices)),
              command_sink(std::move(other.command_sink)),
              inbox(std::move(other.inbox)),
              poll_scratch(std::move(other.poll_scratch)),
              poll_state_scratch(std::move(other.poll_state_scratch)),
              io_ready_state(std::move(other.io_ready_state)) {
            other.active_connections.store(0U, std::memory_order_relaxed);
        }

        auto operator=(WorkerShardState&& other) noexcept -> WorkerShardState& {
            if (this == &other) {
                return *this;
            }

            worker_id = other.worker_id;
            active_connections.store(other.active_connections.load(std::memory_order_relaxed), std::memory_order_relaxed);
            connections = std::move(other.connections);
            poller = std::move(other.poller);
            connection_indices = std::move(other.connection_indices);
            session_connection_indices = std::move(other.session_connection_indices);
            command_sink = std::move(other.command_sink);
            inbox = std::move(other.inbox);
            poll_scratch = std::move(other.poll_scratch);
            poll_state_scratch = std::move(other.poll_state_scratch);
            io_ready_state = std::move(other.io_ready_state);
            other.active_connections.store(0U, std::memory_order_relaxed);
            return *this;
        }

        std::uint32_t worker_id{0};
        std::atomic<std::size_t> active_connections{0};
        std::vector<ConnectionState> connections;
        ShardPoller poller{};
        std::unordered_map<std::uint64_t, std::size_t> connection_indices;
        std::unordered_map<std::uint64_t, std::size_t> session_connection_indices;
        std::shared_ptr<CommandSink> command_sink;
        std::unique_ptr<WorkerInbox> inbox;
        std::vector<pollfd> poll_scratch;
        ShardPoller::PollState poll_state_scratch;
        ShardPoller::IoReadyState io_ready_state;
    };

    auto PollOnce(std::chrono::milliseconds timeout) -> base::Status;
    [[nodiscard]] auto ComputePollTimeout(std::chrono::milliseconds timeout, std::uint64_t timestamp_ns) const
        -> std::chrono::milliseconds;
    [[nodiscard]] auto ComputePollTimeout(
        const WorkerShardState& shard,
        std::chrono::milliseconds timeout,
        std::uint64_t timestamp_ns) const -> std::chrono::milliseconds;
    auto PollWorkerOnce(WorkerShardState& shard, std::chrono::milliseconds timeout) -> base::Status;
    auto AcceptReadyListener(std::size_t listener_index, std::uint64_t timestamp_ns) -> base::Status;
    auto ProcessConnection(
        WorkerShardState& shard,
        std::size_t connection_index,
        bool readable,
        std::uint64_t timestamp_ns) -> base::Status;
    auto MigrateConnectionToRoutedWorker(WorkerShardState& shard, std::size_t connection_index) -> base::Status;
    auto ProcessDueTimers(WorkerShardState& shard, std::uint64_t timestamp_ns) -> base::Status;
    auto RetryPendingAppEvent(ConnectionState& connection, std::uint64_t timestamp_ns) -> base::Status;
    auto BindConnectionFromLogon(
        WorkerShardState& shard,
        std::size_t connection_index,
        const codec::SessionHeaderView& header,
        std::uint64_t timestamp_ns) -> base::Result<ConnectionState*>;
    auto HandleInboundFrame(
        WorkerShardState& shard,
        std::size_t connection_index,
        std::span<const std::byte> frame,
        const codec::SessionHeaderView& header,
        std::uint64_t timestamp_ns) -> base::Status;
    auto HandleProtocolEvent(
        WorkerShardState& shard,
        ConnectionState& connection,
        const session::ProtocolEvent& event,
        std::uint64_t timestamp_ns) -> base::Status;
    auto DispatchSessionEvent(
        const ActiveSession& session,
        SessionEventKind kind,
        std::uint64_t timestamp_ns,
        std::string text,
        bool drain_inline) -> base::Status;
    auto DispatchAdminMessage(
        const ActiveSession& session,
        message::MessageView message,
        std::uint64_t timestamp_ns) -> base::Status;
    auto DispatchAppMessage(
        ConnectionState& connection,
        message::MessageView message,
        std::uint64_t timestamp_ns) -> base::Status;
    auto ServiceTimer(WorkerShardState& shard, ConnectionState& connection, std::uint64_t timestamp_ns) -> base::Status;
    auto SendFrame(
        ConnectionState& connection,
        const session::EncodedFrame& frame,
        std::uint64_t timestamp_ns) -> base::Status;
    auto SendFrames(
        ConnectionState& connection,
        const session::ProtocolFrameList& frames,
        std::uint64_t timestamp_ns) -> base::Status;
    auto LoadSessionSnapshot(std::uint64_t session_id) const -> base::Result<session::SessionSnapshot>;
    auto RegisterSessionSubscriber(std::uint64_t session_id, std::size_t queue_capacity)
        -> base::Result<session::SessionSubscription>;
    auto HasSessionSubscribers(std::uint64_t session_id) -> bool;
    auto UpdateSessionSnapshot(const ActiveSession& session) -> void;
    auto PublishNotification(const session::SessionNotification& notification) -> void;
    auto PollManagedApplicationWorker(std::uint32_t worker_id) -> base::Status;
    auto DrainWorkerCommands(std::uint32_t worker_id, std::uint64_t timestamp_ns) -> base::Status;
    auto RefreshConnectionTimer(WorkerShardState& shard, ConnectionState& connection, std::uint64_t timestamp_ns) -> void;
    auto IndexConnection(WorkerShardState& shard, std::size_t connection_index) -> void;
    auto FindConnectionById(WorkerShardState& shard, std::uint64_t connection_id) -> ConnectionState*;
    auto FindConnectionBySessionId(WorkerShardState& shard, std::uint64_t session_id) -> ConnectionState*;
    auto ReassignConnectionToWorker(
        WorkerShardState& source_shard,
        std::size_t connection_index,
        std::uint32_t target_worker_id) -> ConnectionState*;
    auto CloseConnection(WorkerShardState& shard, std::size_t connection_index, std::uint64_t timestamp_ns) -> void;
    auto MarkConnectionForClose(ConnectionState& connection, std::string reason, bool count_completion) -> void;
    auto EnsureManagedQueueRunnerStarted() -> base::Status;
    auto StopManagedQueueRunner() -> base::Status;
    auto ResetWorkerShards(std::uint32_t worker_count) -> base::Status;
    auto StartWorkerThreads() -> base::Status;
    auto StopWorkerThreads() -> void;
    auto WorkerLoop(std::uint32_t worker_id, std::stop_token stop_token) -> void;
    auto AdoptPendingConnections(WorkerShardState& shard) -> void;
    auto EnqueuePendingConnection(std::uint32_t worker_id, ConnectionState connection) -> base::Status;
    auto SignalWorkerWakeup(std::uint32_t worker_id) -> void;
    [[nodiscard]] auto ResolveWorkerId(std::uint32_t worker_id) const -> std::uint32_t;
    [[nodiscard]] auto SelectAcceptWorkerId(std::uint32_t worker_hint) const -> std::uint32_t;
    auto FindWorkerShard(std::uint32_t worker_id) -> WorkerShardState*;
    [[nodiscard]] auto FindWorkerShard(std::uint32_t worker_id) const -> const WorkerShardState*;
    auto SetTerminalStatus(base::Status status) -> void;
    [[nodiscard]] auto LoadTerminalStatus() const -> std::optional<base::Status>;
    [[nodiscard]] auto HasActiveSession(std::uint64_t session_id) const -> bool;
    auto RegisterActiveSession(std::uint64_t session_id) -> bool;
    auto UnregisterActiveSession(std::uint64_t session_id) -> void;

    auto MakeStore(const CounterpartyConfig& counterparty) const -> base::Result<std::unique_ptr<store::SessionStore>>;
    auto MakeActiveSession(
        const CounterpartyConfig& counterparty,
        profile::NormalizedDictionaryView dictionary) const -> base::Result<ActiveSession>;
    auto RecordInboundMetrics(const ActiveSession& session, std::string_view msg_type) -> void;
    auto RecordOutboundMetrics(const ActiveSession& session, const session::EncodedFrame& frame) -> void;
    auto RecordProtocolFailure(const ActiveSession& session, const base::Status& status) -> void;
    auto RecordTrace(
        TraceEventKind kind,
        std::uint64_t session_id,
        std::uint32_t worker_id,
        std::uint64_t timestamp_ns,
        std::uint64_t arg0,
        std::uint64_t arg1,
        std::string_view text) -> void;

    Engine* engine_{nullptr};
    Options options_{};
    std::vector<pollfd> main_poll_scratch_;
    std::vector<ListenerState> listeners_;
    std::vector<WorkerShardState> worker_shards_;
    std::vector<std::jthread> worker_threads_;
    mutable std::mutex control_mutex_;
    std::unordered_map<std::uint64_t, session::SessionSnapshot> session_snapshots_;
    std::unordered_map<std::uint64_t, std::vector<std::weak_ptr<session::SessionSubscriptionStream>>> session_subscribers_;
    std::unordered_set<std::uint64_t> active_session_ids_;
    std::optional<base::Status> terminal_status_;
    std::uint64_t next_connection_id_{1};
    std::atomic<std::size_t> active_connection_count_{0};
    std::atomic<std::uint64_t> last_progress_ns_{0};
    std::atomic<std::size_t> completed_sessions_{0};
    bool opened_{false};
    std::atomic<bool> stop_requested_{false};
};

}  // namespace fastfix::runtime