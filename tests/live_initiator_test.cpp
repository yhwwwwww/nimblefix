#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <future>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_set>
#include <vector>

#include "fastfix/codec/fix_tags.h"
#include "fastfix/runtime/application.h"
#include "fastfix/runtime/engine.h"
#include "fastfix/runtime/live_initiator.h"
#include "fastfix/session/admin_protocol.h"
#include "fastfix/store/memory_store.h"
#include "fastfix/transport/tcp_transport.h"

#include "test_support.h"

namespace {

using namespace fastfix::codec::tags;

auto NowNs() -> std::uint64_t {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
                                              .count());
}

auto BuildInitiatorMessage() -> fastfix::message::Message {
    fastfix::message::MessageBuilder builder("D");
    builder.set_string(kMsgType, "D");
    auto party = builder.add_group_entry(kNoPartyIDs);
    party.set_string(kPartyID, "INITIATOR-PARTY").set_char(kPartyIDSource, 'D').set_int(kPartyRole, 3);
    return std::move(builder).build();
}

auto ValidateInitiatorEcho(fastfix::message::MessageView message) -> fastfix::base::Status {
    const auto group = message.group(kNoPartyIDs);
    if (!group.has_value() || group->size() != 1U ||
        (*group)[0].get_string(kPartyID) != std::optional<std::string_view>{"INITIATOR-PARTY"}) {
        return fastfix::base::Status::InvalidArgument("runtime initiator application received an invalid echo");
    }
    return fastfix::base::Status::Ok();
}

auto RunAcceptorEchoSession(
    fastfix::transport::TcpAcceptor acceptor_socket,
    fastfix::profile::NormalizedDictionaryView dictionary,
    std::uint64_t session_id,
    std::string sender_comp_id,
    std::string target_comp_id) -> fastfix::base::Status {
    fastfix::store::MemorySessionStore store;
    fastfix::session::AdminProtocol protocol(
        fastfix::session::AdminProtocolConfig{
            .session = fastfix::session::SessionConfig{
                .session_id = session_id,
                .key = fastfix::session::SessionKey{"FIX.4.4", sender_comp_id, target_comp_id},
                .profile_id = dictionary.profile().header().profile_id,
                .heartbeat_interval_seconds = 1U,
                .is_initiator = false,
            },
            .begin_string = "FIX.4.4",
            .sender_comp_id = sender_comp_id,
            .target_comp_id = target_comp_id,
            .heartbeat_interval_seconds = 1U,
            .validation_policy = fastfix::session::ValidationPolicy::Permissive(),
        },
        dictionary,
        &store);

    auto inbound = acceptor_socket.Accept(std::chrono::seconds(5));
    if (!inbound.ok()) {
        return inbound.status();
    }

    auto connection = std::move(inbound).value();
    auto connected = protocol.OnTransportConnected(NowNs());
    if (!connected.ok()) {
        return connected.status();
    }

    while (true) {
        auto frame = connection.ReceiveFrame(std::chrono::seconds(5));
        if (!frame.ok()) {
            return frame.status();
        }

        auto event = protocol.OnInbound(frame.value(), NowNs());
        if (!event.ok()) {
            return event.status();
        }

        for (const auto& outbound : event.value().outbound_frames) {
            auto status = connection.Send(outbound.bytes, std::chrono::seconds(5));
            if (!status.ok()) {
                return status;
            }
        }

        for (const auto& message : event.value().application_messages) {
            auto echo = protocol.SendApplication(message, NowNs());
            if (!echo.ok()) {
                return echo.status();
            }
            auto status = connection.Send(echo.value().bytes, std::chrono::seconds(5));
            if (!status.ok()) {
                return status;
            }
        }

        if (!event.value().disconnect) {
            continue;
        }

        auto close_status = protocol.OnTransportClosed();
        connection.Close();
        return close_status;
    }
}

auto RunAcceptorHeartbeatTimerSession(
    fastfix::transport::TcpAcceptor acceptor_socket,
    fastfix::profile::NormalizedDictionaryView dictionary,
    std::uint64_t session_id) -> fastfix::base::Status {
    fastfix::store::MemorySessionStore store;
    fastfix::session::AdminProtocol protocol(
        fastfix::session::AdminProtocolConfig{
            .session = fastfix::session::SessionConfig{
                .session_id = session_id,
                .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
                .profile_id = dictionary.profile().header().profile_id,
                .heartbeat_interval_seconds = 1U,
                .is_initiator = false,
            },
            .begin_string = "FIX.4.4",
            .sender_comp_id = "SELL",
            .target_comp_id = "BUY",
            .heartbeat_interval_seconds = 1U,
            .validation_policy = fastfix::session::ValidationPolicy::Permissive(),
        },
        dictionary,
        &store);

    auto inbound = acceptor_socket.Accept(std::chrono::seconds(5));
    if (!inbound.ok()) {
        return inbound.status();
    }

    auto connection = std::move(inbound).value();
    auto connected = protocol.OnTransportConnected(NowNs());
    if (!connected.ok()) {
        return connected.status();
    }

    bool saw_heartbeat = false;
    bool sent_logout = false;
    while (true) {
        auto frame = connection.ReceiveFrame(sent_logout ? std::chrono::seconds(5) : std::chrono::seconds(3));
        if (!frame.ok()) {
            return frame.status();
        }

        auto header = fastfix::codec::PeekSessionHeaderView(frame.value());
        if (!header.ok()) {
            return header.status();
        }

        auto event = protocol.OnInbound(frame.value(), NowNs());
        if (!event.ok()) {
            return event.status();
        }

        for (const auto& outbound : event.value().outbound_frames) {
            auto status = connection.Send(outbound.bytes, std::chrono::seconds(5));
            if (!status.ok()) {
                return status;
            }
        }

        if (!sent_logout && header.value().msg_type == "0" &&
            protocol.session().state() == fastfix::session::SessionState::kActive) {
            saw_heartbeat = true;
            auto logout = protocol.BeginLogout({}, NowNs());
            if (!logout.ok()) {
                return logout.status();
            }
            auto status = connection.Send(logout.value().bytes, std::chrono::seconds(5));
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
                "live initiator did not emit heartbeat before the long poll timeout elapsed");
        }

        auto close_status = protocol.OnTransportClosed();
        connection.Close();
        return close_status;
    }
}

auto RunAcceptorAwaitLogoutSession(
    fastfix::transport::TcpAcceptor acceptor_socket,
    fastfix::profile::NormalizedDictionaryView dictionary,
    std::uint64_t session_id) -> fastfix::base::Status {
    fastfix::store::MemorySessionStore store;
    fastfix::session::AdminProtocol protocol(
        fastfix::session::AdminProtocolConfig{
            .session = fastfix::session::SessionConfig{
                .session_id = session_id,
                .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
                .profile_id = dictionary.profile().header().profile_id,
                .heartbeat_interval_seconds = 30U,
                .is_initiator = false,
            },
            .begin_string = "FIX.4.4",
            .sender_comp_id = "SELL",
            .target_comp_id = "BUY",
            .heartbeat_interval_seconds = 30U,
            .validation_policy = fastfix::session::ValidationPolicy::Permissive(),
        },
        dictionary,
        &store);

    auto inbound = acceptor_socket.Accept(std::chrono::seconds(5));
    if (!inbound.ok()) {
        return inbound.status();
    }

    auto connection = std::move(inbound).value();
    auto connected = protocol.OnTransportConnected(NowNs());
    if (!connected.ok()) {
        return connected.status();
    }

    bool session_active = false;
    bool saw_logout = false;
    while (true) {
        auto frame = connection.ReceiveFrame(session_active ? std::chrono::seconds(1) : std::chrono::seconds(5));
        if (!frame.ok()) {
            return frame.status();
        }

        auto header = fastfix::codec::PeekSessionHeaderView(frame.value());
        if (!header.ok()) {
            return header.status();
        }
        if (header.value().msg_type == "5") {
            saw_logout = true;
        }

        auto event = protocol.OnInbound(frame.value(), NowNs());
        if (!event.ok()) {
            return event.status();
        }
        if (event.value().session_active) {
            session_active = true;
        }

        for (const auto& outbound : event.value().outbound_frames) {
            auto status = connection.Send(outbound.bytes, std::chrono::seconds(5));
            if (!status.ok()) {
                return status;
            }
        }

        if (!event.value().disconnect) {
            continue;
        }

        if (!saw_logout) {
            return fastfix::base::Status::InvalidArgument(
                "live initiator did not wake promptly for an externally requested logout");
        }

        auto close_status = protocol.OnTransportClosed();
        connection.Close();
        return close_status;
    }
}

class InitiatorApplication final : public fastfix::runtime::ApplicationCallbacks {
  public:
    InitiatorApplication(fastfix::runtime::LiveInitiator* initiator, std::uint64_t session_id)
        : initiator_(initiator),
          session_id_(session_id) {
    }

        auto BindInitiator(fastfix::runtime::LiveInitiator* initiator) -> void {
                initiator_ = initiator;
        }

    auto OnSessionEvent(const fastfix::runtime::RuntimeEvent& event) -> fastfix::base::Status override {
        ++session_events_;
        auto status = TrackSession(event.handle);
        if (!status.ok()) {
            return status;
        }
        if (event.handle.session_id() != session_id_ ||
            event.session_event != fastfix::runtime::SessionEventKind::kActive ||
            sent_application_.exchange(true)) {
            return fastfix::base::Status::Ok();
        }
        return event.handle.Send(BuildInitiatorMessage());
    }

