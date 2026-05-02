#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "nimblefix/advanced/session_handle.h"
#include "nimblefix/base/result.h"
#include "nimblefix/base/status.h"
#include "nimblefix/message/message_ref.h"
#include "nimblefix/runtime/application.h"
#include "nimblefix/session/session_key.h"

namespace nimble::runtime {

/// Advanced raw runtime callback and queue-delivery surface.
///
/// Prefer `runtime::Application<Profile>`, generated handlers, and typed
/// `runtime::Session<Profile>` / `runtime::InlineSession<Profile>` for normal
/// business flows. Use this header when you intentionally need the untyped
/// runtime callback, queue-drain, or diagnostic surface.

/// Session lifecycle events surfaced to applications.
enum class SessionEventKind : std::uint32_t
{
  kBound = 0,
  kActive,
  kClosed,
};

/// Top-level runtime event categories delivered to applications.
enum class RuntimeEventKind : std::uint32_t
{
  kSession = 0,
  kAdminMessage,
  kApplicationMessage,
};

/// Queue behavior when a worker-local application queue is full.
enum class QueueOverflowPolicy : std::uint32_t
{
  /// Fail the runtime callback so the session is closed by the caller above.
  kCloseSession = 0,
  /// Report Busy and let the caller decide when to retry.
  kBackpressure,
  /// Silently keep running and drop the newest event.
  kDropNewest,
};

/// Queue-decoupled application settings for QueueApplication.
///
/// Design intent: split control traffic from application traffic while keeping
/// both queues worker-local and SPSC-friendly.
struct QueueApplicationOptions
{
  // Capacity of each worker-local application-message queue.
  std::size_t queue_capacity{ kDefaultQueueEventCapacity };
  // Overflow policy for application-message queues.
  QueueOverflowPolicy overflow_policy{ QueueOverflowPolicy::kCloseSession };
  // Capacity of each worker-local control queue. Zero means reuse queue_capacity.
  std::size_t control_queue_capacity{ kUseApplicationQueueCapacity };
  // Optional control-queue-specific overflow policy.
  std::optional<QueueOverflowPolicy> control_overflow_policy{};
};

/// Event payload delivered through inline callbacks or queue-drained handlers.
///
/// Design intent: keep session handle, identity, decoded message, and transport
/// metadata together so applications do not need to reach back into the runtime
/// to interpret one callback.
///
/// Boundary condition: message is populated only for admin/app message kinds,
/// and session_event is meaningful only when kind == kSession.
struct RuntimeEvent
{
  RuntimeEventKind kind{ RuntimeEventKind::kSession };
  SessionEventKind session_event{ SessionEventKind::kBound };
  session::SessionHandle handle;
  session::SessionKey session_key;
  message::MessageRef message;
  std::string text;
  std::uint64_t timestamp_ns{ 0 };
  bool poss_resend{ false };
  bool is_warmup{ false };

  [[nodiscard]] auto message_view() const -> message::MessageView { return message.view(); }
};

/// Application callback surface used by live runtimes.
///
/// Design intent: give integrators one minimal virtual interface for inline
/// delivery, with QueueApplication available as the decoupling adapter when
/// they do not want to run business logic on the runtime worker.
class ApplicationCallbacks
{
public:
  virtual ~ApplicationCallbacks() = default;

  // Session event ordering and send boundaries:
  // - kBound: Logon matched and the session is attached to a runtime worker,
  //   but replay/recovery may still be in progress. Use this for snapshot or
  //   subscription setup. Borrowed SessionHandle::Send(...) is valid only when
  //   this callback is delivered inline on the runtime worker.
  // - kActive: Recovery is complete and application traffic may flow. This is
  //   the normal "ready to send first business message" event.
  // - kClosed: Transport is closed; do not send from this event.
  /// Handle one session lifecycle event.
  ///
  /// \param event Runtime event payload.
  /// \return Ok on success, otherwise an error propagated back into the runtime.
  virtual auto OnSessionEvent(const RuntimeEvent& event) -> base::Status;
  // Inline admin callbacks may use borrowed SessionHandle::Send(...) only while
  // executing on the runtime worker. Queue-drained handlers must use owned
  // send refs.
  /// Handle one decoded administrative message.
  ///
  /// \param event Runtime event payload.
  /// \return Ok on success, otherwise an error propagated back into the runtime.
  virtual auto OnAdminMessage(const RuntimeEvent& event) -> base::Status;
  // Inline application callbacks follow the same rule: borrowed
  // SessionHandle::Send(...) is valid only from the direct runtime callback,
  // never from a queue-drained application thread.
  /// Handle one decoded application message.
  ///
  /// \param event Runtime event payload.
  /// \return Ok on success, otherwise an error propagated back into the runtime.
  virtual auto OnAppMessage(const RuntimeEvent& event) -> base::Status;
};

/// Minimal demo application that echoes application messages back inline.
///
/// Boundary condition: this only works in inline callback context because it
/// uses SendInlineBorrowed on the incoming message view.
class EchoApplication final : public ApplicationCallbacks
{
public:
  auto OnAppMessage(const RuntimeEvent& event) -> base::Status override;
};

// Queue-decoupled mode owns one worker-local queue pair per runtime worker.
// Multi-worker consumers must preserve that mapping by polling worker i with
// the handler bound to worker i.
/// Queue-backed ApplicationCallbacks adapter.
///
/// Design intent: capture runtime events on the worker thread and hand them to
/// a later consumer through worker-local SPSC queues.
///
/// Performance/lifecycle contract:
/// - one control queue and one application queue exist per runtime worker
/// - TryPopEvent always drains control traffic before application traffic
/// - worker ids are part of the public contract; callers must preserve handler
///   to worker affinity in multi-worker mode
class QueueApplication final : public ApplicationCallbacks
{
public:
  /// Construct with a shared queue capacity for control and application queues.
  ///
  /// \param worker_count Runtime worker count. Zero is normalized internally to one worker.
  /// \param queue_capacity Per-worker queue capacity.
  explicit QueueApplication(std::uint32_t worker_count, std::size_t queue_capacity = kDefaultQueueEventCapacity);

