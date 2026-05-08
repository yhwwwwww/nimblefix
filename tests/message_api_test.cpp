#include <catch2/catch_test_macros.hpp>

#include <string>

#include "fix44_api.h"
#include "nimblefix/codec/fix_codec.h"
#include "nimblefix/codec/fix_tags.h"

#include "test_support.h"

using namespace nimble::codec::tags;

// ---------------------------------------------------------------------------
// Generated typed API tests
// ---------------------------------------------------------------------------

using namespace nimble::generated::profile_4400;

namespace {

auto
PopulateGeneratedOrder(NewOrderSingleBuilder* order, std::string_view account, std::string_view cl_ord_id) -> void
{
  REQUIRE(order != nullptr);
  order->clear();
  order->account(account)
    .cl_ord_id(cl_ord_id)
    .symbol("AAPL")
    .side(Side::Buy)
    .transact_time("20260406-12:00:00.000")
    .order_qty(100)
    .ord_type(OrdType::Limit);
}

auto
AddGeneratedPartyWithNestedSubId(NewOrderSingleBuilder* order) -> void
{
  REQUIRE(order != nullptr);
  auto& party = order->add_party();
  party.party_id("PTY1").party_id_source(PartyIdSource::Proprietary).party_role(PartyRole::ExecutingFirm);
  party.add_party_sub_id().party_sub_id("SUB1").party_sub_id_type(PartySubIdType::Firm);
}

auto
AssertGeneratedOrderView(const NewOrderSingleView& view) -> void
{
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
EncodeGeneratedOrder(const NewOrderSingleBuilder& order,
                     const nimble::profile::NormalizedDictionaryView& dictionary,
                     const nimble::codec::EncodeOptions& options,
                     nimble::codec::EncodeBuffer* buffer) -> nimble::base::Status
{
  if (buffer == nullptr) {
    return nimble::base::Status::InvalidArgument("buffer is null");
  }
  auto message = order.ToMessage();
  if (!message.ok()) {
    return message.status();
  }
  return nimble::codec::EncodeFixMessageToBuffer(message.value(), dictionary, options, buffer);
}

} // namespace

TEST_CASE("generated-api-scalar-fields", "[message-api][generated-api]")
{
  auto dictionary_view = nimble::tests::LoadFix44DictionaryViewOrSkip();
  auto dictionary = nimble::base::Result<nimble::profile::NormalizedDictionaryView>(std::move(dictionary_view));

  NewOrderSingleBuilder order;
  PopulateGeneratedOrder(&order, "ACC-1", "ORD-001");

  nimble::codec::EncodeOptions options;
  options.begin_string = "FIX.4.4";
  options.sender_comp_id = "BUY";
  options.target_comp_id = "SELL";
  options.msg_seq_num = 1U;
  options.sending_time = "20260406-12:00:00.000";

  nimble::codec::EncodeBuffer buf;
  REQUIRE(EncodeGeneratedOrder(order, dictionary.value(), options, &buf).ok());

  auto decoded = nimble::codec::DecodeFixMessageView(buf.bytes(), dictionary.value());
  REQUIRE(decoded.ok());
  auto view = decoded.value().message.view();

  CHECK(view.msg_type() == "D");
  REQUIRE(view.get_string(Tag::Account).has_value());
  CHECK(view.get_string(Tag::Account).value() == "ACC-1");
  REQUIRE(view.get_string(Tag::ClOrdID).has_value());
  CHECK(view.get_string(Tag::ClOrdID).value() == "ORD-001");
}

TEST_CASE("generated-api-with-groups", "[message-api][generated-api]")
{
  auto dictionary_view = nimble::tests::LoadFix44DictionaryViewOrSkip();
  auto dictionary = nimble::base::Result<nimble::profile::NormalizedDictionaryView>(std::move(dictionary_view));

  NewOrderSingleBuilder order;
  PopulateGeneratedOrder(&order, "ACC-1", "ORD-001");
  order.add_party()
    .party_id("PTY1")
    .party_id_source(PartyIdSource::Proprietary)
    .party_role(PartyRole::ExecutingFirm);
  order.add_party().party_id("PTY2").party_id_source(PartyIdSource::Mic).party_role(PartyRole::ClearingFirm);

  nimble::codec::EncodeOptions options;
  options.begin_string = "FIX.4.4";
  options.sender_comp_id = "BUY";
  options.target_comp_id = "SELL";
  options.msg_seq_num = 2U;
  options.sending_time = "20260406-12:00:00.000";

  nimble::codec::EncodeBuffer buf;
  REQUIRE(EncodeGeneratedOrder(order, dictionary.value(), options, &buf).ok());

  auto decoded = nimble::codec::DecodeFixMessageView(buf.bytes(), dictionary.value());
  REQUIRE(decoded.ok());
  auto view = decoded.value().message.view();

  auto group = view.group(Tag::NoPartyIDs);
  REQUIRE(group.has_value());
  REQUIRE(group->size() == 2U);
  CHECK((*group)[0].get_string(Tag::PartyID).value() == "PTY1");
  CHECK((*group)[0].get_char(Tag::PartyIDSource).value() == 'D');
  CHECK((*group)[0].get_int(Tag::PartyRole).value() == 1);
  CHECK((*group)[1].get_string(Tag::PartyID).value() == "PTY2");
  CHECK((*group)[1].get_char(Tag::PartyIDSource).value() == 'G');
  CHECK((*group)[1].get_int(Tag::PartyRole).value() == 4);
}

TEST_CASE("generated-api-clear-and-reuse", "[message-api][generated-api]")
{
  auto dictionary_view = nimble::tests::LoadFix44DictionaryViewOrSkip();
  auto dictionary = nimble::base::Result<nimble::profile::NormalizedDictionaryView>(std::move(dictionary_view));

  NewOrderSingleBuilder order;

  nimble::codec::EncodeOptions options;
  options.begin_string = "FIX.4.4";
  options.sender_comp_id = "BUY";
  options.target_comp_id = "SELL";
  options.sending_time = "20260406-12:00:00.000";

  // First encode.
  PopulateGeneratedOrder(&order, "ACC-1", "ORD-001");
  options.msg_seq_num = 1U;
  nimble::codec::EncodeBuffer buf1;
  REQUIRE(EncodeGeneratedOrder(order, dictionary.value(), options, &buf1).ok());

  // Clear and reuse.
  PopulateGeneratedOrder(&order, "ACC-2", "ORD-002");
  options.msg_seq_num = 2U;
  nimble::codec::EncodeBuffer buf2;
  REQUIRE(EncodeGeneratedOrder(order, dictionary.value(), options, &buf2).ok());

  auto decoded = nimble::codec::DecodeFixMessageView(buf2.bytes(), dictionary.value());
  REQUIRE(decoded.ok());
  auto view = decoded.value().message.view();

  CHECK(view.msg_type() == "D");
  REQUIRE(view.get_string(Tag::Account).has_value());
  CHECK(view.get_string(Tag::Account).value() == "ACC-2");
  REQUIRE(view.get_string(Tag::ClOrdID).has_value());
  CHECK(view.get_string(Tag::ClOrdID).value() == "ORD-002");

  // First-encode fields should NOT leak into second encode.
  const auto wire = buf2.text();
  CHECK(wire.find("1=ACC-2") != std::string_view::npos);
  CHECK(wire.find("1=ACC-1") == std::string_view::npos);
}

TEST_CASE("generated-inbound-view-supports-owned-and-parsed-messages", "[message-api][generated-api]")
{
  auto dictionary_view = nimble::tests::LoadFix44DictionaryViewOrSkip();
  auto dictionary = nimble::base::Result<nimble::profile::NormalizedDictionaryView>(std::move(dictionary_view));

  NewOrderSingleBuilder order;
  PopulateGeneratedOrder(&order, "ACC-1", "ORD-001");
  AddGeneratedPartyWithNestedSubId(&order);

  auto owned_message = order.ToMessage();
  REQUIRE(owned_message.ok());
  auto owned_view = NewOrderSingleView::Bind(owned_message.value().view());
  REQUIRE(owned_view.ok());
  AssertGeneratedOrderView(owned_view.value());

  nimble::codec::EncodeOptions options;
  options.begin_string = "FIX.4.4";
  options.sender_comp_id = "BUY";
  options.target_comp_id = "SELL";
  options.msg_seq_num = 7U;
  options.sending_time = "20260406-12:00:00.000";

  nimble::codec::EncodeBuffer buf;
  REQUIRE(EncodeGeneratedOrder(order, dictionary.value(), options, &buf).ok());

  auto decoded = nimble::codec::DecodeFixMessageView(buf.bytes(), dictionary.value());
  REQUIRE(decoded.ok());
  auto parsed_view = NewOrderSingleView::Bind(decoded.value().message.view());
  REQUIRE(parsed_view.ok());
  AssertGeneratedOrderView(parsed_view.value());
}

TEST_CASE("generated-inbound-view-reports-unknown-enum-values", "[message-api][generated-api]")
{
  auto dictionary_view = nimble::tests::LoadFix44DictionaryViewOrSkip();
  auto dictionary = nimble::base::Result<nimble::profile::NormalizedDictionaryView>(std::move(dictionary_view));

  const auto frame = nimble::tests::EncodeFixFrame(
    "35=D|49=BUY|56=SELL|11=ORD-001|55=AAPL|54=?|60=20260406-12:00:00.000|38=100|40=2|");
  auto decoded = nimble::codec::DecodeFixMessageView(frame, dictionary.value());
  REQUIRE(decoded.ok());
  auto view = NewOrderSingleView::Bind(decoded.value().message.view());
  REQUIRE(view.ok());

  auto side = view.value().side();
  REQUIRE(!side.ok());
  CHECK(side.status().code() == nimble::base::ErrorCode::kFormatError);
  REQUIRE(view.value().side_raw().has_value());
  CHECK(view.value().side_raw().value() == '?');
}