    auto OnAdminMessage(const fastfix::runtime::RuntimeEvent& event) -> fastfix::base::Status override {
        if (event.handle.session_id() == session_id_) {
            ++admin_events_;
        }
        return fastfix::base::Status::Ok();
    }

    auto OnAppMessage(const fastfix::runtime::RuntimeEvent& event) -> fastfix::base::Status override {
        if (event.handle.session_id() != session_id_) {
            return fastfix::base::Status::Ok();
        }

        ++application_events_;
        auto status = ValidateInitiatorEcho(event.message.view());
        if (!status.ok()) {
            return status;
        }

        received_echo_.store(true);
        return initiator_->RequestLogout(session_id_);
    }

    [[nodiscard]] auto received_echo() const -> bool {
        return received_echo_.load();
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

    [[nodiscard]] auto Snapshot() const -> std::optional<fastfix::session::SessionSnapshot> {
        std::lock_guard lock(mutex_);
        return snapshot_;
    }

    auto DrainSubscription() -> fastfix::base::Result<std::vector<fastfix::session::SessionNotification>> {
        fastfix::session::SessionSubscription subscription;
        {
            std::lock_guard lock(mutex_);
            if (!subscription_.valid()) {
                return fastfix::base::Status::NotFound("initiator runtime subscription was not registered");
            }
            subscription = subscription_;
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
        snapshot_ = snapshot.value();
        if (!subscription_.valid()) {
            auto subscription = handle.Subscribe(32U);
            if (!subscription.ok()) {
                return subscription.status();
            }
            subscription_ = std::move(subscription).value();
        }
        return fastfix::base::Status::Ok();
    }

    fastfix::runtime::LiveInitiator* initiator_{nullptr};
    std::uint64_t session_id_{0};
    mutable std::mutex mutex_;
    std::optional<fastfix::session::SessionSnapshot> snapshot_;
    fastfix::session::SessionSubscription subscription_;
    std::atomic<bool> sent_application_{false};
    std::atomic<bool> received_echo_{false};
    std::atomic<std::uint64_t> session_events_{0};
    std::atomic<std::uint64_t> admin_events_{0};
    std::atomic<std::uint64_t> application_events_{0};
};

class MultiInitiatorApplication final : public fastfix::runtime::ApplicationCallbacks {
  public:
    MultiInitiatorApplication(
        fastfix::runtime::LiveInitiator* initiator,
        std::vector<std::uint64_t> session_ids)
        : initiator_(initiator),
          session_ids_(session_ids.begin(), session_ids.end()) {
    }

    auto BindInitiator(fastfix::runtime::LiveInitiator* initiator) -> void {
        initiator_ = initiator;
    }

    auto OnSessionEvent(const fastfix::runtime::RuntimeEvent& event) -> fastfix::base::Status override {
        if (!session_ids_.contains(event.handle.session_id()) ||
            event.session_event != fastfix::runtime::SessionEventKind::kActive) {
            return fastfix::base::Status::Ok();
        }

        {
            std::lock_guard lock(mutex_);
            if (!sent_sessions_.insert(event.handle.session_id()).second) {
                return fastfix::base::Status::Ok();
            }
        }

        return event.handle.Send(BuildInitiatorMessage());
    }

    auto OnAdminMessage(const fastfix::runtime::RuntimeEvent& event) -> fastfix::base::Status override {
        if (session_ids_.contains(event.handle.session_id())) {
            ++admin_events_;
        }
        return fastfix::base::Status::Ok();
    }

    auto OnAppMessage(const fastfix::runtime::RuntimeEvent& event) -> fastfix::base::Status override {
        if (!session_ids_.contains(event.handle.session_id())) {
            return fastfix::base::Status::Ok();
        }

        auto status = ValidateInitiatorEcho(event.message.view());
        if (!status.ok()) {
            return status;
        }

        {
            std::lock_guard lock(mutex_);
            received_sessions_.insert(event.handle.session_id());
        }
        ++application_events_;
        return initiator_->RequestLogout(event.handle.session_id());
    }

    [[nodiscard]] auto AllReceivedEchoes() const -> bool {
        std::lock_guard lock(mutex_);
        return received_sessions_.size() == session_ids_.size();
    }

    [[nodiscard]] auto admin_events() const -> std::uint64_t {
        return admin_events_.load();
    }

    [[nodiscard]] auto application_events() const -> std::uint64_t {
        return application_events_.load();
    }

  private:
    fastfix::runtime::LiveInitiator* initiator_{nullptr};
    std::unordered_set<std::uint64_t> session_ids_;
    mutable std::mutex mutex_;
    std::unordered_set<std::uint64_t> sent_sessions_;
    std::unordered_set<std::uint64_t> received_sessions_;
    std::atomic<std::uint64_t> admin_events_{0};
    std::atomic<std::uint64_t> application_events_{0};
};

class PassiveInitiatorApplication final : public fastfix::runtime::ApplicationCallbacks {
    public:
        explicit PassiveInitiatorApplication(std::uint64_t session_id)
                : session_id_(session_id),
                    active_future_(active_promise_.get_future()) {
        }

        auto OnSessionEvent(const fastfix::runtime::RuntimeEvent& event) -> fastfix::base::Status override {
                if (event.handle.session_id() == session_id_ &&
                        event.session_event == fastfix::runtime::SessionEventKind::kActive &&
                        !active_seen_.exchange(true)) {
                        active_promise_.set_value();
                }
                return fastfix::base::Status::Ok();
        }

        [[nodiscard]] auto WaitForActive(std::chrono::milliseconds timeout) -> bool {
                return active_future_.wait_for(timeout) == std::future_status::ready;
        }

    private:
        std::uint64_t session_id_{0};
        std::promise<void> active_promise_;
        std::future<void> active_future_;
        std::atomic<bool> active_seen_{false};
};

struct QueueInitiatorState {
    std::atomic<bool> sent_application{false};
    std::atomic<bool> received_echo{false};
};

class QueueInitiatorApplication final : public fastfix::runtime::ApplicationCallbacks,
                                        public fastfix::runtime::QueueApplicationProvider {
  public:
    QueueInitiatorApplication(
        std::uint64_t session_id,
        std::uint32_t worker_count,
        std::shared_ptr<QueueInitiatorState> state)
        : session_id_(session_id),
          state_(std::move(state)),
          queue_(worker_count) {
    }

    auto OnSessionEvent(const fastfix::runtime::RuntimeEvent& event) -> fastfix::base::Status override {
        ++session_events_;
        auto status = TrackSession(event.handle);
        if (!status.ok()) {
            return status;
        }
        if (event.handle.session_id() != session_id_) {
            return fastfix::base::Status::Ok();
        }
        return queue_.OnSessionEvent(event);
    }

    auto OnAdminMessage(const fastfix::runtime::RuntimeEvent& event) -> fastfix::base::Status override {
        if (event.handle.session_id() == session_id_) {
            ++admin_events_;
            return queue_.OnAdminMessage(event);
        }
        return fastfix::base::Status::Ok();
    }

    auto OnAppMessage(const fastfix::runtime::RuntimeEvent& event) -> fastfix::base::Status override {
        if (event.handle.session_id() == session_id_) {
            ++application_events_;
            return queue_.OnAppMessage(event);
        }
        return fastfix::base::Status::Ok();
    }

    [[nodiscard]] auto received_echo() const -> bool {
        return state_->received_echo.load();
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

    [[nodiscard]] auto Snapshot() const -> std::optional<fastfix::session::SessionSnapshot> {
        std::lock_guard lock(mutex_);
        return snapshot_;
    }

    auto DrainSubscription() -> fastfix::base::Result<std::vector<fastfix::session::SessionNotification>> {
        fastfix::session::SessionSubscription subscription;
        {
            std::lock_guard lock(mutex_);
            if (!subscription_.valid()) {
                return fastfix::base::Status::NotFound("initiator runtime subscription was not registered");
            }
            subscription = subscription_;
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

    auto queue_application() -> fastfix::runtime::QueueApplication& override {
        return queue_;
    }

  private:
    auto TrackSession(const fastfix::session::SessionHandle& handle) -> fastfix::base::Status {
        auto snapshot = handle.Snapshot();
        if (!snapshot.ok()) {
            return snapshot.status();
        }

        std::lock_guard lock(mutex_);
        snapshot_ = snapshot.value();
        if (!subscription_.valid()) {
            auto subscription = handle.Subscribe(32U);
            if (!subscription.ok()) {
                return subscription.status();
            }
            subscription_ = std::move(subscription).value();
        }
        return fastfix::base::Status::Ok();
    }

    std::uint64_t session_id_{0};
    std::shared_ptr<QueueInitiatorState> state_;
    fastfix::runtime::QueueApplication queue_;
    mutable std::mutex mutex_;
    std::optional<fastfix::session::SessionSnapshot> snapshot_;
    fastfix::session::SessionSubscription subscription_;
    std::atomic<std::uint64_t> session_events_{0};
    std::atomic<std::uint64_t> admin_events_{0};
    std::atomic<std::uint64_t> application_events_{0};
};

class QueueInitiatorHandler final : public fastfix::runtime::QueueApplicationEventHandler {
  public:
    QueueInitiatorHandler(
        fastfix::runtime::LiveInitiator* initiator,
        std::uint64_t session_id,
        std::shared_ptr<QueueInitiatorState> state)
        : initiator_(initiator),
          session_id_(session_id),
          state_(std::move(state)) {
    }

    auto BindInitiator(fastfix::runtime::LiveInitiator* initiator) -> void {
        initiator_ = initiator;
    }

    auto OnRuntimeEvent(const fastfix::runtime::RuntimeEvent& event) -> fastfix::base::Status override {
        if (event.handle.session_id() != session_id_) {
            return fastfix::base::Status::Ok();
        }

        if (event.kind == fastfix::runtime::RuntimeEventKind::kSession &&
            event.session_event == fastfix::runtime::SessionEventKind::kActive &&
            !state_->sent_application.exchange(true)) {
            return event.handle.Send(BuildInitiatorMessage());
        }

        if (event.kind != fastfix::runtime::RuntimeEventKind::kApplicationMessage) {
            return fastfix::base::Status::Ok();
        }

        auto status = ValidateInitiatorEcho(event.message.view());
        if (!status.ok()) {
            return status;
        }
        state_->received_echo.store(true);
        return initiator_->RequestLogout(session_id_);
    }

  private:
    fastfix::runtime::LiveInitiator* initiator_{nullptr};
    std::uint64_t session_id_{0};
    std::shared_ptr<QueueInitiatorState> state_;
};

}  // namespace

TEST_CASE("live-initiator", "[live-initiator]") {
    auto dictionary = fastfix::tests::LoadFix44DictionaryView();
    if (!dictionary.ok()) {
        SKIP("FIX44 artifact not available: " << dictionary.status().message());
    }
    const auto artifact_path =
        std::filesystem::path(FASTFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.art";
    const auto profile_id = dictionary.value().profile().header().profile_id;

    auto acceptor = fastfix::transport::TcpAcceptor::Listen("127.0.0.1", 0U);
    REQUIRE(acceptor.ok());
    const auto listen_port = acceptor.value().port();

    std::promise<fastfix::base::Status> acceptor_result;
    auto acceptor_future = acceptor_result.get_future();
    std::jthread acceptor_thread([
        acceptor_socket = std::move(acceptor).value(),
        dictionary = dictionary.value(),
        &acceptor_result]() mutable {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 2201U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
                    .profile_id = dictionary.profile().header().profile_id,
                    .heartbeat_interval_seconds = 1U,
                    .is_initiator = false,
                },
                .begin_string = "FIX.4.4",
                .sender_comp_id = "SELL",
                .target_comp_id = "BUY",
                .heartbeat_interval_seconds = 1U,
                .validation_policy = fastfix::session::ValidationPolicy::Permissive(),
            },
            dictionary,
            &store);

        auto inbound = acceptor_socket.Accept(std::chrono::seconds(5));
        if (!inbound.ok()) {
            acceptor_result.set_value(inbound.status());
            return;
        }

        auto connection = std::move(inbound).value();
        auto connected = protocol.OnTransportConnected(NowNs());
        if (!connected.ok()) {
            acceptor_result.set_value(connected.status());
            return;
        }

        while (true) {
            auto frame = connection.ReceiveFrame(std::chrono::seconds(5));
            if (!frame.ok()) {
                acceptor_result.set_value(frame.status());
                return;
            }

            auto event = protocol.OnInbound(frame.value(), NowNs());
            if (!event.ok()) {
                acceptor_result.set_value(event.status());
                return;
            }

            for (const auto& outbound : event.value().outbound_frames) {
                auto status = connection.Send(outbound.bytes, std::chrono::seconds(5));
                if (!status.ok()) {
                    acceptor_result.set_value(status);
                    return;
                }
            }

            for (const auto& message : event.value().application_messages) {
                auto echo = protocol.SendApplication(message, NowNs());
                if (!echo.ok()) {
                    acceptor_result.set_value(echo.status());
                    return;
                }
                auto status = connection.Send(echo.value().bytes, std::chrono::seconds(5));
                if (!status.ok()) {
                    acceptor_result.set_value(status);
                    return;
                }
            }

            if (event.value().disconnect) {
                auto close_status = protocol.OnTransportClosed();
                connection.Close();
                acceptor_result.set_value(close_status);
                return;
            }
        }
    });

    fastfix::runtime::EngineConfig config;
    config.worker_count = 1U;
    config.enable_metrics = true;
    config.trace_mode = fastfix::runtime::TraceMode::kRing;
    config.trace_capacity = 16U;
    config.profile_artifacts.push_back(artifact_path);
    config.counterparties.push_back(fastfix::runtime::CounterpartyConfig{
        .name = "buy-sell-initiator",
        .session = fastfix::session::SessionConfig{
            .session_id = 1201U,
            .key = fastfix::session::SessionKey{"FIX.4.4", "BUY", "SELL"},
            .profile_id = profile_id,
            .default_appl_ver_id = {},
            .heartbeat_interval_seconds = 1U,
            .is_initiator = true,
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

    auto application = std::make_shared<InitiatorApplication>(nullptr, 1201U);
    fastfix::runtime::LiveInitiator runtime(
        &engine,
        fastfix::runtime::LiveInitiator::Options{
            .poll_timeout = std::chrono::milliseconds(25),
            .io_timeout = std::chrono::seconds(5),
            .application = application,
        });
    application->BindInitiator(&runtime);

    REQUIRE(runtime.OpenSession(1201U, "127.0.0.1", listen_port).ok());
    REQUIRE(runtime.Run(1U, std::chrono::seconds(10)).ok());
    REQUIRE(acceptor_future.get().ok());
    REQUIRE(runtime.completed_session_count() == 1U);
    REQUIRE(application->received_echo());
    REQUIRE(application->session_events() >= 3U);
    REQUIRE(application->admin_events() >= 2U);
    REQUIRE(application->application_events() == 1U);

    const auto snapshot = application->Snapshot();
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->state == fastfix::session::SessionState::kDisconnected);

    auto notifications = application->DrainSubscription();
    REQUIRE(notifications.ok());
    auto count_notifications = [&](fastfix::session::SessionNotificationKind kind) {
        return static_cast<std::size_t>(std::count_if(
            notifications.value().begin(),
            notifications.value().end(),
            [&](const auto& notification) { return notification.kind == kind; }));
    };
    auto find_notification = [&](fastfix::session::SessionNotificationKind kind) {
        const auto it = std::find_if(
            notifications.value().begin(),
            notifications.value().end(),
            [&](const auto& notification) { return notification.kind == kind; });
        return it == notifications.value().end() ? nullptr : &(*it);
    };
    REQUIRE(count_notifications(fastfix::session::SessionNotificationKind::kSessionBound) == 1U);
    REQUIRE(count_notifications(fastfix::session::SessionNotificationKind::kSessionActive) == 1U);
    REQUIRE(count_notifications(fastfix::session::SessionNotificationKind::kSessionClosed) == 1U);
    REQUIRE(count_notifications(fastfix::session::SessionNotificationKind::kAdminMessage) >= 2U);
    REQUIRE(count_notifications(fastfix::session::SessionNotificationKind::kApplicationMessage) == 1U);

    const auto* app_notification = find_notification(fastfix::session::SessionNotificationKind::kApplicationMessage);
    REQUIRE(app_notification != nullptr);
    REQUIRE(app_notification->message.valid());
    const auto parties = app_notification->message_view().group(kNoPartyIDs);
    REQUIRE(parties.has_value());
    REQUIRE(parties->size() == 1U);
    REQUIRE((*parties)[0].get_string(kPartyID).value() == "INITIATOR-PARTY");

    const auto* metrics = engine.metrics().FindSession(1201U);
    REQUIRE(metrics != nullptr);
    REQUIRE(metrics->inbound_messages > 0U);
    REQUIRE(metrics->outbound_messages > 0U);

    auto logout_acceptor = fastfix::transport::TcpAcceptor::Listen("127.0.0.1", 0U);
    REQUIRE(logout_acceptor.ok());
    const auto logout_listen_port = logout_acceptor.value().port();

    std::promise<fastfix::base::Status> logout_acceptor_result;
    auto logout_acceptor_future = logout_acceptor_result.get_future();
    std::jthread logout_acceptor_thread([
        acceptor_socket = std::move(logout_acceptor).value(),
        dictionary = dictionary.value(),
        &logout_acceptor_result]() mutable {
        logout_acceptor_result.set_value(
            RunAcceptorAwaitLogoutSession(std::move(acceptor_socket), std::move(dictionary), 2204U));
    });

    fastfix::runtime::EngineConfig logout_config;
    logout_config.worker_count = 1U;
    logout_config.enable_metrics = true;
    logout_config.trace_mode = fastfix::runtime::TraceMode::kRing;
    logout_config.trace_capacity = 16U;
    logout_config.profile_artifacts.push_back(artifact_path);
    logout_config.counterparties.push_back(fastfix::runtime::CounterpartyConfig{
        .name = "buy-sell-initiator-logout-wakeup",
        .session = fastfix::session::SessionConfig{
            .session_id = 1206U,
            .key = fastfix::session::SessionKey{"FIX.4.4", "BUY", "SELL"},
            .profile_id = profile_id,
            .default_appl_ver_id = {},
            .heartbeat_interval_seconds = 30U,
            .is_initiator = true,
        },
        .store_path = {},
        .default_appl_ver_id = {},
        .store_mode = fastfix::runtime::StoreMode::kMemory,
        .recovery_mode = fastfix::session::RecoveryMode::kMemoryOnly,
        .dispatch_mode = fastfix::runtime::AppDispatchMode::kInline,
        .validation_policy = fastfix::session::ValidationPolicy::Permissive(),
    });

    fastfix::runtime::Engine logout_engine;
    REQUIRE(logout_engine.Boot(logout_config).ok());

    auto logout_application = std::make_shared<PassiveInitiatorApplication>(1206U);
    fastfix::runtime::LiveInitiator logout_runtime(
        &logout_engine,
        fastfix::runtime::LiveInitiator::Options{
            .poll_timeout = std::chrono::seconds(5),
            .io_timeout = std::chrono::seconds(5),
            .application = logout_application,
        });

    REQUIRE(logout_runtime.OpenSession(1206U, "127.0.0.1", logout_listen_port).ok());

    std::promise<fastfix::base::Status> logout_runtime_result;
    auto logout_runtime_future = logout_runtime_result.get_future();
    std::jthread logout_runtime_thread([&logout_runtime, &logout_runtime_result]() {
        logout_runtime_result.set_value(logout_runtime.Run(1U, std::chrono::seconds(10)));
    });

    REQUIRE(logout_application->WaitForActive(std::chrono::seconds(5)));
    // Give the runtime loop time to re-enter poll() before the external command arrives.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    REQUIRE(logout_runtime.RequestLogout(1206U).ok());
    REQUIRE(logout_runtime_future.get().ok());
    REQUIRE(logout_acceptor_future.get().ok());
    REQUIRE(logout_runtime.completed_session_count() == 1U);

    auto routed_sender_for_worker = [](std::uint32_t worker_id) {
        std::string sender_comp_id;
        for (std::uint32_t attempt = 0U; attempt < 256U; ++attempt) {
            const auto candidate = std::string("BUY-SHARD-") + std::to_string(worker_id) + '-' + std::to_string(attempt);
            if ((fastfix::session::SessionKeyHash{}(fastfix::session::SessionKey{"FIX.4.4", candidate, "SELL"}) % 2U) == worker_id) {
                sender_comp_id = candidate;
                break;
            }
        }
        return sender_comp_id;
    };

    const auto routed_sender_0 = routed_sender_for_worker(0U);
    const auto routed_sender_1 = routed_sender_for_worker(1U);
    REQUIRE(!routed_sender_0.empty());
    REQUIRE(!routed_sender_1.empty());
    REQUIRE(routed_sender_0 != routed_sender_1);

    auto shard_acceptor_0 = fastfix::transport::TcpAcceptor::Listen("127.0.0.1", 0U);
    auto shard_acceptor_1 = fastfix::transport::TcpAcceptor::Listen("127.0.0.1", 0U);
    REQUIRE(shard_acceptor_0.ok());
    REQUIRE(shard_acceptor_1.ok());
    const auto shard_port_0 = shard_acceptor_0.value().port();
    const auto shard_port_1 = shard_acceptor_1.value().port();

    std::promise<fastfix::base::Status> shard_acceptor_result_0;
    std::promise<fastfix::base::Status> shard_acceptor_result_1;
    auto shard_acceptor_future_0 = shard_acceptor_result_0.get_future();
    auto shard_acceptor_future_1 = shard_acceptor_result_1.get_future();
    std::jthread shard_acceptor_thread_0([
        acceptor_socket = std::move(shard_acceptor_0).value(),
        dictionary = dictionary.value(),
        sender_comp_id = routed_sender_0,
        &shard_acceptor_result_0]() mutable {
        shard_acceptor_result_0.set_value(
            RunAcceptorEchoSession(std::move(acceptor_socket), std::move(dictionary), 2301U, "SELL", sender_comp_id));
    });
    std::jthread shard_acceptor_thread_1([
        acceptor_socket = std::move(shard_acceptor_1).value(),
        dictionary = dictionary.value(),
        sender_comp_id = routed_sender_1,
        &shard_acceptor_result_1]() mutable {
        shard_acceptor_result_1.set_value(
            RunAcceptorEchoSession(std::move(acceptor_socket), std::move(dictionary), 2302U, "SELL", sender_comp_id));
    });

    fastfix::runtime::EngineConfig shard_config;
    shard_config.worker_count = 2U;
    shard_config.enable_metrics = true;
    shard_config.trace_mode = fastfix::runtime::TraceMode::kRing;
    shard_config.trace_capacity = 16U;
    shard_config.profile_artifacts.push_back(artifact_path);
    shard_config.counterparties.push_back(fastfix::runtime::CounterpartyConfig{
        .name = "buy-sell-initiator-shard-0",
        .session = fastfix::session::SessionConfig{
            .session_id = 1204U,
            .key = fastfix::session::SessionKey{"FIX.4.4", routed_sender_0, "SELL"},
            .profile_id = profile_id,
            .default_appl_ver_id = {},
            .heartbeat_interval_seconds = 1U,
            .is_initiator = true,
        },
        .store_path = {},
        .default_appl_ver_id = {},
        .store_mode = fastfix::runtime::StoreMode::kMemory,
        .recovery_mode = fastfix::session::RecoveryMode::kMemoryOnly,
        .dispatch_mode = fastfix::runtime::AppDispatchMode::kInline,
        .validation_policy = fastfix::session::ValidationPolicy::Permissive(),
    });
    shard_config.counterparties.push_back(fastfix::runtime::CounterpartyConfig{
        .name = "buy-sell-initiator-shard-1",
        .session = fastfix::session::SessionConfig{
            .session_id = 1205U,
            .key = fastfix::session::SessionKey{"FIX.4.4", routed_sender_1, "SELL"},
            .profile_id = profile_id,
            .default_appl_ver_id = {},
            .heartbeat_interval_seconds = 1U,
            .is_initiator = true,
        },
        .store_path = {},
        .default_appl_ver_id = {},
        .store_mode = fastfix::runtime::StoreMode::kMemory,
        .recovery_mode = fastfix::session::RecoveryMode::kMemoryOnly,
        .dispatch_mode = fastfix::runtime::AppDispatchMode::kInline,
        .validation_policy = fastfix::session::ValidationPolicy::Permissive(),
    });

    fastfix::runtime::Engine shard_engine;
    REQUIRE(shard_engine.Boot(shard_config).ok());
    const auto* session_shard_0 = shard_engine.runtime()->FindSessionShard(1204U);
    const auto* session_shard_1 = shard_engine.runtime()->FindSessionShard(1205U);
    REQUIRE(session_shard_0 != nullptr);
    REQUIRE(session_shard_1 != nullptr);
    REQUIRE(session_shard_0->worker_id == 0U);
    REQUIRE(session_shard_1->worker_id == 1U);

    auto shard_application = std::make_shared<MultiInitiatorApplication>(nullptr, std::vector<std::uint64_t>{1204U, 1205U});
    fastfix::runtime::LiveInitiator shard_runtime(
        &shard_engine,
        fastfix::runtime::LiveInitiator::Options{
            .poll_timeout = std::chrono::milliseconds(25),
            .io_timeout = std::chrono::seconds(5),
            .application = shard_application,
        });
    shard_application->BindInitiator(&shard_runtime);

    REQUIRE(shard_runtime.OpenSession(1204U, "127.0.0.1", shard_port_0).ok());
    REQUIRE(shard_runtime.OpenSession(1205U, "127.0.0.1", shard_port_1).ok());
    REQUIRE(shard_runtime.Run(2U, std::chrono::seconds(10)).ok());
    REQUIRE(shard_acceptor_future_0.get().ok());
    REQUIRE(shard_acceptor_future_1.get().ok());
    REQUIRE(shard_runtime.completed_session_count() == 2U);
    REQUIRE(shard_application->AllReceivedEchoes());
    REQUIRE(shard_application->application_events() == 2U);
    REQUIRE(shard_application->admin_events() >= 4U);

    auto timer_acceptor = fastfix::transport::TcpAcceptor::Listen("127.0.0.1", 0U);
    REQUIRE(timer_acceptor.ok());
    const auto timer_listen_port = timer_acceptor.value().port();

    std::promise<fastfix::base::Status> timer_acceptor_result;
    auto timer_acceptor_future = timer_acceptor_result.get_future();
    std::jthread timer_acceptor_thread([
        acceptor_socket = std::move(timer_acceptor).value(),
        dictionary = dictionary.value(),
        &timer_acceptor_result]() mutable {
        timer_acceptor_result.set_value(RunAcceptorHeartbeatTimerSession(std::move(acceptor_socket), std::move(dictionary), 2203U));
    });

    fastfix::runtime::EngineConfig timer_config;
    timer_config.worker_count = 1U;
    timer_config.enable_metrics = true;
    timer_config.trace_mode = fastfix::runtime::TraceMode::kRing;
    timer_config.trace_capacity = 16U;
    timer_config.profile_artifacts.push_back(artifact_path);
    timer_config.counterparties.push_back(fastfix::runtime::CounterpartyConfig{
        .name = "buy-sell-initiator-timer",
        .session = fastfix::session::SessionConfig{
            .session_id = 1203U,
            .key = fastfix::session::SessionKey{"FIX.4.4", "BUY", "SELL"},
            .profile_id = profile_id,
            .default_appl_ver_id = {},
            .heartbeat_interval_seconds = 1U,
            .is_initiator = true,
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

    fastfix::runtime::LiveInitiator timer_runtime(
        &timer_engine,
        fastfix::runtime::LiveInitiator::Options{
            .poll_timeout = std::chrono::seconds(5),
            .io_timeout = std::chrono::seconds(5),
            .application = {},
        });

    REQUIRE(timer_runtime.OpenSession(1203U, "127.0.0.1", timer_listen_port).ok());
    REQUIRE(timer_runtime.Run(1U, std::chrono::seconds(10)).ok());
    REQUIRE(timer_acceptor_future.get().ok());
    REQUIRE(timer_runtime.completed_session_count() == 1U);

    const auto* timer_metrics = timer_engine.metrics().FindSession(1203U);
    REQUIRE(timer_metrics != nullptr);
    REQUIRE(timer_metrics->outbound_messages > 0U);
}

TEST_CASE("live-initiator-queue", "[live-initiator-queue]") {
    auto dictionary = fastfix::tests::LoadFix44DictionaryView();
    if (!dictionary.ok()) {
        SKIP("FIX44 artifact not available: " << dictionary.status().message());
    }
    const auto artifact_path =
        std::filesystem::path(FASTFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.art";
    const auto profile_id = dictionary.value().profile().header().profile_id;

    auto acceptor = fastfix::transport::TcpAcceptor::Listen("127.0.0.1", 0U);
    REQUIRE(acceptor.ok());
    const auto listen_port = acceptor.value().port();

    std::promise<fastfix::base::Status> acceptor_result;
    auto acceptor_future = acceptor_result.get_future();
    std::jthread acceptor_thread([
        acceptor_socket = std::move(acceptor).value(),
        dictionary = dictionary.value(),
        &acceptor_result]() mutable {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 2202U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
                    .profile_id = dictionary.profile().header().profile_id,
                    .heartbeat_interval_seconds = 1U,
                    .is_initiator = false,
                },
                .begin_string = "FIX.4.4",
                .sender_comp_id = "SELL",
                .target_comp_id = "BUY",
                .heartbeat_interval_seconds = 1U,
                .validation_policy = fastfix::session::ValidationPolicy::Permissive(),
            },
            dictionary,
            &store);

        auto inbound = acceptor_socket.Accept(std::chrono::seconds(5));
        if (!inbound.ok()) {
            acceptor_result.set_value(inbound.status());
            return;
        }

        auto connection = std::move(inbound).value();
        auto connected = protocol.OnTransportConnected(NowNs());
        if (!connected.ok()) {
            acceptor_result.set_value(connected.status());
            return;
        }

        while (true) {
            auto frame = connection.ReceiveFrame(std::chrono::seconds(5));
            if (!frame.ok()) {
                acceptor_result.set_value(frame.status());
                return;
            }

            auto event = protocol.OnInbound(frame.value(), NowNs());
            if (!event.ok()) {
                acceptor_result.set_value(event.status());
                return;
            }

            for (const auto& outbound : event.value().outbound_frames) {
                auto status = connection.Send(outbound.bytes, std::chrono::seconds(5));
                if (!status.ok()) {
                    acceptor_result.set_value(status);
                    return;
                }
            }

            for (const auto& message : event.value().application_messages) {
                auto echo = protocol.SendApplication(message, NowNs());
                if (!echo.ok()) {
                    acceptor_result.set_value(echo.status());
                    return;
                }
                auto status = connection.Send(echo.value().bytes, std::chrono::seconds(5));
                if (!status.ok()) {
                    acceptor_result.set_value(status);
                    return;
                }
            }

            if (event.value().disconnect) {
                auto close_status = protocol.OnTransportClosed();
                connection.Close();
                acceptor_result.set_value(close_status);
                return;
            }
        }
    });

    fastfix::runtime::EngineConfig config;
    config.worker_count = 1U;
    config.enable_metrics = true;
    config.trace_mode = fastfix::runtime::TraceMode::kRing;
    config.trace_capacity = 16U;
    config.profile_artifacts.push_back(artifact_path);
    config.counterparties.push_back(fastfix::runtime::CounterpartyConfig{
        .name = "buy-sell-initiator-queue",
        .session = fastfix::session::SessionConfig{
            .session_id = 1202U,
            .key = fastfix::session::SessionKey{"FIX.4.4", "BUY", "SELL"},
            .profile_id = profile_id,
            .default_appl_ver_id = {},
            .heartbeat_interval_seconds = 1U,
            .is_initiator = true,
        },
        .store_path = {},
        .default_appl_ver_id = {},
        .store_mode = fastfix::runtime::StoreMode::kMemory,
        .recovery_mode = fastfix::session::RecoveryMode::kMemoryOnly,
        .dispatch_mode = fastfix::runtime::AppDispatchMode::kQueueDecoupled,
        .validation_policy = fastfix::session::ValidationPolicy::Permissive(),
    });

    fastfix::runtime::Engine engine;
    REQUIRE(engine.Boot(config).ok());

    auto state = std::make_shared<QueueInitiatorState>();
    auto application = std::make_shared<QueueInitiatorApplication>(1202U, config.worker_count, state);
    fastfix::runtime::ManagedQueueApplicationRunnerOptions managed_queue_runner;
    managed_queue_runner.mode = fastfix::runtime::ManagedQueueApplicationRunnerMode::kCoScheduled;
    std::vector<QueueInitiatorHandler*> handler_ptrs;
    managed_queue_runner.handlers.reserve(application->queue_application().worker_count());
    for (std::uint32_t worker_id = 0; worker_id < application->queue_application().worker_count(); ++worker_id) {
        (void)worker_id;
        auto handler = std::make_unique<QueueInitiatorHandler>(nullptr, 1202U, state);
        handler_ptrs.push_back(handler.get());
        managed_queue_runner.handlers.push_back(std::move(handler));
    }

    fastfix::runtime::LiveInitiator runtime(
        &engine,
        fastfix::runtime::LiveInitiator::Options{
            .poll_timeout = std::chrono::milliseconds(25),
            .io_timeout = std::chrono::seconds(5),
            .application = application,
            .managed_queue_runner = std::move(managed_queue_runner),
        });
    for (auto* handler : handler_ptrs) {
        handler->BindInitiator(&runtime);
    }

    REQUIRE(runtime.OpenSession(1202U, "127.0.0.1", listen_port).ok());
    REQUIRE(runtime.Run(1U, std::chrono::seconds(10)).ok());
    REQUIRE(acceptor_future.get().ok());
    REQUIRE(runtime.completed_session_count() == 1U);
    REQUIRE(application->received_echo());
    REQUIRE(application->session_events() >= 3U);
    REQUIRE(application->admin_events() >= 2U);
    REQUIRE(application->application_events() == 1U);

    const auto snapshot = application->Snapshot();
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->state == fastfix::session::SessionState::kDisconnected);

    auto notifications = application->DrainSubscription();
    REQUIRE(notifications.ok());
    auto count_notifications = [&](fastfix::session::SessionNotificationKind kind) {
        return static_cast<std::size_t>(std::count_if(
            notifications.value().begin(),
            notifications.value().end(),
            [&](const auto& notification) { return notification.kind == kind; }));
    };
    auto find_notification = [&](fastfix::session::SessionNotificationKind kind) {
        const auto it = std::find_if(
            notifications.value().begin(),
            notifications.value().end(),
            [&](const auto& notification) { return notification.kind == kind; });
        return it == notifications.value().end() ? nullptr : &(*it);
    };
    REQUIRE(count_notifications(fastfix::session::SessionNotificationKind::kSessionBound) == 1U);
    REQUIRE(count_notifications(fastfix::session::SessionNotificationKind::kSessionActive) == 1U);
    REQUIRE(count_notifications(fastfix::session::SessionNotificationKind::kSessionClosed) == 1U);
    REQUIRE(count_notifications(fastfix::session::SessionNotificationKind::kAdminMessage) >= 2U);
    REQUIRE(count_notifications(fastfix::session::SessionNotificationKind::kApplicationMessage) == 1U);

    const auto* app_notification = find_notification(fastfix::session::SessionNotificationKind::kApplicationMessage);
    REQUIRE(app_notification != nullptr);
    REQUIRE(app_notification->message.valid());
    const auto parties = app_notification->message_view().group(kNoPartyIDs);
    REQUIRE(parties.has_value());
    REQUIRE(parties->size() == 1U);
    REQUIRE((*parties)[0].get_string(kPartyID).value() == "INITIATOR-PARTY");

    const auto* metrics = engine.metrics().FindSession(1202U);
    REQUIRE(metrics != nullptr);
    REQUIRE(metrics->inbound_messages > 0U);
    REQUIRE(metrics->outbound_messages > 0U);

    auto mismatch_acceptor = fastfix::transport::TcpAcceptor::Listen("127.0.0.1", 0U);
    REQUIRE(mismatch_acceptor.ok());
    const auto mismatch_port = mismatch_acceptor.value().port();

    fastfix::runtime::EngineConfig mismatch_config;
    mismatch_config.worker_count = 2U;
    mismatch_config.trace_mode = fastfix::runtime::TraceMode::kRing;
    mismatch_config.trace_capacity = 16U;
    mismatch_config.profile_artifacts.push_back(artifact_path);
    mismatch_config.counterparties.push_back(fastfix::runtime::CounterpartyConfig{
        .name = "buy-sell-initiator-queue-mismatch",
        .session = fastfix::session::SessionConfig{
            .session_id = 1206U,
            .key = fastfix::session::SessionKey{"FIX.4.4", "BUY-MISMATCH", "SELL-MISMATCH"},
            .profile_id = profile_id,
            .default_appl_ver_id = {},
            .heartbeat_interval_seconds = 1U,
            .is_initiator = true,
        },
        .store_path = {},
        .default_appl_ver_id = {},
        .store_mode = fastfix::runtime::StoreMode::kMemory,
        .recovery_mode = fastfix::session::RecoveryMode::kMemoryOnly,
        .dispatch_mode = fastfix::runtime::AppDispatchMode::kQueueDecoupled,
        .validation_policy = fastfix::session::ValidationPolicy::Permissive(),
    });

    fastfix::runtime::Engine mismatch_engine;
    REQUIRE(mismatch_engine.Boot(mismatch_config).ok());

    auto mismatch_state = std::make_shared<QueueInitiatorState>();
    auto mismatch_application = std::make_shared<QueueInitiatorApplication>(1206U, 1U, mismatch_state);
    fastfix::runtime::ManagedQueueApplicationRunnerOptions mismatch_runner;
    mismatch_runner.mode = fastfix::runtime::ManagedQueueApplicationRunnerMode::kCoScheduled;
    auto mismatch_handler = std::make_unique<QueueInitiatorHandler>(nullptr, 1206U, mismatch_state);
    auto* mismatch_handler_ptr = mismatch_handler.get();
    mismatch_runner.handlers.push_back(std::move(mismatch_handler));

    fastfix::runtime::LiveInitiator mismatch_runtime(
        &mismatch_engine,
        fastfix::runtime::LiveInitiator::Options{
            .poll_timeout = std::chrono::milliseconds(5),
            .io_timeout = std::chrono::seconds(1),
            .application = mismatch_application,
            .managed_queue_runner = std::move(mismatch_runner),
        });
    mismatch_handler_ptr->BindInitiator(&mismatch_runtime);

    auto open_status = mismatch_runtime.OpenSession(1206U, "127.0.0.1", mismatch_port);
    if (open_status.ok()) {
        // Mismatch caught at Run() — session happened to route to a valid worker_id.
        auto mismatch_status = mismatch_runtime.Run(1U, std::chrono::milliseconds(5));
        REQUIRE(!mismatch_status.ok());
        REQUIRE(mismatch_status.code() == fastfix::base::ErrorCode::kInvalidArgument);
        REQUIRE(mismatch_status.message().find("queue application worker_count must match engine worker_count") !=
                std::string::npos);
    } else {
        // Mismatch caught at OpenSession() — session routed to a worker_id beyond queue's worker_count.
        REQUIRE(!open_status.ok());
    }
    mismatch_runtime.Stop();

    auto threaded_acceptor = fastfix::transport::TcpAcceptor::Listen("127.0.0.1", 0U);
    REQUIRE(threaded_acceptor.ok());
    const auto threaded_port = threaded_acceptor.value().port();

    std::promise<fastfix::base::Status> threaded_acceptor_result;
    auto threaded_acceptor_future = threaded_acceptor_result.get_future();
    std::jthread threaded_acceptor_thread([
        acceptor_socket = std::move(threaded_acceptor).value(),
        dictionary = dictionary.value(),
        &threaded_acceptor_result]() mutable {
        threaded_acceptor_result.set_value(
            RunAcceptorEchoSession(std::move(acceptor_socket), std::move(dictionary), 2205U, "SELL", "BUY"));
    });

    fastfix::runtime::EngineConfig threaded_config;
    threaded_config.worker_count = 1U;
    threaded_config.enable_metrics = true;
    threaded_config.trace_mode = fastfix::runtime::TraceMode::kRing;
    threaded_config.trace_capacity = 16U;
    threaded_config.queue_app_mode = fastfix::runtime::QueueAppThreadingMode::kThreaded;
    threaded_config.profile_artifacts.push_back(artifact_path);
    threaded_config.counterparties.push_back(fastfix::runtime::CounterpartyConfig{
        .name = "buy-sell-initiator-threaded-queue",
        .session = fastfix::session::SessionConfig{
            .session_id = 1207U,
            .key = fastfix::session::SessionKey{"FIX.4.4", "BUY", "SELL"},
            .profile_id = profile_id,
            .default_appl_ver_id = {},
            .heartbeat_interval_seconds = 1U,
            .is_initiator = true,
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

    auto threaded_state = std::make_shared<QueueInitiatorState>();
    auto threaded_application = std::make_shared<QueueInitiatorApplication>(1207U, threaded_config.worker_count, threaded_state);
    fastfix::runtime::ManagedQueueApplicationRunnerOptions threaded_runner;
    threaded_runner.mode = fastfix::runtime::ManagedQueueApplicationRunnerMode::kThreaded;
    std::vector<QueueInitiatorHandler*> threaded_handler_ptrs;
    threaded_runner.handlers.reserve(threaded_application->queue_application().worker_count());
    for (std::uint32_t worker_id = 0; worker_id < threaded_application->queue_application().worker_count(); ++worker_id) {
        auto handler = std::make_unique<QueueInitiatorHandler>(nullptr, 1207U, threaded_state);
        threaded_handler_ptrs.push_back(handler.get());
        threaded_runner.handlers.push_back(std::move(handler));
    }

    fastfix::runtime::LiveInitiator threaded_runtime(
        &threaded_engine,
        fastfix::runtime::LiveInitiator::Options{
            .poll_timeout = std::chrono::milliseconds(25),
            .io_timeout = std::chrono::seconds(5),
            .application = threaded_application,
            .managed_queue_runner = std::move(threaded_runner),
        });
    for (auto* handler : threaded_handler_ptrs) {
        handler->BindInitiator(&threaded_runtime);
    }

    REQUIRE(threaded_runtime.OpenSession(1207U, "127.0.0.1", threaded_port).ok());
    REQUIRE(threaded_runtime.Run(1U, std::chrono::seconds(10)).ok());
    REQUIRE(threaded_acceptor_future.get().ok());
    REQUIRE(threaded_runtime.completed_session_count() == 1U);
    REQUIRE(threaded_application->received_echo());

}

namespace {

auto RunAcceptorDropAndReconnectEcho(
    fastfix::transport::TcpAcceptor& acceptor_socket,
    fastfix::profile::NormalizedDictionaryView dictionary,
    std::uint64_t session_id) -> fastfix::base::Status {
    fastfix::store::MemorySessionStore store;
    fastfix::session::AdminProtocol protocol(
        fastfix::session::AdminProtocolConfig{
            .session = fastfix::session::SessionConfig{
                .session_id = session_id,
                .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
                .profile_id = dictionary.profile().header().profile_id,
                .heartbeat_interval_seconds = 30U,
                .is_initiator = false,
            },
            .begin_string = "FIX.4.4",
            .sender_comp_id = "SELL",
            .target_comp_id = "BUY",
            .heartbeat_interval_seconds = 30U,
            .validation_policy = fastfix::session::ValidationPolicy::Permissive(),
        },
        dictionary,
        &store);

    // --- First connection: establish session then force-close ---
    auto inbound = acceptor_socket.Accept(std::chrono::seconds(5));
    if (!inbound.ok()) {
        return inbound.status();
    }
    auto connection = std::move(inbound).value();
    auto connected = protocol.OnTransportConnected(NowNs());
    if (!connected.ok()) {
        return connected.status();
    }

    bool session_active = false;
    while (!session_active) {
        auto frame = connection.ReceiveFrame(std::chrono::seconds(5));
        if (!frame.ok()) {
            return frame.status();
        }
        auto event = protocol.OnInbound(frame.value(), NowNs());
        if (!event.ok()) {
            return event.status();
        }
        for (const auto& outbound : event.value().outbound_frames) {
            auto status = connection.Send(outbound.bytes, std::chrono::seconds(5));
            if (!status.ok()) {
                return status;
            }
        }
        if (event.value().session_active) {
            session_active = true;
        }
    }

    // Force close: signal transport drop to protocol, close the connection
    static_cast<void>(protocol.OnTransportClosed());
    connection.Close();

    // --- Second connection: accept reconnect, echo, clean logout ---
    auto inbound2 = acceptor_socket.Accept(std::chrono::seconds(10));
    if (!inbound2.ok()) {
        return inbound2.status();
    }
    auto connection2 = std::move(inbound2).value();
    auto connected2 = protocol.OnTransportConnected(NowNs());
    if (!connected2.ok()) {
        return connected2.status();
    }
    for (const auto& outbound : connected2.value().outbound_frames) {
        auto status = connection2.Send(outbound.bytes, std::chrono::seconds(5));
        if (!status.ok()) {
            return status;
        }
    }

    while (true) {
        auto frame = connection2.ReceiveFrame(std::chrono::seconds(5));
        if (!frame.ok()) {
            return frame.status();
        }
        auto event = protocol.OnInbound(frame.value(), NowNs());
        if (!event.ok()) {
            return event.status();
        }
        for (const auto& outbound : event.value().outbound_frames) {
            auto status = connection2.Send(outbound.bytes, std::chrono::seconds(5));
            if (!status.ok()) {
                return status;
            }
        }
        for (const auto& message : event.value().application_messages) {
            auto echo = protocol.SendApplication(message, NowNs());
            if (!echo.ok()) {
                return echo.status();
            }
            auto status = connection2.Send(echo.value().bytes, std::chrono::seconds(5));
            if (!status.ok()) {
                return status;
            }
        }
        if (event.value().disconnect) {
            static_cast<void>(protocol.OnTransportClosed());
            connection2.Close();
            return fastfix::base::Status::Ok();
        }
    }
}

auto RunAcceptorDropOnce(
    fastfix::transport::TcpAcceptor& acceptor_socket,
    fastfix::profile::NormalizedDictionaryView dictionary,
    std::uint64_t session_id) -> fastfix::base::Status {
    fastfix::store::MemorySessionStore store;
    fastfix::session::AdminProtocol protocol(
        fastfix::session::AdminProtocolConfig{
            .session = fastfix::session::SessionConfig{
                .session_id = session_id,
                .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
                .profile_id = dictionary.profile().header().profile_id,
                .heartbeat_interval_seconds = 30U,
                .is_initiator = false,
            },
            .begin_string = "FIX.4.4",
            .sender_comp_id = "SELL",
            .target_comp_id = "BUY",
            .heartbeat_interval_seconds = 30U,
            .validation_policy = fastfix::session::ValidationPolicy::Permissive(),
        },
        dictionary,
        &store);

    auto inbound = acceptor_socket.Accept(std::chrono::seconds(5));
    if (!inbound.ok()) {
        return inbound.status();
    }
    auto connection = std::move(inbound).value();
    auto connected = protocol.OnTransportConnected(NowNs());
    if (!connected.ok()) {
        return connected.status();
    }

    bool session_active = false;
    while (!session_active) {
        auto frame = connection.ReceiveFrame(std::chrono::seconds(5));
        if (!frame.ok()) {
            return frame.status();
        }
        auto event = protocol.OnInbound(frame.value(), NowNs());
        if (!event.ok()) {
            return event.status();
        }
        for (const auto& outbound : event.value().outbound_frames) {
            auto status = connection.Send(outbound.bytes, std::chrono::seconds(5));
            if (!status.ok()) {
                return status;
            }
        }
        if (event.value().session_active) {
            session_active = true;
        }
    }

    // Force close without clean logout
    static_cast<void>(protocol.OnTransportClosed());
    connection.Close();
    return fastfix::base::Status::Ok();
}

class ReconnectInitiatorApplication final : public fastfix::runtime::ApplicationCallbacks {
  public:
    ReconnectInitiatorApplication(fastfix::runtime::LiveInitiator* initiator, std::uint64_t session_id)
        : initiator_(initiator),
          session_id_(session_id) {
    }

    auto BindInitiator(fastfix::runtime::LiveInitiator* initiator) -> void {
        initiator_ = initiator;
    }

    auto OnSessionEvent(const fastfix::runtime::RuntimeEvent& event) -> fastfix::base::Status override {
        if (event.handle.session_id() != session_id_) {
            return fastfix::base::Status::Ok();
        }
        if (event.session_event == fastfix::runtime::SessionEventKind::kClosed) {
            close_count_.fetch_add(1U);
            sent_application_.store(false);
        }
        if (event.session_event == fastfix::runtime::SessionEventKind::kActive &&
            close_count_.load() >= 1U &&
            !sent_application_.exchange(true)) {
            active_count_.fetch_add(1U);
            return event.handle.Send(BuildInitiatorMessage());
        }
        return fastfix::base::Status::Ok();
    }

    auto OnAppMessage(const fastfix::runtime::RuntimeEvent& event) -> fastfix::base::Status override {
        if (event.handle.session_id() != session_id_) {
            return fastfix::base::Status::Ok();
        }
        auto status = ValidateInitiatorEcho(event.message.view());
        if (!status.ok()) {
            return status;
        }
        received_echo_.store(true);
        return initiator_->RequestLogout(session_id_);
    }

    [[nodiscard]] auto received_echo() const -> bool { return received_echo_.load(); }
    [[nodiscard]] auto close_count() const -> std::uint64_t { return close_count_.load(); }
    [[nodiscard]] auto active_count() const -> std::uint64_t { return active_count_.load(); }

  private:
    fastfix::runtime::LiveInitiator* initiator_{nullptr};
    std::uint64_t session_id_{0};
    std::atomic<bool> sent_application_{false};
    std::atomic<bool> received_echo_{false};
    std::atomic<std::uint64_t> close_count_{0};
    std::atomic<std::uint64_t> active_count_{0};
};

class TimingInitiatorApplication final : public fastfix::runtime::ApplicationCallbacks {
  public:
    explicit TimingInitiatorApplication(std::uint64_t session_id)
        : session_id_(session_id) {
    }

    auto OnSessionEvent(const fastfix::runtime::RuntimeEvent& event) -> fastfix::base::Status override {
        if (event.handle.session_id() != session_id_) {
            return fastfix::base::Status::Ok();
        }
        if (event.session_event == fastfix::runtime::SessionEventKind::kClosed) {
            close_time_ns_.store(NowNs());
        }
        if (event.session_event == fastfix::runtime::SessionEventKind::kBound) {
            bound_count_.fetch_add(1U);
            last_bound_time_ns_.store(NowNs());
        }
        return fastfix::base::Status::Ok();
    }

    [[nodiscard]] auto close_time_ns() const -> std::uint64_t { return close_time_ns_.load(); }
    [[nodiscard]] auto last_bound_time_ns() const -> std::uint64_t { return last_bound_time_ns_.load(); }
    [[nodiscard]] auto bound_count() const -> std::uint64_t { return bound_count_.load(); }

  private:
    std::uint64_t session_id_{0};
    std::atomic<std::uint64_t> close_time_ns_{0};
    std::atomic<std::uint64_t> last_bound_time_ns_{0};
    std::atomic<std::uint64_t> bound_count_{0};
};

}  // namespace

TEST_CASE("live-initiator-reconnect-after-disconnect", "[live-initiator]") {
    auto dictionary = fastfix::tests::LoadFix44DictionaryView();
    if (!dictionary.ok()) {
        SKIP("FIX44 artifact not available: " << dictionary.status().message());
    }
    const auto artifact_path =
        std::filesystem::path(FASTFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.art";
    const auto profile_id = dictionary.value().profile().header().profile_id;

    auto acceptor = fastfix::transport::TcpAcceptor::Listen("127.0.0.1", 0U);
    REQUIRE(acceptor.ok());
    const auto listen_port = acceptor.value().port();

    std::promise<fastfix::base::Status> acceptor_result;
    auto acceptor_future = acceptor_result.get_future();
    std::jthread acceptor_thread([
        &acceptor_socket = acceptor.value(),
        dictionary = dictionary.value(),
        &acceptor_result]() mutable {
        acceptor_result.set_value(
            RunAcceptorDropAndReconnectEcho(acceptor_socket, std::move(dictionary), 2401U));
    });

    fastfix::runtime::EngineConfig config;
    config.worker_count = 1U;
    config.enable_metrics = true;
    config.profile_artifacts.push_back(artifact_path);
    config.counterparties.push_back(fastfix::runtime::CounterpartyConfig{
        .name = "reconnect-initiator",
        .session = fastfix::session::SessionConfig{
            .session_id = 1401U,
            .key = fastfix::session::SessionKey{"FIX.4.4", "BUY", "SELL"},
            .profile_id = profile_id,
            .default_appl_ver_id = {},
            .heartbeat_interval_seconds = 30U,
            .is_initiator = true,
        },
        .store_path = {},
        .default_appl_ver_id = {},
        .store_mode = fastfix::runtime::StoreMode::kMemory,
        .recovery_mode = fastfix::session::RecoveryMode::kMemoryOnly,
        .dispatch_mode = fastfix::runtime::AppDispatchMode::kInline,
        .validation_policy = fastfix::session::ValidationPolicy::Permissive(),
        .reconnect_enabled = true,
        .reconnect_initial_ms = 100,
        .reconnect_max_ms = 2000,
        .reconnect_max_retries = 0,
    });

    fastfix::runtime::Engine engine;
    REQUIRE(engine.Boot(config).ok());

    auto application = std::make_shared<ReconnectInitiatorApplication>(nullptr, 1401U);
    fastfix::runtime::LiveInitiator runtime(
        &engine,
        fastfix::runtime::LiveInitiator::Options{
            .poll_timeout = std::chrono::milliseconds(25),
            .io_timeout = std::chrono::seconds(5),
            .application = application,
        });
    application->BindInitiator(&runtime);

    REQUIRE(runtime.OpenSession(1401U, "127.0.0.1", listen_port).ok());
    REQUIRE(runtime.Run(1U, std::chrono::seconds(15)).ok());
    REQUIRE(acceptor_future.get().ok());
    REQUIRE(runtime.completed_session_count() == 1U);
    REQUIRE(application->received_echo());
    REQUIRE(application->close_count() >= 1U);
    REQUIRE(application->active_count() >= 1U);
}

TEST_CASE("live-initiator-reconnect-max-retries", "[live-initiator]") {
    auto dictionary = fastfix::tests::LoadFix44DictionaryView();
    if (!dictionary.ok()) {
        SKIP("FIX44 artifact not available: " << dictionary.status().message());
    }
    const auto artifact_path =
        std::filesystem::path(FASTFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.art";
    const auto profile_id = dictionary.value().profile().header().profile_id;

    auto acceptor = fastfix::transport::TcpAcceptor::Listen("127.0.0.1", 0U);
    REQUIRE(acceptor.ok());
    const auto listen_port = acceptor.value().port();

    std::promise<fastfix::base::Status> acceptor_result;
    auto acceptor_future = acceptor_result.get_future();
    std::jthread acceptor_thread([
        &acceptor_socket = acceptor.value(),
        dictionary = dictionary.value(),
        &acceptor_result]() mutable {
        acceptor_result.set_value(
            RunAcceptorDropOnce(acceptor_socket, std::move(dictionary), 2402U));
    });

    fastfix::runtime::EngineConfig config;
    config.worker_count = 1U;
    config.enable_metrics = true;
    config.profile_artifacts.push_back(artifact_path);
    config.counterparties.push_back(fastfix::runtime::CounterpartyConfig{
        .name = "reconnect-max-retries",
        .session = fastfix::session::SessionConfig{
            .session_id = 1402U,
            .key = fastfix::session::SessionKey{"FIX.4.4", "BUY", "SELL"},
            .profile_id = profile_id,
            .default_appl_ver_id = {},
            .heartbeat_interval_seconds = 30U,
            .is_initiator = true,
        },
        .store_path = {},
        .default_appl_ver_id = {},
        .store_mode = fastfix::runtime::StoreMode::kMemory,
        .recovery_mode = fastfix::session::RecoveryMode::kMemoryOnly,
        .dispatch_mode = fastfix::runtime::AppDispatchMode::kInline,
        .validation_policy = fastfix::session::ValidationPolicy::Permissive(),
        .reconnect_enabled = true,
        .reconnect_initial_ms = 50,
        .reconnect_max_ms = 200,
        .reconnect_max_retries = 2,
    });

    fastfix::runtime::Engine engine;
    REQUIRE(engine.Boot(config).ok());

    auto application = std::make_shared<ReconnectInitiatorApplication>(nullptr, 1402U);
    fastfix::runtime::LiveInitiator runtime(
        &engine,
        fastfix::runtime::LiveInitiator::Options{
            .poll_timeout = std::chrono::milliseconds(10),
            .io_timeout = std::chrono::milliseconds(500),
            .application = application,
        });
    application->BindInitiator(&runtime);

    REQUIRE(runtime.OpenSession(1402U, "127.0.0.1", listen_port).ok());

    // Close the acceptor so reconnect attempts will fail
    REQUIRE(acceptor_future.get().ok());
    acceptor.value().Close();

    auto run_status = runtime.Run(1U, std::chrono::seconds(15));
    REQUIRE(!run_status.ok());
    REQUIRE(run_status.message().find("reconnect gave up") != std::string::npos);
    REQUIRE(runtime.completed_session_count() == 0U);
    REQUIRE(runtime.pending_reconnect_count() == 0U);
}

TEST_CASE("live-initiator-reconnect-backoff-timing", "[live-initiator]") {
    auto dictionary = fastfix::tests::LoadFix44DictionaryView();
    if (!dictionary.ok()) {
        SKIP("FIX44 artifact not available: " << dictionary.status().message());
    }
    const auto artifact_path =
        std::filesystem::path(FASTFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.art";
    const auto profile_id = dictionary.value().profile().header().profile_id;

    auto acceptor = fastfix::transport::TcpAcceptor::Listen("127.0.0.1", 0U);
    REQUIRE(acceptor.ok());
    const auto listen_port = acceptor.value().port();

    // Acceptor: establish session, force close, then accept reconnect
    std::promise<fastfix::base::Status> acceptor_result;
    auto acceptor_future = acceptor_result.get_future();
    std::jthread acceptor_thread([
        &acceptor_socket = acceptor.value(),
        dictionary = dictionary.value(),
        &acceptor_result]() mutable {
        acceptor_result.set_value(
            RunAcceptorDropAndReconnectEcho(acceptor_socket, std::move(dictionary), 2403U));
    });

    const std::uint32_t initial_ms = 200;
    fastfix::runtime::EngineConfig config;
    config.worker_count = 1U;
    config.enable_metrics = true;
    config.profile_artifacts.push_back(artifact_path);
    config.counterparties.push_back(fastfix::runtime::CounterpartyConfig{
        .name = "reconnect-timing",
        .session = fastfix::session::SessionConfig{
            .session_id = 1403U,
            .key = fastfix::session::SessionKey{"FIX.4.4", "BUY", "SELL"},
            .profile_id = profile_id,
            .default_appl_ver_id = {},
            .heartbeat_interval_seconds = 30U,
            .is_initiator = true,
        },
        .store_path = {},
        .default_appl_ver_id = {},
        .store_mode = fastfix::runtime::StoreMode::kMemory,
        .recovery_mode = fastfix::session::RecoveryMode::kMemoryOnly,
        .dispatch_mode = fastfix::runtime::AppDispatchMode::kInline,
        .validation_policy = fastfix::session::ValidationPolicy::Permissive(),
        .reconnect_enabled = true,
        .reconnect_initial_ms = initial_ms,
        .reconnect_max_ms = 5000,
        .reconnect_max_retries = 0,
    });

    fastfix::runtime::Engine engine;
    REQUIRE(engine.Boot(config).ok());

    auto application = std::make_shared<TimingInitiatorApplication>(1403U);
    auto reconnect_app = std::make_shared<ReconnectInitiatorApplication>(nullptr, 1403U);

    // Combine: timing tracks events, reconnect_app drives the echo/logout
    class CombinedApplication final : public fastfix::runtime::ApplicationCallbacks {
      public:
        CombinedApplication(
            std::shared_ptr<TimingInitiatorApplication> timing,
            std::shared_ptr<ReconnectInitiatorApplication> driver)
            : timing_(std::move(timing)),
              driver_(std::move(driver)) {
        }

        auto OnSessionEvent(const fastfix::runtime::RuntimeEvent& event) -> fastfix::base::Status override {
            auto status = timing_->OnSessionEvent(event);
            if (!status.ok()) {
                return status;
            }
            return driver_->OnSessionEvent(event);
        }

        auto OnAppMessage(const fastfix::runtime::RuntimeEvent& event) -> fastfix::base::Status override {
            return driver_->OnAppMessage(event);
        }

      private:
        std::shared_ptr<TimingInitiatorApplication> timing_;
        std::shared_ptr<ReconnectInitiatorApplication> driver_;
    };

    auto combined = std::make_shared<CombinedApplication>(application, reconnect_app);

    fastfix::runtime::LiveInitiator runtime(
        &engine,
        fastfix::runtime::LiveInitiator::Options{
            .poll_timeout = std::chrono::milliseconds(10),
            .io_timeout = std::chrono::seconds(5),
            .application = combined,
        });
    reconnect_app->BindInitiator(&runtime);

    REQUIRE(runtime.OpenSession(1403U, "127.0.0.1", listen_port).ok());
    REQUIRE(runtime.Run(1U, std::chrono::seconds(15)).ok());
    REQUIRE(acceptor_future.get().ok());
    REQUIRE(reconnect_app->received_echo());

    // Verify backoff: the reconnect (second Bound) should occur at least initial_ms after the Close
    const auto close_ns = application->close_time_ns();
    const auto bound_ns = application->last_bound_time_ns();
    REQUIRE(close_ns > 0U);
    REQUIRE(bound_ns > 0U);
    REQUIRE(application->bound_count() >= 2U);

    const auto elapsed_ms = (bound_ns - close_ns) / 1000000ULL;
    REQUIRE(elapsed_ms >= initial_ms);
}

TEST_CASE("live-initiator-defers-outside-logon-window", "[live-initiator]") {
    auto dictionary = fastfix::tests::LoadFix44DictionaryView();
    if (!dictionary.ok()) {
        SKIP("FIX44 artifact not available: " << dictionary.status().message());
    }

    const auto artifact_path =
        std::filesystem::path(FASTFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.art";
    const auto profile_id = dictionary.value().profile().header().profile_id;

    auto acceptor = fastfix::transport::TcpAcceptor::Listen("127.0.0.1", 0U);
    REQUIRE(acceptor.ok());
    const auto listen_port = acceptor.value().port();

    const auto now = std::time(nullptr);
    std::tm utc_now{};
    gmtime_r(&now, &utc_now);

    fastfix::runtime::EngineConfig config;
    config.worker_count = 1U;
    config.profile_artifacts.push_back(artifact_path);
    config.counterparties.push_back(fastfix::runtime::CounterpartyConfig{
        .name = "buy-sell-windowed-initiator",
        .session = fastfix::session::SessionConfig{
            .session_id = 1450U,
            .key = fastfix::session::SessionKey{"FIX.4.4", "BUY", "SELL"},
            .profile_id = profile_id,
            .default_appl_ver_id = {},
            .heartbeat_interval_seconds = 1U,
            .is_initiator = true,
        },
        .store_path = {},
        .default_appl_ver_id = {},
        .store_mode = fastfix::runtime::StoreMode::kMemory,
        .recovery_mode = fastfix::session::RecoveryMode::kMemoryOnly,
        .dispatch_mode = fastfix::runtime::AppDispatchMode::kInline,
        .validation_policy = fastfix::session::ValidationPolicy::Permissive(),
        .session_schedule = fastfix::runtime::SessionScheduleConfig{
            .use_local_time = false,
            .non_stop_session = false,
            .start_time = fastfix::runtime::SessionTimeOfDay{
                static_cast<std::uint8_t>((utc_now.tm_hour + 1) % 24),
                0U,
                0U,
            },
            .end_time = fastfix::runtime::SessionTimeOfDay{
                static_cast<std::uint8_t>((utc_now.tm_hour + 2) % 24),
                0U,
                0U,
            },
        },
    });

    fastfix::runtime::Engine engine;
    REQUIRE(engine.Boot(config).ok());

    fastfix::runtime::LiveInitiator runtime(
        &engine,
        fastfix::runtime::LiveInitiator::Options{
            .poll_timeout = std::chrono::milliseconds(10),
            .io_timeout = std::chrono::milliseconds(50),
        });

    REQUIRE(runtime.OpenSession(1450U, "127.0.0.1", listen_port).ok());
    REQUIRE(runtime.active_connection_count() == 0U);
    REQUIRE(runtime.pending_reconnect_count() == 1U);
}