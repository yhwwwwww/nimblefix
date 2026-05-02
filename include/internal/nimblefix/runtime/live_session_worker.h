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
#include <vector>

#include "nimblefix/advanced/runtime_application.h"
#include "nimblefix/base/result.h"
#include "nimblefix/base/spsc_queue.h"
#include "nimblefix/base/status.h"
#include "nimblefix/message/message_ref.h"
#include "nimblefix/profile/normalized_dictionary.h"
#include "nimblefix/runtime/config.h"
#include "nimblefix/runtime/engine.h"
#include "nimblefix/runtime/live_session_registry.h"
#include "nimblefix/runtime/shard_poller.h"
#include "nimblefix/session/admin_protocol.h"
#include "nimblefix/store/session_store.h"
#include "nimblefix/transport/transport_connection.h"

namespace nimble::runtime {

// Common base for LiveAcceptor and LiveInitiator. Holds all shared inner types,
// state, and ~40 shared method implementations. Subclasses implement the
// handful of virtual methods that differ between acceptor and initiator.
class LiveSessionWorker
{
public:
  struct Options
  {
    std::chrono::milliseconds poll_timeout{ kDefaultRuntimePollTimeout };
    std::chrono::milliseconds io_timeout{ kDefaultRuntimeIoTimeout };
    std::shared_ptr<ApplicationCallbacks> application;
    std::optional<ManagedQueueApplicationRunnerOptions> managed_queue_runner;
    std::size_t command_queue_capacity{ kDefaultQueueEventCapacity };
  };

  virtual ~LiveSessionWorker();

  [[nodiscard]] auto active_connection_count() const -> std::size_t { return active_connection_count_.load(); }

  [[nodiscard]] auto completed_session_count() const -> std::size_t { return completed_sessions_.load(); }

protected:
  enum class OutboundCommandKind : std::uint32_t
  {
    kSendApplication = 0,
    kBeginLogout,
  };

  struct OutboundCommand
  {
    OutboundCommandKind kind{ OutboundCommandKind::kSendApplication };
    std::uint64_t session_id{ 0 };
    std::uint64_t enqueue_timestamp_ns{ 0 };
    message::MessageRef message;
    std::string text;
  };

  class CommandSink final : public session::SessionCommandSink
  {
  public:
    CommandSink(LiveSessionWorker* owner, std::uint32_t worker_id, std::size_t queue_capacity)
      : owner_(owner)
      , worker_id_(worker_id)
      , queue_(queue_capacity)
    {
    }

    auto EnqueueOwnedMessage(std::uint64_t session_id, message::MessageRef message) -> base::Status override
    {
      if (!queue_.TryPush(OutboundCommand{
            .kind = OutboundCommandKind::kSendApplication,
            .session_id = session_id,
            .enqueue_timestamp_ns =
              static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()),
            .message = std::move(message),
            .text = {},
          })) {
        return base::Status::IoError("runtime outbound command queue is full");
      }
      owner_->SignalWorkerWakeup(worker_id_);
      return base::Status::Ok();
    }

    auto TrySendInlineBorrowedMessage(std::uint64_t session_id, const message::MessageRef& message)
      -> base::Result<bool> override;

    auto LoadSnapshot(std::uint64_t session_id) const -> base::Result<session::SessionSnapshot> override
    {
      return owner_->LoadSessionSnapshot(session_id);
    }

    auto Subscribe(std::uint64_t session_id, std::size_t queue_capacity)
      -> base::Result<session::SessionSubscription> override
    {
      return owner_->RegisterSessionSubscriber(session_id, queue_capacity);
    }

    auto EnqueueLogout(std::uint64_t session_id, std::string text) -> base::Status
    {
      if (!queue_.TryPush(OutboundCommand{
            .kind = OutboundCommandKind::kBeginLogout,
            .session_id = session_id,
            .enqueue_timestamp_ns =
              static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()),
            .message = {},
            .text = std::move(text),
          })) {
        return base::Status::IoError("runtime outbound command queue is full");
      }
      owner_->SignalWorkerWakeup(worker_id_);
      return base::Status::Ok();
    }

    auto TryPop() -> std::optional<OutboundCommand> { return queue_.TryPop(); }

