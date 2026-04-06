#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "fastfix/base/result.h"
#include "fastfix/base/status.h"
#include "fastfix/message/message.h"
#include "fastfix/session/session_handle.h"
#include "fastfix/session/session_key.h"

namespace fastfix::runtime {

inline constexpr std::size_t kDefaultQueueEventCapacity = 1024U;
inline constexpr std::size_t kUseApplicationQueueCapacity = 0U;
inline constexpr std::size_t kDefaultQueuePollerBatchLimit = 256U;
inline constexpr std::size_t kDrainAllQueueEvents = 0U;
inline constexpr auto kDefaultRuntimePollTimeout = std::chrono::milliseconds{50};
inline constexpr auto kDefaultRuntimeIoTimeout = std::chrono::milliseconds{5'000};

enum class SessionEventKind : std::uint32_t {
    kBound = 0,
    kActive,
    kClosed,
};

enum class RuntimeEventKind : std::uint32_t {
    kSession = 0,
    kAdminMessage,
    kApplicationMessage,
};

enum class QueueOverflowPolicy : std::uint32_t {
  kCloseSession = 0,
  kBackpressure,
  kDropNewest,
};

struct QueueApplicationOptions {
  std::size_t queue_capacity{kDefaultQueueEventCapacity};
    QueueOverflowPolicy overflow_policy{QueueOverflowPolicy::kCloseSession};
  std::size_t control_queue_capacity{kUseApplicationQueueCapacity};
    std::optional<QueueOverflowPolicy> control_overflow_policy{};
};

struct RuntimeEvent {
    RuntimeEventKind kind{RuntimeEventKind::kSession};
    SessionEventKind session_event{SessionEventKind::kBound};
    session::SessionHandle handle;
    session::SessionKey session_key;
  message::MessageRef message;
    std::string text;
    std::uint64_t timestamp_ns{0};
    bool poss_resend{false};

  [[nodiscard]] auto message_view() const -> message::MessageView {
    return message.view();
  }
};

class ApplicationCallbacks {
  public:
    virtual ~ApplicationCallbacks() = default;

    virtual auto OnSessionEvent(const RuntimeEvent& event) -> base::Status;
    virtual auto OnAdminMessage(const RuntimeEvent& event) -> base::Status;
    virtual auto OnAppMessage(const RuntimeEvent& event) -> base::Status;
};

class EchoApplication final : public ApplicationCallbacks {
  public:
    auto OnAppMessage(const RuntimeEvent& event) -> base::Status override;
};

// Queue-decoupled mode owns one worker-local queue pair per runtime worker.
// Multi-worker consumers must preserve that mapping by polling worker i with the
// handler bound to worker i.
class QueueApplication final : public ApplicationCallbacks {
  public:
    explicit QueueApplication(std::uint32_t worker_count, std::size_t queue_capacity = kDefaultQueueEventCapacity);
    QueueApplication(std::uint32_t worker_count, QueueApplicationOptions options);
    ~QueueApplication() override;

    auto OnSessionEvent(const RuntimeEvent& event) -> base::Status override;
    auto OnAdminMessage(const RuntimeEvent& event) -> base::Status override;
    auto OnAppMessage(const RuntimeEvent& event) -> base::Status override;

    [[nodiscard]] auto worker_count() const -> std::uint32_t;
    [[nodiscard]] auto overflow_policy() const -> QueueOverflowPolicy;
    [[nodiscard]] auto control_overflow_policy() const -> QueueOverflowPolicy;
    [[nodiscard]] auto overflow_events() const -> std::uint64_t;
    [[nodiscard]] auto dropped_events() const -> std::uint64_t;
    [[nodiscard]] auto app_overflow_events() const -> std::uint64_t;
    [[nodiscard]] auto app_dropped_events() const -> std::uint64_t;
    [[nodiscard]] auto control_overflow_events() const -> std::uint64_t;
    [[nodiscard]] auto control_dropped_events() const -> std::uint64_t;
    auto TryPopEvent(std::uint32_t worker_id) -> base::Result<std::optional<RuntimeEvent>>;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class QueueApplicationProvider {
  public:
    virtual ~QueueApplicationProvider() = default;

    virtual auto queue_application() -> QueueApplication& = 0;
};

struct QueueApplicationPollerOptions {
  std::size_t max_events_per_poll{kDefaultQueuePollerBatchLimit};
    bool yield_when_idle{true};
};

struct QueueApplicationRunnerThreadOptions {
  std::vector<std::uint32_t> cpu_affinity;
  std::string thread_name_prefix{"ff-app-w"};
};

enum class ManagedQueueApplicationRunnerMode : std::uint32_t {
  // Drain worker i from the runtime worker thread that already owns worker i.
  kCoScheduled = 0,
  // Run one dedicated application thread per worker; handler[i] is bound to worker i.
  kThreaded,
};

class QueueApplicationEventHandler {
  public:
    virtual ~QueueApplicationEventHandler() = default;

    virtual auto OnRuntimeEvent(const RuntimeEvent& event) -> base::Status = 0;
};

struct ManagedQueueApplicationRunnerOptions {
  ManagedQueueApplicationRunnerMode mode{ManagedQueueApplicationRunnerMode::kCoScheduled};
    // handlers[i] is bound to worker i for both co-scheduled and threaded queue mode.
    std::vector<std::unique_ptr<QueueApplicationEventHandler>> handlers;
    QueueApplicationPollerOptions poller_options{};
    QueueApplicationRunnerThreadOptions thread_options{};
};

// This single-handler poller is worker-local. Multi-worker applications must use
// PollWorkerOnce/RunWorker with the matching worker id. PollAllOnce/RunAll remain
// as single-worker convenience only.
class QueueApplicationPoller {
  public:
    QueueApplicationPoller(QueueApplication* application, QueueApplicationEventHandler* handler);
    QueueApplicationPoller(
        QueueApplication* application,
        QueueApplicationEventHandler* handler,
        QueueApplicationPollerOptions options);

    auto PollWorkerOnce(std::uint32_t worker_id) -> base::Result<std::size_t>;
    // Single-worker convenience only; multi-worker returns InvalidArgument.
    auto PollAllOnce() -> base::Result<std::size_t>;
    auto RunWorker(std::uint32_t worker_id, const std::atomic<bool>& stop_requested) -> base::Status;
    // Single-worker convenience only; multi-worker returns InvalidArgument.
    auto RunAll(const std::atomic<bool>& stop_requested) -> base::Status;

  private:
    QueueApplication* application_{nullptr};
    QueueApplicationEventHandler* handler_{nullptr};
    QueueApplicationPollerOptions options_{};
};

// Threaded queue-mode helper that binds handler[i] to worker i.
class QueueApplicationRunner {
  public:
    QueueApplicationRunner(
        QueueApplication* application,
        std::vector<std::unique_ptr<QueueApplicationEventHandler>> handlers);
    QueueApplicationRunner(
        QueueApplication* application,
        std::vector<std::unique_ptr<QueueApplicationEventHandler>> handlers,
        QueueApplicationPollerOptions options);
  QueueApplicationRunner(
    QueueApplication* application,
    std::vector<std::unique_ptr<QueueApplicationEventHandler>> handlers,
    QueueApplicationPollerOptions options,
    QueueApplicationRunnerThreadOptions thread_options);
    ~QueueApplicationRunner();

    auto Start() -> base::Status;
    auto Stop() -> base::Status;

    [[nodiscard]] auto running() const -> bool;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fastfix::runtime