class CommandSink;
struct ActiveSession;
struct ListenerState;
struct ConnectionState;
struct WorkerInbox;
struct WorkerShardState;
struct Impl;

auto PollOnce(std::chrono::milliseconds timeout) -> base::Status;
[[nodiscard]] auto ComputePollTimeout(std::chrono::milliseconds timeout, std::uint64_t timestamp_ns) const
	-> std::chrono::milliseconds;
[[nodiscard]] auto ComputePollTimeout(const WorkerShardState& shard,
																std::chrono::milliseconds timeout,
																std::uint64_t timestamp_ns) const -> std::chrono::milliseconds;
auto PollWorkerOnce(WorkerShardState& shard, std::chrono::milliseconds timeout) -> base::Status;
auto AcceptReadyListener(std::size_t listener_index, std::uint64_t timestamp_ns) -> base::Status;
auto ProcessConnection(WorkerShardState& shard,
						 std::size_t connection_index,
						 bool readable,
						 std::uint64_t timestamp_ns) -> base::Status;
auto MigrateConnectionToRoutedWorker(WorkerShardState& shard, std::size_t connection_index) -> base::Status;
auto ProcessDueTimers(WorkerShardState& shard, std::uint64_t timestamp_ns) -> base::Status;
auto RetryPendingAppEvent(ConnectionState& connection, std::uint64_t timestamp_ns) -> base::Status;
auto RetryPendingAppEvents(WorkerShardState& shard, std::uint64_t timestamp_ns) -> base::Status;
auto BindConnectionFromLogon(WorkerShardState& shard,
							 std::size_t connection_index,
							 const codec::SessionHeaderView& header,
							 std::uint64_t timestamp_ns) -> base::Result<ConnectionState*>;
auto HandleInboundFrame(WorkerShardState& shard,
							std::size_t connection_index,
							std::span<const std::byte> frame,
							const codec::SessionHeaderView& header,
							std::uint64_t timestamp_ns) -> base::Status;
auto HandleProtocolEvent(WorkerShardState& shard,
							 ConnectionState& connection,
							 const session::ProtocolEvent& event,
							 std::uint64_t timestamp_ns) -> base::Status;
auto DispatchSessionEvent(const ActiveSession& session,
							SessionEventKind kind,
							std::uint64_t timestamp_ns,
							std::string text,
							bool drain_inline) -> base::Status;
auto DispatchAdminMessage(const ActiveSession& session, message::MessageView message, std::uint64_t timestamp_ns)
	-> base::Status;
auto DispatchAppMessage(ConnectionState& connection, message::MessageView message, std::uint64_t timestamp_ns)
	-> base::Status;
auto ServiceTimer(WorkerShardState& shard, ConnectionState& connection, std::uint64_t timestamp_ns) -> base::Status;
auto SendFrame(ConnectionState& connection, const session::EncodedFrame& frame, std::uint64_t timestamp_ns)
	-> base::Status;
auto SendFrames(ConnectionState& connection,
						const session::ProtocolFrameCollection& frames,
						std::uint64_t timestamp_ns) -> base::Status;
auto SendFramesBatch(ConnectionState& connection,
						 const session::ProtocolFrameCollection& frames,
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
auto ReassignConnectionToWorker(WorkerShardState& source_shard,
									 std::size_t connection_index,
									 std::uint32_t target_worker_id) -> ConnectionState*;
auto CloseConnection(WorkerShardState& shard, std::size_t connection_index, std::uint64_t timestamp_ns) -> void;
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
[[nodiscard]] auto SelectAcceptWorkerId(std::uint32_t worker_hint) const -> std::uint32_t;
auto FindWorkerShard(std::uint32_t worker_id) -> WorkerShardState*;
[[nodiscard]] auto FindWorkerShard(std::uint32_t worker_id) const -> const WorkerShardState*;
auto SetTerminalStatus(base::Status status) -> void;
[[nodiscard]] auto LoadTerminalStatus() const -> std::optional<base::Status>;
[[nodiscard]] auto HasActiveSession(std::uint64_t session_id) const -> bool;
auto RegisterActiveSession(std::uint64_t session_id) -> bool;
auto UnregisterActiveSession(std::uint64_t session_id) -> void;
auto MakeStore(const CounterpartyConfig& counterparty) const -> base::Result<std::unique_ptr<store::SessionStore>>;
auto MakeActiveSession(const CounterpartyConfig& counterparty,
						 profile::NormalizedDictionaryView dictionary) const
	-> base::Result<std::unique_ptr<ActiveSession>>;
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