  private:
    LiveSessionWorker* owner_{ nullptr };
    std::uint32_t worker_id_{ 0 };
    base::SpscQueue<OutboundCommand> queue_;
  };

  // Merged ActiveSession: acceptor uses routed_worker_id; initiator uses
  // host/port.
  struct ActiveSession
  {
    CounterpartyConfig counterparty;
    std::uint32_t worker_id{ 0 };
    std::uint32_t routed_worker_id{ 0 };
    session::SessionHandle handle;
    std::unique_ptr<profile::NormalizedDictionaryView> dictionary;
    std::unique_ptr<store::SessionStore> store;
    std::optional<session::AdminProtocol> protocol;
    std::string host;
    std::uint16_t port{ 0 };
  };

  struct ConnectionState
  {
    std::uint64_t connection_id{ 0 };
    transport::TransportConnection connection;
    std::unique_ptr<ActiveSession> session;
    std::uint64_t last_progress_ns{ 0 };
    std::uint64_t last_backlog_notify_ns{ 0 };
    std::optional<RuntimeEvent> pending_app_event;
    bool close_requested{ false };
    bool count_completion{ false };
    std::string close_reason;
  };

  // Initiator reconnect queue entry (stored in WorkerShardState; always empty
  // for acceptor).
  struct PendingReconnect
  {
    std::uint64_t session_id{ 0 };
    std::string host;
    std::uint16_t port{ 0 };
    std::uint32_t retry_count{ 0 };
    std::uint32_t current_backoff_ms{ 0 };
    std::uint64_t next_attempt_ns{ 0 };
    std::unique_ptr<ActiveSession> session;
  };

  struct WorkerInbox
  {
    std::mutex mutex;
    std::vector<ConnectionState> pending_connections;
  };

  // Merged WorkerShardState: acceptor uses active_connections; initiator uses
  // pending_reconnects.
  struct WorkerShardState
  {
    WorkerShardState() = default;
    WorkerShardState(const WorkerShardState&) = delete;
    auto operator=(const WorkerShardState&) -> WorkerShardState& = delete;

    WorkerShardState(WorkerShardState&& other) noexcept
      : worker_id(other.worker_id)
      , active_connections(other.active_connections.load(std::memory_order_relaxed))
      , connections(std::move(other.connections))
      , poller(std::move(other.poller))
      , connection_indices(std::move(other.connection_indices))
      , session_connection_indices(std::move(other.session_connection_indices))
      , command_sink(std::move(other.command_sink))
      , inbox(std::move(other.inbox))
      , pending_reconnects(std::move(other.pending_reconnects))
      , io_ready_state(std::move(other.io_ready_state))
      , expired_timer_ids(std::move(other.expired_timer_ids))
    {
      other.active_connections.store(0U, std::memory_order_relaxed);
    }

    auto operator=(WorkerShardState&& other) noexcept -> WorkerShardState&
    {
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
      pending_reconnects = std::move(other.pending_reconnects);
      io_ready_state = std::move(other.io_ready_state);
      expired_timer_ids = std::move(other.expired_timer_ids);
      other.active_connections.store(0U, std::memory_order_relaxed);
      return *this;
    }

    std::uint32_t worker_id{ 0 };
    std::atomic<std::size_t> active_connections{ 0 }; // acceptor: connection load balance
    std::vector<ConnectionState> connections;
    ShardPoller poller{};
    std::unordered_map<std::uint64_t, std::size_t> connection_indices;
    std::unordered_map<std::uint64_t, std::size_t> session_connection_indices;
    std::shared_ptr<CommandSink> command_sink;
    std::unique_ptr<WorkerInbox> inbox;
    std::vector<PendingReconnect> pending_reconnects; // initiator: reconnect queue
    ShardPoller::IoReadyState io_ready_state;
    std::vector<std::uint64_t> expired_timer_ids;
  };

  explicit LiveSessionWorker(Engine* engine, Options options);

  // --- Pure virtual: differ between acceptor and initiator ---

  // Set thread name and CPU affinity for a worker thread.
  virtual auto ApplyWorkerThreadSetup(Engine* engine, std::uint32_t worker_id) -> base::Status = 0;

  // Process one inbound frame for connection at connection_index.
  // Acceptor: may bind session from Logon and migrate the connection.
  // Initiator: session already exists; decodes and dispatches.
  virtual auto HandleInboundFrame(WorkerShardState& shard,
                                  std::size_t connection_index,
                                  std::span<const std::byte> frame,
                                  const codec::SessionHeaderView& header,
                                  std::uint64_t timestamp_ns) -> base::Status = 0;

  // Apply outbound frames and application messages from a protocol event.
  // Differs: acceptor calls SendApplication when options_.application ==
  // nullptr; initiator skips application_messages in that case.
  virtual auto HandleProtocolEvent(WorkerShardState& shard,
                                   ConnectionState& connection,
                                   const session::ProtocolEvent& event,
                                   std::uint64_t timestamp_ns) -> base::Status = 0;

  // Poll one connection: receive frames, retry pending events, close if needed.
  // Acceptor: must re-find connection after HandleInboundFrame (may migrate
  // shard). Initiator: no migration; simpler re-use of pointer.
  virtual auto ProcessConnection(WorkerShardState& shard,
                                 std::size_t connection_index,
                                 bool readable,
                                 std::uint64_t timestamp_ns) -> base::Status = 0;

  // Teardown and remove connection at connection_index from shard.
  // Acceptor: no reconnect logic.
  // Initiator: schedules reconnect if configured.
  virtual auto CloseConnection(WorkerShardState& shard, std::size_t connection_index, std::uint64_t timestamp_ns)
    -> void = 0;

  // --- Virtual with default (no-op): initiator overrides ---

  // Process pending reconnect entries in shard; no-op for acceptor.
  virtual auto ProcessPendingReconnects(WorkerShardState& shard, std::uint64_t timestamp_ns) -> base::Status;

  // --- Shared implementations ---

  auto PollWorkerOnce(WorkerShardState& shard, std::chrono::milliseconds timeout) -> base::Status;
  [[nodiscard]] auto ComputePollTimeout(const WorkerShardState& shard,
                                        std::chrono::milliseconds timeout,
                                        std::uint64_t timestamp_ns) const -> std::chrono::milliseconds;
  auto ProcessDueTimers(WorkerShardState& shard, std::uint64_t timestamp_ns) -> base::Status;
  auto RetryPendingAppEvent(ConnectionState& connection, std::uint64_t timestamp_ns) -> base::Status;
  auto DispatchSessionEvent(const ActiveSession& session,
                            SessionEventKind kind,
                            std::uint64_t timestamp_ns,
                            std::string text,
                            bool drain_inline) -> base::Status;
  auto DispatchAdminMessage(const ActiveSession& session, message::MessageView message, std::uint64_t timestamp_ns)
    -> base::Status;
  auto DispatchAppMessage(ConnectionState& connection, message::MessageView message, std::uint64_t timestamp_ns)
    -> base::Status;
  auto PrepareAppMessageCallback(ConnectionState& connection) -> bool;
  auto ServiceTimer(WorkerShardState& shard, ConnectionState& connection, std::uint64_t timestamp_ns) -> base::Status;
  auto SendFrame(ConnectionState& connection, const session::EncodedFrame& frame, std::uint64_t timestamp_ns)
    -> base::Status;
  auto SendFrames(ConnectionState& connection, const session::ProtocolFrameList& frames, std::uint64_t timestamp_ns)
    -> base::Status;
  auto SendFramesBatch(ConnectionState& connection,
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
  auto MarkConnectionForClose(ConnectionState& connection, std::string_view reason, bool count_completion) -> void;
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
  auto FindWorkerShard(std::uint32_t worker_id) -> WorkerShardState*;
  [[nodiscard]] auto FindWorkerShard(std::uint32_t worker_id) const -> const WorkerShardState*;
  auto SetTerminalStatus(base::Status status) -> void;
  [[nodiscard]] auto LoadTerminalStatus() const -> std::optional<base::Status>;
  [[nodiscard]] auto HasActiveSession(std::uint64_t session_id) const -> bool;
  auto RegisterActiveSession(std::uint64_t session_id) -> bool;
  auto UnregisterActiveSession(std::uint64_t session_id) -> void;
  auto MakeStore(const CounterpartyConfig& counterparty) const -> base::Result<std::unique_ptr<store::SessionStore>>;
  // Construct an ActiveSession without host/port (acceptor path).
  auto MakeActiveSessionBase(const CounterpartyConfig& counterparty, profile::NormalizedDictionaryView dictionary) const
    -> base::Result<ActiveSession>;
  auto RecordInboundMetrics(const ActiveSession& session, std::string_view msg_type) -> void;
  auto RecordOutboundMetrics(const ActiveSession& session, const session::EncodedFrame& frame) -> void;
  auto RecordProtocolFailure(const ActiveSession& session, const base::Status& status) -> void;
  auto RecordTrace(TraceEventKind kind,
                   std::uint64_t session_id,
                   std::uint32_t worker_id,
                   std::uint64_t timestamp_ns,
                   std::uint64_t arg0,
                   std::uint64_t arg1,
                   std::string_view text) -> void;

  // --- Shared state ---

  Engine* engine_{ nullptr };
  Options options_{};
  LiveSessionRegistry session_registry_;
  std::vector<WorkerShardState> worker_shards_;
  std::vector<std::jthread> worker_threads_;
  std::atomic<uint64_t> next_connection_id_{ 1 };
  std::atomic<std::size_t> active_connection_count_{ 0 };
  std::atomic<std::uint64_t> last_progress_ns_{ 0 };
  std::atomic<std::size_t> completed_sessions_{ 0 };
  std::atomic<std::size_t> pending_reconnect_count_{ 0 };
  std::atomic<bool> stop_requested_{ false };
};

} // namespace nimble::runtime
