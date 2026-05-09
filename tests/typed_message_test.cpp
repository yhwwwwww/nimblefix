#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>

#include "fix44_api.h"
#include "nimblefix/advanced/typed_message_view.h"
#include "nimblefix/base/status.h"
#include "nimblefix/codec/fix_codec.h"

#include "test_support.h"

namespace {

using namespace nimble::generated::profile_4400;

auto
BuildGeneratedOrder() -> NewOrderSingleBuilder
{
  NewOrderSingleBuilder order;
  order.account("ACC-1")
    .cl_ord_id("ORD-001")
    .symbol("AAPL")
    .side(Side::Buy)
    .transact_time("20260406-12:00:00.000")
    .order_qty(100)
    .ord_type(OrdType::Limit);

  auto& party = order.add_party();
  party.party_id("PTY1").party_id_source(PartyIdSource::Proprietary).party_role(PartyRole::ExecutingFirm);
  party.add_party_sub_id().party_sub_id("SUB1").party_sub_id_type(PartySubIdType::Firm);

  return order;
}

auto
AssertGeneratedOrderView(const NewOrderSingleView& view) -> void
{
  CHECK(view.raw().msg_type() == "D");
  REQUIRE(view.cl_ord_id().has_value());
  CHECK(view.cl_ord_id().value() == "ORD-001");
  REQUIRE(view.account().has_value());
  CHECK(view.account().value() == "ACC-1");
  REQUIRE(view.symbol().has_value());
  CHECK(view.symbol().value() == "AAPL");
  REQUIRE(view.transact_time().has_value());
  CHECK(view.transact_time().value() == "20260406-12:00:00.000");
  REQUIRE(view.order_qty().has_value());
  CHECK(view.order_qty().value() == 100.0);

  auto side = view.side();
  REQUIRE(side.ok());
  CHECK(side.value() == Side::Buy);

  auto ord_type = view.ord_type();
  REQUIRE(ord_type.ok());
  CHECK(ord_type.value() == OrdType::Limit);

  auto parties = view.parties();
  REQUIRE(parties.has_value());
  REQUIRE(parties->size() == 1U);

  const auto party = (*parties)[0];
  REQUIRE(party.party_id().has_value());
  CHECK(party.party_id().value() == "PTY1");

  auto party_id_source = party.party_id_source();
  REQUIRE(party_id_source.ok());
  CHECK(party_id_source.value() == PartyIdSource::Proprietary);

  auto party_role = party.party_role();
  REQUIRE(party_role.ok());
  CHECK(party_role.value() == PartyRole::ExecutingFirm);

  auto sub_ids = party.party_sub_ids();
  REQUIRE(sub_ids.has_value());
  REQUIRE(sub_ids->size() == 1U);

  const auto sub_id = (*sub_ids)[0];
  REQUIRE(sub_id.party_sub_id().has_value());
  CHECK(sub_id.party_sub_id().value() == "SUB1");

  auto sub_id_type = sub_id.party_sub_id_type();
  REQUIRE(sub_id_type.ok());
  CHECK(sub_id_type.value() == PartySubIdType::Firm);
}

auto
ApplicationBodyFromFrame(std::string_view frame) -> std::string_view
{
  const auto msg_type_pos = frame.find("35=");
  REQUIRE(msg_type_pos != std::string_view::npos);
  const auto msg_type_end = frame.find('\x01', msg_type_pos);
  REQUIRE(msg_type_end != std::string_view::npos);

  constexpr std::string_view kSendingTimePrefix{ "\x01"
                                                 "52=",
                                                 4U };
  const auto sending_time_pos = frame.find(kSendingTimePrefix, msg_type_end);
  REQUIRE(sending_time_pos != std::string_view::npos);
  const auto sending_time_end = frame.find('\x01', sending_time_pos + 1U);
  REQUIRE(sending_time_end != std::string_view::npos);

  const auto checksum_pos = frame.rfind("10=");
  REQUIRE(checksum_pos != std::string_view::npos);
  REQUIRE(checksum_pos >= sending_time_end + 1U);
  return frame.substr(sending_time_end + 1U, checksum_pos - (sending_time_end + 1U));
}

} // namespace

