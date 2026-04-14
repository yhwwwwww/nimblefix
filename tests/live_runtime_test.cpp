#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <future>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "fastfix/runtime/application.h"
#include "fastfix/runtime/engine.h"
#include "fastfix/runtime/io_poller.h"
#include "fastfix/runtime/live_acceptor.h"
#include "fastfix/session/admin_protocol.h"
#include "fastfix/store/memory_store.h"
#include "fastfix/transport/tcp_transport.h"

#include "test_support.h"

namespace {

class MixedApplication final : public fastfix::runtime::ApplicationCallbacks,
                               public fastfix::runtime::QueueApplicationProvider {
  public:
        explicit MixedApplication(std::uint32_t worker_count, std::uint64_t queued_session_id = 3102U)
                : queue_(worker_count),
                    queued_session_id_(queued_session_id) {
    }

    auto OnSessionEvent(const fastfix::runtime::RuntimeEvent& event) -> fastfix::base::Status override {
        ++session_events_;
        auto status = TrackSession(event.handle);
        if (!status.ok()) {
            return status;
        }
        if (event.handle.session_id() == queued_session_id_) {
            return queue_.OnSessionEvent(event);
        }
        return fastfix::base::Status::Ok();
    }

    auto OnAdminMessage(const fastfix::runtime::RuntimeEvent& event) -> fastfix::base::Status override {
        ++admin_events_;
        if (event.handle.session_id() == queued_session_id_) {
            return queue_.OnAdminMessage(event);
        }
        return fastfix::base::Status::Ok();
    }

    auto OnAppMessage(const fastfix::runtime::RuntimeEvent& event) -> fastfix::base::Status override {
        ++application_events_;
        if (event.handle.session_id() == queued_session_id_) {
            ++queued_application_events_;
            return queue_.OnAppMessage(event);
        }

        ++inline_application_events_;
        return event.handle.SendBorrowed(event.message);
    }

    [[nodiscard]] auto worker_count() const -> std::uint32_t {
        return queue_.worker_count();
    }

    auto TryPopEvent(std::uint32_t worker_id)
        -> fastfix::base::Result<std::optional<fastfix::runtime::RuntimeEvent>> {
        return queue_.TryPopEvent(worker_id);
    }

    auto queue_application() -> fastfix::runtime::QueueApplication& override {
        return queue_;
    }

    [[nodiscard]] auto session_events() const -> std::uint64_t {
        return session_events_.load();
    }

    [[nodiscard]] auto admin_events() const -> std::uint64_t {
        return admin_events_.load();
    }

    [[nodiscard]] auto application_events() const -> std::uint64_t {
        return application_events_.load();
    }

    [[nodiscard]] auto inline_application_events() const -> std::uint64_t {
        return inline_application_events_.load();
    }

    [[nodiscard]] auto queued_application_events() const -> std::uint64_t {
        return queued_application_events_.load();
    }

    [[nodiscard]] auto SnapshotFor(std::uint64_t session_id) const -> std::optional<fastfix::session::SessionSnapshot> {
        std::lock_guard lock(mutex_);
        const auto it = snapshots_.find(session_id);
        if (it == snapshots_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    auto DrainSubscription(std::uint64_t session_id)
        -> fastfix::base::Result<std::vector<fastfix::session::SessionNotification>> {
        fastfix::session::SessionSubscription subscription;
        {
            std::lock_guard lock(mutex_);
            const auto it = subscriptions_.find(session_id);
            if (it == subscriptions_.end()) {
                return fastfix::base::Status::NotFound("session subscription was not registered");
            }
            subscription = it->second;
        }

        std::vector<fastfix::session::SessionNotification> notifications;
        while (true) {
            auto notification = subscription.TryPop();
            if (!notification.ok()) {
                return notification.status();
            }
            if (!notification.value().has_value()) {
                return notifications;
            }
            notifications.push_back(std::move(*notification.value()));
        }
    }

  private:
    auto TrackSession(const fastfix::session::SessionHandle& handle) -> fastfix::base::Status {
        auto snapshot = handle.Snapshot();
        if (!snapshot.ok()) {
            return snapshot.status();
        }

        std::lock_guard lock(mutex_);
        snapshots_[handle.session_id()] = snapshot.value();
        handles_[handle.session_id()] = handle;
        if (!subscriptions_.contains(handle.session_id())) {
            auto subscription = handle.Subscribe(32U);
            if (!subscription.ok()) {
                return subscription.status();
            }
            subscriptions_.emplace(handle.session_id(), std::move(subscription).value());
        }
        return fastfix::base::Status::Ok();
    }

    fastfix::runtime::QueueApplication queue_;
    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, fastfix::session::SessionHandle> handles_;
    std::unordered_map<std::uint64_t, fastfix::session::SessionSnapshot> snapshots_;
    std::unordered_map<std::uint64_t, fastfix::session::SessionSubscription> subscriptions_;
    std::uint64_t queued_session_id_{0U};
    std::atomic<std::uint64_t> session_events_{0};
    std::atomic<std::uint64_t> admin_events_{0};
    std::atomic<std::uint64_t> application_events_{0};
    std::atomic<std::uint64_t> inline_application_events_{0};
    std::atomic<std::uint64_t> queued_application_events_{0};
};

class BackpressureApplication final : public fastfix::runtime::ApplicationCallbacks,
                                      public fastfix::runtime::QueueApplicationProvider {
  public:
    explicit BackpressureApplication(std::uint32_t worker_count)
        : queue_(
              worker_count,
              fastfix::runtime::QueueApplicationOptions{
                  .queue_capacity = 1U,
                  .overflow_policy = fastfix::runtime::QueueOverflowPolicy::kBackpressure,
              }),
          backpressure_future_(backpressure_promise_.get_future()) {
    }

    auto OnSessionEvent(const fastfix::runtime::RuntimeEvent& event) -> fastfix::base::Status override {
        (void)event;
        return fastfix::base::Status::Ok();
    }

    auto OnAdminMessage(const fastfix::runtime::RuntimeEvent& event) -> fastfix::base::Status override {
        (void)event;
        return fastfix::base::Status::Ok();
    }

    auto OnAppMessage(const fastfix::runtime::RuntimeEvent& event) -> fastfix::base::Status override {
        ++app_attempts_;
        auto status = queue_.OnAppMessage(event);
        if (status.code() == fastfix::base::ErrorCode::kBusy && !backpressure_seen_.exchange(true)) {
            backpressure_promise_.set_value();
        }
        return status;
    }

    auto Prefill(std::uint32_t worker_id) -> fastfix::base::Status {
        return queue_.OnAppMessage(fastfix::runtime::RuntimeEvent{
            .kind = fastfix::runtime::RuntimeEventKind::kApplicationMessage,
            .session_event = fastfix::runtime::SessionEventKind::kBound,
            .handle = fastfix::session::SessionHandle(9000U, worker_id),
            .session_key = fastfix::session::SessionKey{"FIX.4.4", "FAKE", "FAKE"},
            .message = {},
            .text = "prefill",
            .timestamp_ns = 1U,
        });
    }

    [[nodiscard]] auto WaitForBackpressure(std::chrono::milliseconds timeout) -> bool {
        return backpressure_future_.wait_for(timeout) == std::future_status::ready;
    }

    [[nodiscard]] auto worker_count() const -> std::uint32_t {
        return queue_.worker_count();
    }

    auto TryPopEvent(std::uint32_t worker_id)
        -> fastfix::base::Result<std::optional<fastfix::runtime::RuntimeEvent>> {
        return queue_.TryPopEvent(worker_id);
    }

    auto queue_application() -> fastfix::runtime::QueueApplication& override {
        return queue_;
    }

    [[nodiscard]] auto app_attempts() const -> std::uint64_t {
        return app_attempts_.load();
    }

    [[nodiscard]] auto overflow_events() const -> std::uint64_t {
        return queue_.overflow_events();
    }

  private:
    fastfix::runtime::QueueApplication queue_;
    std::promise<void> backpressure_promise_;
    std::future<void> backpressure_future_;
    std::atomic<bool> backpressure_seen_{false};
    std::atomic<std::uint64_t> app_attempts_{0};
};

class QueueReplyHandler final : public fastfix::runtime::QueueApplicationEventHandler {
  public:
    explicit QueueReplyHandler(std::uint64_t expected_session_id)
        : expected_session_id_(expected_session_id) {
    }

    auto OnRuntimeEvent(const fastfix::runtime::RuntimeEvent& event) -> fastfix::base::Status override {
        if (event.kind != fastfix::runtime::RuntimeEventKind::kApplicationMessage) {
            return fastfix::base::Status::Ok();
        }
        if (event.handle.session_id() != expected_session_id_) {
            return fastfix::base::Status::InvalidArgument(
                "queue-decoupled application observed an unexpected session");
        }

        auto status = event.handle.Send(event.message);
        if (!status.ok()) {
            return status;
        }
        last_event_worker_id_.store(event.handle.worker_id());
        ++queued_replies_;
        return fastfix::base::Status::Ok();
    }

    [[nodiscard]] auto queued_replies() const -> std::uint64_t {
        return queued_replies_.load();
    }

    [[nodiscard]] auto last_event_worker_id() const -> std::uint32_t {
        return last_event_worker_id_.load();
    }

  private:
    std::uint64_t expected_session_id_{0};
    std::atomic<std::uint64_t> queued_replies_{0};
    std::atomic<std::uint32_t> last_event_worker_id_{0U};
};

class BackpressureReplyHandler final : public fastfix::runtime::QueueApplicationEventHandler {
  public:
    auto OnRuntimeEvent(const fastfix::runtime::RuntimeEvent& event) -> fastfix::base::Status override {
        if (event.handle.session_id() == 9000U) {
            return fastfix::base::Status::Ok();
        }

        auto status = event.handle.Send(event.message);
        if (!status.ok()) {
            return status;
        }
        ++queued_replies_;
        return fastfix::base::Status::Ok();
    }

    [[nodiscard]] auto queued_replies() const -> std::uint64_t {
        return queued_replies_.load();
    }

  private:
    std::atomic<std::uint64_t> queued_replies_{0};
};

auto NowNs() -> std::uint64_t {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
                                              .count());
}

auto RunInitiatorEchoSession(
    std::uint16_t port,
    const fastfix::profile::NormalizedDictionaryView& dictionary,
    std::uint64_t session_id,
    std::string begin_string,
    std::string default_appl_ver_id,
    std::string sender_comp_id,
    std::string target_comp_id,
    std::string expected_party_id) -> fastfix::base::Status {
    auto connection = fastfix::transport::TcpConnection::Connect("127.0.0.1", port, std::chrono::seconds(5));
    if (!connection.ok()) {
        return connection.status();
    }

    fastfix::store::MemorySessionStore store;
    fastfix::session::AdminProtocol initiator(
        fastfix::session::AdminProtocolConfig{
            .session = fastfix::session::SessionConfig{
                .session_id = session_id,
                .key = fastfix::session::SessionKey{begin_string, sender_comp_id, target_comp_id},
                .profile_id = dictionary.profile().header().profile_id,
                .default_appl_ver_id = default_appl_ver_id,
                .heartbeat_interval_seconds = 1U,
                .is_initiator = true,
            },
            .begin_string = begin_string,
            .sender_comp_id = sender_comp_id,
            .target_comp_id = target_comp_id,
            .default_appl_ver_id = default_appl_ver_id,
            .heartbeat_interval_seconds = 1U,
            .validation_policy = fastfix::session::ValidationPolicy::Permissive(),
        },
        dictionary,
        &store);

    auto start = initiator.OnTransportConnected(NowNs());
    if (!start.ok()) {
        return start.status();
    }
    for (const auto& outbound : start.value().outbound_frames) {
        auto status = connection.value().Send(outbound.bytes, std::chrono::seconds(5));
        if (!status.ok()) {
            return status;
        }
    }

    bool sent_application = false;
    bool received_echo = false;
    while (!received_echo || initiator.session().state() != fastfix::session::SessionState::kAwaitingLogout) {
        auto frame = connection.value().ReceiveFrame(std::chrono::seconds(5));
        if (!frame.ok()) {
            return frame.status();
        }

        auto event = initiator.OnInbound(frame.value(), NowNs());
        if (!event.ok()) {
            return event.status();
        }

        for (const auto& outbound : event.value().outbound_frames) {
            auto status = connection.value().Send(outbound.bytes, std::chrono::seconds(5));
            if (!status.ok()) {
                return status;
            }
        }

        if (event.value().session_active && !sent_application) {
            fastfix::message::MessageBuilder builder("D");
            builder.set_string(35U, "D");
            auto party = builder.add_group_entry(453U);
            party.set_string(448U, expected_party_id).set_char(447U, 'D').set_int(452U, 3);
            auto outbound = initiator.SendApplication(std::move(builder).build(), NowNs());
            if (!outbound.ok()) {
                return outbound.status();
            }
            auto status = connection.value().Send(outbound.value().bytes, std::chrono::seconds(5));
            if (!status.ok()) {
                return status;
            }
            sent_application = true;
            continue;
        }

        if (!event.value().application_messages.empty()) {
            const auto group = event.value().application_messages.front().view().group(453U);
            const auto echoed_party =
                group.has_value() && group->size() == 1U ? (*group)[0].get_string(448U) : std::optional<std::string_view>{};
            if (!echoed_party.has_value() || echoed_party.value() != expected_party_id) {
                return fastfix::base::Status::InvalidArgument("echoed application message did not preserve repeating group data");
            }
            received_echo = true;

            auto logout = initiator.BeginLogout({}, NowNs());
            if (!logout.ok()) {
                return logout.status();
            }
            auto status = connection.value().Send(logout.value().bytes, std::chrono::seconds(5));
            if (!status.ok()) {
                return status;
            }
        }
    }

    auto logout_ack = connection.value().ReceiveFrame(std::chrono::seconds(5));
    if (!logout_ack.ok()) {
        return logout_ack.status();
    }
    auto logout_event = initiator.OnInbound(logout_ack.value(), NowNs());
    if (!logout_event.ok()) {
        return logout_event.status();
    }
    if (!logout_event.value().disconnect) {
        return fastfix::base::Status::InvalidArgument("expected logout acknowledgement to request disconnect");
    }

    auto close_status = initiator.OnTransportClosed();
    connection.value().Close();
    return close_status;
}

auto RunInitiatorEchoSession(
    std::uint16_t port,
    const fastfix::profile::NormalizedDictionaryView& dictionary,
    std::uint64_t session_id,
    std::string begin_string,
    std::string default_appl_ver_id,
    std::string expected_party_id) -> fastfix::base::Status {
    return RunInitiatorEchoSession(
        port,
        dictionary,
        session_id,
        std::move(begin_string),
        std::move(default_appl_ver_id),
        "BUY",
        "SELL",
        std::move(expected_party_id));
}

auto RunInitiatorHeartbeatTimerSession(
    std::uint16_t port,
    const fastfix::profile::NormalizedDictionaryView& dictionary,
    std::uint64_t session_id,
    std::string begin_string,
    std::string default_appl_ver_id) -> fastfix::base::Status {
    auto connection = fastfix::transport::TcpConnection::Connect("127.0.0.1", port, std::chrono::seconds(5));
    if (!connection.ok()) {
        return connection.status();
    }

    fastfix::store::MemorySessionStore store;
    fastfix::session::AdminProtocol initiator(
        fastfix::session::AdminProtocolConfig{
            .session = fastfix::session::SessionConfig{
                .session_id = session_id,
                .key = fastfix::session::SessionKey{begin_string, "BUY", "SELL"},
                .profile_id = dictionary.profile().header().profile_id,
                .default_appl_ver_id = default_appl_ver_id,
                .heartbeat_interval_seconds = 1U,
                .is_initiator = true,
            },
            .begin_string = begin_string,
            .sender_comp_id = "BUY",
            .target_comp_id = "SELL",
            .default_appl_ver_id = default_appl_ver_id,
            .heartbeat_interval_seconds = 1U,
            .validation_policy = fastfix::session::ValidationPolicy::Permissive(),
        },
        dictionary,
        &store);

    auto start = initiator.OnTransportConnected(NowNs());
    if (!start.ok()) {
        return start.status();
    }
    for (const auto& outbound : start.value().outbound_frames) {
        auto status = connection.value().Send(outbound.bytes, std::chrono::seconds(5));
        if (!status.ok()) {
            return status;
        }
    }

    bool saw_heartbeat = false;
    bool sent_logout = false;
    while (true) {
        auto frame = connection.value().ReceiveFrame(sent_logout ? std::chrono::seconds(5) : std::chrono::seconds(3));
        if (!frame.ok()) {
            return frame.status();
        }

        auto header = fastfix::codec::PeekSessionHeaderView(frame.value());
        if (!header.ok()) {
            return header.status();
        }

        auto event = initiator.OnInbound(frame.value(), NowNs());
        if (!event.ok()) {
            return event.status();
        }

        for (const auto& outbound : event.value().outbound_frames) {
            auto status = connection.value().Send(outbound.bytes, std::chrono::seconds(5));
            if (!status.ok()) {
                return status;
            }
        }

        if (!sent_logout && header.value().msg_type == "0" &&
            initiator.session().state() == fastfix::session::SessionState::kActive) {
            saw_heartbeat = true;
            auto logout = initiator.BeginLogout({}, NowNs());
            if (!logout.ok()) {
                return logout.status();
            }
            auto status = connection.value().Send(logout.value().bytes, std::chrono::seconds(5));
            if (!status.ok()) {
                return status;
            }
            sent_logout = true;
        }

        if (!event.value().disconnect) {
            continue;
        }

        if (!saw_heartbeat) {
            return fastfix::base::Status::InvalidArgument(
                "live acceptor did not emit heartbeat before the long poll timeout elapsed");
        }

        auto close_status = initiator.OnTransportClosed();
        connection.value().Close();
        return close_status;
    }
}

}  // namespace

TEST_CASE("live-runtime", "[live-runtime]") {
    auto dictionary = fastfix::tests::LoadFix44DictionaryView();
    if (!dictionary.ok()) {
        SKIP("FIX44 artifact not available: " << dictionary.status().message());
    }
    const auto artifact_path =
        std::filesystem::path(FASTFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.art";
    const auto profile_id = dictionary.value().profile().header().profile_id;

    fastfix::runtime::EngineConfig config;
    config.worker_count = 2U;
    config.enable_metrics = true;
    config.trace_mode = fastfix::runtime::TraceMode::kRing;
    config.trace_capacity = 32U;
    config.profile_artifacts.push_back(artifact_path);
    config.listeners.push_back(fastfix::runtime::ListenerConfig{
        .name = "main",
        .host = "127.0.0.1",
        .port = 0U,
        .worker_hint = 0U,
    });

    fastfix::runtime::CounterpartyConfig fix42;
    fix42.name = "sell-buy-42";
    fix42.session.session_id = 3101U;
    fix42.session.key = fastfix::session::SessionKey{"FIX.4.2", "SELL", "BUY"};
    fix42.session.profile_id = profile_id;
    fix42.session.heartbeat_interval_seconds = 1U;
    fix42.session.is_initiator = false;
    fix42.dispatch_mode = fastfix::runtime::AppDispatchMode::kInline;
    fix42.validation_policy = fastfix::session::ValidationPolicy::Permissive();

    fastfix::runtime::CounterpartyConfig fixt;
    fixt.name = "sell-buy-fixt";
    fixt.session.session_id = 3102U;
    fixt.session.key = fastfix::session::SessionKey{"FIXT.1.1", "SELL", "BUY"};
    fixt.session.profile_id = profile_id;
    fixt.session.default_appl_ver_id = "9";
    fixt.session.heartbeat_interval_seconds = 1U;
    fixt.session.is_initiator = false;
    fixt.default_appl_ver_id = "9";
    fixt.dispatch_mode = fastfix::runtime::AppDispatchMode::kQueueDecoupled;
    fixt.validation_policy = fastfix::session::ValidationPolicy::Permissive();

    config.counterparties.push_back(fix42);
    config.counterparties.push_back(fixt);

    fastfix::runtime::Engine engine;
    REQUIRE(engine.Boot(config).ok());

    auto managed_runner_application = std::make_shared<MixedApplication>(config.worker_count);
    std::optional<fastfix::runtime::ManagedQueueApplicationRunnerOptions> engine_managed_queue_runner;
    engine_managed_queue_runner.emplace();
    engine_managed_queue_runner->mode = fastfix::runtime::ManagedQueueApplicationRunnerMode::kCoScheduled;
    engine_managed_queue_runner->handlers.reserve(managed_runner_application->queue_application().worker_count());
    for (std::uint32_t worker_id = 0; worker_id < managed_runner_application->queue_application().worker_count(); ++worker_id) {
        (void)worker_id;
        engine_managed_queue_runner->handlers.push_back(std::make_unique<QueueReplyHandler>(3102U));
    }
    int managed_runner_owner = 0;
    REQUIRE(engine.EnsureManagedQueueRunnerStarted(
                  &managed_runner_owner,
                  managed_runner_application.get(),
                  &engine_managed_queue_runner)
            .ok());
    REQUIRE(engine.StopManagedQueueRunner(&managed_runner_owner).ok());
    REQUIRE(engine.EnsureManagedQueueRunnerStarted(&managed_runner_owner, managed_runner_application.get(), nullptr).ok());
    REQUIRE(engine.ReleaseManagedQueueRunner(&managed_runner_owner).ok());
    REQUIRE(!engine.EnsureManagedQueueRunnerStarted(&managed_runner_owner, managed_runner_application.get(), nullptr).ok());

    auto application = std::make_shared<MixedApplication>(config.worker_count);
    fastfix::runtime::ManagedQueueApplicationRunnerOptions managed_queue_runner;
    managed_queue_runner.mode = fastfix::runtime::ManagedQueueApplicationRunnerMode::kCoScheduled;
    managed_queue_runner.handlers.reserve(application->queue_application().worker_count());
    for (std::uint32_t worker_id = 0; worker_id < application->queue_application().worker_count(); ++worker_id) {
        (void)worker_id;
        managed_queue_runner.handlers.push_back(std::make_unique<QueueReplyHandler>(3102U));
    }

    fastfix::runtime::LiveAcceptor runtime(
        &engine,
        fastfix::runtime::LiveAcceptor::Options{
            .poll_timeout = std::chrono::milliseconds(25),
            .io_timeout = std::chrono::seconds(5),
            .application = application,
            .managed_queue_runner = std::move(managed_queue_runner),
        });
    REQUIRE(runtime.OpenListeners("main").ok());

    auto port = runtime.listener_port("main");
    REQUIRE(port.ok());

    std::promise<fastfix::base::Status> runtime_result;
    auto runtime_future = runtime_result.get_future();
    std::jthread runtime_thread([&runtime, &runtime_result]() {
        runtime_result.set_value(runtime.Run(2U, std::chrono::seconds(10)));
    });

    auto status = RunInitiatorEchoSession(port.value(), dictionary.value(), 4101U, "FIX.4.2", {}, "PARTY-42");
    REQUIRE(status.ok());
    status = RunInitiatorEchoSession(port.value(), dictionary.value(), 4102U, "FIXT.1.1", "9", "PARTY-FIXT");
    REQUIRE(status.ok());

    REQUIRE(runtime_future.get().ok());
    REQUIRE(runtime.completed_session_count() == 2U);
    REQUIRE(application->session_events() > 0U);
    REQUIRE(application->admin_events() > 0U);
    REQUIRE(application->application_events() == 2U);
    REQUIRE(application->inline_application_events() == 1U);
    REQUIRE(application->queued_application_events() == 1U);

    const auto snapshot_42 = application->SnapshotFor(3101U);
    const auto snapshot_fixt = application->SnapshotFor(3102U);
    REQUIRE(snapshot_42.has_value());
    REQUIRE(snapshot_fixt.has_value());
    REQUIRE(snapshot_42->state == fastfix::session::SessionState::kDisconnected);
    REQUIRE(snapshot_fixt->state == fastfix::session::SessionState::kDisconnected);

    auto notifications_42 = application->DrainSubscription(3101U);
    auto notifications_fixt = application->DrainSubscription(3102U);
    REQUIRE(notifications_42.ok());
    REQUIRE(notifications_fixt.ok());

    auto count_notifications = [](const auto& notifications, fastfix::session::SessionNotificationKind kind) {
        return static_cast<std::size_t>(std::count_if(
            notifications.begin(),
            notifications.end(),
            [&](const auto& notification) { return notification.kind == kind; }));
    };
    auto find_notification = [](const auto& notifications, fastfix::session::SessionNotificationKind kind) {
        const auto it = std::find_if(
            notifications.begin(),
            notifications.end(),
            [&](const auto& notification) { return notification.kind == kind; });
        return it == notifications.end() ? nullptr : &(*it);
    };

    REQUIRE(count_notifications(notifications_42.value(), fastfix::session::SessionNotificationKind::kSessionBound) == 1U);
    REQUIRE(count_notifications(notifications_42.value(), fastfix::session::SessionNotificationKind::kSessionActive) == 1U);
    REQUIRE(count_notifications(notifications_42.value(), fastfix::session::SessionNotificationKind::kSessionClosed) == 1U);
    REQUIRE(count_notifications(notifications_42.value(), fastfix::session::SessionNotificationKind::kAdminMessage) >= 2U);
    REQUIRE(count_notifications(notifications_42.value(), fastfix::session::SessionNotificationKind::kApplicationMessage) == 1U);
    REQUIRE(count_notifications(notifications_fixt.value(), fastfix::session::SessionNotificationKind::kSessionBound) == 1U);
    REQUIRE(count_notifications(notifications_fixt.value(), fastfix::session::SessionNotificationKind::kSessionActive) == 1U);
    REQUIRE(count_notifications(notifications_fixt.value(), fastfix::session::SessionNotificationKind::kSessionClosed) == 1U);
    REQUIRE(count_notifications(notifications_fixt.value(), fastfix::session::SessionNotificationKind::kAdminMessage) >= 2U);
    REQUIRE(count_notifications(notifications_fixt.value(), fastfix::session::SessionNotificationKind::kApplicationMessage) == 1U);

    const auto* app_notification_42 =
        find_notification(notifications_42.value(), fastfix::session::SessionNotificationKind::kApplicationMessage);
    REQUIRE(app_notification_42 != nullptr);
    REQUIRE(app_notification_42->message.valid());
    const auto parties_42 = app_notification_42->message_view().group(453U);
    REQUIRE(parties_42.has_value());
    REQUIRE(parties_42->size() == 1U);
    REQUIRE((*parties_42)[0].get_string(448U).value() == "PARTY-42");

    const auto* app_notification_fixt =
        find_notification(notifications_fixt.value(), fastfix::session::SessionNotificationKind::kApplicationMessage);
    REQUIRE(app_notification_fixt != nullptr);
    REQUIRE(app_notification_fixt->message.valid());
    const auto parties_fixt = app_notification_fixt->message_view().group(453U);
    REQUIRE(parties_fixt.has_value());
    REQUIRE(parties_fixt->size() == 1U);
    REQUIRE((*parties_fixt)[0].get_string(448U).value() == "PARTY-FIXT");

    const auto* metrics_42 = engine.metrics().FindSession(3101U);
    const auto* metrics_fixt = engine.metrics().FindSession(3102U);
    REQUIRE(metrics_42 != nullptr);
    REQUIRE(metrics_fixt != nullptr);
    REQUIRE(metrics_42->inbound_messages > 0U);
    REQUIRE(metrics_42->outbound_messages > 0U);
    REQUIRE(metrics_fixt->inbound_messages > 0U);
    REQUIRE(metrics_fixt->outbound_messages > 0U);

    const auto traces = engine.trace().Snapshot();
    bool saw_pending = false;
    bool saw_session_event = false;
    for (const auto& event : traces) {
        if (event.kind == fastfix::runtime::TraceEventKind::kPendingConnectionRegistered) {
            saw_pending = true;
        }
        if (event.kind == fastfix::runtime::TraceEventKind::kSessionEvent && event.session_id != 0U) {
            saw_session_event = true;
        }
    }
    REQUIRE(saw_pending);
    REQUIRE(saw_session_event);

    constexpr std::uint64_t kShardMigratedSessionId = 3104U;
    std::string routed_target_comp_id;
    for (std::uint32_t attempt = 0U; attempt < 256U; ++attempt) {
        const auto candidate = std::string("BUY-SHARD-") + std::to_string(attempt);
        if ((fastfix::session::SessionKeyHash{}(fastfix::session::SessionKey{"FIX.4.4", "SELL", candidate}) % 2U) == 1U) {
            routed_target_comp_id = candidate;
            break;
        }
    }
    REQUIRE(!routed_target_comp_id.empty());

    fastfix::runtime::EngineConfig shard_config;
    shard_config.worker_count = 2U;
    shard_config.enable_metrics = true;
    shard_config.trace_mode = fastfix::runtime::TraceMode::kRing;
    shard_config.trace_capacity = 16U;
    shard_config.profile_artifacts.push_back(artifact_path);
    shard_config.listeners.push_back(fastfix::runtime::ListenerConfig{
        .name = "shard-main",
        .host = "127.0.0.1",
        .port = 0U,
        .worker_hint = 0U,
    });
    shard_config.counterparties.push_back(fastfix::runtime::CounterpartyConfig{
        .name = "sell-buy-shard-migrate",
        .session = fastfix::session::SessionConfig{
            .session_id = kShardMigratedSessionId,
            .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", routed_target_comp_id},
            .profile_id = profile_id,
            .heartbeat_interval_seconds = 1U,
            .is_initiator = false,
        },
        .store_path = {},
        .default_appl_ver_id = {},
        .store_mode = fastfix::runtime::StoreMode::kMemory,
        .recovery_mode = fastfix::session::RecoveryMode::kMemoryOnly,
        .dispatch_mode = fastfix::runtime::AppDispatchMode::kQueueDecoupled,
        .validation_policy = fastfix::session::ValidationPolicy::Permissive(),
    });

    fastfix::runtime::Engine shard_engine;
    REQUIRE(shard_engine.Boot(shard_config).ok());
    const auto* routed_shard = shard_engine.runtime()->FindSessionShard(kShardMigratedSessionId);
    REQUIRE(routed_shard != nullptr);
    REQUIRE(routed_shard->worker_id == 1U);

    auto shard_application = std::make_shared<MixedApplication>(shard_config.worker_count, kShardMigratedSessionId);
    fastfix::runtime::ManagedQueueApplicationRunnerOptions shard_managed_queue_runner;
    shard_managed_queue_runner.mode = fastfix::runtime::ManagedQueueApplicationRunnerMode::kCoScheduled;
    shard_managed_queue_runner.handlers.reserve(shard_application->queue_application().worker_count());
    std::vector<QueueReplyHandler*> shard_handler_ptrs;
    for (std::uint32_t worker_id = 0; worker_id < shard_application->queue_application().worker_count(); ++worker_id) {
        auto handler = std::make_unique<QueueReplyHandler>(kShardMigratedSessionId);
        shard_handler_ptrs.push_back(handler.get());
        shard_managed_queue_runner.handlers.push_back(std::move(handler));
    }

    fastfix::runtime::LiveAcceptor shard_runtime(
        &shard_engine,
        fastfix::runtime::LiveAcceptor::Options{
            .poll_timeout = std::chrono::milliseconds(25),
            .io_timeout = std::chrono::seconds(5),
            .application = shard_application,
            .managed_queue_runner = std::move(shard_managed_queue_runner),
        });
    REQUIRE(shard_runtime.OpenListeners("shard-main").ok());

    auto shard_port = shard_runtime.listener_port("shard-main");
    REQUIRE(shard_port.ok());

    std::promise<fastfix::base::Status> shard_runtime_result;
    auto shard_runtime_future = shard_runtime_result.get_future();
    std::jthread shard_runtime_thread([&shard_runtime, &shard_runtime_result]() {
        shard_runtime_result.set_value(shard_runtime.Run(1U, std::chrono::seconds(10)));
    });

    status = RunInitiatorEchoSession(
        shard_port.value(),
        dictionary.value(),
        4104U,
        "FIX.4.4",
        {},
        routed_target_comp_id,
        "SELL",
        "PARTY-SHARD");
    REQUIRE(status.ok());
    REQUIRE(shard_runtime_future.get().ok());
    REQUIRE(shard_runtime.completed_session_count() == 1U);
    REQUIRE(shard_application->queued_application_events() == 1U);
    REQUIRE(shard_handler_ptrs.size() == shard_config.worker_count);
    REQUIRE(shard_handler_ptrs[0]->queued_replies() == 0U);
    REQUIRE(shard_handler_ptrs[1]->queued_replies() == 1U);
    REQUIRE(shard_handler_ptrs[1]->last_event_worker_id() == 1U);

    const auto shard_snapshot = shard_application->SnapshotFor(kShardMigratedSessionId);
    REQUIRE(shard_snapshot.has_value());
    REQUIRE(shard_snapshot->state == fastfix::session::SessionState::kDisconnected);

    fastfix::runtime::EngineConfig balance_config;
    balance_config.worker_count = 2U;
    balance_config.trace_mode = fastfix::runtime::TraceMode::kRing;
    balance_config.trace_capacity = 32U;
    balance_config.profile_artifacts.push_back(artifact_path);
    balance_config.listeners.push_back(fastfix::runtime::ListenerConfig{
        .name = "balance-main",
        .host = "127.0.0.1",
        .port = 0U,
        .worker_hint = 0U,
    });

    fastfix::runtime::Engine balance_engine;
    REQUIRE(balance_engine.Boot(balance_config).ok());

    fastfix::runtime::LiveAcceptor balance_runtime(
        &balance_engine,
        fastfix::runtime::LiveAcceptor::Options{
            .poll_timeout = std::chrono::milliseconds(25),
            .io_timeout = std::chrono::seconds(5),
            .application = {},
        });
    REQUIRE(balance_runtime.OpenListeners("balance-main").ok());

    auto balance_port = balance_runtime.listener_port("balance-main");
    REQUIRE(balance_port.ok());

    std::promise<fastfix::base::Status> balance_runtime_result;
    auto balance_runtime_future = balance_runtime_result.get_future();
    std::jthread balance_runtime_thread([&balance_runtime, &balance_runtime_result]() {
        balance_runtime_result.set_value(balance_runtime.Run(0U, std::chrono::seconds(10)));
    });

    std::vector<fastfix::transport::TcpConnection> balance_connections;
    balance_connections.reserve(6U);
    for (std::size_t index = 0U; index < 6U; ++index) {
        auto connection = fastfix::transport::TcpConnection::Connect("127.0.0.1", balance_port.value(), std::chrono::seconds(5));
        REQUIRE(connection.ok());
        balance_connections.push_back(std::move(connection).value());
    }

    auto accept_count = [&balance_engine]() {
        const auto traces = balance_engine.trace().Snapshot();
        return static_cast<std::size_t>(std::count_if(
            traces.begin(),
            traces.end(),
            [](const fastfix::runtime::TraceEvent& event) {
                return event.kind == fastfix::runtime::TraceEventKind::kSessionEvent &&
                       std::string_view(event.text.data()) == "tcp accept";
            }));
    };

    std::optional<fastfix::base::Status> balance_run_status;
    const auto accept_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (accept_count() < balance_connections.size() && std::chrono::steady_clock::now() < accept_deadline) {
        if (balance_runtime_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            balance_run_status = balance_runtime_future.get();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!balance_run_status.has_value() && accept_count() < balance_connections.size() &&
        balance_runtime_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        balance_run_status = balance_runtime_future.get();
    }
    if (balance_run_status.has_value()) {
        INFO(balance_run_status->message());
    } else {
        INFO("balance runtime still running");
    }
    const auto balance_active_connections = balance_runtime.active_connection_count();
    CAPTURE(balance_active_connections);
    CAPTURE(balance_run_status.has_value());
    if (balance_run_status.has_value()) {
        CAPTURE(static_cast<int>(balance_run_status->code()));
        CAPTURE(balance_run_status->message());
    }
    const bool balance_runtime_ok = !balance_run_status.has_value() || balance_run_status->ok();
    REQUIRE(balance_runtime_ok);
    REQUIRE(accept_count() == balance_connections.size());

    const auto balance_traces = balance_engine.trace().Snapshot();
    auto trace_text = [](const fastfix::runtime::TraceEvent& event) {
        return std::string_view(event.text.data());
    };
    std::size_t accepts_worker0 = 0U;
    std::size_t accepts_worker1 = 0U;
    for (const auto& event : balance_traces) {
        if (event.kind != fastfix::runtime::TraceEventKind::kSessionEvent || trace_text(event) != "tcp accept") {
            continue;
        }
        if (event.worker_id == 0U) {
            ++accepts_worker0;
        } else if (event.worker_id == 1U) {
            ++accepts_worker1;
        }
    }
    REQUIRE(accepts_worker0 > 0U);
    REQUIRE(accepts_worker1 > 0U);
    REQUIRE(std::max(accepts_worker0, accepts_worker1) - std::min(accepts_worker0, accepts_worker1) <= 1U);

    balance_runtime.Stop();
    for (auto& connection : balance_connections) {
        connection.Close();
    }
    std::optional<fastfix::base::Status> final_balance_status;
    if (balance_run_status.has_value()) {
        final_balance_status = *balance_run_status;
    } else {
        final_balance_status = balance_runtime_future.get();
    }
    CAPTURE(final_balance_status->message());
    REQUIRE(final_balance_status->ok());

    fastfix::runtime::EngineConfig queue_mismatch_config;
    queue_mismatch_config.worker_count = 2U;
    queue_mismatch_config.trace_mode = fastfix::runtime::TraceMode::kRing;
    queue_mismatch_config.trace_capacity = 16U;
    queue_mismatch_config.profile_artifacts.push_back(artifact_path);
    queue_mismatch_config.listeners.push_back(fastfix::runtime::ListenerConfig{
        .name = "queue-mismatch-main",
        .host = "127.0.0.1",
        .port = 0U,
        .worker_hint = 0U,
    });

    fastfix::runtime::Engine queue_mismatch_engine;
    REQUIRE(queue_mismatch_engine.Boot(queue_mismatch_config).ok());

    auto queue_mismatch_application = std::make_shared<MixedApplication>(1U, 3199U);
    fastfix::runtime::ManagedQueueApplicationRunnerOptions queue_mismatch_runner;
    queue_mismatch_runner.mode = fastfix::runtime::ManagedQueueApplicationRunnerMode::kCoScheduled;
    queue_mismatch_runner.handlers.push_back(std::make_unique<QueueReplyHandler>(3199U));

    fastfix::runtime::LiveAcceptor queue_mismatch_runtime(
        &queue_mismatch_engine,
        fastfix::runtime::LiveAcceptor::Options{
            .poll_timeout = std::chrono::milliseconds(5),
            .io_timeout = std::chrono::seconds(1),
            .application = queue_mismatch_application,
            .managed_queue_runner = std::move(queue_mismatch_runner),
        });
    auto queue_mismatch_status = queue_mismatch_runtime.Run(0U, std::chrono::milliseconds(5));
    REQUIRE(!queue_mismatch_status.ok());
    REQUIRE(queue_mismatch_status.code() == fastfix::base::ErrorCode::kInvalidArgument);
    REQUIRE(queue_mismatch_status.message().find("queue application worker_count must match engine worker_count") !=
            std::string::npos);

    fastfix::runtime::EngineConfig threaded_config;
    threaded_config.worker_count = 1U;
    threaded_config.enable_metrics = true;
    threaded_config.trace_mode = fastfix::runtime::TraceMode::kRing;
    threaded_config.trace_capacity = 16U;
    threaded_config.queue_app_mode = fastfix::runtime::QueueAppThreadingMode::kThreaded;
    threaded_config.profile_artifacts.push_back(artifact_path);
    threaded_config.listeners.push_back(fastfix::runtime::ListenerConfig{
        .name = "threaded-main",
        .host = "127.0.0.1",
        .port = 0U,
        .worker_hint = 0U,
    });
    threaded_config.counterparties.push_back(fastfix::runtime::CounterpartyConfig{
        .name = "sell-buy-threaded-queue",
        .session = fastfix::session::SessionConfig{
            .session_id = 3105U,
            .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
            .profile_id = profile_id,
            .heartbeat_interval_seconds = 1U,
            .is_initiator = false,
        },
        .store_path = {},
        .default_appl_ver_id = {},
        .store_mode = fastfix::runtime::StoreMode::kMemory,
        .recovery_mode = fastfix::session::RecoveryMode::kMemoryOnly,
        .dispatch_mode = fastfix::runtime::AppDispatchMode::kQueueDecoupled,
        .validation_policy = fastfix::session::ValidationPolicy::Permissive(),
    });

    fastfix::runtime::Engine threaded_engine;
    REQUIRE(threaded_engine.Boot(threaded_config).ok());

    auto threaded_application = std::make_shared<MixedApplication>(threaded_config.worker_count, 3105U);
    fastfix::runtime::ManagedQueueApplicationRunnerOptions threaded_runner;
    threaded_runner.mode = fastfix::runtime::ManagedQueueApplicationRunnerMode::kThreaded;
    threaded_runner.handlers.push_back(std::make_unique<QueueReplyHandler>(3105U));

    fastfix::runtime::LiveAcceptor threaded_runtime(
        &threaded_engine,
        fastfix::runtime::LiveAcceptor::Options{
            .poll_timeout = std::chrono::milliseconds(25),
            .io_timeout = std::chrono::seconds(5),
            .application = threaded_application,
            .managed_queue_runner = std::move(threaded_runner),
        });
    REQUIRE(threaded_runtime.OpenListeners("threaded-main").ok());

    auto threaded_port = threaded_runtime.listener_port("threaded-main");
    REQUIRE(threaded_port.ok());

    std::promise<fastfix::base::Status> threaded_runtime_result;
    auto threaded_runtime_future = threaded_runtime_result.get_future();
    std::jthread threaded_runtime_thread([&threaded_runtime, &threaded_runtime_result]() {
        threaded_runtime_result.set_value(threaded_runtime.Run(1U, std::chrono::seconds(10)));
    });

    status = RunInitiatorEchoSession(threaded_port.value(), dictionary.value(), 4105U, "FIX.4.4", {}, "PARTY-THREADED");
    REQUIRE(status.ok());
    REQUIRE(threaded_runtime_future.get().ok());
    REQUIRE(threaded_runtime.completed_session_count() == 1U);
    REQUIRE(threaded_application->queued_application_events() == 1U);

    fastfix::runtime::EngineConfig timer_config;
    timer_config.worker_count = 1U;
    timer_config.enable_metrics = true;
    timer_config.trace_mode = fastfix::runtime::TraceMode::kRing;
    timer_config.trace_capacity = 16U;
    timer_config.profile_artifacts.push_back(artifact_path);
    timer_config.listeners.push_back(fastfix::runtime::ListenerConfig{
        .name = "timer-main",
        .host = "127.0.0.1",
        .port = 0U,
        .worker_hint = 0U,
    });
    timer_config.counterparties.push_back(fastfix::runtime::CounterpartyConfig{
        .name = "sell-buy-timer",
        .session = fastfix::session::SessionConfig{
            .session_id = 3103U,
            .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
            .profile_id = profile_id,
            .heartbeat_interval_seconds = 1U,
            .is_initiator = false,
        },
        .store_path = {},
        .default_appl_ver_id = {},
        .store_mode = fastfix::runtime::StoreMode::kMemory,
        .recovery_mode = fastfix::session::RecoveryMode::kMemoryOnly,
        .dispatch_mode = fastfix::runtime::AppDispatchMode::kInline,
        .validation_policy = fastfix::session::ValidationPolicy::Permissive(),
    });

    fastfix::runtime::Engine timer_engine;
    REQUIRE(timer_engine.Boot(timer_config).ok());

    fastfix::runtime::LiveAcceptor timer_runtime(
        &timer_engine,
        fastfix::runtime::LiveAcceptor::Options{
            .poll_timeout = std::chrono::seconds(5),
            .io_timeout = std::chrono::seconds(5),
            .application = {},
        });
    REQUIRE(timer_runtime.OpenListeners("timer-main").ok());

    auto timer_port = timer_runtime.listener_port("timer-main");
    REQUIRE(timer_port.ok());

    std::promise<fastfix::base::Status> timer_runtime_result;
    auto timer_runtime_future = timer_runtime_result.get_future();
    std::jthread timer_runtime_thread([&timer_runtime, &timer_runtime_result]() {
        timer_runtime_result.set_value(timer_runtime.Run(1U, std::chrono::seconds(10)));
    });

    status = RunInitiatorHeartbeatTimerSession(timer_port.value(), dictionary.value(), 4103U, "FIX.4.4", {});
    REQUIRE(status.ok());
    REQUIRE(timer_runtime_future.get().ok());
    REQUIRE(timer_runtime.completed_session_count() == 1U);

    const auto* timer_metrics = timer_engine.metrics().FindSession(3103U);
    REQUIRE(timer_metrics != nullptr);
    REQUIRE(timer_metrics->outbound_messages > 0U);
}

TEST_CASE("live-backpressure", "[live-backpressure]") {
    auto dictionary = fastfix::tests::LoadFix44DictionaryView();
    if (!dictionary.ok()) {
        SKIP("FIX44 artifact not available: " << dictionary.status().message());
    }
    const auto artifact_path =
        std::filesystem::path(FASTFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.art";
    const auto profile_id = dictionary.value().profile().header().profile_id;

    fastfix::runtime::EngineConfig config;
    config.worker_count = 1U;
    config.enable_metrics = true;
    config.trace_mode = fastfix::runtime::TraceMode::kRing;
    config.trace_capacity = 16U;
    config.profile_artifacts.push_back(artifact_path);
    config.listeners.push_back(fastfix::runtime::ListenerConfig{
        .name = "main",
        .host = "127.0.0.1",
        .port = 0U,
        .worker_hint = 0U,
    });

    fastfix::runtime::CounterpartyConfig counterparty;
    counterparty.name = "sell-buy-backpressure";
    counterparty.session.session_id = 3201U;
    counterparty.session.key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"};
    counterparty.session.profile_id = profile_id;
    counterparty.session.heartbeat_interval_seconds = 1U;
    counterparty.session.is_initiator = false;
    counterparty.dispatch_mode = fastfix::runtime::AppDispatchMode::kQueueDecoupled;
    counterparty.validation_policy = fastfix::session::ValidationPolicy::Permissive();
    config.counterparties.push_back(counterparty);

    fastfix::runtime::Engine engine;
    REQUIRE(engine.Boot(config).ok());

    auto application = std::make_shared<BackpressureApplication>(config.worker_count);
    REQUIRE(application->Prefill(0U).ok());

    fastfix::runtime::LiveAcceptor runtime(
        &engine,
        fastfix::runtime::LiveAcceptor::Options{
            .poll_timeout = std::chrono::milliseconds(25),
            .io_timeout = std::chrono::seconds(5),
            .application = application,
        });
    REQUIRE(runtime.OpenListeners("main").ok());

    auto port = runtime.listener_port("main");
    REQUIRE(port.ok());

    std::promise<fastfix::base::Status> runtime_result;
    auto runtime_future = runtime_result.get_future();
    std::jthread runtime_thread([&runtime, &runtime_result]() {
        runtime_result.set_value(runtime.Run(1U, std::chrono::seconds(10)));
    });

    auto initiator_future = std::async(
        std::launch::async,
        [&]() {
            return RunInitiatorEchoSession(port.value(), dictionary.value(), 4201U, "FIX.4.4", {}, "PARTY-BP");
        });

    REQUIRE(application->WaitForBackpressure(std::chrono::seconds(5)));

    std::atomic<bool> stop_application{false};
    BackpressureReplyHandler queue_handler;
    fastfix::runtime::QueueApplicationPoller poller(&application->queue_application(), &queue_handler);
    std::promise<fastfix::base::Status> application_result;
    auto application_future = application_result.get_future();
    std::jthread application_thread([&]() {
        application_result.set_value(poller.RunWorker(0U, stop_application));
    });

    auto initiator_status = initiator_future.get();
    if (!initiator_status.ok()) {
        UNSCOPED_INFO("initiator failed: " << initiator_status.message());
    }
    REQUIRE(initiator_status.ok());
    auto runtime_status = runtime_future.get();
    stop_application.store(true);
    REQUIRE(application_future.get().ok());
    REQUIRE(runtime.completed_session_count() == 1U);
    REQUIRE(queue_handler.queued_replies() == 1U);
    REQUIRE(application->overflow_events() >= 1U);
    REQUIRE(application->app_attempts() >= 2U);
}

TEST_CASE("dynamic session factory accepts unknown CompID", "[live-session-factory]") {
    auto dictionary = fastfix::tests::LoadFix44DictionaryView();
    if (!dictionary.ok()) {
        SKIP("FIX44 artifact not available: " << dictionary.status().message());
    }
    const auto artifact_path =
        std::filesystem::path(FASTFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.art";
    const auto profile_id = dictionary.value().profile().header().profile_id;

    fastfix::runtime::EngineConfig config;
    config.worker_count = 1U;
    config.enable_metrics = false;
    config.trace_mode = fastfix::runtime::TraceMode::kDisabled;
    config.profile_artifacts.push_back(artifact_path);
    config.listeners.push_back(fastfix::runtime::ListenerConfig{
        .name = "factory-main",
        .host = "127.0.0.1",
        .port = 0U,
        .worker_hint = 0U,
    });
    // No counterparties configured — factory provides them dynamically.

    fastfix::runtime::Engine engine;
    REQUIRE(engine.Boot(config).ok());

    std::atomic<std::uint64_t> next_session_id{7001U};
    engine.SetSessionFactory([&](const fastfix::session::SessionKey& key) -> fastfix::base::Result<fastfix::runtime::CounterpartyConfig> {
        const auto session_id = next_session_id.fetch_add(1U);
        return fastfix::runtime::CounterpartyConfig{
            .name = "dynamic-" + key.target_comp_id,
            .session = fastfix::session::SessionConfig{
                .session_id = session_id,
                .key = key,
                .profile_id = profile_id,
                .heartbeat_interval_seconds = 1U,
                .is_initiator = false,
            },
            .dispatch_mode = fastfix::runtime::AppDispatchMode::kInline,
        .validation_policy = fastfix::session::ValidationPolicy::Permissive(),
        };
    });

    auto application = std::make_shared<MixedApplication>(config.worker_count);

    fastfix::runtime::LiveAcceptor runtime(
        &engine,
        fastfix::runtime::LiveAcceptor::Options{
            .poll_timeout = std::chrono::milliseconds(25),
            .io_timeout = std::chrono::seconds(5),
            .application = application,
        });
    REQUIRE(runtime.OpenListeners("factory-main").ok());

    auto port = runtime.listener_port("factory-main");
    REQUIRE(port.ok());

    std::promise<fastfix::base::Status> runtime_result;
    auto runtime_future = runtime_result.get_future();
    std::jthread runtime_thread([&runtime, &runtime_result]() {
        runtime_result.set_value(runtime.Run(1U, std::chrono::seconds(10)));
    });

    auto status = RunInitiatorEchoSession(
        port.value(), dictionary.value(), 9001U, "FIX.4.2", {},
        "UNKNOWN-BUYER", "SELL", "PARTY-DYN");
    REQUIRE(status.ok());

    REQUIRE(runtime_future.get().ok());
    REQUIRE(runtime.completed_session_count() == 1U);
    REQUIRE(application->application_events() == 1U);
}

TEST_CASE("dynamic session factory rejects unknown CompID", "[live-session-factory]") {
    auto dictionary = fastfix::tests::LoadFix44DictionaryView();
    if (!dictionary.ok()) {
        SKIP("FIX44 artifact not available: " << dictionary.status().message());
    }
    const auto artifact_path =
        std::filesystem::path(FASTFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.art";
    const auto profile_id = dictionary.value().profile().header().profile_id;

    fastfix::runtime::EngineConfig config;
    config.worker_count = 1U;
    config.enable_metrics = false;
    config.trace_mode = fastfix::runtime::TraceMode::kDisabled;
    config.profile_artifacts.push_back(artifact_path);
    config.listeners.push_back(fastfix::runtime::ListenerConfig{
        .name = "factory-reject-main",
        .host = "127.0.0.1",
        .port = 0U,
        .worker_hint = 0U,
    });

    fastfix::runtime::Engine engine;
    REQUIRE(engine.Boot(config).ok());

    engine.SetSessionFactory([](const fastfix::session::SessionKey&) -> fastfix::base::Result<fastfix::runtime::CounterpartyConfig> {
        return fastfix::base::Status::NotFound("factory rejects this session");
    });

    fastfix::runtime::LiveAcceptor runtime(
        &engine,
        fastfix::runtime::LiveAcceptor::Options{
            .poll_timeout = std::chrono::milliseconds(25),
            .io_timeout = std::chrono::seconds(5),
        });
    REQUIRE(runtime.OpenListeners("factory-reject-main").ok());

    auto port = runtime.listener_port("factory-reject-main");
    REQUIRE(port.ok());

    std::promise<fastfix::base::Status> runtime_result;
    auto runtime_future = runtime_result.get_future();
    std::jthread runtime_thread([&runtime, &runtime_result]() {
        runtime_result.set_value(runtime.Run(0U, std::chrono::seconds(3)));
    });

    // Initiator connects; acceptor factory rejects → connection closed.
    auto connection = fastfix::transport::TcpConnection::Connect("127.0.0.1", port.value(), std::chrono::seconds(5));
    REQUIRE(connection.ok());

    fastfix::store::MemorySessionStore store;
    fastfix::session::AdminProtocol initiator(
        fastfix::session::AdminProtocolConfig{
            .session = fastfix::session::SessionConfig{
                .session_id = 9901U,
                .key = fastfix::session::SessionKey{"FIX.4.2", "REJECTED-BUYER", "SELL"},
                .profile_id = profile_id,
                .heartbeat_interval_seconds = 1U,
                .is_initiator = true,
            },
            .begin_string = "FIX.4.2",
            .sender_comp_id = "REJECTED-BUYER",
            .target_comp_id = "SELL",
            .heartbeat_interval_seconds = 1U,
            .validation_policy = fastfix::session::ValidationPolicy::Permissive(),
        },
        dictionary.value(),
        &store);

    auto start = initiator.OnTransportConnected(NowNs());
    REQUIRE(start.ok());
    for (const auto& outbound : start.value().outbound_frames) {
        auto send_status = connection.value().Send(outbound.bytes, std::chrono::seconds(5));
        REQUIRE(send_status.ok());
    }

    // The acceptor should close the connection; reading should fail or get no valid FIX response.
    auto frame = connection.value().ReceiveFrame(std::chrono::seconds(3));
    connection.value().Close();

    runtime.Stop();
    (void)runtime_future.get();
    REQUIRE(runtime.completed_session_count() == 0U);
}

TEST_CASE("whitelist factory accepts listed CompID", "[live-session-factory]") {
    auto dictionary = fastfix::tests::LoadFix44DictionaryView();
    if (!dictionary.ok()) {
        SKIP("FIX44 artifact not available: " << dictionary.status().message());
    }
    const auto artifact_path =
        std::filesystem::path(FASTFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.art";
    const auto profile_id = dictionary.value().profile().header().profile_id;

    fastfix::runtime::EngineConfig config;
    config.worker_count = 1U;
    config.enable_metrics = false;
    config.trace_mode = fastfix::runtime::TraceMode::kDisabled;
    config.profile_artifacts.push_back(artifact_path);
    config.listeners.push_back(fastfix::runtime::ListenerConfig{
        .name = "whitelist-accept-main",
        .host = "127.0.0.1",
        .port = 0U,
        .worker_hint = 0U,
    });

    fastfix::runtime::Engine engine;
    REQUIRE(engine.Boot(config).ok());

    fastfix::runtime::CounterpartyConfig tmpl;
    tmpl.name = "whitelist-template";
    tmpl.session.session_id = 7101U;
    tmpl.session.profile_id = profile_id;
    tmpl.session.heartbeat_interval_seconds = 1U;
    tmpl.session.is_initiator = false;
    tmpl.dispatch_mode = fastfix::runtime::AppDispatchMode::kInline;
    tmpl.validation_policy = fastfix::session::ValidationPolicy::Permissive();

    fastfix::runtime::WhitelistSessionFactory whitelist;
    whitelist.Allow("FIX.4.2", "SELL", tmpl);
    engine.SetSessionFactory(whitelist);

    auto application = std::make_shared<MixedApplication>(config.worker_count);

    fastfix::runtime::LiveAcceptor runtime(
        &engine,
        fastfix::runtime::LiveAcceptor::Options{
            .poll_timeout = std::chrono::milliseconds(25),
            .io_timeout = std::chrono::seconds(5),
            .application = application,
        });
    REQUIRE(runtime.OpenListeners("whitelist-accept-main").ok());

    auto port = runtime.listener_port("whitelist-accept-main");
    REQUIRE(port.ok());

    std::promise<fastfix::base::Status> runtime_result;
    auto runtime_future = runtime_result.get_future();
    std::jthread runtime_thread([&runtime, &runtime_result]() {
        runtime_result.set_value(runtime.Run(1U, std::chrono::seconds(10)));
    });

    auto status = RunInitiatorEchoSession(
        port.value(), dictionary.value(), 9101U, "FIX.4.2", {},
        "WHITELISTED-BUYER", "SELL", "PARTY-WL");
    REQUIRE(status.ok());

    REQUIRE(runtime_future.get().ok());
    REQUIRE(runtime.completed_session_count() == 1U);
    REQUIRE(application->application_events() == 1U);
}

TEST_CASE("whitelist factory rejects unlisted CompID", "[live-session-factory]") {
    auto dictionary = fastfix::tests::LoadFix44DictionaryView();
    if (!dictionary.ok()) {
        SKIP("FIX44 artifact not available: " << dictionary.status().message());
    }
    const auto artifact_path =
        std::filesystem::path(FASTFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.art";
    const auto profile_id = dictionary.value().profile().header().profile_id;

    fastfix::runtime::EngineConfig config;
    config.worker_count = 1U;
    config.enable_metrics = false;
    config.trace_mode = fastfix::runtime::TraceMode::kDisabled;
    config.profile_artifacts.push_back(artifact_path);
    config.listeners.push_back(fastfix::runtime::ListenerConfig{
        .name = "whitelist-reject-main",
        .host = "127.0.0.1",
        .port = 0U,
        .worker_hint = 0U,
    });

    fastfix::runtime::Engine engine;
    REQUIRE(engine.Boot(config).ok());

    fastfix::runtime::CounterpartyConfig tmpl;
    tmpl.name = "whitelist-template";
    tmpl.session.session_id = 7201U;
    tmpl.session.profile_id = profile_id;
    tmpl.session.heartbeat_interval_seconds = 1U;
    tmpl.session.is_initiator = false;
    tmpl.dispatch_mode = fastfix::runtime::AppDispatchMode::kInline;
    tmpl.validation_policy = fastfix::session::ValidationPolicy::Permissive();

    fastfix::runtime::WhitelistSessionFactory whitelist;
    // Only allow sender_comp_id "ALLOWED-SENDER" — we connect as initiator "NOT-ON-LIST"
    // which means the acceptor sees session key {FIX.4.2, SELL, NOT-ON-LIST} where
    // sender_comp_id is "SELL", not "ALLOWED-SENDER".
    whitelist.Allow("FIX.4.2", "ALLOWED-SENDER", tmpl);
    engine.SetSessionFactory(whitelist);

    fastfix::runtime::LiveAcceptor runtime(
        &engine,
        fastfix::runtime::LiveAcceptor::Options{
            .poll_timeout = std::chrono::milliseconds(25),
            .io_timeout = std::chrono::seconds(5),
        });
    REQUIRE(runtime.OpenListeners("whitelist-reject-main").ok());

    auto port = runtime.listener_port("whitelist-reject-main");
    REQUIRE(port.ok());

    std::promise<fastfix::base::Status> runtime_result;
    auto runtime_future = runtime_result.get_future();
    std::jthread runtime_thread([&runtime, &runtime_result]() {
        runtime_result.set_value(runtime.Run(0U, std::chrono::seconds(3)));
    });

    auto connection = fastfix::transport::TcpConnection::Connect("127.0.0.1", port.value(), std::chrono::seconds(5));
    REQUIRE(connection.ok());

    fastfix::store::MemorySessionStore store;
    fastfix::session::AdminProtocol initiator(
        fastfix::session::AdminProtocolConfig{
            .session = fastfix::session::SessionConfig{
                .session_id = 9201U,
                .key = fastfix::session::SessionKey{"FIX.4.2", "NOT-ON-LIST", "SELL"},
                .profile_id = profile_id,
                .heartbeat_interval_seconds = 1U,
                .is_initiator = true,
            },
            .begin_string = "FIX.4.2",
            .sender_comp_id = "NOT-ON-LIST",
            .target_comp_id = "SELL",
            .heartbeat_interval_seconds = 1U,
            .validation_policy = fastfix::session::ValidationPolicy::Permissive(),
        },
        dictionary.value(),
        &store);

    auto start = initiator.OnTransportConnected(NowNs());
    REQUIRE(start.ok());
    for (const auto& outbound : start.value().outbound_frames) {
        auto send_status = connection.value().Send(outbound.bytes, std::chrono::seconds(5));
        REQUIRE(send_status.ok());
    }

    // Connection should be closed by the acceptor.
    auto frame = connection.value().ReceiveFrame(std::chrono::seconds(3));
    connection.value().Close();

    runtime.Stop();
    (void)runtime_future.get();
    REQUIRE(runtime.completed_session_count() == 0U);
}

TEST_CASE("busy-poll mode", "[live-runtime]") {
    auto dictionary = fastfix::tests::LoadFix44DictionaryView();
    if (!dictionary.ok()) {
        SKIP("FIX44 artifact not available: " << dictionary.status().message());
    }
    const auto artifact_path =
        std::filesystem::path(FASTFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.art";
    const auto profile_id = dictionary.value().profile().header().profile_id;

    fastfix::runtime::EngineConfig config;
    config.worker_count = 1U;
    config.enable_metrics = true;
    config.poll_mode = fastfix::runtime::PollMode::kBusy;
    config.profile_artifacts.push_back(artifact_path);
    config.listeners.push_back(fastfix::runtime::ListenerConfig{
        .name = "busy-main",
        .host = "127.0.0.1",
        .port = 0U,
        .worker_hint = 0U,
    });
    config.counterparties.push_back(fastfix::runtime::CounterpartyConfig{
        .name = "sell-buy-busy",
        .session = fastfix::session::SessionConfig{
            .session_id = 3301U,
            .key = fastfix::session::SessionKey{"FIX.4.2", "SELL", "BUY"},
            .profile_id = profile_id,
            .heartbeat_interval_seconds = 1U,
            .is_initiator = false,
        },
        .store_path = {},
        .default_appl_ver_id = {},
        .store_mode = fastfix::runtime::StoreMode::kMemory,
        .recovery_mode = fastfix::session::RecoveryMode::kMemoryOnly,
        .dispatch_mode = fastfix::runtime::AppDispatchMode::kInline,
        .validation_policy = fastfix::session::ValidationPolicy::Permissive(),
    });

    fastfix::runtime::Engine engine;
    REQUIRE(engine.Boot(config).ok());

    auto application = std::make_shared<MixedApplication>(config.worker_count);
    fastfix::runtime::LiveAcceptor runtime(
        &engine,
        fastfix::runtime::LiveAcceptor::Options{
            .poll_timeout = std::chrono::milliseconds(50),
            .io_timeout = std::chrono::seconds(5),
            .application = application,
        });
    REQUIRE(runtime.OpenListeners("busy-main").ok());
    auto port = runtime.listener_port("busy-main");
    REQUIRE(port.ok());

    std::promise<fastfix::base::Status> runtime_result;
    auto runtime_future = runtime_result.get_future();
    std::jthread runtime_thread([&runtime, &runtime_result]() {
        runtime_result.set_value(runtime.Run(1U, std::chrono::seconds(10)));
    });

    auto status = RunInitiatorEchoSession(port.value(), dictionary.value(), 4301U, "FIX.4.2", {}, "BUSY-PARTY");
    REQUIRE(status.ok());

    REQUIRE(runtime_future.get().ok());
    REQUIRE(runtime.completed_session_count() == 1U);
    REQUIRE(application->application_events() == 1U);
}

TEST_CASE("busy-poll mode no missed events", "[live-runtime]") {
    auto dictionary = fastfix::tests::LoadFix44DictionaryView();
    if (!dictionary.ok()) {
        SKIP("FIX44 artifact not available: " << dictionary.status().message());
    }
    const auto artifact_path =
        std::filesystem::path(FASTFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.art";
    const auto profile_id = dictionary.value().profile().header().profile_id;

    fastfix::runtime::EngineConfig config;
    config.worker_count = 1U;
    config.enable_metrics = true;
    config.poll_mode = fastfix::runtime::PollMode::kBusy;
    config.profile_artifacts.push_back(artifact_path);
    config.listeners.push_back(fastfix::runtime::ListenerConfig{
        .name = "busy-multi-main",
        .host = "127.0.0.1",
        .port = 0U,
        .worker_hint = 0U,
    });
    config.counterparties.push_back(fastfix::runtime::CounterpartyConfig{
        .name = "sell-buy-busy-a",
        .session = fastfix::session::SessionConfig{
            .session_id = 3302U,
            .key = fastfix::session::SessionKey{"FIX.4.2", "SELL", "BUYA"},
            .profile_id = profile_id,
            .heartbeat_interval_seconds = 1U,
            .is_initiator = false,
        },
        .store_path = {},
        .default_appl_ver_id = {},
        .store_mode = fastfix::runtime::StoreMode::kMemory,
        .recovery_mode = fastfix::session::RecoveryMode::kMemoryOnly,
        .dispatch_mode = fastfix::runtime::AppDispatchMode::kInline,
        .validation_policy = fastfix::session::ValidationPolicy::Permissive(),
    });
    config.counterparties.push_back(fastfix::runtime::CounterpartyConfig{
        .name = "sell-buy-busy-b",
        .session = fastfix::session::SessionConfig{
            .session_id = 3303U,
            .key = fastfix::session::SessionKey{"FIX.4.2", "SELL", "BUYB"},
            .profile_id = profile_id,
            .heartbeat_interval_seconds = 1U,
            .is_initiator = false,
        },
        .store_path = {},
        .default_appl_ver_id = {},
        .store_mode = fastfix::runtime::StoreMode::kMemory,
        .recovery_mode = fastfix::session::RecoveryMode::kMemoryOnly,
        .dispatch_mode = fastfix::runtime::AppDispatchMode::kInline,
        .validation_policy = fastfix::session::ValidationPolicy::Permissive(),
    });

    fastfix::runtime::Engine engine;
    REQUIRE(engine.Boot(config).ok());

    auto application = std::make_shared<MixedApplication>(config.worker_count);
    fastfix::runtime::LiveAcceptor runtime(
        &engine,
        fastfix::runtime::LiveAcceptor::Options{
            .poll_timeout = std::chrono::milliseconds(50),
            .io_timeout = std::chrono::seconds(5),
            .application = application,
        });
    REQUIRE(runtime.OpenListeners("busy-multi-main").ok());
    auto port = runtime.listener_port("busy-multi-main");
    REQUIRE(port.ok());

    std::promise<fastfix::base::Status> runtime_result;
    auto runtime_future = runtime_result.get_future();
    std::jthread runtime_thread([&runtime, &runtime_result]() {
        runtime_result.set_value(runtime.Run(2U, std::chrono::seconds(10)));
    });

    auto status_a = RunInitiatorEchoSession(
        port.value(), dictionary.value(), 4302U, "FIX.4.2", {}, "BUYA", "SELL", "BUSY-A");
    REQUIRE(status_a.ok());
    auto status_b = RunInitiatorEchoSession(
        port.value(), dictionary.value(), 4303U, "FIX.4.2", {}, "BUYB", "SELL", "BUSY-B");
    REQUIRE(status_b.ok());

    REQUIRE(runtime_future.get().ok());
    REQUIRE(runtime.completed_session_count() == 2U);
    REQUIRE(application->application_events() == 2U);
}

TEST_CASE("epoll backend single session", "[live-runtime]") {
    auto dictionary = fastfix::tests::LoadFix44DictionaryView();
    if (!dictionary.ok()) {
        SKIP("FIX44 artifact not available: " << dictionary.status().message());
    }
    if (!fastfix::runtime::IsIoBackendAvailable(fastfix::runtime::IoBackend::kEpoll)) {
        SKIP("epoll not available");
    }
    const auto artifact_path =
        std::filesystem::path(FASTFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.art";
    const auto profile_id = dictionary.value().profile().header().profile_id;

    fastfix::runtime::EngineConfig config;
    config.worker_count = 1U;
    config.enable_metrics = true;
    config.io_backend = fastfix::runtime::IoBackend::kEpoll;
    config.profile_artifacts.push_back(artifact_path);
    config.listeners.push_back(fastfix::runtime::ListenerConfig{
        .name = "epoll-main",
        .host = "127.0.0.1",
        .port = 0U,
        .worker_hint = 0U,
    });
    config.counterparties.push_back(fastfix::runtime::CounterpartyConfig{
        .name = "sell-buy-epoll",
        .session = fastfix::session::SessionConfig{
            .session_id = 3401U,
            .key = fastfix::session::SessionKey{"FIX.4.2", "SELL", "BUY"},
            .profile_id = profile_id,
            .heartbeat_interval_seconds = 1U,
            .is_initiator = false,
        },
        .store_path = {},
        .default_appl_ver_id = {},
        .store_mode = fastfix::runtime::StoreMode::kMemory,
        .recovery_mode = fastfix::session::RecoveryMode::kMemoryOnly,
        .dispatch_mode = fastfix::runtime::AppDispatchMode::kInline,
        .validation_policy = fastfix::session::ValidationPolicy::Permissive(),
    });

    fastfix::runtime::Engine engine;
    REQUIRE(engine.Boot(config).ok());

    auto application = std::make_shared<MixedApplication>(config.worker_count);
    fastfix::runtime::LiveAcceptor runtime(
        &engine,
        fastfix::runtime::LiveAcceptor::Options{
            .poll_timeout = std::chrono::milliseconds(50),
            .io_timeout = std::chrono::seconds(5),
            .application = application,
        });
    REQUIRE(runtime.OpenListeners("epoll-main").ok());
    auto port = runtime.listener_port("epoll-main");
    REQUIRE(port.ok());

    std::promise<fastfix::base::Status> runtime_result;
    auto runtime_future = runtime_result.get_future();
    std::jthread runtime_thread([&runtime, &runtime_result]() {
        runtime_result.set_value(runtime.Run(1U, std::chrono::seconds(10)));
    });

    auto status = RunInitiatorEchoSession(port.value(), dictionary.value(), 4401U, "FIX.4.2", {}, "EPOLL-PARTY");
    REQUIRE(status.ok());

    REQUIRE(runtime_future.get().ok());
    REQUIRE(runtime.completed_session_count() == 1U);
    REQUIRE(application->application_events() == 1U);
}

TEST_CASE("io_uring backend single session", "[live-runtime]") {
    auto dictionary = fastfix::tests::LoadFix44DictionaryView();
    if (!dictionary.ok()) {
        SKIP("FIX44 artifact not available: " << dictionary.status().message());
    }
    if (!fastfix::runtime::IsIoBackendAvailable(fastfix::runtime::IoBackend::kIoUring)) {
        SUCCEED("io_uring not available");
        return;
    }
    const auto artifact_path =
        std::filesystem::path(FASTFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.art";
    const auto profile_id = dictionary.value().profile().header().profile_id;

    fastfix::runtime::EngineConfig config;
    config.worker_count = 1U;
    config.enable_metrics = true;
    config.io_backend = fastfix::runtime::IoBackend::kIoUring;
    config.profile_artifacts.push_back(artifact_path);
    config.listeners.push_back(fastfix::runtime::ListenerConfig{
        .name = "uring-main",
        .host = "127.0.0.1",
        .port = 0U,
        .worker_hint = 0U,
    });
    config.counterparties.push_back(fastfix::runtime::CounterpartyConfig{
        .name = "sell-buy-uring",
        .session = fastfix::session::SessionConfig{
            .session_id = 3501U,
            .key = fastfix::session::SessionKey{"FIX.4.2", "SELL", "BUY"},
            .profile_id = profile_id,
            .heartbeat_interval_seconds = 1U,
            .is_initiator = false,
        },
        .store_path = {},
        .default_appl_ver_id = {},
        .store_mode = fastfix::runtime::StoreMode::kMemory,
        .recovery_mode = fastfix::session::RecoveryMode::kMemoryOnly,
        .dispatch_mode = fastfix::runtime::AppDispatchMode::kInline,
        .validation_policy = fastfix::session::ValidationPolicy::Permissive(),
    });

    fastfix::runtime::Engine engine;
    REQUIRE(engine.Boot(config).ok());

    auto application = std::make_shared<MixedApplication>(config.worker_count);
    fastfix::runtime::LiveAcceptor runtime(
        &engine,
        fastfix::runtime::LiveAcceptor::Options{
            .poll_timeout = std::chrono::milliseconds(50),
            .io_timeout = std::chrono::seconds(5),
            .application = application,
        });
    REQUIRE(runtime.OpenListeners("uring-main").ok());
    auto port = runtime.listener_port("uring-main");
    REQUIRE(port.ok());

    std::promise<fastfix::base::Status> runtime_result;
    auto runtime_future = runtime_result.get_future();
    std::jthread runtime_thread([&runtime, &runtime_result]() {
        runtime_result.set_value(runtime.Run(1U, std::chrono::seconds(10)));
    });

    auto status = RunInitiatorEchoSession(port.value(), dictionary.value(), 4501U, "FIX.4.2", {}, "URING-PARTY");
    REQUIRE(status.ok());

    REQUIRE(runtime_future.get().ok());
    REQUIRE(runtime.completed_session_count() == 1U);
    REQUIRE(application->application_events() == 1U);
}
