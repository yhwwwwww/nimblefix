#include <catch2/catch_test_macros.hpp>

#include "fastfix/runtime/application.h"

#include "test_support.h"

namespace {

class OwnedOnlyCommandSink final : public fastfix::session::SessionCommandSink {
  public:
    auto EnqueueSend(std::uint64_t session_id, fastfix::message::MessageRef message) -> fastfix::base::Status override {
        last_session_id_ = session_id;
        last_message_ = std::move(message);
        ++enqueued_;
        return fastfix::base::Status::Ok();
    }

    auto LoadSnapshot(std::uint64_t session_id) const -> fastfix::base::Result<fastfix::session::SessionSnapshot> override {
        (void)session_id;
        return fastfix::base::Status::InvalidArgument("snapshot unsupported in test sink");
    }

    auto Subscribe(std::uint64_t session_id, std::size_t queue_capacity)
        -> fastfix::base::Result<fastfix::session::SessionSubscription> override {
        (void)session_id;
        (void)queue_capacity;
        return fastfix::base::Status::InvalidArgument("subscribe unsupported in test sink");
    }

    [[nodiscard]] auto enqueued() const -> std::uint64_t {
        return enqueued_;
    }

  private:
    std::uint64_t last_session_id_{0};
    fastfix::message::MessageRef last_message_{};
    std::uint64_t enqueued_{0};
};

auto MakeEvent(
    std::uint64_t session_id,
    std::uint32_t worker_id,
    std::string text,
    fastfix::runtime::RuntimeEventKind kind = fastfix::runtime::RuntimeEventKind::kApplicationMessage)
    -> fastfix::runtime::RuntimeEvent {
    return fastfix::runtime::RuntimeEvent{
        .kind = kind,
        .session_event = fastfix::runtime::SessionEventKind::kBound,
        .handle = fastfix::session::SessionHandle(session_id, worker_id),
        .session_key = fastfix::session::SessionKey{"FIX.4.4", "BUY", "SELL"},
        .message = {},
        .text = std::move(text),
        .timestamp_ns = session_id,
    };
}

}  // namespace