TEST_CASE("generated typed view binds owned and parsed fix44 messages", "[typed-message]")
{
  auto dictionary_view = nimble::tests::LoadFix44DictionaryViewOrSkip();
  auto dictionary = nimble::base::Result<nimble::profile::NormalizedDictionaryView>(std::move(dictionary_view));

  auto order = BuildGeneratedOrder();

  auto owned_message = order.ToMessage();
  REQUIRE(owned_message.ok());
  auto owned_view = NewOrderSingleView::Bind(owned_message.value().view());
  REQUIRE(owned_view.ok());
  AssertGeneratedOrderView(owned_view.value());

  nimble::codec::EncodeOptions options;
  options.begin_string = "FIX.4.4";
  options.sender_comp_id = "BUY";
  options.target_comp_id = "SELL";
  options.msg_seq_num = 2U;
  options.sending_time = "20260406-12:00:01.000";

  nimble::codec::EncodeBuffer buffer;
  auto encode_status =
    nimble::codec::EncodeFixMessageToBuffer(owned_message.value(), dictionary.value(), options, &buffer);
  REQUIRE(encode_status.ok());

  auto decoded = nimble::codec::DecodeFixMessageView(buffer.bytes(), dictionary.value());
  REQUIRE(decoded.ok());
  auto parsed_view = NewOrderSingleView::Bind(decoded.value().message.view());
  REQUIRE(parsed_view.ok());
  AssertGeneratedOrderView(parsed_view.value());
}

TEST_CASE("generated EncodeBody matches ToMessage application body bytes", "[typed-message]")
{
  auto dictionary_view = nimble::tests::LoadFix44DictionaryViewOrSkip();
  auto dictionary = nimble::base::Result<nimble::profile::NormalizedDictionaryView>(std::move(dictionary_view));

  auto order = BuildGeneratedOrder();

  auto owned_message = order.ToMessage();
  REQUIRE(owned_message.ok());

  nimble::codec::EncodeOptions options;
  options.begin_string = "FIX.4.4";
  options.sender_comp_id = "BUY";
  options.target_comp_id = "SELL";
  options.msg_seq_num = 2U;
  options.sending_time = "20260406-12:00:01.000";

  nimble::codec::EncodeBuffer buffer;
  auto encode_status =
    nimble::codec::EncodeFixMessageToBuffer(owned_message.value(), dictionary.value(), options, &buffer);
  REQUIRE(encode_status.ok());

  nimble::generated::detail::BodyEncodeBuffer body_buffer;
  auto body_status = order.EncodeBody(body_buffer);
  REQUIRE(body_status.ok());

  const auto encoded_body = body_buffer.data();
  const auto materialized_body = ApplicationBodyFromFrame(buffer.text());
  CHECK(encoded_body == materialized_body);
}

TEST_CASE("generated set_tag appends raw extras consistently", "[typed-message]")
{
  auto dictionary_view = nimble::tests::LoadFix44DictionaryViewOrSkip();
  auto dictionary = nimble::base::Result<nimble::profile::NormalizedDictionaryView>(std::move(dictionary_view));

  auto order = BuildGeneratedOrder();
  order.set_tag<9001>(std::int32_t{ 7 });
  order.set_tag<9002>(42U);
  order.set_tag<9003>(true);
  order.set_tag<9004>('Z');
  order.set_tag<9005>(12.5);
  order.set_tag<9006>(std::string_view("RAW-EXTRA"));

  auto owned_message = order.ToMessage();
  REQUIRE(owned_message.ok());

  nimble::codec::EncodeOptions options;
  options.begin_string = "FIX.4.4";
  options.sender_comp_id = "BUY";
  options.target_comp_id = "SELL";
  options.msg_seq_num = 2U;
  options.sending_time = "20260406-12:00:01.000";

  nimble::codec::EncodeBuffer buffer;
  auto encode_status =
    nimble::codec::EncodeFixMessageToBuffer(owned_message.value(), dictionary.value(), options, &buffer);
  REQUIRE(encode_status.ok());

  nimble::generated::detail::BodyEncodeBuffer body_buffer;
  auto body_status = order.EncodeBody(body_buffer);
  REQUIRE(body_status.ok());

  const auto encoded_body = body_buffer.data();
  const auto materialized_body = ApplicationBodyFromFrame(buffer.text());
  CHECK(encoded_body == materialized_body);

  CHECK(encoded_body.find("9001=7\x01") != std::string_view::npos);
  CHECK(encoded_body.find("9002=42\x01") != std::string_view::npos);
  CHECK(encoded_body.find("9003=Y\x01") != std::string_view::npos);
  CHECK(encoded_body.find("9004=Z\x01") != std::string_view::npos);
  CHECK(encoded_body.find("9005=12.5\x01") != std::string_view::npos);
  CHECK(encoded_body.find("9006=RAW-EXTRA\x01") != std::string_view::npos);
}

