#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>

#include "nimblefix/codec/fix_tags.h"
#include "nimblefix/runtime/application.h"
#include "nimblefix/runtime/live_runtime_support.h"

#include "test_support.h"

namespace {

class OwnedOnlyCommandSink final : public nimble::session::SessionCommandSink
{
public:
  auto EnqueueSend(std::uint64_t session_id, nimble::message::MessageRef message) -> nimble::base::Status override
  {
    last_session_id_ = session_id;
    last_message_ = std::move(message);
    ++enqueued_;
    return nimble::base::Status::Ok();
  }

  auto LoadSnapshot(std::uint64_t session_id) const -> nimble::base::Result<nimble::session::SessionSnapshot> override
  {
    (void)session_id;
    return nimble::base::Status::InvalidArgument("snapshot unsupported in test sink");
  }

  auto Subscribe(std::uint64_t session_id, std::size_t queue_capacity)
    -> nimble::base::Result<nimble::session::SessionSubscription> override
  {
    (void)session_id;
    (void)queue_capacity;
    return nimble::base::Status::InvalidArgument("subscribe unsupported in test sink");
  }

  [[nodiscard]] auto enqueued() const -> std::uint64_t { return enqueued_; }

private:
  std::uint64_t last_session_id_{ 0 };
  nimble::message::MessageRef last_message_{};
  std::uint64_t enqueued_{ 0 };
};

class EncodedCommandSink final : public nimble::session::SessionCommandSink
{
public:
  auto EnqueueSend(std::uint64_t session_id, nimble::message::MessageRef message) -> nimble::base::Status override
  {
    return EnqueueSendWithEnvelope(session_id, std::move(message), {});
  }

  auto EnqueueSendWithEnvelope(std::uint64_t session_id,
                               nimble::message::MessageRef message,
                               nimble::session::SessionSendEnvelopeRef envelope) -> nimble::base::Status override
  {
    last_session_id_ = session_id;
    last_message_ = std::move(message);
    last_plain_envelope_ = std::move(envelope);
    ++plain_enqueued_;
    return nimble::base::Status::Ok();
  }

  auto EnqueueSendEncoded(std::uint64_t session_id, nimble::session::EncodedApplicationMessageRef message)
    -> nimble::base::Status override
  {
    return EnqueueSendEncodedWithEnvelope(session_id, std::move(message), {});
  }

  auto EnqueueSendEncodedWithEnvelope(std::uint64_t session_id,
                                      nimble::session::EncodedApplicationMessageRef message,
                                      nimble::session::SessionSendEnvelopeRef envelope) -> nimble::base::Status override
  {
    last_session_id_ = session_id;
    last_encoded_message_ = std::move(message);
    last_encoded_envelope_ = std::move(envelope);
    ++encoded_enqueued_;
    return nimble::base::Status::Ok();
  }

  auto LoadSnapshot(std::uint64_t session_id) const -> nimble::base::Result<nimble::session::SessionSnapshot> override
  {
    (void)session_id;
    return nimble::base::Status::InvalidArgument("snapshot unsupported in test sink");
  }

  auto Subscribe(std::uint64_t session_id, std::size_t queue_capacity)
    -> nimble::base::Result<nimble::session::SessionSubscription> override
  {
    (void)session_id;
    (void)queue_capacity;
    return nimble::base::Status::InvalidArgument("subscribe unsupported in test sink");
  }

  [[nodiscard]] auto plain_enqueued() const -> std::uint64_t { return plain_enqueued_; }

  [[nodiscard]] auto encoded_enqueued() const -> std::uint64_t { return encoded_enqueued_; }

  [[nodiscard]] auto last_plain_envelope() const -> nimble::session::SessionSendEnvelopeView
  {
    return last_plain_envelope_.view();
  }

  [[nodiscard]] auto last_encoded_view() const -> nimble::session::EncodedApplicationMessageView
  {
    return last_encoded_message_.view();
  }

