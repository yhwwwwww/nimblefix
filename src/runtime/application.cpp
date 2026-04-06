#include "fastfix/runtime/application.h"

#include <atomic>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "fastfix/base/spsc_queue.h"
#include "fastfix/runtime/thread_affinity.h"

namespace fastfix::runtime {

namespace {

enum class QueueResourceClass : std::uint32_t {
    kControl = 0,
    kApplication,
};

auto ResourceClassFor(const RuntimeEvent& event) -> QueueResourceClass {
    return event.kind == RuntimeEventKind::kApplicationMessage
               ? QueueResourceClass::kApplication
               : QueueResourceClass::kControl;
}

auto ValidateWorkerLocalPoller(
    QueueApplication* application,
    QueueApplicationEventHandler* handler) -> base::Status {
    if (application == nullptr) {
        return base::Status::InvalidArgument("queue application poller requires an application");
    }
    if (handler == nullptr) {
        return base::Status::InvalidArgument("queue application poller requires an event handler");
    }
    return base::Status::Ok();
}

auto ValidateAllWorkersPoller(
    QueueApplication* application,
    QueueApplicationEventHandler* handler) -> base::Status {
    auto status = ValidateWorkerLocalPoller(application, handler);
    if (!status.ok()) {
        return status;
    }
    if (application->worker_count() > 1U) {
        return base::Status::InvalidArgument(
            "queue application poller all-workers mode is only available for single-worker applications; use PollWorkerOnce/RunWorker per worker");
    }
    return base::Status::Ok();
}

auto ApplyQueueApplicationWorkerThreadSetup(
    const QueueApplicationRunnerThreadOptions& options,
    std::uint32_t worker_id) -> base::Status {
    SetCurrentThreadName(options.thread_name_prefix + std::to_string(worker_id));
    if (worker_id >= options.cpu_affinity.size()) {
        return base::Status::Ok();
    }

    return ApplyCurrentThreadAffinity(
        options.cpu_affinity[worker_id],
        "application worker " + std::to_string(worker_id));
}

}  // namespace

auto ApplicationCallbacks::OnSessionEvent(const RuntimeEvent& event) -> base::Status {
    (void)event;
    return base::Status::Ok();
}

auto ApplicationCallbacks::OnAdminMessage(const RuntimeEvent& event) -> base::Status {
    (void)event;
    return base::Status::Ok();
}

auto ApplicationCallbacks::OnAppMessage(const RuntimeEvent& event) -> base::Status {
    (void)event;
    return base::Status::Ok();
}

auto EchoApplication::OnAppMessage(const RuntimeEvent& event) -> base::Status {
    return event.handle.SendBorrowed(event.message);
}

struct QueueApplication::Impl {
    explicit Impl(std::uint32_t worker_count, QueueApplicationOptions options)
        : options(options) {
        const auto count = worker_count == 0U ? 1U : worker_count;
        const auto control_capacity = resolved_control_queue_capacity();
        control_queues.reserve(count);
        app_queues.reserve(count);
        for (std::uint32_t worker_id = 0; worker_id < count; ++worker_id) {
            control_queues.push_back(std::make_unique<base::SpscQueue<RuntimeEvent>>(control_capacity));
            app_queues.push_back(std::make_unique<base::SpscQueue<RuntimeEvent>>(options.queue_capacity));
        }
    }

    [[nodiscard]] auto resolved_control_queue_capacity() const -> std::size_t {
        return options.control_queue_capacity == kUseApplicationQueueCapacity
                   ? options.queue_capacity
                   : options.control_queue_capacity;
    }

    [[nodiscard]] auto resolved_control_overflow_policy() const -> QueueOverflowPolicy {
        return options.control_overflow_policy.value_or(options.overflow_policy);
    }

    auto Push(const RuntimeEvent& event) -> base::Status {
        const auto resource_class = ResourceClassFor(event);
        const auto worker_id = event.handle.worker_id();
        if (worker_id >= control_queues.size()) {
            return base::Status::NotFound("runtime application queue worker was not found");
        }

        auto* queue = resource_class == QueueResourceClass::kControl ? control_queues[worker_id].get() : app_queues[worker_id].get();
        const auto overflow_policy =
            resource_class == QueueResourceClass::kControl ? resolved_control_overflow_policy() : options.overflow_policy;
        if (queue->TryPush(event)) {
            return base::Status::Ok();
        }

        ++overflow_events;
        if (resource_class == QueueResourceClass::kControl) {
            ++control_overflow_events;
        } else {
            ++app_overflow_events;
        }

        switch (overflow_policy) {
            case QueueOverflowPolicy::kCloseSession:
                return base::Status::IoError("runtime application queue is full");
            case QueueOverflowPolicy::kBackpressure:
                return base::Status::Busy("runtime application queue is full");
            case QueueOverflowPolicy::kDropNewest:
                ++dropped_events;
                if (resource_class == QueueResourceClass::kControl) {
                    ++control_dropped_events;
                } else {
                    ++app_dropped_events;
                }
                return base::Status::Ok();
        }

            return base::Status::InvalidArgument("runtime application queue overflow policy is invalid");
    }