TEST_CASE("application-queue", "[application-queue]") {
    SECTION("backpressure") {
        fastfix::runtime::QueueApplication queue(
            1U,
            fastfix::runtime::QueueApplicationOptions{
                .queue_capacity = 1U,
                .overflow_policy = fastfix::runtime::QueueOverflowPolicy::kBackpressure,
            });
        REQUIRE(queue.overflow_policy() == fastfix::runtime::QueueOverflowPolicy::kBackpressure);
        REQUIRE(queue.OnAppMessage(MakeEvent(1001U, 0U, "first")).ok());

        const auto busy = queue.OnAppMessage(MakeEvent(1002U, 0U, "second"));
        REQUIRE(busy.code() == fastfix::base::ErrorCode::kBusy);
        REQUIRE(queue.overflow_events() == 1U);
        REQUIRE(queue.dropped_events() == 0U);

        auto queued = queue.TryPopEvent(0U);
        REQUIRE(queued.ok());
        REQUIRE(queued.value().has_value());
        REQUIRE(queued.value()->text == "first");
        REQUIRE(queue.OnAppMessage(MakeEvent(1002U, 0U, "second")).ok());
    }

    SECTION("drop-newest") {
        fastfix::runtime::QueueApplication queue(
            1U,
            fastfix::runtime::QueueApplicationOptions{
                .queue_capacity = 1U,
                .overflow_policy = fastfix::runtime::QueueOverflowPolicy::kDropNewest,
            });
        REQUIRE(queue.OnSessionEvent(MakeEvent(2001U, 0U, "first")).ok());
        REQUIRE(queue.OnSessionEvent(MakeEvent(2002U, 0U, "second")).ok());
        REQUIRE(queue.overflow_events() == 1U);
        REQUIRE(queue.dropped_events() == 1U);

        auto queued = queue.TryPopEvent(0U);
        REQUIRE(queued.ok());
        REQUIRE(queued.value().has_value());
        REQUIRE(queued.value()->text == "first");

        auto empty = queue.TryPopEvent(0U);
        REQUIRE(empty.ok());
        REQUIRE(!empty.value().has_value());
    }

    SECTION("close-session") {
        fastfix::runtime::QueueApplication queue(
            1U,
            fastfix::runtime::QueueApplicationOptions{
                .queue_capacity = 1U,
                .overflow_policy = fastfix::runtime::QueueOverflowPolicy::kCloseSession,
            });
        REQUIRE(queue.OnAdminMessage(MakeEvent(3001U, 0U, "first")).ok());

        const auto failure = queue.OnAdminMessage(MakeEvent(3002U, 0U, "second"));
        REQUIRE(failure.code() == fastfix::base::ErrorCode::kIoError);
        REQUIRE(queue.overflow_events() == 1U);
        REQUIRE(queue.dropped_events() == 0U);
    }

    SECTION("split queues") {
        fastfix::runtime::QueueApplication queue(
            1U,
            fastfix::runtime::QueueApplicationOptions{
                .queue_capacity = 1U,
                .overflow_policy = fastfix::runtime::QueueOverflowPolicy::kDropNewest,
                .control_queue_capacity = 1U,
                .control_overflow_policy = fastfix::runtime::QueueOverflowPolicy::kBackpressure,
            });
        REQUIRE(queue.control_overflow_policy() == fastfix::runtime::QueueOverflowPolicy::kBackpressure);

        REQUIRE(queue.OnAppMessage(MakeEvent(5001U, 0U, "app-first")).ok());
        REQUIRE(queue.OnAdminMessage(
                     MakeEvent(5002U, 0U, "admin-first", fastfix::runtime::RuntimeEventKind::kAdminMessage))
                .ok());

        REQUIRE(queue.OnAppMessage(MakeEvent(5003U, 0U, "app-second")).ok());
        REQUIRE(queue.app_overflow_events() == 1U);
        REQUIRE(queue.app_dropped_events() == 1U);

        const auto control_busy = queue.OnSessionEvent(
            MakeEvent(5004U, 0U, "session-second", fastfix::runtime::RuntimeEventKind::kSession));
        REQUIRE(control_busy.code() == fastfix::base::ErrorCode::kBusy);
        REQUIRE(queue.control_overflow_events() == 1U);
        REQUIRE(queue.control_dropped_events() == 0U);

        auto event = queue.TryPopEvent(0U);
        REQUIRE(event.ok());
        REQUIRE(event.value().has_value());
        REQUIRE(event.value()->kind == fastfix::runtime::RuntimeEventKind::kAdminMessage);
        REQUIRE(event.value()->text == "admin-first");

        REQUIRE(queue.OnSessionEvent(
                     MakeEvent(5004U, 0U, "session-second", fastfix::runtime::RuntimeEventKind::kSession))
                .ok());

        event = queue.TryPopEvent(0U);
        REQUIRE(event.ok());
        REQUIRE(event.value().has_value());
        REQUIRE(event.value()->kind == fastfix::runtime::RuntimeEventKind::kSession);
        REQUIRE(event.value()->text == "session-second");

        event = queue.TryPopEvent(0U);
        REQUIRE(event.ok());
        REQUIRE(event.value().has_value());
        REQUIRE(event.value()->kind == fastfix::runtime::RuntimeEventKind::kApplicationMessage);
        REQUIRE(event.value()->text == "app-first");

        auto empty = queue.TryPopEvent(0U);
        REQUIRE(empty.ok());
        REQUIRE(!empty.value().has_value());
    }

    SECTION("owned-only command sink") {
        auto sink = std::make_shared<OwnedOnlyCommandSink>();
        fastfix::session::SessionHandle handle(4001U, 0U, sink);

        fastfix::message::MessageBuilder builder("D");
        builder.set_string(35U, "D");
        builder.set_string(49U, "BUY");
        builder.set_string(56U, "SELL");
        auto message = std::move(builder).build();

        const auto borrowed = handle.SendBorrowed(message.view());
        REQUIRE(borrowed.code() == fastfix::base::ErrorCode::kInvalidArgument);
        REQUIRE(sink->enqueued() == 0U);

        REQUIRE(handle.Send(message.view()).ok());
        REQUIRE(sink->enqueued() == 1U);
    }

}