TEST_CASE("generated typed view surfaces bind and enum parse errors", "[typed-message]")
{
  auto dictionary_view = nimble::tests::LoadFix44DictionaryViewOrSkip();
  auto dictionary = nimble::base::Result<nimble::profile::NormalizedDictionaryView>(std::move(dictionary_view));

  const auto wrong_frame = nimble::tests::EncodeFixFrame("35=0|49=BUY|56=SELL|");
  auto wrong_decoded = nimble::codec::DecodeFixMessageView(wrong_frame, dictionary.value());
  REQUIRE(wrong_decoded.ok());
  auto wrong_bind = NewOrderSingleView::Bind(wrong_decoded.value().message.view());
  REQUIRE(!wrong_bind.ok());
  CHECK(wrong_bind.status().code() == nimble::base::ErrorCode::kInvalidArgument);

  const auto frame =
    nimble::tests::EncodeFixFrame("35=D|49=BUY|56=SELL|11=ORD-001|55=AAPL|54=?|60=20260406-12:00:00.000|38=100|40=2|"
                                  "453=1|448=PTY1|447=?|452=1|802=1|523=SUB1|803=999|");
  auto decoded = nimble::codec::DecodeFixMessageView(frame, dictionary.value());
  REQUIRE(decoded.ok());
  auto bound = NewOrderSingleView::Bind(decoded.value().message.view());
  REQUIRE(bound.ok());

  auto side = bound.value().side();
  REQUIRE(!side.ok());
  CHECK(side.status().code() == nimble::base::ErrorCode::kFormatError);
  REQUIRE(bound.value().side_raw().has_value());
  CHECK(bound.value().side_raw().value() == '?');

  auto parties = bound.value().parties();
  REQUIRE(parties.has_value());
  REQUIRE(parties->size() == 1U);

  const auto party_view = (*parties)[0];
  auto party_id_source = party_view.party_id_source();
  REQUIRE(!party_id_source.ok());
  CHECK(party_id_source.status().code() == nimble::base::ErrorCode::kFormatError);
  REQUIRE(party_view.party_id_source_raw().has_value());
  CHECK(party_view.party_id_source_raw().value() == '?');

  auto sub_ids = party_view.party_sub_ids();
  REQUIRE(sub_ids.has_value());
  REQUIRE(sub_ids->size() == 1U);

  const auto sub_id_view = (*sub_ids)[0];
  auto sub_id_type = sub_id_view.party_sub_id_type();
  REQUIRE(!sub_id_type.ok());
  CHECK(sub_id_type.status().code() == nimble::base::ErrorCode::kFormatError);
  REQUIRE(sub_id_view.party_sub_id_type_raw().has_value());
  CHECK(sub_id_view.party_sub_id_type_raw().value() == 999);
}

TEST_CASE("TypedMessageView dictionary-required validation remains auxiliary", "[typed-message]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryViewOrSkip();

  const auto frame =
    nimble::tests::EncodeFixFrame("35=D|49=BUY|56=SELL|55=AAPL|54=1|60=20260406-12:00:00.000|38=100|40=1|");
  auto decoded = nimble::codec::DecodeFixMessageView(frame, dictionary);
  REQUIRE(decoded.ok());

  auto typed = nimble::message::TypedMessageView::Bind(dictionary, decoded.value().message.view());
  REQUIRE(typed.ok());

  std::uint32_t missing_tag = 0U;
  auto status = typed.value().validate_required_fields(&missing_tag);
  REQUIRE_FALSE(status.ok());
  CHECK(status.code() == nimble::base::ErrorCode::kInvalidArgument);
  CHECK(missing_tag == 11U);
}