  [[nodiscard]] auto last_encoded_envelope() const -> nimble::session::SessionSendEnvelopeView
  {
    return last_encoded_envelope_.view();
  }

private:
  std::uint64_t last_session_id_{ 0 };
  nimble::message::MessageRef last_message_{};
  nimble::session::EncodedApplicationMessageRef last_encoded_message_{};
  nimble::session::SessionSendEnvelopeRef last_plain_envelope_{};
  nimble::session::SessionSendEnvelopeRef last_encoded_envelope_{};
  std::uint64_t plain_enqueued_{ 0 };
  std::uint64_t encoded_enqueued_{ 0 };
};

class SingleProducerCommandSink final : public nimble::session::SessionCommandSink
{
public:
  auto EnqueueSend(std::uint64_t session_id, nimble::message::MessageRef message) -> nimble::base::Status override
  {
    return EnqueueSendWithEnvelope(session_id, std::move(message), {});
  }

  auto EnqueueSendWithEnvelope(std::uint64_t session_id,
                               nimble::message::MessageRef message,
                               nimble::session::SessionSendEnvelopeRef envelope) -> nimble::base::Status override
  {
    (void)session_id;
    (void)message;
    (void)envelope;
    auto status = producer_guard_.Validate();
    if (!status.ok()) {
      return status;
    }
    enqueued_.fetch_add(1U, std::memory_order_relaxed);
    return nimble::base::Status::Ok();
  }

  auto LoadSnapshot(std::uint64_t session_id) const -> nimble::base::Result<nimble::session::SessionSnapshot> override
  {
    (void)session_id;
    return nimble::base::Status::InvalidArgument("snapshot unsupported in test sink");
  }

  auto Subscribe(std::uint64_t session_id, std::size_t queue_capacity)
    -> nimble::base::Result<nimble::session::SessionSubscription> override
  {
    (void)session_id;
    (void)queue_capacity;
    return nimble::base::Status::InvalidArgument("subscribe unsupported in test sink");
  }

  [[nodiscard]] auto enqueued() const -> std::uint64_t { return enqueued_.load(std::memory_order_relaxed); }

private:
  nimble::runtime::SingleProducerGuard producer_guard_{};
  std::atomic<std::uint64_t> enqueued_{ 0U };
};

auto
MakeEvent(std::uint64_t session_id,
          std::uint32_t worker_id,
          std::string text,
          nimble::runtime::RuntimeEventKind kind = nimble::runtime::RuntimeEventKind::kApplicationMessage)
  -> nimble::runtime::RuntimeEvent
{
  return nimble::runtime::RuntimeEvent{
    .kind = kind,
    .session_event = nimble::runtime::SessionEventKind::kBound,
    .handle = nimble::session::SessionHandle(session_id, worker_id),
    .session_key = nimble::session::SessionKey{ "FIX.4.4", "BUY", "SELL" },
    .message = {},
    .text = std::move(text),
    .timestamp_ns = session_id,
  };
}

} // namespace

