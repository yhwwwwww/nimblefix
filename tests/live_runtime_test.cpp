#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "nimblefix/codec/fix_tags.h"
#include "nimblefix/message/message_builder.h"
#include "nimblefix/runtime/application.h"
#include "nimblefix/runtime/engine.h"
#include "nimblefix/runtime/live_acceptor.h"
#include "nimblefix/session/admin_protocol.h"
#include "nimblefix/store/memory_store.h"
#include "nimblefix/transport/tcp_transport.h"

#include "test_support.h"

namespace {

using namespace nimble::codec::tags;

class MixedApplication final
  : public nimble::runtime::ApplicationCallbacks
  , public nimble::runtime::QueueApplicationProvider
{
public:
  explicit MixedApplication(std::uint32_t worker_count, std::uint64_t queued_session_id = 3102U)
    : queue_(worker_count)
    , queued_session_id_(queued_session_id)
  {
  }

  auto OnSessionEvent(const nimble::runtime::RuntimeEvent& event) -> nimble::base::Status override
  {
    ++session_events_;
    auto status = TrackSession(event.handle);
    if (!status.ok()) {
      return status;
    }
    if (event.handle.session_id() == queued_session_id_) {
      return queue_.OnSessionEvent(event);
    }
    return nimble::base::Status::Ok();
  }

  auto OnAdminMessage(const nimble::runtime::RuntimeEvent& event) -> nimble::base::Status override
  {
    ++admin_events_;
    if (event.handle.session_id() == queued_session_id_) {
      return queue_.OnAdminMessage(event);
    }
    return nimble::base::Status::Ok();
  }

  auto OnAppMessage(const nimble::runtime::RuntimeEvent& event) -> nimble::base::Status override
  {
    ++application_events_;
    if (event.handle.session_id() == queued_session_id_) {
      ++queued_application_events_;
      return queue_.OnAppMessage(event);
    }

    ++inline_application_events_;
    return event.handle.SendInlineBorrowed(event.message_view());
  }

  [[nodiscard]] auto worker_count() const -> std::uint32_t { return queue_.worker_count(); }

  auto TryPopEvent(std::uint32_t worker_id) -> nimble::base::Result<std::optional<nimble::runtime::RuntimeEvent>>
  {
    return queue_.TryPopEvent(worker_id);
  }

  auto queue_application() -> nimble::runtime::QueueApplication& override { return queue_; }

  [[nodiscard]] auto session_events() const -> std::uint64_t { return session_events_.load(); }

  [[nodiscard]] auto admin_events() const -> std::uint64_t { return admin_events_.load(); }

  [[nodiscard]] auto application_events() const -> std::uint64_t { return application_events_.load(); }

  [[nodiscard]] auto inline_application_events() const -> std::uint64_t { return inline_application_events_.load(); }

  [[nodiscard]] auto queued_application_events() const -> std::uint64_t { return queued_application_events_.load(); }

  [[nodiscard]] auto SnapshotFor(std::uint64_t session_id) const -> std::optional<nimble::session::SessionSnapshot>
  {
    std::lock_guard lock(mutex_);
    const auto it = snapshots_.find(session_id);
    if (it == snapshots_.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  auto DrainSubscription(std::uint64_t session_id)
    -> nimble::base::Result<std::vector<nimble::session::SessionNotification>>
  {
    nimble::session::SessionSubscription subscription;
    {
      std::lock_guard lock(mutex_);
      const auto it = subscriptions_.find(session_id);
      if (it == subscriptions_.end()) {
        return nimble::base::Status::NotFound("session subscription was not registered");
      }
      subscription = it->second;
    }

    std::vector<nimble::session::SessionNotification> notifications;
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
  auto TrackSession(const nimble::session::SessionHandle& handle) -> nimble::base::Status
  {
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
    return nimble::base::Status::Ok();
  }

  nimble::runtime::QueueApplication queue_;
  mutable std::mutex mutex_;
  std::unordered_map<std::uint64_t, nimble::session::SessionHandle> handles_;
  std::unordered_map<std::uint64_t, nimble::session::SessionSnapshot> snapshots_;
  std::unordered_map<std::uint64_t, nimble::session::SessionSubscription> subscriptions_;
  std::uint64_t queued_session_id_{ 0U };
  std::atomic<std::uint64_t> session_events_{ 0 };
  std::atomic<std::uint64_t> admin_events_{ 0 };
  std::atomic<std::uint64_t> application_events_{ 0 };
  std::atomic<std::uint64_t> inline_application_events_{ 0 };
  std::atomic<std::uint64_t> queued_application_events_{ 0 };
};

class BackpressureApplication final
  : public nimble::runtime::ApplicationCallbacks
  , public nimble::runtime::QueueApplicationProvider
{
public:
  explicit BackpressureApplication(std::uint32_t worker_count)
    : queue_(worker_count,
             nimble::runtime::QueueApplicationOptions{
               .queue_capacity = 1U,
               .overflow_policy = nimble::runtime::QueueOverflowPolicy::kBackpressure,
             })
    , backpressure_future_(backpressure_promise_.get_future())
  {
  }

  auto OnSessionEvent(const nimble::runtime::RuntimeEvent& event) -> nimble::base::Status override
  {
    (void)event;
    return nimble::base::Status::Ok();
  }

  auto OnAdminMessage(const nimble::runtime::RuntimeEvent& event) -> nimble::base::Status override
  {
    (void)event;
    return nimble::base::Status::Ok();
  }

  auto OnAppMessage(const nimble::runtime::RuntimeEvent& event) -> nimble::base::Status override
  {
    ++app_attempts_;
    auto status = queue_.OnAppMessage(event);
    if (status.code() == nimble::base::ErrorCode::kBusy && !backpressure_seen_.exchange(true)) {
      backpressure_promise_.set_value();
    }
    return status;
  }

  auto Prefill(std::uint32_t worker_id) -> nimble::base::Status
  {
    return queue_.OnAppMessage(nimble::runtime::RuntimeEvent{
      .kind = nimble::runtime::RuntimeEventKind::kApplicationMessage,
      .session_event = nimble::runtime::SessionEventKind::kBound,
      .handle = nimble::session::SessionHandle(9000U, worker_id),
      .session_key = nimble::session::SessionKey{ "FIX.4.4", "FAKE", "FAKE" },
      .message = {},
      .text = "prefill",
      .timestamp_ns = 1U,
    });
  }

  [[nodiscard]] auto WaitForBackpressure(std::chrono::milliseconds timeout) -> bool
  {
    return backpressure_future_.wait_for(timeout) == std::future_status::ready;
  }

  [[nodiscard]] auto worker_count() const -> std::uint32_t { return queue_.worker_count(); }

  auto TryPopEvent(std::uint32_t worker_id) -> nimble::base::Result<std::optional<nimble::runtime::RuntimeEvent>>
  {
    return queue_.TryPopEvent(worker_id);
  }

  auto queue_application() -> nimble::runtime::QueueApplication& override { return queue_; }

  [[nodiscard]] auto app_attempts() const -> std::uint64_t { return app_attempts_.load(); }

  [[nodiscard]] auto overflow_events() const -> std::uint64_t { return queue_.overflow_events(); }

private:
  nimble::runtime::QueueApplication queue_;
  std::promise<void> backpressure_promise_;
  std::future<void> backpressure_future_;
  std::atomic<bool> backpressure_seen_{ false };
  std::atomic<std::uint64_t> app_attempts_{ 0 };
};

class QueueReplyHandler final : public nimble::runtime::QueueApplicationEventHandler
{
public:
  explicit QueueReplyHandler(std::uint64_t expected_session_id)
    : expected_session_id_(expected_session_id)
  {
  }

  auto OnRuntimeEvent(const nimble::runtime::RuntimeEvent& event) -> nimble::base::Status override
  {
    if (event.kind != nimble::runtime::RuntimeEventKind::kApplicationMessage) {
      return nimble::base::Status::Ok();
    }
    if (event.handle.session_id() != expected_session_id_) {
      return nimble::base::Status::InvalidArgument("queue-decoupled application observed an unexpected session");
    }

    auto status = event.handle.SendCopy(event.message_view());
    if (!status.ok()) {
      return status;
    }
    last_event_worker_id_.store(event.handle.worker_id());
    ++queued_replies_;
    return nimble::base::Status::Ok();
  }

  [[nodiscard]] auto queued_replies() const -> std::uint64_t { return queued_replies_.load(); }

  [[nodiscard]] auto last_event_worker_id() const -> std::uint32_t { return last_event_worker_id_.load(); }

private:
  std::uint64_t expected_session_id_{ 0 };
  std::atomic<std::uint64_t> queued_replies_{ 0 };
  std::atomic<std::uint32_t> last_event_worker_id_{ 0U };
};

class BackpressureReplyHandler final : public nimble::runtime::QueueApplicationEventHandler
{
public:
  auto OnRuntimeEvent(const nimble::runtime::RuntimeEvent& event) -> nimble::base::Status override
  {
    if (event.handle.session_id() == 9000U) {
      return nimble::base::Status::Ok();
    }

    auto status = event.handle.SendCopy(event.message_view());
    if (!status.ok()) {
      return status;
    }
    ++queued_replies_;
    return nimble::base::Status::Ok();
  }

  [[nodiscard]] auto queued_replies() const -> std::uint64_t { return queued_replies_.load(); }

private:
  std::atomic<std::uint64_t> queued_replies_{ 0 };
};

auto
NowNs() -> std::uint64_t
{
  return static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

auto
RunInitiatorEchoSession(std::uint16_t port,
                        const nimble::profile::NormalizedDictionaryView& dictionary,
                        std::uint64_t session_id,
                        std::string begin_string,
                        std::string default_appl_ver_id,
                        std::string sender_comp_id,
                        std::string target_comp_id,
                        std::string expected_party_id) -> nimble::base::Status
{
  auto connection = nimble::transport::TcpConnection::Connect("127.0.0.1", port, std::chrono::seconds(5));
  if (!connection.ok()) {
    return connection.status();
  }

  nimble::store::MemorySessionStore store;
  nimble::session::AdminProtocol initiator(
    nimble::session::AdminProtocolConfig{
      .session =
        nimble::session::SessionConfig{
          .session_id = session_id,
          .key = nimble::session::SessionKey{ begin_string, sender_comp_id, target_comp_id },
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
      .validation_policy = nimble::session::ValidationPolicy::Permissive(),
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
  while (!received_echo || initiator.session().state() != nimble::session::SessionState::kAwaitingLogout) {
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
      nimble::message::MessageBuilder builder("D");
      builder.set_string(kMsgType, "D");
      auto party = builder.add_group_entry(kNoPartyIDs);
      party.set_string(kPartyID, expected_party_id).set_char(kPartyIDSource, 'D').set_int(kPartyRole, 3);
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
      const auto group = event.value().application_messages.front().view().group(kNoPartyIDs);
      const auto echoed_party =
        group.has_value() && group->size() == 1U ? (*group)[0].get_string(kPartyID) : std::optional<std::string_view>{};
      if (!echoed_party.has_value() || echoed_party.value() != expected_party_id) {
        return nimble::base::Status::InvalidArgument(
          "echoed application message did not preserve repeating group data");
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
    return nimble::base::Status::InvalidArgument("expected logout acknowledgement to request disconnect");
  }

  auto close_status = initiator.OnTransportClosed();
  connection.value().Close();
  return close_status;
}

auto
RunInitiatorEchoSession(std::uint16_t port,
                        const nimble::profile::NormalizedDictionaryView& dictionary,
                        std::uint64_t session_id,
                        std::string begin_string,
                        std::string default_appl_ver_id,
                        std::string expected_party_id) -> nimble::base::Status
{
  return RunInitiatorEchoSession(port,
                                 dictionary,
                                 session_id,
                                 std::move(begin_string),
                                 std::move(default_appl_ver_id),
                                 "BUY",
                                 "SELL",
                                 std::move(expected_party_id));
}

auto
RunInitiatorHeartbeatTimerSession(std::uint16_t port,
                                  const nimble::profile::NormalizedDictionaryView& dictionary,
                                  std::uint64_t session_id,
                                  std::string begin_string,
                                  std::string default_appl_ver_id) -> nimble::base::Status
{
  auto connection = nimble::transport::TcpConnection::Connect("127.0.0.1", port, std::chrono::seconds(5));
  if (!connection.ok()) {
    return connection.status();
  }

  nimble::store::MemorySessionStore store;
  nimble::session::AdminProtocol initiator(
    nimble::session::AdminProtocolConfig{
      .session =
        nimble::session::SessionConfig{
          .session_id = session_id,
          .key = nimble::session::SessionKey{ begin_string, "BUY", "SELL" },
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
      .validation_policy = nimble::session::ValidationPolicy::Permissive(),
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

    auto header = nimble::codec::PeekSessionHeaderView(frame.value());
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
        initiator.session().state() == nimble::session::SessionState::kActive) {
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
      return nimble::base::Status::InvalidArgument("live acceptor did not emit heartbeat before the long poll timeout "
                                                   "elapsed");
    }

    auto close_status = initiator.OnTransportClosed();
    connection.value().Close();
    return close_status;
  }
}

} // namespace

TEST_CASE("live-runtime", "[live-runtime]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }
  const auto artifact_path = std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.nfa";
  const auto profile_id = dictionary.value().profile().header().profile_id;

  nimble::runtime::EngineConfig config;
  config.worker_count = 2U;
  config.enable_metrics = true;
  config.trace_mode = nimble::runtime::TraceMode::kRing;
  config.trace_capacity = 32U;
  config.profile_artifacts.push_back(artifact_path);
  config.listeners.push_back(nimble::runtime::ListenerConfig{
    .name = "main",
    .host = "127.0.0.1",
    .port = 0U,
    .worker_hint = 0U,
  });

  nimble::runtime::CounterpartyConfig fix42;
  fix42.name = "sell-buy-42";
  fix42.session.session_id = 3101U;
  fix42.session.key = nimble::session::SessionKey{ "FIX.4.2", "SELL", "BUY" };
  fix42.session.profile_id = profile_id;
  fix42.session.heartbeat_interval_seconds = 1U;
  fix42.session.is_initiator = false;
  fix42.dispatch_mode = nimble::runtime::AppDispatchMode::kInline;
  fix42.validation_policy = nimble::session::ValidationPolicy::Permissive();

  nimble::runtime::CounterpartyConfig fixt;
  fixt.name = "sell-buy-fixt";
  fixt.session.session_id = 3102U;
  fixt.session.key = nimble::session::SessionKey{ "FIXT.1.1", "SELL", "BUY" };
  fixt.session.profile_id = profile_id;
  fixt.session.default_appl_ver_id = "9";
  fixt.session.heartbeat_interval_seconds = 1U;
  fixt.session.is_initiator = false;
  fixt.default_appl_ver_id = "9";
  fixt.dispatch_mode = nimble::runtime::AppDispatchMode::kQueueDecoupled;
  fixt.validation_policy = nimble::session::ValidationPolicy::Permissive();

  config.counterparties.push_back(fix42);
  config.counterparties.push_back(fixt);

  nimble::runtime::Engine engine;
  REQUIRE(engine.Boot(config).ok());

  auto managed_runner_application = std::make_shared<MixedApplication>(config.worker_count);
  std::optional<nimble::runtime::ManagedQueueApplicationRunnerOptions> engine_managed_queue_runner;
  engine_managed_queue_runner.emplace();
  engine_managed_queue_runner->mode = nimble::runtime::ManagedQueueApplicationRunnerMode::kCoScheduled;
  engine_managed_queue_runner->handlers.reserve(managed_runner_application->queue_application().worker_count());
  for (std::uint32_t worker_id = 0; worker_id < managed_runner_application->queue_application().worker_count();
       ++worker_id) {
    (void)worker_id;
    engine_managed_queue_runner->handlers.push_back(std::make_unique<QueueReplyHandler>(3102U));
  }
  int managed_runner_owner = 0;
  REQUIRE(engine
            .EnsureManagedQueueRunnerStarted(
              &managed_runner_owner, managed_runner_application.get(), &engine_managed_queue_runner)
            .ok());
  REQUIRE(engine.StopManagedQueueRunner(&managed_runner_owner).ok());
  REQUIRE(
    engine.EnsureManagedQueueRunnerStarted(&managed_runner_owner, managed_runner_application.get(), nullptr).ok());
  REQUIRE(engine.ReleaseManagedQueueRunner(&managed_runner_owner).ok());
  REQUIRE(
    !engine.EnsureManagedQueueRunnerStarted(&managed_runner_owner, managed_runner_application.get(), nullptr).ok());

  auto application = std::make_shared<MixedApplication>(config.worker_count);
  nimble::runtime::ManagedQueueApplicationRunnerOptions managed_queue_runner;
  managed_queue_runner.mode = nimble::runtime::ManagedQueueApplicationRunnerMode::kCoScheduled;
  managed_queue_runner.handlers.reserve(application->queue_application().worker_count());
  for (std::uint32_t worker_id = 0; worker_id < application->queue_application().worker_count(); ++worker_id) {
    (void)worker_id;
    managed_queue_runner.handlers.push_back(std::make_unique<QueueReplyHandler>(3102U));
  }

  nimble::runtime::LiveAcceptor runtime(&engine,
                                        nimble::runtime::LiveAcceptor::Options{
                                          .poll_timeout = std::chrono::milliseconds(25),
                                          .io_timeout = std::chrono::seconds(5),
                                          .application = application,
                                          .managed_queue_runner = std::move(managed_queue_runner),
                                        });
  REQUIRE(runtime.OpenListeners("main").ok());

  auto port = runtime.listener_port("main");
  REQUIRE(port.ok());

  std::promise<nimble::base::Status> runtime_result;
  auto runtime_future = runtime_result.get_future();
  std::jthread runtime_thread(
    [&runtime, &runtime_result]() { runtime_result.set_value(runtime.Run(2U, std::chrono::seconds(10))); });

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
  REQUIRE(snapshot_42->state == nimble::session::SessionState::kDisconnected);
  REQUIRE(snapshot_fixt->state == nimble::session::SessionState::kDisconnected);

  auto notifications_42 = application->DrainSubscription(3101U);
  auto notifications_fixt = application->DrainSubscription(3102U);
  REQUIRE(notifications_42.ok());
  REQUIRE(notifications_fixt.ok());

  auto count_notifications = [](const auto& notifications, nimble::session::SessionNotificationKind kind) {
    return static_cast<std::size_t>(std::count_if(
      notifications.begin(), notifications.end(), [&](const auto& notification) { return notification.kind == kind; }));
  };
  auto find_notification = [](const auto& notifications, nimble::session::SessionNotificationKind kind) {
    const auto it = std::find_if(
      notifications.begin(), notifications.end(), [&](const auto& notification) { return notification.kind == kind; });
    return it == notifications.end() ? nullptr : &(*it);
  };

  REQUIRE(count_notifications(notifications_42.value(), nimble::session::SessionNotificationKind::kSessionBound) == 1U);
  REQUIRE(count_notifications(notifications_42.value(), nimble::session::SessionNotificationKind::kSessionActive) ==
          1U);
  REQUIRE(count_notifications(notifications_42.value(), nimble::session::SessionNotificationKind::kSessionClosed) ==
          1U);
  REQUIRE(count_notifications(notifications_42.value(), nimble::session::SessionNotificationKind::kAdminMessage) >= 2U);
  REQUIRE(
    count_notifications(notifications_42.value(), nimble::session::SessionNotificationKind::kApplicationMessage) == 1U);
  REQUIRE(count_notifications(notifications_fixt.value(), nimble::session::SessionNotificationKind::kSessionBound) ==
          1U);
  REQUIRE(count_notifications(notifications_fixt.value(), nimble::session::SessionNotificationKind::kSessionActive) ==
          1U);
  REQUIRE(count_notifications(notifications_fixt.value(), nimble::session::SessionNotificationKind::kSessionClosed) ==
          1U);
  REQUIRE(count_notifications(notifications_fixt.value(), nimble::session::SessionNotificationKind::kAdminMessage) >=
          2U);
  REQUIRE(count_notifications(notifications_fixt.value(),
                              nimble::session::SessionNotificationKind::kApplicationMessage) == 1U);

  const auto* app_notification_42 =
    find_notification(notifications_42.value(), nimble::session::SessionNotificationKind::kApplicationMessage);
  REQUIRE(app_notification_42 != nullptr);
  REQUIRE(app_notification_42->message.valid());
  const auto parties_42 = app_notification_42->message_view().group(kNoPartyIDs);
  REQUIRE(parties_42.has_value());
  REQUIRE(parties_42->size() == 1U);
  REQUIRE((*parties_42)[0].get_string(kPartyID).value() == "PARTY-42");

  const auto* app_notification_fixt =
    find_notification(notifications_fixt.value(), nimble::session::SessionNotificationKind::kApplicationMessage);
  REQUIRE(app_notification_fixt != nullptr);
  REQUIRE(app_notification_fixt->message.valid());
  const auto parties_fixt = app_notification_fixt->message_view().group(kNoPartyIDs);
  REQUIRE(parties_fixt.has_value());
  REQUIRE(parties_fixt->size() == 1U);
  REQUIRE((*parties_fixt)[0].get_string(kPartyID).value() == "PARTY-FIXT");

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
    if (event.kind == nimble::runtime::TraceEventKind::kPendingConnectionRegistered) {
      saw_pending = true;
    }
    if (event.kind == nimble::runtime::TraceEventKind::kSessionEvent && event.session_id != 0U) {
      saw_session_event = true;
    }
  }
  REQUIRE(saw_pending);
  REQUIRE(saw_session_event);

  constexpr std::uint64_t kShardMigratedSessionId = 3104U;
  std::string routed_target_comp_id;
  for (std::uint32_t attempt = 0U; attempt < 256U; ++attempt) {
    const auto candidate = std::string("BUY-SHARD-") + std::to_string(attempt);
    if ((nimble::session::SessionKeyHash{}(nimble::session::SessionKey{ "FIX.4.4", "SELL", candidate }) % 2U) == 1U) {
      routed_target_comp_id = candidate;
      break;
    }
  }
  REQUIRE(!routed_target_comp_id.empty());

  nimble::runtime::EngineConfig shard_config;
  shard_config.worker_count = 2U;
  shard_config.enable_metrics = true;
  shard_config.trace_mode = nimble::runtime::TraceMode::kRing;
  shard_config.trace_capacity = 16U;
  shard_config.profile_artifacts.push_back(artifact_path);
  shard_config.listeners.push_back(nimble::runtime::ListenerConfig{
    .name = "shard-main",
    .host = "127.0.0.1",
    .port = 0U,
    .worker_hint = 0U,
  });
  shard_config.counterparties.push_back(nimble::runtime::CounterpartyConfig{
    .name = "sell-buy-shard-migrate",
    .session =
      nimble::session::SessionConfig{
        .session_id = kShardMigratedSessionId,
        .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", routed_target_comp_id },
        .profile_id = profile_id,
        .heartbeat_interval_seconds = 1U,
        .is_initiator = false,
      },
    .store_path = {},
    .default_appl_ver_id = {},
    .store_mode = nimble::runtime::StoreMode::kMemory,
    .recovery_mode = nimble::session::RecoveryMode::kMemoryOnly,
    .dispatch_mode = nimble::runtime::AppDispatchMode::kQueueDecoupled,
    .validation_policy = nimble::session::ValidationPolicy::Permissive(),
  });

  nimble::runtime::Engine shard_engine;
  REQUIRE(shard_engine.Boot(shard_config).ok());
  const auto* routed_shard = shard_engine.runtime()->FindSessionShard(kShardMigratedSessionId);
  REQUIRE(routed_shard != nullptr);
  REQUIRE(routed_shard->worker_id == 1U);

  auto shard_application = std::make_shared<MixedApplication>(shard_config.worker_count, kShardMigratedSessionId);
  nimble::runtime::ManagedQueueApplicationRunnerOptions shard_managed_queue_runner;
  shard_managed_queue_runner.mode = nimble::runtime::ManagedQueueApplicationRunnerMode::kCoScheduled;
  shard_managed_queue_runner.handlers.reserve(shard_application->queue_application().worker_count());
  std::vector<QueueReplyHandler*> shard_handler_ptrs;
  for (std::uint32_t worker_id = 0; worker_id < shard_application->queue_application().worker_count(); ++worker_id) {
    auto handler = std::make_unique<QueueReplyHandler>(kShardMigratedSessionId);
    shard_handler_ptrs.push_back(handler.get());
    shard_managed_queue_runner.handlers.push_back(std::move(handler));
  }

  nimble::runtime::LiveAcceptor shard_runtime(&shard_engine,
                                              nimble::runtime::LiveAcceptor::Options{
                                                .poll_timeout = std::chrono::milliseconds(25),
                                                .io_timeout = std::chrono::seconds(5),
                                                .application = shard_application,
                                                .managed_queue_runner = std::move(shard_managed_queue_runner),
                                              });
  REQUIRE(shard_runtime.OpenListeners("shard-main").ok());

  auto shard_port = shard_runtime.listener_port("shard-main");
  REQUIRE(shard_port.ok());

  std::promise<nimble::base::Status> shard_runtime_result;
  auto shard_runtime_future = shard_runtime_result.get_future();
  std::jthread shard_runtime_thread([&shard_runtime, &shard_runtime_result]() {
    shard_runtime_result.set_value(shard_runtime.Run(1U, std::chrono::seconds(10)));
  });

  status = RunInitiatorEchoSession(
    shard_port.value(), dictionary.value(), 4104U, "FIX.4.4", {}, routed_target_comp_id, "SELL", "PARTY-SHARD");
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
  REQUIRE(shard_snapshot->state == nimble::session::SessionState::kDisconnected);

  nimble::runtime::EngineConfig balance_config;
  balance_config.worker_count = 2U;
  balance_config.trace_mode = nimble::runtime::TraceMode::kRing;
  balance_config.trace_capacity = 32U;
  balance_config.profile_artifacts.push_back(artifact_path);
  balance_config.listeners.push_back(nimble::runtime::ListenerConfig{
    .name = "balance-main",
    .host = "127.0.0.1",
    .port = 0U,
    .worker_hint = 0U,
  });

  nimble::runtime::Engine balance_engine;
  REQUIRE(balance_engine.Boot(balance_config).ok());

  nimble::runtime::LiveAcceptor balance_runtime(&balance_engine,
                                                nimble::runtime::LiveAcceptor::Options{
                                                  .poll_timeout = std::chrono::milliseconds(25),
                                                  .io_timeout = std::chrono::seconds(5),
                                                  .application = {},
                                                });
  REQUIRE(balance_runtime.OpenListeners("balance-main").ok());

  auto balance_port = balance_runtime.listener_port("balance-main");
  REQUIRE(balance_port.ok());

  std::promise<nimble::base::Status> balance_runtime_result;
  auto balance_runtime_future = balance_runtime_result.get_future();
  std::jthread balance_runtime_thread([&balance_runtime, &balance_runtime_result]() {
    balance_runtime_result.set_value(balance_runtime.Run(0U, std::chrono::seconds(10)));
  });

  std::vector<nimble::transport::TcpConnection> balance_connections;
  balance_connections.reserve(6U);
  for (std::size_t index = 0U; index < 6U; ++index) {
    auto connection =
      nimble::transport::TcpConnection::Connect("127.0.0.1", balance_port.value(), std::chrono::seconds(5));
    REQUIRE(connection.ok());
    balance_connections.push_back(std::move(connection).value());
  }

  auto accept_count = [&balance_engine]() {
    const auto traces = balance_engine.trace().Snapshot();
    return static_cast<std::size_t>(
      std::count_if(traces.begin(), traces.end(), [](const nimble::runtime::TraceEvent& event) {
        return event.kind == nimble::runtime::TraceEventKind::kSessionEvent &&
               std::string_view(event.text.data()) == "tcp accept";
      }));
  };

  std::optional<nimble::base::Status> balance_run_status;
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
  auto trace_text = [](const nimble::runtime::TraceEvent& event) { return std::string_view(event.text.data()); };
  std::size_t accepts_worker0 = 0U;
  std::size_t accepts_worker1 = 0U;
  for (const auto& event : balance_traces) {
    if (event.kind != nimble::runtime::TraceEventKind::kSessionEvent || trace_text(event) != "tcp accept") {
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
  std::optional<nimble::base::Status> final_balance_status;
  if (balance_run_status.has_value()) {
    final_balance_status = *balance_run_status;
  } else {
    final_balance_status = balance_runtime_future.get();
  }
  CAPTURE(final_balance_status->message());
  REQUIRE(final_balance_status->ok());

  nimble::runtime::EngineConfig queue_mismatch_config;
  queue_mismatch_config.worker_count = 2U;
  queue_mismatch_config.trace_mode = nimble::runtime::TraceMode::kRing;
  queue_mismatch_config.trace_capacity = 16U;
  queue_mismatch_config.profile_artifacts.push_back(artifact_path);
  queue_mismatch_config.listeners.push_back(nimble::runtime::ListenerConfig{
    .name = "queue-mismatch-main",
    .host = "127.0.0.1",
    .port = 0U,
    .worker_hint = 0U,
  });

  nimble::runtime::Engine queue_mismatch_engine;
  REQUIRE(queue_mismatch_engine.Boot(queue_mismatch_config).ok());

  auto queue_mismatch_application = std::make_shared<MixedApplication>(1U, 3199U);
  nimble::runtime::ManagedQueueApplicationRunnerOptions queue_mismatch_runner;
  queue_mismatch_runner.mode = nimble::runtime::ManagedQueueApplicationRunnerMode::kCoScheduled;
  queue_mismatch_runner.handlers.push_back(std::make_unique<QueueReplyHandler>(3199U));

  nimble::runtime::LiveAcceptor queue_mismatch_runtime(&queue_mismatch_engine,
                                                       nimble::runtime::LiveAcceptor::Options{
                                                         .poll_timeout = std::chrono::milliseconds(5),
                                                         .io_timeout = std::chrono::seconds(1),
                                                         .application = queue_mismatch_application,
                                                         .managed_queue_runner = std::move(queue_mismatch_runner),
                                                       });
  auto queue_mismatch_status = queue_mismatch_runtime.Run(0U, std::chrono::milliseconds(5));
  REQUIRE(!queue_mismatch_status.ok());
  REQUIRE(queue_mismatch_status.code() == nimble::base::ErrorCode::kInvalidArgument);
  REQUIRE(queue_mismatch_status.message().find("queue application worker_count must match engine worker_count") !=
          std::string::npos);

  nimble::runtime::EngineConfig threaded_config;
  threaded_config.worker_count = 1U;
  threaded_config.enable_metrics = true;
  threaded_config.trace_mode = nimble::runtime::TraceMode::kRing;
  threaded_config.trace_capacity = 16U;
  threaded_config.queue_app_mode = nimble::runtime::QueueAppThreadingMode::kThreaded;
  threaded_config.profile_artifacts.push_back(artifact_path);
  threaded_config.listeners.push_back(nimble::runtime::ListenerConfig{
    .name = "threaded-main",
    .host = "127.0.0.1",
    .port = 0U,
    .worker_hint = 0U,
  });
  threaded_config.counterparties.push_back(nimble::runtime::CounterpartyConfig{
    .name = "sell-buy-threaded-queue",
    .session =
      nimble::session::SessionConfig{
        .session_id = 3105U,
        .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
        .profile_id = profile_id,
        .heartbeat_interval_seconds = 1U,
        .is_initiator = false,
      },
    .store_path = {},
    .default_appl_ver_id = {},
    .store_mode = nimble::runtime::StoreMode::kMemory,
    .recovery_mode = nimble::session::RecoveryMode::kMemoryOnly,
    .dispatch_mode = nimble::runtime::AppDispatchMode::kQueueDecoupled,
    .validation_policy = nimble::session::ValidationPolicy::Permissive(),
  });

  nimble::runtime::Engine threaded_engine;
  REQUIRE(threaded_engine.Boot(threaded_config).ok());

  auto threaded_application = std::make_shared<MixedApplication>(threaded_config.worker_count, 3105U);
  nimble::runtime::ManagedQueueApplicationRunnerOptions threaded_runner;
  threaded_runner.mode = nimble::runtime::ManagedQueueApplicationRunnerMode::kThreaded;
  threaded_runner.handlers.push_back(std::make_unique<QueueReplyHandler>(3105U));

  nimble::runtime::LiveAcceptor threaded_runtime(&threaded_engine,
                                                 nimble::runtime::LiveAcceptor::Options{
                                                   .poll_timeout = std::chrono::milliseconds(25),
                                                   .io_timeout = std::chrono::seconds(5),
                                                   .application = threaded_application,
                                                   .managed_queue_runner = std::move(threaded_runner),
                                                 });
  REQUIRE(threaded_runtime.OpenListeners("threaded-main").ok());

  auto threaded_port = threaded_runtime.listener_port("threaded-main");
  REQUIRE(threaded_port.ok());

  std::promise<nimble::base::Status> threaded_runtime_result;
  auto threaded_runtime_future = threaded_runtime_result.get_future();
  std::jthread threaded_runtime_thread([&threaded_runtime, &threaded_runtime_result]() {
    threaded_runtime_result.set_value(threaded_runtime.Run(1U, std::chrono::seconds(10)));
  });

  status = RunInitiatorEchoSession(threaded_port.value(), dictionary.value(), 4105U, "FIX.4.4", {}, "PARTY-THREADED");
  REQUIRE(status.ok());
  REQUIRE(threaded_runtime_future.get().ok());
  REQUIRE(threaded_runtime.completed_session_count() == 1U);
  REQUIRE(threaded_application->queued_application_events() == 1U);

  nimble::runtime::EngineConfig timer_config;
  timer_config.worker_count = 1U;
  timer_config.enable_metrics = true;
  timer_config.trace_mode = nimble::runtime::TraceMode::kRing;
  timer_config.trace_capacity = 16U;
  timer_config.profile_artifacts.push_back(artifact_path);
  timer_config.listeners.push_back(nimble::runtime::ListenerConfig{
    .name = "timer-main",
    .host = "127.0.0.1",
    .port = 0U,
    .worker_hint = 0U,
  });
  timer_config.counterparties.push_back(nimble::runtime::CounterpartyConfig{
    .name = "sell-buy-timer",
    .session =
      nimble::session::SessionConfig{
        .session_id = 3103U,
        .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
        .profile_id = profile_id,
        .heartbeat_interval_seconds = 1U,
        .is_initiator = false,
      },
    .store_path = {},
    .default_appl_ver_id = {},
    .store_mode = nimble::runtime::StoreMode::kMemory,
    .recovery_mode = nimble::session::RecoveryMode::kMemoryOnly,
    .dispatch_mode = nimble::runtime::AppDispatchMode::kInline,
    .validation_policy = nimble::session::ValidationPolicy::Permissive(),
  });

  nimble::runtime::Engine timer_engine;
  REQUIRE(timer_engine.Boot(timer_config).ok());

  nimble::runtime::LiveAcceptor timer_runtime(&timer_engine,
                                              nimble::runtime::LiveAcceptor::Options{
                                                .poll_timeout = std::chrono::seconds(5),
                                                .io_timeout = std::chrono::seconds(5),
                                                .application = {},
                                              });
  REQUIRE(timer_runtime.OpenListeners("timer-main").ok());

  auto timer_port = timer_runtime.listener_port("timer-main");
  REQUIRE(timer_port.ok());

  std::promise<nimble::base::Status> timer_runtime_result;
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

TEST_CASE("live-backpressure", "[live-backpressure]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }
  const auto artifact_path = std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.nfa";
  const auto profile_id = dictionary.value().profile().header().profile_id;

  nimble::runtime::EngineConfig config;
  config.worker_count = 1U;
  config.enable_metrics = true;
  config.trace_mode = nimble::runtime::TraceMode::kRing;
  config.trace_capacity = 16U;
  config.profile_artifacts.push_back(artifact_path);
  config.listeners.push_back(nimble::runtime::ListenerConfig{
    .name = "main",
    .host = "127.0.0.1",
    .port = 0U,
    .worker_hint = 0U,
  });

  nimble::runtime::CounterpartyConfig counterparty;
  counterparty.name = "sell-buy-backpressure";
  counterparty.session.session_id = 3201U;
  counterparty.session.key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" };
  counterparty.session.profile_id = profile_id;
  counterparty.session.heartbeat_interval_seconds = 1U;
  counterparty.session.is_initiator = false;
  counterparty.dispatch_mode = nimble::runtime::AppDispatchMode::kQueueDecoupled;
  counterparty.validation_policy = nimble::session::ValidationPolicy::Permissive();
  config.counterparties.push_back(counterparty);

  nimble::runtime::Engine engine;
  REQUIRE(engine.Boot(config).ok());

  auto application = std::make_shared<BackpressureApplication>(config.worker_count);
  REQUIRE(application->Prefill(0U).ok());

  nimble::runtime::LiveAcceptor runtime(&engine,
                                        nimble::runtime::LiveAcceptor::Options{
                                          .poll_timeout = std::chrono::milliseconds(25),
                                          .io_timeout = std::chrono::seconds(5),
                                          .application = application,
                                        });
  REQUIRE(runtime.OpenListeners("main").ok());

  auto port = runtime.listener_port("main");
  REQUIRE(port.ok());

  std::promise<nimble::base::Status> runtime_result;
  auto runtime_future = runtime_result.get_future();
  std::jthread runtime_thread(
    [&runtime, &runtime_result]() { runtime_result.set_value(runtime.Run(1U, std::chrono::seconds(10))); });

  auto initiator_future = std::async(std::launch::async, [&]() {
    return RunInitiatorEchoSession(port.value(), dictionary.value(), 4201U, "FIX.4.4", {}, "PARTY-BP");
  });

  REQUIRE(application->WaitForBackpressure(std::chrono::seconds(5)));

  std::atomic<bool> stop_application{ false };
  BackpressureReplyHandler queue_handler;
  nimble::runtime::QueueApplicationPoller poller(&application->queue_application(), &queue_handler);
  std::promise<nimble::base::Status> application_result;
  auto application_future = application_result.get_future();
  std::jthread application_thread([&]() { application_result.set_value(poller.RunWorker(0U, stop_application)); });

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

TEST_CASE("dynamic session factory accepts unknown CompID", "[live-session-factory]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }
  const auto artifact_path = std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.nfa";
  const auto profile_id = dictionary.value().profile().header().profile_id;

  nimble::runtime::EngineConfig config;
  config.worker_count = 1U;
  config.enable_metrics = false;
  config.trace_mode = nimble::runtime::TraceMode::kDisabled;
  config.accept_unknown_sessions = true;
  config.profile_artifacts.push_back(artifact_path);
  config.listeners.push_back(nimble::runtime::ListenerConfig{
    .name = "factory-main",
    .host = "127.0.0.1",
    .port = 0U,
    .worker_hint = 0U,
  });
  // No counterparties configured — factory provides them dynamically.

  nimble::runtime::Engine engine;
  REQUIRE(engine.Boot(config).ok());

  std::atomic<std::uint64_t> next_session_id{ 7001U };
  engine.SetSessionFactory(
    [&](const nimble::session::SessionKey& key) -> nimble::base::Result<nimble::runtime::CounterpartyConfig> {
      const auto session_id = next_session_id.fetch_add(1U);
      return nimble::runtime::CounterpartyConfig{
        .name = "dynamic-" + key.target_comp_id,
        .session =
          nimble::session::SessionConfig{
            .session_id = session_id,
            .key = key,
            .profile_id = profile_id,
            .heartbeat_interval_seconds = 1U,
            .is_initiator = false,
          },
        .dispatch_mode = nimble::runtime::AppDispatchMode::kInline,
        .validation_policy = nimble::session::ValidationPolicy::Permissive(),
      };
    });

  auto application = std::make_shared<MixedApplication>(config.worker_count);

  nimble::runtime::LiveAcceptor runtime(&engine,
                                        nimble::runtime::LiveAcceptor::Options{
                                          .poll_timeout = std::chrono::milliseconds(25),
                                          .io_timeout = std::chrono::seconds(5),
                                          .application = application,
                                        });
  REQUIRE(runtime.OpenListeners("factory-main").ok());

  auto port = runtime.listener_port("factory-main");
  REQUIRE(port.ok());

  std::promise<nimble::base::Status> runtime_result;
  auto runtime_future = runtime_result.get_future();
  std::jthread runtime_thread(
    [&runtime, &runtime_result]() { runtime_result.set_value(runtime.Run(1U, std::chrono::seconds(10))); });

  auto status = RunInitiatorEchoSession(
    port.value(), dictionary.value(), 9001U, "FIX.4.2", {}, "UNKNOWN-BUYER", "SELL", "PARTY-DYN");
  REQUIRE(status.ok());

  REQUIRE(runtime_future.get().ok());
  REQUIRE(runtime.completed_session_count() == 1U);
  REQUIRE(application->application_events() == 1U);
}

TEST_CASE("dynamic session factory rejects unknown CompID", "[live-session-factory]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }
  const auto artifact_path = std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.nfa";
  const auto profile_id = dictionary.value().profile().header().profile_id;

  nimble::runtime::EngineConfig config;
  config.worker_count = 1U;
  config.enable_metrics = false;
  config.trace_mode = nimble::runtime::TraceMode::kDisabled;
  config.accept_unknown_sessions = true;
  config.profile_artifacts.push_back(artifact_path);
  config.listeners.push_back(nimble::runtime::ListenerConfig{
    .name = "factory-reject-main",
    .host = "127.0.0.1",
    .port = 0U,
    .worker_hint = 0U,
  });

  nimble::runtime::Engine engine;
  REQUIRE(engine.Boot(config).ok());

  engine.SetSessionFactory(
    [](const nimble::session::SessionKey&) -> nimble::base::Result<nimble::runtime::CounterpartyConfig> {
      return nimble::base::Status::NotFound("factory rejects this session");
    });

  nimble::runtime::LiveAcceptor runtime(&engine,
                                        nimble::runtime::LiveAcceptor::Options{
                                          .poll_timeout = std::chrono::milliseconds(25),
                                          .io_timeout = std::chrono::seconds(5),
                                        });
  REQUIRE(runtime.OpenListeners("factory-reject-main").ok());

  auto port = runtime.listener_port("factory-reject-main");
  REQUIRE(port.ok());

  std::promise<nimble::base::Status> runtime_result;
  auto runtime_future = runtime_result.get_future();
  std::jthread runtime_thread(
    [&runtime, &runtime_result]() { runtime_result.set_value(runtime.Run(0U, std::chrono::seconds(3))); });

  // Initiator connects; acceptor factory rejects → connection closed.
  auto connection = nimble::transport::TcpConnection::Connect("127.0.0.1", port.value(), std::chrono::seconds(5));
  REQUIRE(connection.ok());

  nimble::store::MemorySessionStore store;
  nimble::session::AdminProtocol initiator(
    nimble::session::AdminProtocolConfig{
      .session =
        nimble::session::SessionConfig{
          .session_id = 9901U,
          .key = nimble::session::SessionKey{ "FIX.4.2", "REJECTED-BUYER", "SELL" },
          .profile_id = profile_id,
          .heartbeat_interval_seconds = 1U,
          .is_initiator = true,
        },
      .begin_string = "FIX.4.2",
      .sender_comp_id = "REJECTED-BUYER",
      .target_comp_id = "SELL",
      .heartbeat_interval_seconds = 1U,
      .validation_policy = nimble::session::ValidationPolicy::Permissive(),
    },
    dictionary.value(),
    &store);

  auto start = initiator.OnTransportConnected(NowNs());
  REQUIRE(start.ok());
  for (const auto& outbound : start.value().outbound_frames) {
    auto send_status = connection.value().Send(outbound.bytes, std::chrono::seconds(5));
    REQUIRE(send_status.ok());
  }

  // The acceptor should close the connection; reading should fail or get no
  // valid FIX response.
  auto frame = connection.value().ReceiveFrame(std::chrono::seconds(3));
  connection.value().Close();

  runtime.Stop();
  (void)runtime_future.get();
  REQUIRE(runtime.completed_session_count() == 0U);
}

TEST_CASE("whitelist factory accepts listed CompID", "[live-session-factory]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }
  const auto artifact_path = std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.nfa";
  const auto profile_id = dictionary.value().profile().header().profile_id;

  nimble::runtime::EngineConfig config;
  config.worker_count = 1U;
  config.enable_metrics = false;
  config.trace_mode = nimble::runtime::TraceMode::kDisabled;
  config.accept_unknown_sessions = true;
  config.profile_artifacts.push_back(artifact_path);
  config.listeners.push_back(nimble::runtime::ListenerConfig{
    .name = "whitelist-accept-main",
    .host = "127.0.0.1",
    .port = 0U,
    .worker_hint = 0U,
  });

  nimble::runtime::Engine engine;
  REQUIRE(engine.Boot(config).ok());

  nimble::runtime::CounterpartyConfig tmpl;
  tmpl.name = "whitelist-template";
  tmpl.session.session_id = 7101U;
  tmpl.session.profile_id = profile_id;
  tmpl.session.heartbeat_interval_seconds = 1U;
  tmpl.session.is_initiator = false;
  tmpl.dispatch_mode = nimble::runtime::AppDispatchMode::kInline;
  tmpl.validation_policy = nimble::session::ValidationPolicy::Permissive();

  nimble::runtime::WhitelistSessionFactory whitelist;
  whitelist.Allow("FIX.4.2", "SELL", tmpl);
  engine.SetSessionFactory(whitelist);

  auto application = std::make_shared<MixedApplication>(config.worker_count);

  nimble::runtime::LiveAcceptor runtime(&engine,
                                        nimble::runtime::LiveAcceptor::Options{
                                          .poll_timeout = std::chrono::milliseconds(25),
                                          .io_timeout = std::chrono::seconds(5),
                                          .application = application,
                                        });
  REQUIRE(runtime.OpenListeners("whitelist-accept-main").ok());

  auto port = runtime.listener_port("whitelist-accept-main");
  REQUIRE(port.ok());

  std::promise<nimble::base::Status> runtime_result;
  auto runtime_future = runtime_result.get_future();
  std::jthread runtime_thread(
    [&runtime, &runtime_result]() { runtime_result.set_value(runtime.Run(1U, std::chrono::seconds(10))); });

  auto status = RunInitiatorEchoSession(
    port.value(), dictionary.value(), 9101U, "FIX.4.2", {}, "WHITELISTED-BUYER", "SELL", "PARTY-WL");
  REQUIRE(status.ok());

  REQUIRE(runtime_future.get().ok());
  REQUIRE(runtime.completed_session_count() == 1U);
  REQUIRE(application->application_events() == 1U);
}

TEST_CASE("whitelist factory rejects unlisted CompID", "[live-session-factory]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }
  const auto artifact_path = std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.nfa";
  const auto profile_id = dictionary.value().profile().header().profile_id;

  nimble::runtime::EngineConfig config;
  config.worker_count = 1U;
  config.enable_metrics = false;
  config.trace_mode = nimble::runtime::TraceMode::kDisabled;
  config.accept_unknown_sessions = true;
  config.profile_artifacts.push_back(artifact_path);
  config.listeners.push_back(nimble::runtime::ListenerConfig{
    .name = "whitelist-reject-main",
    .host = "127.0.0.1",
    .port = 0U,
    .worker_hint = 0U,
  });

  nimble::runtime::Engine engine;
  REQUIRE(engine.Boot(config).ok());

  nimble::runtime::CounterpartyConfig tmpl;
  tmpl.name = "whitelist-template";
  tmpl.session.session_id = 7201U;
  tmpl.session.profile_id = profile_id;
  tmpl.session.heartbeat_interval_seconds = 1U;
  tmpl.session.is_initiator = false;
  tmpl.dispatch_mode = nimble::runtime::AppDispatchMode::kInline;
  tmpl.validation_policy = nimble::session::ValidationPolicy::Permissive();

  nimble::runtime::WhitelistSessionFactory whitelist;
  // Only allow sender_comp_id "ALLOWED-SENDER" — we connect as initiator
  // "NOT-ON-LIST" which means the acceptor sees session key {FIX.4.2, SELL,
  // NOT-ON-LIST} where sender_comp_id is "SELL", not "ALLOWED-SENDER".
  whitelist.Allow("FIX.4.2", "ALLOWED-SENDER", tmpl);
  engine.SetSessionFactory(whitelist);

  nimble::runtime::LiveAcceptor runtime(&engine,
                                        nimble::runtime::LiveAcceptor::Options{
                                          .poll_timeout = std::chrono::milliseconds(25),
                                          .io_timeout = std::chrono::seconds(5),
                                        });
  REQUIRE(runtime.OpenListeners("whitelist-reject-main").ok());

  auto port = runtime.listener_port("whitelist-reject-main");
  REQUIRE(port.ok());

  std::promise<nimble::base::Status> runtime_result;
  auto runtime_future = runtime_result.get_future();
  std::jthread runtime_thread(
    [&runtime, &runtime_result]() { runtime_result.set_value(runtime.Run(0U, std::chrono::seconds(3))); });

  auto connection = nimble::transport::TcpConnection::Connect("127.0.0.1", port.value(), std::chrono::seconds(5));
  REQUIRE(connection.ok());

  nimble::store::MemorySessionStore store;
  nimble::session::AdminProtocol initiator(
    nimble::session::AdminProtocolConfig{
      .session =
        nimble::session::SessionConfig{
          .session_id = 9201U,
          .key = nimble::session::SessionKey{ "FIX.4.2", "NOT-ON-LIST", "SELL" },
          .profile_id = profile_id,
          .heartbeat_interval_seconds = 1U,
          .is_initiator = true,
        },
      .begin_string = "FIX.4.2",
      .sender_comp_id = "NOT-ON-LIST",
      .target_comp_id = "SELL",
      .heartbeat_interval_seconds = 1U,
      .validation_policy = nimble::session::ValidationPolicy::Permissive(),
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

TEST_CASE("dynamic session factory is ignored when accept_unknown_sessions is disabled", "[live-session-factory]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }
  const auto artifact_path = std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.nfa";
  const auto profile_id = dictionary.value().profile().header().profile_id;

  nimble::runtime::EngineConfig config;
  config.worker_count = 1U;
  config.enable_metrics = false;
  config.trace_mode = nimble::runtime::TraceMode::kDisabled;
  config.profile_artifacts.push_back(artifact_path);
  config.listeners.push_back(nimble::runtime::ListenerConfig{
    .name = "factory-disabled-main",
    .host = "127.0.0.1",
    .port = 0U,
    .worker_hint = 0U,
  });

  nimble::runtime::Engine engine;
  REQUIRE(engine.Boot(config).ok());

  std::atomic<std::uint32_t> factory_calls{ 0U };
  engine.SetSessionFactory(
    [&](const nimble::session::SessionKey& key) -> nimble::base::Result<nimble::runtime::CounterpartyConfig> {
      ++factory_calls;
      return nimble::runtime::CounterpartyConfig{
        .name = "dynamic-" + key.target_comp_id,
        .session =
          nimble::session::SessionConfig{
            .session_id = 7301U,
            .key = key,
            .profile_id = profile_id,
            .heartbeat_interval_seconds = 1U,
            .is_initiator = false,
          },
        .dispatch_mode = nimble::runtime::AppDispatchMode::kInline,
        .validation_policy = nimble::session::ValidationPolicy::Permissive(),
      };
    });

  nimble::runtime::LiveAcceptor runtime(&engine,
                                        nimble::runtime::LiveAcceptor::Options{
                                          .poll_timeout = std::chrono::milliseconds(25),
                                          .io_timeout = std::chrono::seconds(5),
                                        });
  REQUIRE(runtime.OpenListeners("factory-disabled-main").ok());

  auto port = runtime.listener_port("factory-disabled-main");
  REQUIRE(port.ok());

  std::promise<nimble::base::Status> runtime_result;
  auto runtime_future = runtime_result.get_future();
  std::jthread runtime_thread(
    [&runtime, &runtime_result]() { runtime_result.set_value(runtime.Run(0U, std::chrono::seconds(3))); });

  auto connection = nimble::transport::TcpConnection::Connect("127.0.0.1", port.value(), std::chrono::seconds(5));
  REQUIRE(connection.ok());

  nimble::store::MemorySessionStore store;
  nimble::session::AdminProtocol initiator(
    nimble::session::AdminProtocolConfig{
      .session =
        nimble::session::SessionConfig{
          .session_id = 9902U,
          .key = nimble::session::SessionKey{ "FIX.4.2", "DISABLED-BUYER", "SELL" },
          .profile_id = profile_id,
          .heartbeat_interval_seconds = 1U,
          .is_initiator = true,
        },
      .begin_string = "FIX.4.2",
      .sender_comp_id = "DISABLED-BUYER",
      .target_comp_id = "SELL",
      .heartbeat_interval_seconds = 1U,
      .validation_policy = nimble::session::ValidationPolicy::Permissive(),
    },
    dictionary.value(),
    &store);

  auto start = initiator.OnTransportConnected(NowNs());
  REQUIRE(start.ok());
  for (const auto& outbound : start.value().outbound_frames) {
    auto send_status = connection.value().Send(outbound.bytes, std::chrono::seconds(5));
    REQUIRE(send_status.ok());
  }

  auto frame = connection.value().ReceiveFrame(std::chrono::seconds(3));
  connection.value().Close();

  runtime.Stop();
  (void)runtime_future.get();
  REQUIRE(factory_calls.load() == 0U);
  REQUIRE(runtime.completed_session_count() == 0U);
}

TEST_CASE("busy-poll mode", "[live-runtime]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }
  const auto artifact_path = std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.nfa";
  const auto profile_id = dictionary.value().profile().header().profile_id;

  nimble::runtime::EngineConfig config;
  config.worker_count = 1U;
  config.enable_metrics = true;
  config.poll_mode = nimble::runtime::PollMode::kBusy;
  config.profile_artifacts.push_back(artifact_path);
  config.listeners.push_back(nimble::runtime::ListenerConfig{
    .name = "busy-main",
    .host = "127.0.0.1",
    .port = 0U,
    .worker_hint = 0U,
  });
  config.counterparties.push_back(nimble::runtime::CounterpartyConfig{
    .name = "sell-buy-busy",
    .session =
      nimble::session::SessionConfig{
        .session_id = 3301U,
        .key = nimble::session::SessionKey{ "FIX.4.2", "SELL", "BUY" },
        .profile_id = profile_id,
        .heartbeat_interval_seconds = 1U,
        .is_initiator = false,
      },
    .store_path = {},
    .default_appl_ver_id = {},
    .store_mode = nimble::runtime::StoreMode::kMemory,
    .recovery_mode = nimble::session::RecoveryMode::kMemoryOnly,
    .dispatch_mode = nimble::runtime::AppDispatchMode::kInline,
    .validation_policy = nimble::session::ValidationPolicy::Permissive(),
  });

  nimble::runtime::Engine engine;
  REQUIRE(engine.Boot(config).ok());

  auto application = std::make_shared<MixedApplication>(config.worker_count);
  nimble::runtime::LiveAcceptor runtime(&engine,
                                        nimble::runtime::LiveAcceptor::Options{
                                          .poll_timeout = std::chrono::milliseconds(50),
                                          .io_timeout = std::chrono::seconds(5),
                                          .application = application,
                                        });
  REQUIRE(runtime.OpenListeners("busy-main").ok());
  auto port = runtime.listener_port("busy-main");
  REQUIRE(port.ok());

  std::promise<nimble::base::Status> runtime_result;
  auto runtime_future = runtime_result.get_future();
  std::jthread runtime_thread(
    [&runtime, &runtime_result]() { runtime_result.set_value(runtime.Run(1U, std::chrono::seconds(10))); });

  auto status = RunInitiatorEchoSession(port.value(), dictionary.value(), 4301U, "FIX.4.2", {}, "BUSY-PARTY");
  REQUIRE(status.ok());

  REQUIRE(runtime_future.get().ok());
  REQUIRE(runtime.completed_session_count() == 1U);
  REQUIRE(application->application_events() == 1U);
}

TEST_CASE("busy-poll mode no missed events", "[live-runtime]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }
  const auto artifact_path = std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.nfa";
  const auto profile_id = dictionary.value().profile().header().profile_id;

  nimble::runtime::EngineConfig config;
  config.worker_count = 1U;
  config.enable_metrics = true;
  config.poll_mode = nimble::runtime::PollMode::kBusy;
  config.profile_artifacts.push_back(artifact_path);
  config.listeners.push_back(nimble::runtime::ListenerConfig{
    .name = "busy-multi-main",
    .host = "127.0.0.1",
    .port = 0U,
    .worker_hint = 0U,
  });
  config.counterparties.push_back(nimble::runtime::CounterpartyConfig{
    .name = "sell-buy-busy-a",
    .session =
      nimble::session::SessionConfig{
        .session_id = 3302U,
        .key = nimble::session::SessionKey{ "FIX.4.2", "SELL", "BUYA" },
        .profile_id = profile_id,
        .heartbeat_interval_seconds = 1U,
        .is_initiator = false,
      },
    .store_path = {},
    .default_appl_ver_id = {},
    .store_mode = nimble::runtime::StoreMode::kMemory,
    .recovery_mode = nimble::session::RecoveryMode::kMemoryOnly,
    .dispatch_mode = nimble::runtime::AppDispatchMode::kInline,
    .validation_policy = nimble::session::ValidationPolicy::Permissive(),
  });
  config.counterparties.push_back(nimble::runtime::CounterpartyConfig{
    .name = "sell-buy-busy-b",
    .session =
      nimble::session::SessionConfig{
        .session_id = 3303U,
        .key = nimble::session::SessionKey{ "FIX.4.2", "SELL", "BUYB" },
        .profile_id = profile_id,
        .heartbeat_interval_seconds = 1U,
        .is_initiator = false,
      },
    .store_path = {},
    .default_appl_ver_id = {},
    .store_mode = nimble::runtime::StoreMode::kMemory,
    .recovery_mode = nimble::session::RecoveryMode::kMemoryOnly,
    .dispatch_mode = nimble::runtime::AppDispatchMode::kInline,
    .validation_policy = nimble::session::ValidationPolicy::Permissive(),
  });

  nimble::runtime::Engine engine;
  REQUIRE(engine.Boot(config).ok());

  auto application = std::make_shared<MixedApplication>(config.worker_count);
  nimble::runtime::LiveAcceptor runtime(&engine,
                                        nimble::runtime::LiveAcceptor::Options{
                                          .poll_timeout = std::chrono::milliseconds(50),
                                          .io_timeout = std::chrono::seconds(5),
                                          .application = application,
                                        });
  REQUIRE(runtime.OpenListeners("busy-multi-main").ok());
  auto port = runtime.listener_port("busy-multi-main");
  REQUIRE(port.ok());

  std::promise<nimble::base::Status> runtime_result;
  auto runtime_future = runtime_result.get_future();
  std::jthread runtime_thread(
    [&runtime, &runtime_result]() { runtime_result.set_value(runtime.Run(2U, std::chrono::seconds(10))); });

  auto status_a =
    RunInitiatorEchoSession(port.value(), dictionary.value(), 4302U, "FIX.4.2", {}, "BUYA", "SELL", "BUSY-A");
  REQUIRE(status_a.ok());
  auto status_b =
    RunInitiatorEchoSession(port.value(), dictionary.value(), 4303U, "FIX.4.2", {}, "BUYB", "SELL", "BUSY-B");
  REQUIRE(status_b.ok());

  REQUIRE(runtime_future.get().ok());
  REQUIRE(runtime.completed_session_count() == 2U);
  REQUIRE(application->application_events() == 2U);
}

TEST_CASE("epoll backend single session", "[live-runtime]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }
  if (!nimble::runtime::IsIoBackendAvailable(nimble::runtime::IoBackend::kEpoll)) {
    SKIP("epoll not available");
  }
  const auto artifact_path = std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.nfa";
  const auto profile_id = dictionary.value().profile().header().profile_id;

  nimble::runtime::EngineConfig config;
  config.worker_count = 1U;
  config.enable_metrics = true;
  config.io_backend = nimble::runtime::IoBackend::kEpoll;
  config.profile_artifacts.push_back(artifact_path);
  config.listeners.push_back(nimble::runtime::ListenerConfig{
    .name = "epoll-main",
    .host = "127.0.0.1",
    .port = 0U,
    .worker_hint = 0U,
  });
  config.counterparties.push_back(nimble::runtime::CounterpartyConfig{
    .name = "sell-buy-epoll",
    .session =
      nimble::session::SessionConfig{
        .session_id = 3401U,
        .key = nimble::session::SessionKey{ "FIX.4.2", "SELL", "BUY" },
        .profile_id = profile_id,
        .heartbeat_interval_seconds = 1U,
        .is_initiator = false,
      },
    .store_path = {},
    .default_appl_ver_id = {},
    .store_mode = nimble::runtime::StoreMode::kMemory,
    .recovery_mode = nimble::session::RecoveryMode::kMemoryOnly,
    .dispatch_mode = nimble::runtime::AppDispatchMode::kInline,
    .validation_policy = nimble::session::ValidationPolicy::Permissive(),
  });

  nimble::runtime::Engine engine;
  REQUIRE(engine.Boot(config).ok());

  auto application = std::make_shared<MixedApplication>(config.worker_count);
  nimble::runtime::LiveAcceptor runtime(&engine,
                                        nimble::runtime::LiveAcceptor::Options{
                                          .poll_timeout = std::chrono::milliseconds(50),
                                          .io_timeout = std::chrono::seconds(5),
                                          .application = application,
                                        });
  REQUIRE(runtime.OpenListeners("epoll-main").ok());
  auto port = runtime.listener_port("epoll-main");
  REQUIRE(port.ok());

  std::promise<nimble::base::Status> runtime_result;
  auto runtime_future = runtime_result.get_future();
  std::jthread runtime_thread(
    [&runtime, &runtime_result]() { runtime_result.set_value(runtime.Run(1U, std::chrono::seconds(10))); });

  auto status = RunInitiatorEchoSession(port.value(), dictionary.value(), 4401U, "FIX.4.2", {}, "EPOLL-PARTY");
  REQUIRE(status.ok());

  REQUIRE(runtime_future.get().ok());
  REQUIRE(runtime.completed_session_count() == 1U);
  REQUIRE(application->application_events() == 1U);
}

TEST_CASE("io_uring backend single session", "[live-runtime]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }
  if (!nimble::runtime::IsIoBackendAvailable(nimble::runtime::IoBackend::kIoUring)) {
    SUCCEED("io_uring not available");
    return;
  }
  const auto artifact_path = std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.nfa";
  const auto profile_id = dictionary.value().profile().header().profile_id;

  nimble::runtime::EngineConfig config;
  config.worker_count = 1U;
  config.enable_metrics = true;
  config.io_backend = nimble::runtime::IoBackend::kIoUring;
  config.profile_artifacts.push_back(artifact_path);
  config.listeners.push_back(nimble::runtime::ListenerConfig{
    .name = "uring-main",
    .host = "127.0.0.1",
    .port = 0U,
    .worker_hint = 0U,
  });
  config.counterparties.push_back(nimble::runtime::CounterpartyConfig{
    .name = "sell-buy-uring",
    .session =
      nimble::session::SessionConfig{
        .session_id = 3501U,
        .key = nimble::session::SessionKey{ "FIX.4.2", "SELL", "BUY" },
        .profile_id = profile_id,
        .heartbeat_interval_seconds = 1U,
        .is_initiator = false,
      },
    .store_path = {},
    .default_appl_ver_id = {},
    .store_mode = nimble::runtime::StoreMode::kMemory,
    .recovery_mode = nimble::session::RecoveryMode::kMemoryOnly,
    .dispatch_mode = nimble::runtime::AppDispatchMode::kInline,
    .validation_policy = nimble::session::ValidationPolicy::Permissive(),
  });

  nimble::runtime::Engine engine;
  REQUIRE(engine.Boot(config).ok());

  auto application = std::make_shared<MixedApplication>(config.worker_count);
  nimble::runtime::LiveAcceptor runtime(&engine,
                                        nimble::runtime::LiveAcceptor::Options{
                                          .poll_timeout = std::chrono::milliseconds(50),
                                          .io_timeout = std::chrono::seconds(5),
                                          .application = application,
                                        });
  REQUIRE(runtime.OpenListeners("uring-main").ok());
  auto port = runtime.listener_port("uring-main");
  REQUIRE(port.ok());

  std::promise<nimble::base::Status> runtime_result;
  auto runtime_future = runtime_result.get_future();
  std::jthread runtime_thread(
    [&runtime, &runtime_result]() { runtime_result.set_value(runtime.Run(1U, std::chrono::seconds(10))); });

  auto status = RunInitiatorEchoSession(port.value(), dictionary.value(), 4501U, "FIX.4.2", {}, "URING-PARTY");
  REQUIRE(status.ok());

  REQUIRE(runtime_future.get().ok());
  REQUIRE(runtime.completed_session_count() == 1U);
  REQUIRE(application->application_events() == 1U);
}