  /// Construct with explicit queue options.
  ///
  /// \param worker_count Runtime worker count. Zero is normalized internally to one worker.
  /// \param options Queue sizing and overflow policy.
  QueueApplication(std::uint32_t worker_count, QueueApplicationOptions options);
  ~QueueApplication() override;

  auto OnSessionEvent(const RuntimeEvent& event) -> base::Status override;
  auto OnAdminMessage(const RuntimeEvent& event) -> base::Status override;
  auto OnAppMessage(const RuntimeEvent& event) -> base::Status override;

  /// \return Number of worker-local queue pairs.
  [[nodiscard]] auto worker_count() const -> std::uint32_t;
  /// \return Overflow policy for application-message queues.
  [[nodiscard]] auto overflow_policy() const -> QueueOverflowPolicy;
  /// \return Overflow policy for control queues.
  [[nodiscard]] auto control_overflow_policy() const -> QueueOverflowPolicy;
  /// \return Total number of queue overflow incidents.
  [[nodiscard]] auto overflow_events() const -> std::uint64_t;
  /// \return Total number of dropped events across all queues.
  [[nodiscard]] auto dropped_events() const -> std::uint64_t;
  /// \return Overflow incidents observed on application-message queues.
  [[nodiscard]] auto app_overflow_events() const -> std::uint64_t;
  /// \return Dropped events observed on application-message queues.
  [[nodiscard]] auto app_dropped_events() const -> std::uint64_t;
  /// \return Overflow incidents observed on control queues.
  [[nodiscard]] auto control_overflow_events() const -> std::uint64_t;
  /// \return Dropped events observed on control queues.
  [[nodiscard]] auto control_dropped_events() const -> std::uint64_t;

  /// Try to pop one event for a specific worker.
  ///
  /// Control events are returned before application-message events for the same
  /// worker. The call never blocks.
  ///
  /// \param worker_id Worker queue to drain.
  /// \return Next event for that worker, nullopt when idle, or an error status.
  auto TryPopEvent(std::uint32_t worker_id) -> base::Result<std::optional<RuntimeEvent>>;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Interface for components that expose a QueueApplication instance.
class QueueApplicationProvider
{
public:
  virtual ~QueueApplicationProvider() = default;

  virtual auto queue_application() -> QueueApplication& = 0;
};

/// Poll-step tuning for queue-drained application handlers.
struct QueueApplicationPollerOptions
{
  // Maximum events to drain from one worker on a single PollWorkerOnce call.
  std::size_t max_events_per_poll{ kDefaultQueuePollerBatchLimit };
  // Yield the current thread when no work was drained.
  bool yield_when_idle{ true };
};

/// Thread naming and CPU affinity for QueueApplicationRunner worker threads.
struct QueueApplicationRunnerThreadOptions
{
  std::vector<std::uint32_t> cpu_affinity;
  std::string thread_name_prefix{ "nf-app-w" };
};

/// Execution strategy for the managed queue application runner.
enum class ManagedQueueApplicationRunnerMode : std::uint32_t
{
  // Drain worker i from the runtime worker thread that already owns worker i.
  kCoScheduled = 0,
  // Run one dedicated application thread per worker; handler[i] is bound to
  // worker i.
  kThreaded,
};

/// Business-logic sink for queue-drained runtime events.
class QueueApplicationEventHandler
{
public:
  virtual ~QueueApplicationEventHandler() = default;