    QueueApplicationOptions options;
    std::vector<std::unique_ptr<base::SpscQueue<RuntimeEvent>>> control_queues;
    std::vector<std::unique_ptr<base::SpscQueue<RuntimeEvent>>> app_queues;
    std::atomic<std::uint64_t> overflow_events{0};
    std::atomic<std::uint64_t> dropped_events{0};
    std::atomic<std::uint64_t> app_overflow_events{0};
    std::atomic<std::uint64_t> app_dropped_events{0};
    std::atomic<std::uint64_t> control_overflow_events{0};
    std::atomic<std::uint64_t> control_dropped_events{0};
};

QueueApplication::QueueApplication(std::uint32_t worker_count, std::size_t queue_capacity)
    : QueueApplication(worker_count, QueueApplicationOptions{.queue_capacity = queue_capacity}) {
}

QueueApplication::QueueApplication(std::uint32_t worker_count, QueueApplicationOptions options)
    : impl_(std::make_unique<Impl>(worker_count, options)) {
}

QueueApplication::~QueueApplication() = default;

auto QueueApplication::OnSessionEvent(const RuntimeEvent& event) -> base::Status {
    return impl_->Push(event);
}

auto QueueApplication::OnAdminMessage(const RuntimeEvent& event) -> base::Status {
    return impl_->Push(event);
}

auto QueueApplication::OnAppMessage(const RuntimeEvent& event) -> base::Status {
    return impl_->Push(event);
}

auto QueueApplication::worker_count() const -> std::uint32_t {
    return static_cast<std::uint32_t>(impl_->app_queues.size());
}

auto QueueApplication::overflow_policy() const -> QueueOverflowPolicy {
    return impl_->options.overflow_policy;
}

auto QueueApplication::control_overflow_policy() const -> QueueOverflowPolicy {
    return impl_->resolved_control_overflow_policy();
}

auto QueueApplication::overflow_events() const -> std::uint64_t {
    return impl_->overflow_events.load();
}

auto QueueApplication::dropped_events() const -> std::uint64_t {
    return impl_->dropped_events.load();
}

auto QueueApplication::app_overflow_events() const -> std::uint64_t {
    return impl_->app_overflow_events.load();
}

auto QueueApplication::app_dropped_events() const -> std::uint64_t {
    return impl_->app_dropped_events.load();
}

auto QueueApplication::control_overflow_events() const -> std::uint64_t {
    return impl_->control_overflow_events.load();
}

auto QueueApplication::control_dropped_events() const -> std::uint64_t {
    return impl_->control_dropped_events.load();
}

auto QueueApplication::TryPopEvent(std::uint32_t worker_id) -> base::Result<std::optional<RuntimeEvent>> {
    if (worker_id >= impl_->app_queues.size()) {
        return base::Status::NotFound("runtime application queue worker was not found");
    }

    auto control_event = impl_->control_queues[worker_id]->TryPop();
    if (control_event.has_value()) {
        return std::optional<RuntimeEvent>{std::move(*control_event)};
    }
    return impl_->app_queues[worker_id]->TryPop();
}

QueueApplicationPoller::QueueApplicationPoller(QueueApplication* application, QueueApplicationEventHandler* handler)
    : QueueApplicationPoller(application, handler, QueueApplicationPollerOptions{}) {
}

QueueApplicationPoller::QueueApplicationPoller(
    QueueApplication* application,
    QueueApplicationEventHandler* handler,
    QueueApplicationPollerOptions options)
    : application_(application),
      handler_(handler),
      options_(options) {
}

auto QueueApplicationPoller::PollWorkerOnce(std::uint32_t worker_id) -> base::Result<std::size_t> {
    auto status = ValidateWorkerLocalPoller(application_, handler_);
    if (!status.ok()) {
        return status;
    }

    const auto max_events = options_.max_events_per_poll == kDrainAllQueueEvents
                                ? std::numeric_limits<std::size_t>::max()
                                : options_.max_events_per_poll;

    std::size_t drained = 0U;
    while (drained < max_events) {
        auto event = application_->TryPopEvent(worker_id);
        if (!event.ok()) {
            return event.status();
        }
        if (!event.value().has_value()) {
            return drained;
        }

        auto status = handler_->OnRuntimeEvent(*event.value());
        if (!status.ok()) {
            return status;
        }
        ++drained;
    }

    return drained;
}

auto QueueApplicationPoller::PollAllOnce() -> base::Result<std::size_t> {
    auto status = ValidateAllWorkersPoller(application_, handler_);
    if (!status.ok()) {
        return status;
    }

    std::size_t drained = 0U;
    for (std::uint32_t worker_id = 0; worker_id < application_->worker_count(); ++worker_id) {
        auto worker_drained = PollWorkerOnce(worker_id);
        if (!worker_drained.ok()) {
            return worker_drained.status();
        }
        drained += worker_drained.value();
    }
    return drained;
}

auto QueueApplicationPoller::RunWorker(std::uint32_t worker_id, const std::atomic<bool>& stop_requested) -> base::Status {
    while (!stop_requested.load()) {
        auto drained = PollWorkerOnce(worker_id);
        if (!drained.ok()) {
            return drained.status();
        }
        if (drained.value() == 0U && options_.yield_when_idle) {
            std::this_thread::yield();
        }
    }
    return base::Status::Ok();
}

auto QueueApplicationPoller::RunAll(const std::atomic<bool>& stop_requested) -> base::Status {
    while (!stop_requested.load()) {
        auto drained = PollAllOnce();
        if (!drained.ok()) {
            return drained.status();
        }
        if (drained.value() == 0U && options_.yield_when_idle) {
            std::this_thread::yield();
        }
    }
    return base::Status::Ok();
}

struct QueueApplicationRunner::Impl {
    QueueApplication* application{nullptr};
    std::vector<std::unique_ptr<QueueApplicationEventHandler>> handlers;
    QueueApplicationPollerOptions options{};
    QueueApplicationRunnerThreadOptions thread_options{};
    std::vector<std::jthread> threads;
    std::vector<std::optional<base::Status>> worker_statuses;
    std::atomic<bool> stop_requested{false};
    bool running{false};
};

QueueApplicationRunner::QueueApplicationRunner(
    QueueApplication* application,
    std::vector<std::unique_ptr<QueueApplicationEventHandler>> handlers)
    : QueueApplicationRunner(
          application,
          std::move(handlers),
          QueueApplicationPollerOptions{},
          QueueApplicationRunnerThreadOptions{}) {
}

QueueApplicationRunner::QueueApplicationRunner(
    QueueApplication* application,
    std::vector<std::unique_ptr<QueueApplicationEventHandler>> handlers,
    QueueApplicationPollerOptions options)
    : QueueApplicationRunner(
          application,
          std::move(handlers),
          options,
          QueueApplicationRunnerThreadOptions{}) {
}

QueueApplicationRunner::QueueApplicationRunner(
    QueueApplication* application,
    std::vector<std::unique_ptr<QueueApplicationEventHandler>> handlers,
    QueueApplicationPollerOptions options,
    QueueApplicationRunnerThreadOptions thread_options)
    : impl_(std::make_unique<Impl>()) {
    impl_->application = application;
    impl_->handlers = std::move(handlers);
    impl_->options = options;
    impl_->thread_options = std::move(thread_options);
}

QueueApplicationRunner::~QueueApplicationRunner() {
    static_cast<void>(Stop());
}

auto QueueApplicationRunner::Start() -> base::Status {
    if (impl_->application == nullptr) {
        return base::Status::InvalidArgument("queue application runner requires an application");
    }
    if (impl_->running) {
        return base::Status::AlreadyExists("queue application runner is already running");
    }
    if (impl_->handlers.size() != impl_->application->worker_count()) {
        return base::Status::InvalidArgument("queue application runner requires one handler per worker");
    }
    if (impl_->thread_options.cpu_affinity.size() > impl_->application->worker_count()) {
        return base::Status::InvalidArgument(
            "queue application runner cpu_affinity must not contain more entries than worker_count");
    }
    for (const auto& handler : impl_->handlers) {
        if (handler == nullptr) {
            return base::Status::InvalidArgument("queue application runner handlers must not be null");
        }
    }

    impl_->worker_statuses.assign(impl_->handlers.size(), std::nullopt);
    impl_->threads.clear();
    impl_->threads.reserve(impl_->handlers.size());
    impl_->stop_requested.store(false);
    impl_->running = true;

    for (std::uint32_t worker_id = 0; worker_id < impl_->application->worker_count(); ++worker_id) {
        impl_->threads.emplace_back([impl = impl_.get(), worker_id]() {
            auto thread_status = ApplyQueueApplicationWorkerThreadSetup(impl->thread_options, worker_id);
            if (!thread_status.ok()) {
                impl->stop_requested.store(true);
                impl->worker_statuses[worker_id] = thread_status;
                return;
            }
            QueueApplicationPoller poller(impl->application, impl->handlers[worker_id].get(), impl->options);
            impl->worker_statuses[worker_id] = poller.RunWorker(worker_id, impl->stop_requested);
        });
    }

    return base::Status::Ok();
}

auto QueueApplicationRunner::Stop() -> base::Status {
    if (impl_ == nullptr || !impl_->running) {
        return base::Status::Ok();
    }

    impl_->stop_requested.store(true);
    impl_->threads.clear();
    impl_->running = false;

    for (const auto& status : impl_->worker_statuses) {
        if (status.has_value() && !status->ok()) {
            return *status;
        }
    }
    return base::Status::Ok();
}

auto QueueApplicationRunner::running() const -> bool {
    return impl_ != nullptr && impl_->running;
}

}  // namespace fastfix::runtime