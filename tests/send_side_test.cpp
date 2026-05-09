#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "nimblefix/advanced/encoded_application_message.h"
#include "nimblefix/advanced/message_builder.h"
#include "nimblefix/base/result.h"
#include "nimblefix/codec/fix_codec.h"
#include "nimblefix/codec/fix_tags.h"
#include "nimblefix/generated/detail/api_support.h"
#include "nimblefix/generated/detail/message_shape.h"
#include "nimblefix/message/message_ref.h"
#include "nimblefix/runtime/detail/typed_runtime_application.h"
#include "nimblefix/runtime/profile_binding.h"
#include "nimblefix/runtime/session.h"
#include "nimblefix/session/admin_protocol.h"
#include "nimblefix/store/memory_store.h"
#include "sample_basic_api.h"

#include "test_support.h"

namespace {

namespace sample = nimble::generated::profile_1001;
namespace generated_detail = nimble::generated::detail;
using namespace nimble::codec::tags;

auto
SohField(std::string_view field) -> std::string
{
  std::string encoded(field);
  encoded.push_back('\x01');
  return encoded;
}

auto
BytesToString(std::span<const std::byte> bytes) -> std::string
{
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

auto
RequireFieldPosition(std::string_view text, std::string_view field) -> std::size_t
{
  const auto encoded = SohField(field);
  const auto position = text.find(encoded);
  REQUIRE(position != std::string_view::npos);
  return position;
}

auto
RequireFieldPrefixPosition(std::string_view text, std::string_view prefix) -> std::size_t
{
  auto position = text.find(prefix);
  while (position != std::string_view::npos) {
    const auto at_field_start = position == 0U || text[position - 1U] == '\x01';
    if (at_field_start) {
      const auto field_end = text.find('\x01', position);
      REQUIRE(field_end != std::string_view::npos);
      return position;
    }
    position = text.find(prefix, position + 1U);
  }
  REQUIRE(position != std::string_view::npos);
  return position;
}

auto
RequireNoField(std::string_view text, std::string_view field) -> void
{
  CHECK(text.find(SohField(field)) == std::string_view::npos);
}

auto
RequireFieldsInOrder(std::string_view text, std::initializer_list<std::string_view> fields) -> void
{
  auto previous = std::size_t{ 0U };
  auto first = true;
  for (const auto field : fields) {
    const auto position = RequireFieldPosition(text, field);
    if (!first) {
      CHECK(previous < position);
    }
    previous = position;
    first = false;
  }
}

auto
PopulateRequiredOrder(sample::NewOrderSingleBuilder& order,
                      std::string_view cl_ord_id = "ORD-001",
                      std::string_view symbol = "AAPL",
                      std::int64_t order_qty = 100,
                      std::string_view venue_order_type = "LIMIT") -> sample::NewOrderSingleBuilder&
{
  return order.msg_type(sample::NewOrderSingle::kMsgType)
    .cl_ord_id(cl_ord_id)
    .sender_comp_id("BUY")
    .target_comp_id("SELL")
    .symbol(symbol)
    .side('1')
    .transact_time("20260406-12:00:00.000")
    .order_qty(order_qty)
    .ord_type('2')
    .venue_order_type(venue_order_type);
}

auto
EncodeBody(const sample::NewOrderSingleBuilder& order) -> std::string
{
  generated_detail::BodyEncodeBuffer buffer;
  const auto status = order.EncodeBody(buffer);
  REQUIRE(status.ok());
  return std::string(buffer.data());
}

auto
MakeAcceptorProtocolConfig(std::uint64_t session_id, const nimble::profile::NormalizedDictionaryView& dictionary)
  -> nimble::session::AdminProtocolConfig
{
  return nimble::session::AdminProtocolConfig{
    .session =
      nimble::session::SessionConfig{
        .session_id = session_id,
        .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
        .profile_id = dictionary.profile().header().profile_id,
        .heartbeat_interval_seconds = 30U,
        .is_initiator = false,
      },
    .begin_string = "FIX.4.4",
    .sender_comp_id = "SELL",
    .target_comp_id = "BUY",
    .heartbeat_interval_seconds = 30U,
  };
}

auto
EncodeInboundFrame(const nimble::message::Message& message,
                   const nimble::profile::NormalizedDictionaryView& dictionary,
                   std::uint32_t seq_num) -> nimble::base::Result<std::vector<std::byte>>
{
  nimble::codec::EncodeOptions options;
  options.begin_string = "FIX.4.4";
  options.sender_comp_id = "BUY";
  options.target_comp_id = "SELL";
  options.msg_seq_num = seq_num;
  options.sending_time = "20260406-12:00:00.000";
  return nimble::codec::EncodeFixMessage(message, dictionary, options);
}

auto
ActivateAcceptorSession(nimble::session::AdminProtocol& protocol,
                        const nimble::profile::NormalizedDictionaryView& dictionary) -> nimble::base::Status
{
  nimble::message::MessageBuilder logon_builder("A");
  logon_builder.set_string(kMsgType, "A").set_int(kEncryptMethod, 0).set_int(kHeartBtInt, 30);

  auto inbound = EncodeInboundFrame(std::move(logon_builder).build(), dictionary, 1U);
  if (!inbound.ok()) {
    return inbound.status();
  }

  auto event = protocol.OnInbound(inbound.value(), 2U);
  if (!event.ok()) {
    return event.status();
  }
  if (!event.value().session_active || event.value().outbound_frames.empty()) {
    return nimble::base::Status::InvalidArgument("acceptor session did not activate");
  }
  return nimble::base::Status::Ok();
}

class RecordingEncodedSink final : public nimble::session::SessionCommandSink
{
public:
  auto EnqueueOwnedMessage(std::uint64_t session_id, nimble::message::MessageRef message)
    -> nimble::base::Status override
  {
    (void)session_id;
    (void)message;
    return nimble::base::Status::InvalidArgument("plain send is not expected");
  }

  auto EnqueueOwnedEncodedMessage(std::uint64_t session_id, nimble::session::EncodedApplicationMessageRef message)
    -> nimble::base::Status override
  {
    last_session_id_ = session_id;
    last_encoded_message_ = std::move(message);
    ++encoded_enqueued_;
    return nimble::base::Status::Ok();
  }

  auto LoadSnapshot(std::uint64_t session_id) const -> nimble::base::Result<nimble::session::SessionSnapshot> override
  {
    (void)session_id;
    return nimble::base::Status::InvalidArgument("snapshot unsupported in send-side test sink");
  }

  auto Subscribe(std::uint64_t session_id, std::size_t queue_capacity)
    -> nimble::base::Result<nimble::session::SessionSubscription> override
  {
    (void)session_id;
    (void)queue_capacity;
    return nimble::base::Status::InvalidArgument("subscribe unsupported in send-side test sink");
  }

  [[nodiscard]] auto encoded_enqueued() const -> std::uint64_t { return encoded_enqueued_; }
  [[nodiscard]] auto last_session_id() const -> std::uint64_t { return last_session_id_; }
  [[nodiscard]] auto last_encoded_view() const -> nimble::session::EncodedApplicationMessageView
  {
    return last_encoded_message_.view();
  }

private:
  std::uint64_t last_session_id_{ 0U };
  std::uint64_t encoded_enqueued_{ 0U };
  nimble::session::EncodedApplicationMessageRef last_encoded_message_{};
};

class RecordingShapeDispatchApp final : public sample::Handler
{
public:
  auto OnNewOrderSingle(nimble::runtime::InlineSession<sample::Profile>& session, sample::NewOrderSingleView view)
    -> nimble::base::Status override
  {
    (void)session;
    ++order_count;
    last_cl_ord_id = view.cl_ord_id().value_or(std::string_view{});
    return nimble::base::Status::Ok();
  }

  int order_count{ 0 };
  std::string last_cl_ord_id;
};

static constexpr generated_detail::BodyNode kUsageNewOrderSingleBody[] = {
  generated_detail::BodyNode{ generated_detail::BodyNode::Kind::kScalar,
                              sample::Tag::ClOrdID,
                              generated_detail::ScalarKind::kString,
                              generated_detail::Presence::kAlways,
                              0U,
                              nullptr,
                              0U },
  generated_detail::BodyNode{ generated_detail::BodyNode::Kind::kScalar,
                              sample::Tag::Symbol,
                              generated_detail::ScalarKind::kString,
                              generated_detail::Presence::kAlways,
                              0U,
                              nullptr,
                              0U },
  generated_detail::BodyNode{ generated_detail::BodyNode::Kind::kScalar,
                              sample::Tag::Side,
                              generated_detail::ScalarKind::kChar,
                              generated_detail::Presence::kAlways,
                              0U,
                              nullptr,
                              0U },
  generated_detail::BodyNode{ generated_detail::BodyNode::Kind::kScalar,
                              sample::Tag::TransactTime,
                              generated_detail::ScalarKind::kString,
                              generated_detail::Presence::kAlways,
                              0U,
                              nullptr,
                              0U },
  generated_detail::BodyNode{ generated_detail::BodyNode::Kind::kScalar,
                              sample::Tag::OrderQty,
                              generated_detail::ScalarKind::kInt,
                              generated_detail::Presence::kAlways,
                              0U,
                              nullptr,
                              0U },
  generated_detail::BodyNode{ generated_detail::BodyNode::Kind::kScalar,
                              sample::Tag::OrdType,
                              generated_detail::ScalarKind::kChar,
                              generated_detail::Presence::kAlways,
                              0U,
                              nullptr,
                              0U },
  generated_detail::BodyNode{ generated_detail::BodyNode::Kind::kScalar,
                              sample::Tag::VenueOrderType,
                              generated_detail::ScalarKind::kString,
                              generated_detail::Presence::kAlways,
                              0U,
                              nullptr,
                              0U },
};

static constexpr generated_detail::MessageShape kUsageNewOrderSingleShape{
  sample::Profile::kSchemaHash, sample::NewOrderSingle::kMsgType, kUsageNewOrderSingleBody, 7U, nullptr, 0U,
};

static constexpr const generated_detail::MessageShape* kUsageShapes[] = { &kUsageNewOrderSingleShape };

auto
MakeRuntimeEvent(nimble::message::MessageView view) -> nimble::runtime::RuntimeEvent
{
  return nimble::runtime::RuntimeEvent{
    .kind = nimble::runtime::RuntimeEventKind::kApplicationMessage,
    .handle = nimble::session::SessionHandle(9901U, 0U),
    .session_key = nimble::session::SessionKey{ "FIX.4.4", "BUY", "SELL" },
    .message = nimble::message::MessageRef::Borrow(view),
  };
}

} // namespace

TEST_CASE("MessageShape metadata correctness", "[send-side][message-shape]")
{
  const auto& shape = sample::NewOrderSingle::kShape;
  CHECK(shape.schema_hash == sample::Profile::kSchemaHash);
  CHECK(shape.msg_type == "D");
  REQUIRE(shape.body_data != nullptr);
  REQUIRE(shape.body_count == 10U);
  CHECK(shape.raw_extras_data == nullptr);
  CHECK(shape.raw_extras_count == 0U);

  const auto* body = shape.body_data;
  CHECK(body[0].kind == generated_detail::BodyNode::Kind::kScalar);
  CHECK(body[0].tag == sample::Tag::ClOrdID);
  CHECK(body[0].scalar_kind == generated_detail::ScalarKind::kString);
  CHECK(body[0].presence == generated_detail::Presence::kAlways);

  CHECK(body[1].tag == sample::Tag::Symbol);
  CHECK(body[1].scalar_kind == generated_detail::ScalarKind::kString);
  CHECK(body[1].presence == generated_detail::Presence::kAlways);

  CHECK(body[2].tag == sample::Tag::Side);
  CHECK(body[2].scalar_kind == generated_detail::ScalarKind::kChar);
  CHECK(body[2].presence == generated_detail::Presence::kAlways);

  CHECK(body[6].tag == sample::Tag::Price);
  CHECK(body[6].scalar_kind == generated_detail::ScalarKind::kFloat);
  CHECK(body[6].presence == generated_detail::Presence::kOptional);

  CHECK(body[7].kind == generated_detail::BodyNode::Kind::kGroup);
  CHECK(body[7].tag == sample::Tag::NoPartyIDs);
  CHECK(body[7].delimiter_tag == sample::Tag::PartyID);
  REQUIRE(body[7].entry_data != nullptr);
  CHECK(body[7].entry_count == 3U);

  const auto& heartbeat = sample::Heartbeat::kShape;
  CHECK(heartbeat.schema_hash == sample::Profile::kSchemaHash);
  CHECK(heartbeat.msg_type == "0");
  CHECK(heartbeat.body_data == nullptr);
  CHECK(heartbeat.body_count == 0U);
  CHECK(heartbeat.raw_extras_data == nullptr);
  CHECK(heartbeat.raw_extras_count == 0U);
}

TEST_CASE("Raw-static extras are appended after schema fields", "[send-side][raw-extras]")
{
  sample::NewOrderSingleBuilder order;
  PopulateRequiredOrder(order);
  order.price(12.5).venue_account("ACC-1");
  order.template set_tag<9001>(std::string_view("CUSTOM"));
  order.template set_tag<9002>(std::string_view("SECOND"));

  const auto body = EncodeBody(order);
  RequireFieldsInOrder(body,
                       {
                         "35=D",
                         "11=ORD-001",
                         "49=BUY",
                         "56=SELL",
                         "55=AAPL",
                         "54=1",
                         "60=20260406-12:00:00.000",
                         "38=100",
                         "40=2",
                         "44=12.5",
                         "5001=LIMIT",
                         "5002=ACC-1",
                         "9001=CUSTOM",
                         "9002=SECOND",
                       });
}

TEST_CASE("Hybrid ordering contract appends header and body fragments", "[send-side][hybrid-ordering]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }

  nimble::store::MemorySessionStore store;
  nimble::session::AdminProtocol protocol(
    MakeAcceptorProtocolConfig(8801U, dictionary.value()), dictionary.value(), &store);

  REQUIRE(protocol.OnTransportConnected(1U).ok());
  REQUIRE(ActivateAcceptorSession(protocol, dictionary.value()).ok());

  const auto encoded_body = nimble::tests::Bytes("11=ORD-HYB"
                                                 "\x01"
                                                 "55=AAPL"
                                                 "\x01");
  nimble::session::EncodedApplicationMessage encoded(
    "D", std::span<const std::byte>(encoded_body.data(), encoded_body.size()));
  encoded.extras = nimble::codec::EncodedOutboundExtras{
    .header_fragment = std::string("50=DESK"
                                   "\x01"),
    .body_fragment = std::string("9999=TAIL"
                                 "\x01"),
  };

  auto outbound = protocol.SendEncodedApplication(encoded, 100U);
  REQUIRE(outbound.ok());

  const auto frame = BytesToString(outbound.value().bytes.view());
  const auto sending_time = RequireFieldPrefixPosition(frame, "52=");
  const auto header_extra = RequireFieldPosition(frame, "50=DESK");
  const auto cl_ord_id = RequireFieldPosition(frame, "11=ORD-HYB");
  const auto symbol = RequireFieldPosition(frame, "55=AAPL");
  const auto body_extra = RequireFieldPosition(frame, "9999=TAIL");
  const auto checksum = RequireFieldPrefixPosition(frame, "10=");

  CHECK(sending_time < header_extra);
  CHECK(header_extra < cl_ord_id);
  CHECK(cl_ord_id < symbol);
  CHECK(symbol < body_extra);
  CHECK(body_extra < checksum);
  CHECK(checksum == frame.rfind("10="));
  REQUIRE(!frame.empty());
  CHECK(frame.back() == '\x01');
}

TEST_CASE("Schema-known set_tag is emitted as a raw extra", "[send-side][raw-extras]")
{
  sample::NewOrderSingleBuilder order;
  PopulateRequiredOrder(order, "ORD-SCHEMA", "AAPL");
  order.template set_tag<sample::Tag::Symbol>(std::string_view("RAW-SYMBOL"));

  const auto body = EncodeBody(order);
  const auto schema_symbol = RequireFieldPosition(body, "55=AAPL");
  const auto venue_order_type = RequireFieldPosition(body, "5001=LIMIT");
  const auto raw_symbol = RequireFieldPosition(body, "55=RAW-SYMBOL");

  CHECK(schema_symbol < venue_order_type);
  CHECK(venue_order_type < raw_symbol);
}

TEST_CASE("Repeating group encode emits count and entries in order", "[send-side][group-encode]")
{
  sample::NewOrderSingleBuilder order;
  PopulateRequiredOrder(order, "ORD-GRP");
  order.add_party().party_id("PTY1").party_id_source('D').party_role(1);
  order.add_party().party_id("PTY2").party_id_source('G').party_role(3);

  const auto body = EncodeBody(order);
  RequireFieldsInOrder(body,
                       {
                         "40=2",
                         "453=2",
                         "448=PTY1",
                         "447=D",
                         "452=1",
                         "448=PTY2",
                         "447=G",
                         "452=3",
                         "5001=LIMIT",
                       });
}

TEST_CASE("Builder clear resets schema fields groups and raw extras", "[send-side][builder-reuse]")
{
  sample::NewOrderSingleBuilder order;
  PopulateRequiredOrder(order, "ORD-FIRST", "AAPL", 100, "LIMIT");
  order.add_party().party_id("PTY-OLD").party_id_source('D').party_role(1);
  order.template set_tag<9001>(std::string_view("OLD-RAW"));

  const auto first = EncodeBody(order);
  RequireFieldPosition(first, "11=ORD-FIRST");
  RequireFieldPosition(first, "448=PTY-OLD");
  RequireFieldPosition(first, "9001=OLD-RAW");

  order.clear();
  PopulateRequiredOrder(order, "ORD-SECOND", "MSFT", 250, "MARKET");
  order.template set_tag<9002>(std::string_view("NEW-RAW"));

  const auto second = EncodeBody(order);
  RequireFieldPosition(second, "11=ORD-SECOND");
  RequireFieldPosition(second, "55=MSFT");
  RequireFieldPosition(second, "38=250");
  RequireFieldPosition(second, "5001=MARKET");
  RequireFieldPosition(second, "9002=NEW-RAW");
  RequireNoField(second, "11=ORD-FIRST");
  RequireNoField(second, "55=AAPL");
  RequireNoField(second, "448=PTY-OLD");
  RequireNoField(second, "453=1");
  RequireNoField(second, "9001=OLD-RAW");
}

TEST_CASE("send<Msg> integration encodes builder body into encoded application message",
          "[send-side][send-integration]")
{
  auto sink = std::make_shared<RecordingEncodedSink>();
  nimble::session::SessionHandle handle(8802U, 0U, sink);
  nimble::runtime::Session<sample::Profile> session(handle);

  auto lambda_called = false;
  const auto status = session.send<sample::NewOrderSingle>(
    [&](auto& order) {
      static_assert(std::is_same_v<std::remove_cvref_t<decltype(order)>, sample::NewOrderSingleBuilder>);
      lambda_called = true;
      PopulateRequiredOrder(order, "ORD-SEND", "IBM", 400, "PEG");
      order.template set_tag<9001>(std::string_view("RAW-SEND"));
    },
    { .header_fragment = "50=DESK-SEND"
                         "\x01",
      .body_fragment = "9999=TAIL-SEND"
                       "\x01" });

  REQUIRE(status.ok());
  CHECK(lambda_called);
  CHECK(sink->encoded_enqueued() == 1U);
  CHECK(sink->last_session_id() == 8802U);

  const auto encoded = sink->last_encoded_view();
  CHECK(encoded.msg_type == sample::NewOrderSingle::kMsgType);
  CHECK(encoded.extras.header_fragment == "50=DESK-SEND"
                                          "\x01");
  CHECK(encoded.extras.body_fragment == "9999=TAIL-SEND"
                                        "\x01");

  const auto body = BytesToString(encoded.body);
  RequireFieldsInOrder(body,
                       {
                         "11=ORD-SEND",
                         "55=IBM",
                         "38=400",
                         "5001=PEG",
                         "9001=RAW-SEND",
                       });
}

TEST_CASE("MessageShape body nodes preserve canonical schema ordering", "[send-side][message-shape]")
{
  const auto& shape = sample::NewOrderSingle::kShape;
  REQUIRE(shape.body_data != nullptr);
  REQUIRE(shape.body_count == 10U);

  constexpr std::array<std::uint32_t, 10U> kExpectedBodyTags{
    sample::Tag::ClOrdID,        sample::Tag::Symbol,       sample::Tag::Side,  sample::Tag::TransactTime,
    sample::Tag::OrderQty,       sample::Tag::OrdType,      sample::Tag::Price, sample::Tag::NoPartyIDs,
    sample::Tag::VenueOrderType, sample::Tag::VenueAccount,
  };

  for (std::size_t index = 0U; index < kExpectedBodyTags.size(); ++index) {
    CHECK(shape.body_data[index].tag == kExpectedBodyTags[index]);
  }

  const auto& parties = shape.body_data[7];
  REQUIRE(parties.kind == generated_detail::BodyNode::Kind::kGroup);
  CHECK(parties.tag == sample::Tag::NoPartyIDs);
  CHECK(parties.delimiter_tag == sample::Tag::PartyID);
  REQUIRE(parties.entry_data != nullptr);
  REQUIRE(parties.entry_count == 3U);

  CHECK(parties.entry_data[0].kind == generated_detail::BodyNode::Kind::kScalar);
  CHECK(parties.entry_data[0].tag == sample::Tag::PartyID);
  CHECK(parties.entry_data[0].scalar_kind == generated_detail::ScalarKind::kString);
  CHECK(parties.entry_data[0].presence == generated_detail::Presence::kAlways);

  CHECK(parties.entry_data[1].tag == sample::Tag::PartyIDSource);
  CHECK(parties.entry_data[1].scalar_kind == generated_detail::ScalarKind::kChar);
  CHECK(parties.entry_data[1].presence == generated_detail::Presence::kAlways);

  CHECK(parties.entry_data[2].tag == sample::Tag::PartyRole);
  CHECK(parties.entry_data[2].scalar_kind == generated_detail::ScalarKind::kInt);
  CHECK(parties.entry_data[2].presence == generated_detail::Presence::kAlways);
}

TEST_CASE("Shape-aware generated dispatch gates handlers on active message shape",
          "[send-side][message-shape][generate-runtime]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryViewOrSkip();
  nimble::runtime::ProfileBinding<sample::Profile> binding(dictionary, kUsageShapes, 1U);
  auto app = std::make_shared<RecordingShapeDispatchApp>();
  nimble::runtime::detail::TypedRuntimeApplication<sample::Profile, sample::Handler> runtime_app(&binding, app);

  auto accepted_message = sample::NewOrderSingleBuilder{}
                            .msg_type(sample::NewOrderSingle::kMsgType)
                            .cl_ord_id("ORD-SHAPE")
                            .sender_comp_id("BUY")
                            .target_comp_id("SELL")
                            .symbol("AAPL")
                            .side('1')
                            .transact_time("20260406-12:00:00.000")
                            .order_qty(100)
                            .ord_type('2')
                            .venue_order_type("LIMIT")
                            .ToMessage();
  REQUIRE(accepted_message.ok());

  const auto accepted = runtime_app.OnAppMessage(MakeRuntimeEvent(accepted_message.value().view()));
  REQUIRE(accepted.ok());
  CHECK(app->order_count == 1);
  CHECK(app->last_cl_ord_id == "ORD-SHAPE");

  nimble::message::MessageBuilder missing_usage_builder{ std::string(sample::NewOrderSingle::kMsgType) };
  missing_usage_builder.set_string(kMsgType, sample::NewOrderSingle::kMsgType)
    .set_string(sample::Tag::ClOrdID, "ORD-MISSING")
    .set_string(sample::Tag::SenderCompID, "BUY")
    .set_string(sample::Tag::TargetCompID, "SELL")
    .set_string(sample::Tag::Symbol, "AAPL")
    .set_char(sample::Tag::Side, '1')
    .set_string(sample::Tag::TransactTime, "20260406-12:00:00.000")
    .set_int(sample::Tag::OrderQty, 100)
    .set_char(sample::Tag::OrdType, '2');
  const auto missing_usage_message = std::move(missing_usage_builder).build();

  const auto missing_usage = runtime_app.OnAppMessage(MakeRuntimeEvent(missing_usage_message.view()));
  REQUIRE_FALSE(missing_usage.ok());
  CHECK(missing_usage.code() == nimble::base::ErrorCode::kInvalidArgument);
  CHECK(app->order_count == 1);

  nimble::message::MessageBuilder missing_dictionary_builder{ std::string(sample::NewOrderSingle::kMsgType) };
  missing_dictionary_builder.set_string(kMsgType, sample::NewOrderSingle::kMsgType)
    .set_string(sample::Tag::ClOrdID, "ORD-USAGE")
    .set_string(sample::Tag::Symbol, "AAPL")
    .set_char(sample::Tag::Side, '1')
    .set_string(sample::Tag::TransactTime, "20260406-12:00:00.000")
    .set_int(sample::Tag::OrderQty, 100)
    .set_char(sample::Tag::OrdType, '2')
    .set_string(sample::Tag::VenueOrderType, "LIMIT");
  const auto missing_dictionary_message = std::move(missing_dictionary_builder).build();

  const auto missing_dictionary = runtime_app.OnAppMessage(MakeRuntimeEvent(missing_dictionary_message.view()));
  REQUIRE(missing_dictionary.ok());
  CHECK(app->order_count == 2);
  CHECK(app->last_cl_ord_id == "ORD-USAGE");
}

TEST_CASE("ValidateInboundMessageShape rejects zero-count required groups", "[send-side][message-shape]")
{
  static constexpr generated_detail::BodyNode kPartyEntryNodes[] = {
    generated_detail::BodyNode{ generated_detail::BodyNode::Kind::kScalar,
                                sample::Tag::PartyID,
                                generated_detail::ScalarKind::kString,
                                generated_detail::Presence::kAlways,
                                0U,
                                nullptr,
                                0U },
  };
  static constexpr generated_detail::BodyNode kBodyNodes[] = {
    generated_detail::BodyNode{ generated_detail::BodyNode::Kind::kGroup,
                                sample::Tag::NoPartyIDs,
                                generated_detail::ScalarKind::kString,
                                generated_detail::Presence::kAlways,
                                sample::Tag::PartyID,
                                kPartyEntryNodes,
                                1U },
  };
  static constexpr generated_detail::MessageShape kRequiredGroupShape{
    sample::Profile::kSchemaHash, sample::NewOrderSingle::kMsgType, kBodyNodes, 1U, nullptr, 0U,
  };

  struct NoEnumValidators
  {
    static auto Validate(const generated_detail::BodyNode& node, nimble::message::MessageView view)
      -> nimble::base::Status
    {
      (void)node;
      (void)view;
      return nimble::base::Status::Ok();
    }
  };

  nimble::message::MessageBuilder builder{ std::string(sample::NewOrderSingle::kMsgType) };
  builder.set_string(kMsgType, sample::NewOrderSingle::kMsgType).reserve_group_entries(sample::Tag::NoPartyIDs, 0U);

  const auto message = std::move(builder).build();
  const auto status =
    generated_detail::ValidateInboundMessageShape<NoEnumValidators>(message.view(), kRequiredGroupShape);
  REQUIRE_FALSE(status.ok());
  CHECK(status.code() == nimble::base::ErrorCode::kInvalidArgument);
}