  /// Handle one queue-drained runtime event.
  ///
  /// \param event Runtime event payload.
  /// \return Ok on success, otherwise an error that stops the poller/runner.
  virtual auto OnRuntimeEvent(const RuntimeEvent& event) -> base::Status = 0;
};

/// Configuration for Engine-managed queue runners.
///
/// handlers[i] is part of the contract: it is permanently bound to worker i in
/// both co-scheduled and threaded mode.
struct ManagedQueueApplicationRunnerOptions
{
  ManagedQueueApplicationRunnerMode mode{ ManagedQueueApplicationRunnerMode::kCoScheduled };
  // handlers[i] is bound to worker i for both co-scheduled and threaded queue
  // mode.
  std::vector<std::unique_ptr<QueueApplicationEventHandler>> handlers;
  QueueApplicationPollerOptions poller_options{};
  QueueApplicationRunnerThreadOptions thread_options{};
};

// This single-handler poller is worker-local. Multi-worker applications must
// use PollWorkerOnce/RunWorker with the matching worker id. PollAllOnce/RunAll
// remain as single-worker convenience only.
/// Synchronous poll loop over QueueApplication.
///
/// Design intent: provide a tiny, explicit abstraction for draining one
/// worker-local queue with backpressure and handler errors surfaced as Status.
class QueueApplicationPoller
{
public:
  /// \param application Queue application to drain.
  /// \param handler Event handler invoked for each drained event.
  QueueApplicationPoller(QueueApplication* application, QueueApplicationEventHandler* handler);

  /// \param application Queue application to drain.
  /// \param handler Event handler invoked for each drained event.
  /// \param options Poll-step tuning.
  QueueApplicationPoller(QueueApplication* application,
                         QueueApplicationEventHandler* handler,
                         QueueApplicationPollerOptions options);

  /// Drain up to max_events_per_poll events for one worker.
  ///
  /// \param worker_id Worker queue to drain.
  /// \return Number of drained events, or an error status.
  auto PollWorkerOnce(std::uint32_t worker_id) -> base::Result<std::size_t>;
  // Single-worker convenience only; multi-worker returns InvalidArgument.
  /// Drain all workers once in single-worker mode only.
  auto PollAllOnce() -> base::Result<std::size_t>;

  /// Run a blocking poll loop for one worker until stop_requested or error.
  ///
  /// \param worker_id Worker queue to drain.
  /// \param stop_requested Shared stop flag.
  /// \return Ok on clean stop, otherwise the first handler or queue error.
  auto RunWorker(std::uint32_t worker_id, const std::atomic<bool>& stop_requested) -> base::Status;
  // Single-worker convenience only; multi-worker returns InvalidArgument.
  /// Run a blocking poll loop over all workers in single-worker mode only.
  auto RunAll(const std::atomic<bool>& stop_requested) -> base::Status;

private:
  QueueApplication* application_{ nullptr };
  QueueApplicationEventHandler* handler_{ nullptr };
  QueueApplicationPollerOptions options_{};
};

// Threaded queue-mode helper that binds handler[i] to worker i.
/// Threaded helper that owns one queue poller thread per worker.
///
/// Design intent: give applications a ready-made threaded queue-drain path when
/// they do not want to integrate poll loops into their own scheduling layer.
class QueueApplicationRunner
{
public:
  /// \param application Queue application to drain.
  /// \param handlers One non-null handler per worker.
  QueueApplicationRunner(QueueApplication* application,
                         std::vector<std::unique_ptr<QueueApplicationEventHandler>> handlers);

  /// \param application Queue application to drain.
  /// \param handlers One non-null handler per worker.
  /// \param options Poll-step tuning.
  QueueApplicationRunner(QueueApplication* application,
                         std::vector<std::unique_ptr<QueueApplicationEventHandler>> handlers,
                         QueueApplicationPollerOptions options);

  /// \param application Queue application to drain.
  /// \param handlers One non-null handler per worker.
  /// \param options Poll-step tuning.
  /// \param thread_options Thread naming and affinity options.
  QueueApplicationRunner(QueueApplication* application,
                         std::vector<std::unique_ptr<QueueApplicationEventHandler>> handlers,
                         QueueApplicationPollerOptions options,
                         QueueApplicationRunnerThreadOptions thread_options);
  ~QueueApplicationRunner();

  /// Start one poller thread per worker.
  ///
  /// Boundary condition: Start fails if handler count does not match
  /// worker_count, any handler is null, or affinity vectors are oversized.
  ///
  /// \return Ok on success, otherwise an error status.
  auto Start() -> base::Status;

  /// Request stop, join worker threads, and surface the first worker error.
  ///
  /// \return Ok on clean shutdown, otherwise the first worker error.
  auto Stop() -> base::Status;

  /// \return True when runner threads are currently active.
  [[nodiscard]] auto running() const -> bool;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace nimble::runtime