TEST_CASE("application-queue", "[application-queue]")
{
  SECTION("backpressure")
  {
    nimble::runtime::QueueApplication queue(1U,
                                            nimble::runtime::QueueApplicationOptions{
                                              .queue_capacity = 1U,
                                              .overflow_policy = nimble::runtime::QueueOverflowPolicy::kBackpressure,
                                            });
    REQUIRE(queue.overflow_policy() == nimble::runtime::QueueOverflowPolicy::kBackpressure);
    REQUIRE(queue.OnAppMessage(MakeEvent(1001U, 0U, "first")).ok());

    const auto busy = queue.OnAppMessage(MakeEvent(1002U, 0U, "second"));
    REQUIRE(busy.code() == nimble::base::ErrorCode::kBusy);
    REQUIRE(queue.overflow_events() == 1U);
    REQUIRE(queue.dropped_events() == 0U);

    auto queued = queue.TryPopEvent(0U);
    REQUIRE(queued.ok());
    REQUIRE(queued.value().has_value());
    REQUIRE(queued.value()->text == "first");
    REQUIRE(queue.OnAppMessage(MakeEvent(1002U, 0U, "second")).ok());
  }

  SECTION("drop-newest")
  {
    nimble::runtime::QueueApplication queue(1U,
                                            nimble::runtime::QueueApplicationOptions{
                                              .queue_capacity = 1U,
                                              .overflow_policy = nimble::runtime::QueueOverflowPolicy::kDropNewest,
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

  SECTION("close-session")
  {
    nimble::runtime::QueueApplication queue(1U,
                                            nimble::runtime::QueueApplicationOptions{
                                              .queue_capacity = 1U,
                                              .overflow_policy = nimble::runtime::QueueOverflowPolicy::kCloseSession,
                                            });
    REQUIRE(queue.OnAdminMessage(MakeEvent(3001U, 0U, "first")).ok());

    const auto failure = queue.OnAdminMessage(MakeEvent(3002U, 0U, "second"));
    REQUIRE(failure.code() == nimble::base::ErrorCode::kIoError);
    REQUIRE(queue.overflow_events() == 1U);
    REQUIRE(queue.dropped_events() == 0U);
  }

  SECTION("split queues")
  {
    nimble::runtime::QueueApplication queue(
      1U,
      nimble::runtime::QueueApplicationOptions{
        .queue_capacity = 1U,
        .overflow_policy = nimble::runtime::QueueOverflowPolicy::kDropNewest,
        .control_queue_capacity = 1U,
        .control_overflow_policy = nimble::runtime::QueueOverflowPolicy::kBackpressure,
      });
    REQUIRE(queue.control_overflow_policy() == nimble::runtime::QueueOverflowPolicy::kBackpressure);

    REQUIRE(queue.OnAppMessage(MakeEvent(5001U, 0U, "app-first")).ok());
    REQUIRE(
      queue.OnAdminMessage(MakeEvent(5002U, 0U, "admin-first", nimble::runtime::RuntimeEventKind::kAdminMessage)).ok());

    REQUIRE(queue.OnAppMessage(MakeEvent(5003U, 0U, "app-second")).ok());
    REQUIRE(queue.app_overflow_events() == 1U);
    REQUIRE(queue.app_dropped_events() == 1U);

    const auto control_busy =
      queue.OnSessionEvent(MakeEvent(5004U, 0U, "session-second", nimble::runtime::RuntimeEventKind::kSession));
    REQUIRE(control_busy.code() == nimble::base::ErrorCode::kBusy);
    REQUIRE(queue.control_overflow_events() == 1U);
    REQUIRE(queue.control_dropped_events() == 0U);

    auto event = queue.TryPopEvent(0U);
    REQUIRE(event.ok());
    REQUIRE(event.value().has_value());
    REQUIRE(event.value()->kind == nimble::runtime::RuntimeEventKind::kAdminMessage);
    REQUIRE(event.value()->text == "admin-first");

    REQUIRE(
      queue.OnSessionEvent(MakeEvent(5004U, 0U, "session-second", nimble::runtime::RuntimeEventKind::kSession)).ok());

    event = queue.TryPopEvent(0U);
    REQUIRE(event.ok());
    REQUIRE(event.value().has_value());
    REQUIRE(event.value()->kind == nimble::runtime::RuntimeEventKind::kSession);
    REQUIRE(event.value()->text == "session-second");

    event = queue.TryPopEvent(0U);
    REQUIRE(event.ok());
    REQUIRE(event.value().has_value());
    REQUIRE(event.value()->kind == nimble::runtime::RuntimeEventKind::kApplicationMessage);
    REQUIRE(event.value()->text == "app-first");

    auto empty = queue.TryPopEvent(0U);
    REQUIRE(empty.ok());
    REQUIRE(!empty.value().has_value());
  }

  SECTION("owned-only command sink")
  {
    auto sink = std::make_shared<OwnedOnlyCommandSink>();
    nimble::session::SessionHandle handle(4001U, 0U, sink);

    nimble::message::MessageBuilder builder("D");
    builder.set_string(nimble::codec::tags::kMsgType, "D");
    builder.set_string(nimble::codec::tags::kSenderCompID, "BUY");
    builder.set_string(nimble::codec::tags::kTargetCompID, "SELL");
    auto message = std::move(builder).build();

    const auto borrowed = handle.SendBorrowed(message.view());
    REQUIRE(borrowed.code() == nimble::base::ErrorCode::kInvalidArgument);
    REQUIRE(sink->enqueued() == 0U);

    REQUIRE(handle.Send(message.view()).ok());
    REQUIRE(sink->enqueued() == 1U);
  }

  SECTION("encoded command sink")
  {
    auto sink = std::make_shared<EncodedCommandSink>();
    nimble::session::SessionHandle handle(4002U, 0U, sink);

    nimble::message::MessageBuilder plain_builder("D");
    plain_builder.set_string(nimble::codec::tags::kMsgType, "D");
    plain_builder.set_string(nimble::codec::tags::kClOrdID, "ORD-PLAIN");
    auto plain_message = std::move(plain_builder).build();

    REQUIRE(handle.Send(plain_message.view(), { .sender_sub_id = "DESK-1", .target_sub_id = "ROUTE-1" }).ok());
    REQUIRE(sink->plain_enqueued() == 1U);
    REQUIRE(sink->last_plain_envelope().sender_sub_id == "DESK-1");
    REQUIRE(sink->last_plain_envelope().target_sub_id == "ROUTE-1");

    const auto encoded_body = nimble::tests::Bytes("11=ORD-ENC\x01"
                                                   "55=AAPL\x01");
    nimble::session::EncodedApplicationMessage encoded(
      "D", std::span<const std::byte>(encoded_body.data(), encoded_body.size()));

    REQUIRE(handle.SendEncoded(encoded, { .sender_sub_id = "DESK-9", .target_sub_id = "ROUTE-7" }).ok());
    REQUIRE(sink->plain_enqueued() == 1U);
    REQUIRE(sink->encoded_enqueued() == 1U);
    REQUIRE(sink->last_encoded_view().msg_type == "D");
    REQUIRE(sink->last_encoded_view().body.size() == encoded_body.size());
    REQUIRE(sink->last_encoded_envelope().sender_sub_id == "DESK-9");
    REQUIRE(sink->last_encoded_envelope().target_sub_id == "ROUTE-7");

    const auto borrowed = handle.SendEncodedBorrowed(
      nimble::session::EncodedApplicationMessageView{
        .msg_type = "D",
        .body = std::span<const std::byte>(encoded_body.data(), encoded_body.size()),
      },
      { .sender_sub_id = "DESK-2" });
    REQUIRE(borrowed.code() == nimble::base::ErrorCode::kInvalidArgument);
    REQUIRE(sink->encoded_enqueued() == 1U);
  }

  SECTION("single-producer send fast-fail")
  {
    auto sink = std::make_shared<SingleProducerCommandSink>();
    nimble::session::SessionHandle handle(4003U, 0U, sink);

    nimble::message::MessageBuilder builder("D");
    builder.set_string(nimble::codec::tags::kMsgType, "D");
    builder.set_string(nimble::codec::tags::kClOrdID, "ORD-SP");
    const auto message = std::move(builder).build();

    REQUIRE(handle.Send(message.view()).ok());
    REQUIRE(handle.Send(message.view()).ok());

    nimble::base::Status cross_thread_status = nimble::base::Status::Ok();
    std::jthread other_thread([&]() { cross_thread_status = handle.Send(message.view()); });
    other_thread.join();

    REQUIRE(cross_thread_status.code() == nimble::base::ErrorCode::kInvalidArgument);
    REQUIRE(sink->enqueued() == 2U);
  }
}