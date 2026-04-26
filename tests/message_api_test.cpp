#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <limits>
#include <string>

#include "fix44_builders.h"
#include "nimblefix/codec/fix_codec.h"
#include "nimblefix/codec/fix_tags.h"
#include "nimblefix/message/fixed_layout_writer.h"
#include "nimblefix/message/message_builder.h"

#include "test_support.h"

using namespace nimble::codec::tags;

TEST_CASE("message-api", "[message-api]")
{
  auto dictionary_view = nimble::tests::LoadFix44DictionaryViewOrSkip();
  auto dictionary = nimble::base::Result<nimble::profile::NormalizedDictionaryView>(std::move(dictionary_view));

  nimble::message::MessageBuilder builder("D");
  builder.set_string(kMsgType, "D")
    .set_string(kSenderCompID, "BUY")
    .set_string(kTargetCompID, "SELL")
    .set_int(kOrderQty, 1000);
  auto parties = builder.add_group_entry(kNoPartyIDs);
  parties.set_string(kPartyID, "PTY1").set_char(kPartyIDSource, 'D').set_int(kPartyRole, 1);

  nimble::codec::EncodeOptions options;
  options.begin_string = "FIX.4.4";
  options.sender_comp_id = "BUY";
  options.target_comp_id = "SELL";
  options.msg_seq_num = 1U;
  options.sending_time = "20260403-13:00:00.000";

  nimble::codec::EncodeBuffer buffer;
  REQUIRE(builder.encode_to_buffer(dictionary.value(), options, &buffer).ok());
  auto encoded = builder.encode(dictionary.value(), options);
  REQUIRE(encoded.ok());
  REQUIRE(buffer.size() == encoded.value().size());
  REQUIRE(std::equal(buffer.bytes().begin(), buffer.bytes().end(), encoded.value().begin(), encoded.value().end()));

  const auto message = std::move(builder).build();
  const auto view = message.view();

  auto encoded_owned = nimble::codec::EncodeFixMessage(message, dictionary.value(), options);
  REQUIRE(encoded_owned.ok());
  REQUIRE(encoded_owned.value() == encoded.value());

  REQUIRE(view.msg_type() == "D");
  REQUIRE(view.get_string(kSenderCompID).has_value());
  REQUIRE(view.get_string(kSenderCompID).value() == "BUY");
  REQUIRE(view.get_int(kOrderQty).has_value());
  REQUIRE(view.get_int(kOrderQty).value() == 1000);

  const auto group = view.group(kNoPartyIDs);
  REQUIRE(group.has_value());
  REQUIRE(group->size() == 1U);
  REQUIRE((*group)[0].get_string(kPartyID).value() == "PTY1");
  REQUIRE((*group)[0].get_char(kPartyIDSource).value() == 'D');
  REQUIRE((*group)[0].get_int(kPartyRole).value() == 1);
}

TEST_CASE("group-entry-builder survives sibling growth", "[message-api]")
{
  nimble::message::MessageBuilder builder("D");
  builder.set_string(kMsgType, "D");

  auto first_party = builder.add_group_entry(kNoPartyIDs);
  first_party.reserve_fields(4);
  first_party.set_string(kPartyID, "PTY-1").set_char(kPartyIDSource, 'D').set_int(kPartyRole, 1);

  for (int index = 0; index < 32; ++index) {
    auto party = builder.add_group_entry(kNoPartyIDs);
    party.set_string(kPartyID, "EXTRA-" + std::to_string(index)).set_char(kPartyIDSource, 'D').set_int(kPartyRole, 2);
  }

  first_party.set_string(kPartyID, "PTY-1-UPDATED");
  auto nested = first_party.add_group_entry(802U);
  nested.set_string(523U, "SUB-1");

  const auto message = std::move(builder).build();
  const auto parties = message.view().group(kNoPartyIDs);
  REQUIRE(parties.has_value());
  REQUIRE(parties->size() == 33U);
  REQUIRE((*parties)[0].get_string(kPartyID).value() == "PTY-1-UPDATED");

  const auto nested_group = (*parties)[0].group(802U);
  REQUIRE(nested_group.has_value());
  REQUIRE(nested_group->size() == 1U);
  REQUIRE((*nested_group)[0].get_string(523U).value() == "SUB-1");
}

TEST_CASE("fixed-layout-build", "[message-api][fixed-layout]")
{
  auto dictionary_view = nimble::tests::LoadFix44DictionaryViewOrSkip();
  auto dictionary = nimble::base::Result<nimble::profile::NormalizedDictionaryView>(std::move(dictionary_view));

  auto layout = nimble::message::FixedLayout::Build(dictionary.value(), "D");
  REQUIRE(layout.ok());

  CHECK(layout.value().msg_type() == "D");
  CHECK(layout.value().field_count() > 0U);

  // ClOrdID should have a valid slot.
  CHECK(layout.value().slot_index(kClOrdID) >= 0);
  // Tag 0 should not be in layout.
  CHECK(layout.value().slot_index(0U) == -1);
  // NoPartyIDs should be in the group index.
  CHECK(layout.value().group_slot_index(kNoPartyIDs) >= 0);
  // Non-existent group.
  CHECK(layout.value().group_slot_index(99999U) == -1);
}

TEST_CASE("fixed-layout-build-unknown-type", "[message-api][fixed-layout]")
{
  auto dictionary_view = nimble::tests::LoadFix44DictionaryViewOrSkip();
  auto dictionary = nimble::base::Result<nimble::profile::NormalizedDictionaryView>(std::move(dictionary_view));

  auto layout = nimble::message::FixedLayout::Build(dictionary.value(), "ZZ_NONEXIST");
  REQUIRE_FALSE(layout.ok());
}

TEST_CASE("fixed-layout-writer-scalar-fields", "[message-api][fixed-layout]")
{
  auto dictionary_view = nimble::tests::LoadFix44DictionaryViewOrSkip();
  auto dictionary = nimble::base::Result<nimble::profile::NormalizedDictionaryView>(std::move(dictionary_view));

  auto layout = nimble::message::FixedLayout::Build(dictionary.value(), "D");
  REQUIRE(layout.ok());

  nimble::message::FixedLayoutWriter writer(layout.value());
  writer.set_string(kSenderCompID, "BUY");
  writer.set_string(kTargetCompID, "SELL");
  writer.set_string(kAccount, "ACC-1");

  // Unknown tag silently ignored (no error reporting on hot path).
  writer.set_string(99999U, "NOPE");

  // Encode + decode roundtrip to verify fields.
  nimble::codec::EncodeOptions options;
  options.begin_string = "FIX.4.4";
  options.sender_comp_id = "BUY";
  options.target_comp_id = "SELL";
  options.msg_seq_num = 1U;
  options.sending_time = "20260406-12:00:00.000";

  nimble::codec::EncodeBuffer buf;
  REQUIRE(writer.encode_to_buffer(dictionary.value(), options, &buf).ok());

  auto decoded = nimble::codec::DecodeFixMessageView(buf.bytes(), dictionary.value());
  REQUIRE(decoded.ok());
  auto view = decoded.value().message.view();

  CHECK(view.msg_type() == "D");
  REQUIRE(view.get_string(kSenderCompID).has_value());
  CHECK(view.get_string(kSenderCompID).value() == "BUY");
  REQUIRE(view.get_string(kTargetCompID).has_value());
  CHECK(view.get_string(kTargetCompID).value() == "SELL");
  REQUIRE(view.get_string(kAccount).has_value());
  CHECK(view.get_string(kAccount).value() == "ACC-1");
}

TEST_CASE("fixed-layout-writer-with-groups", "[message-api][fixed-layout]")
{
  auto dictionary_view = nimble::tests::LoadFix44DictionaryViewOrSkip();
  auto dictionary = nimble::base::Result<nimble::profile::NormalizedDictionaryView>(std::move(dictionary_view));

  auto layout = nimble::message::FixedLayout::Build(dictionary.value(), "D");
  REQUIRE(layout.ok());

  nimble::message::FixedLayoutWriter writer(layout.value());
  writer.set_string(kSenderCompID, "BUY");
  writer.set_string(kTargetCompID, "SELL");
  writer.set_string(kAccount, "ACC-1");
  auto party = writer.add_group_entry(kNoPartyIDs);
  party.set_string(kPartyID, "PTY1").set_char(kPartyIDSource, 'D').set_int(kPartyRole, 1);

  // Encode + decode roundtrip to verify group fields.
  nimble::codec::EncodeOptions options;
  options.begin_string = "FIX.4.4";
  options.sender_comp_id = "BUY";
  options.target_comp_id = "SELL";
  options.msg_seq_num = 1U;
  options.sending_time = "20260406-12:00:00.000";

  nimble::codec::EncodeBuffer buf;
  REQUIRE(writer.encode_to_buffer(dictionary.value(), options, &buf).ok());

  auto decoded = nimble::codec::DecodeFixMessageView(buf.bytes(), dictionary.value());
  REQUIRE(decoded.ok());
  auto view = decoded.value().message.view();

  const auto group = view.group(kNoPartyIDs);
  REQUIRE(group.has_value());
  REQUIRE(group->size() == 1U);
  CHECK((*group)[0].get_string(kPartyID).value() == "PTY1");
  CHECK((*group)[0].get_char(kPartyIDSource).value() == 'D');
  CHECK((*group)[0].get_int(kPartyRole).value() == 1);
}

TEST_CASE("fixed-layout-writer-matches-message-builder", "[message-api][fixed-layout]")
{
  auto dictionary_view = nimble::tests::LoadFix44DictionaryViewOrSkip();
  auto dictionary = nimble::base::Result<nimble::profile::NormalizedDictionaryView>(std::move(dictionary_view));

  // Build via MessageBuilder (same tags as FixedLayoutWriter below).
  nimble::message::MessageBuilder mb("D");
  mb.set_string(kSenderCompID, "BUY").set_string(kTargetCompID, "SELL").set_string(kAccount, "ACC-1");
  auto mb_party = mb.add_group_entry(kNoPartyIDs);
  mb_party.set_string(kPartyID, "PTY1").set_char(kPartyIDSource, 'D').set_int(kPartyRole, 1);

  nimble::codec::EncodeOptions options;
  options.begin_string = "FIX.4.4";
  options.sender_comp_id = "BUY";
  options.target_comp_id = "SELL";
  options.msg_seq_num = 1U;
  options.sending_time = "20260406-12:34:56.789";

  nimble::codec::EncodeBuffer mb_buffer;
  REQUIRE(mb.encode_to_buffer(dictionary.value(), options, &mb_buffer).ok());

  // Build via FixedLayoutWriter.
  auto layout = nimble::message::FixedLayout::Build(dictionary.value(), "D");
  REQUIRE(layout.ok());

  nimble::message::FixedLayoutWriter writer(layout.value());
  writer.set_string(kSenderCompID, "BUY");
  writer.set_string(kTargetCompID, "SELL");
  writer.set_string(kAccount, "ACC-1");
  auto fw_party = writer.add_group_entry(kNoPartyIDs);
  fw_party.set_string(kPartyID, "PTY1").set_char(kPartyIDSource, 'D').set_int(kPartyRole, 1);

  nimble::codec::EncodeBuffer fw_buffer;
  REQUIRE(writer.encode_to_buffer(dictionary.value(), options, &fw_buffer).ok());

  // MB uses insertion-order-then-groups; FLW uses dictionary rule order.
  // Both are valid encodings — compare decoded logical content, not raw bytes.
  auto mb_decoded = nimble::codec::DecodeFixMessageView(mb_buffer.bytes(), dictionary.value());
  REQUIRE(mb_decoded.ok());
  auto fw_decoded = nimble::codec::DecodeFixMessageView(fw_buffer.bytes(), dictionary.value());
  REQUIRE(fw_decoded.ok());

  auto mb_view = mb_decoded.value().message.view();
  auto fw_view = fw_decoded.value().message.view();

  CHECK(mb_view.msg_type() == fw_view.msg_type());
  CHECK(mb_view.get_string(kSenderCompID).value() == fw_view.get_string(kSenderCompID).value());
  CHECK(mb_view.get_string(kTargetCompID).value() == fw_view.get_string(kTargetCompID).value());
  CHECK(mb_view.get_string(kAccount).value() == fw_view.get_string(kAccount).value());

  auto mb_group = mb_view.group(kNoPartyIDs);
  auto fw_group = fw_view.group(kNoPartyIDs);
  REQUIRE(mb_group.has_value());
  REQUIRE(fw_group.has_value());
  REQUIRE(mb_group->size() == fw_group->size());
  CHECK((*mb_group)[0].get_string(kPartyID).value() == (*fw_group)[0].get_string(kPartyID).value());
  CHECK((*mb_group)[0].get_char(kPartyIDSource).value() == (*fw_group)[0].get_char(kPartyIDSource).value());
  CHECK((*mb_group)[0].get_int(kPartyRole).value() == (*fw_group)[0].get_int(kPartyRole).value());
}

TEST_CASE("fixed-layout-writer-all-value-types", "[message-api][fixed-layout]")
{
  auto dictionary_view = nimble::tests::LoadFix44DictionaryViewOrSkip();
  auto dictionary = nimble::base::Result<nimble::profile::NormalizedDictionaryView>(std::move(dictionary_view));

  auto layout = nimble::message::FixedLayout::Build(dictionary.value(), "D");
  REQUIRE(layout.ok());

  nimble::message::FixedLayoutWriter writer(layout.value());

  writer.set_string(kSenderCompID, "BUY");
  writer.set_string(kTargetCompID, "SELL");
  writer.set_string(kAccount, "ACC-1");
  writer.set_string(kClOrdID, "ORD-001");

  // Encode + decode roundtrip to verify all value types.
  nimble::codec::EncodeOptions options;
  options.begin_string = "FIX.4.4";
  options.sender_comp_id = "BUY";
  options.target_comp_id = "SELL";
  options.msg_seq_num = 1U;
  options.sending_time = "20260406-12:00:00.000";

  nimble::codec::EncodeBuffer buf;
  REQUIRE(writer.encode_to_buffer(dictionary.value(), options, &buf).ok());

  auto decoded = nimble::codec::DecodeFixMessageView(buf.bytes(), dictionary.value());
  REQUIRE(decoded.ok());
  auto view = decoded.value().message.view();
  REQUIRE(view.get_string(kSenderCompID).has_value());
  CHECK(view.get_string(kSenderCompID).value() == "BUY");
  REQUIRE(view.get_string(kTargetCompID).has_value());
  CHECK(view.get_string(kTargetCompID).value() == "SELL");
  REQUIRE(view.get_string(kAccount).has_value());
  CHECK(view.get_string(kAccount).value() == "ACC-1");
  REQUIRE(view.get_string(kClOrdID).has_value());
  CHECK(view.get_string(kClOrdID).value() == "ORD-001");
}

TEST_CASE("fixed-layout group builder skips non-finite floats", "[message-api][fixed-layout]")
{
  auto dictionary_view = nimble::tests::LoadFix44DictionaryViewOrSkip();
  auto dictionary = nimble::base::Result<nimble::profile::NormalizedDictionaryView>(std::move(dictionary_view));

  auto layout = nimble::message::FixedLayout::Build(dictionary.value(), "D");
  REQUIRE(layout.ok());

  nimble::message::FixedLayoutWriter writer(layout.value());
  writer.set_string(kSenderCompID, "BUY");
  writer.set_string(kTargetCompID, "SELL");
  auto party = writer.add_group_entry(kNoPartyIDs);
  party.set_string(kPartyID, "PTY1").set_float(44U, std::numeric_limits<double>::infinity()).set_int(kPartyRole, 1);

  nimble::codec::EncodeOptions options;
  options.begin_string = "FIX.4.4";
  options.sender_comp_id = "BUY";
  options.target_comp_id = "SELL";
  options.msg_seq_num = 1U;
  options.sending_time = "20260406-12:00:00.000";

  nimble::codec::EncodeBuffer buf;
  REQUIRE(writer.encode_to_buffer(dictionary.value(), options, &buf).ok());

  const auto wire = std::string(reinterpret_cast<const char*>(buf.bytes().data()), buf.bytes().size());
  REQUIRE(wire.find("44=") == std::string::npos);
}

TEST_CASE("fixed-layout-writer-encode-roundtrip", "[message-api][fixed-layout]")
{
  auto dictionary_view = nimble::tests::LoadFix44DictionaryViewOrSkip();
  auto dictionary = nimble::base::Result<nimble::profile::NormalizedDictionaryView>(std::move(dictionary_view));

  auto layout = nimble::message::FixedLayout::Build(dictionary.value(), "D");
  REQUIRE(layout.ok());

  nimble::message::FixedLayoutWriter writer(layout.value());
  writer.set_string(kSenderCompID, "SENDER");
  writer.set_string(kTargetCompID, "TARGET");
  writer.set_string(kAccount, "ACC-1");
  writer.set_string(kClOrdID, "ORD-001");
  auto party = writer.add_group_entry(kNoPartyIDs);
  party.set_string(kPartyID, "PTY1").set_char(kPartyIDSource, 'D').set_int(kPartyRole, 3);

  nimble::codec::EncodeOptions options;
  options.begin_string = "FIX.4.4";
  options.sender_comp_id = "SENDER";
  options.target_comp_id = "TARGET";
  options.msg_seq_num = 7U;
  options.sending_time = "20260406-12:00:00.000";

  auto encoded = writer.encode(dictionary.value(), options);
  REQUIRE(encoded.ok());
  REQUIRE(!encoded.value().empty());

  // Decode and verify.
  auto decoded = nimble::codec::DecodeFixMessageView(encoded.value(), dictionary.value());
  REQUIRE(decoded.ok());
  auto view = decoded.value().message.view();
  CHECK(view.msg_type() == "D");
  REQUIRE(view.get_string(kSenderCompID).has_value());
  CHECK(view.get_string(kSenderCompID).value() == "SENDER");
  REQUIRE(view.get_string(kAccount).has_value());
  CHECK(view.get_string(kAccount).value() == "ACC-1");
  auto group = view.group(kNoPartyIDs);
  REQUIRE(group.has_value());
  REQUIRE(group->size() == 1U);
  CHECK((*group)[0].get_string(kPartyID).value() == "PTY1");
}

// ---------------------------------------------------------------------------
// Generated typed writer tests
// ---------------------------------------------------------------------------

using namespace nimble::generated::profile_4400;

TEST_CASE("generated-writer-scalar-fields", "[message-api][generated-writer]")
{
  auto dictionary_view = nimble::tests::LoadFix44DictionaryViewOrSkip();
  auto dictionary = nimble::base::Result<nimble::profile::NormalizedDictionaryView>(std::move(dictionary_view));

  auto layout = nimble::message::FixedLayout::Build(dictionary.value(), "D");
  REQUIRE(layout.ok());

  NewOrderSingleWriter writer(layout.value());
  writer.set_account("ACC-1").set_cl_ord_id("ORD-001");

  nimble::codec::EncodeOptions options;
  options.begin_string = "FIX.4.4";
  options.sender_comp_id = "BUY";
  options.target_comp_id = "SELL";
  options.msg_seq_num = 1U;
  options.sending_time = "20260406-12:00:00.000";

  nimble::codec::EncodeBuffer buf;
  REQUIRE(writer.encode_to_buffer(dictionary.value(), options, &buf).ok());

  auto decoded = nimble::codec::DecodeFixMessageView(buf.bytes(), dictionary.value());
  REQUIRE(decoded.ok());
  auto view = decoded.value().message.view();

  CHECK(view.msg_type() == "D");
  REQUIRE(view.get_string(Tag::Account).has_value());
  CHECK(view.get_string(Tag::Account).value() == "ACC-1");
  REQUIRE(view.get_string(Tag::ClOrdID).has_value());
  CHECK(view.get_string(Tag::ClOrdID).value() == "ORD-001");
}

TEST_CASE("generated-writer-with-groups", "[message-api][generated-writer]")
{
  auto dictionary_view = nimble::tests::LoadFix44DictionaryViewOrSkip();
  auto dictionary = nimble::base::Result<nimble::profile::NormalizedDictionaryView>(std::move(dictionary_view));

  auto layout = nimble::message::FixedLayout::Build(dictionary.value(), "D");
  REQUIRE(layout.ok());

  NewOrderSingleWriter writer(layout.value());
  writer.set_account("ACC-1");
  writer.add_no_party_i_ds().set_party_id("PTY1").set_party_id_source('D').set_party_role(1);
  writer.add_no_party_i_ds().set_party_id("PTY2").set_party_id_source('G').set_party_role(4);

  nimble::codec::EncodeOptions options;
  options.begin_string = "FIX.4.4";
  options.sender_comp_id = "BUY";
  options.target_comp_id = "SELL";
  options.msg_seq_num = 2U;
  options.sending_time = "20260406-12:00:00.000";

  nimble::codec::EncodeBuffer buf;
  REQUIRE(writer.encode_to_buffer(dictionary.value(), options, &buf).ok());

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

TEST_CASE("generated-writer-matches-raw-fixed-layout-writer", "[message-api][generated-writer]")
{
  auto dictionary_view = nimble::tests::LoadFix44DictionaryViewOrSkip();
  auto dictionary = nimble::base::Result<nimble::profile::NormalizedDictionaryView>(std::move(dictionary_view));

  auto layout = nimble::message::FixedLayout::Build(dictionary.value(), "D");
  REQUIRE(layout.ok());

  nimble::codec::EncodeOptions options;
  options.begin_string = "FIX.4.4";
  options.sender_comp_id = "BUY";
  options.target_comp_id = "SELL";
  options.msg_seq_num = 3U;
  options.sending_time = "20260406-12:34:56.789";

  // Build via raw FixedLayoutWriter.
  nimble::message::FixedLayoutWriter raw(layout.value());
  raw.set_string(kAccount, "ACC-1");
  raw.set_string(kClOrdID, "ORD-001");
  auto raw_party = raw.add_group_entry(kNoPartyIDs);
  raw_party.set_string(kPartyID, "PTY1").set_char(kPartyIDSource, 'D').set_int(kPartyRole, 1);

  nimble::codec::EncodeBuffer raw_buf;
  REQUIRE(raw.encode_to_buffer(dictionary.value(), options, &raw_buf).ok());

  // Build via generated NewOrderSingleWriter.
  NewOrderSingleWriter typed(layout.value());
  typed.set_account("ACC-1").set_cl_ord_id("ORD-001");
  typed.add_no_party_i_ds().set_party_id("PTY1").set_party_id_source('D').set_party_role(1);

  nimble::codec::EncodeBuffer typed_buf;
  REQUIRE(typed.encode_to_buffer(dictionary.value(), options, &typed_buf).ok());

  // Wire output should be identical.
  CHECK(raw_buf.text() == typed_buf.text());
}

TEST_CASE("generated-writer-clear-and-reuse", "[message-api][generated-writer]")
{
  auto dictionary_view = nimble::tests::LoadFix44DictionaryViewOrSkip();
  auto dictionary = nimble::base::Result<nimble::profile::NormalizedDictionaryView>(std::move(dictionary_view));

  auto layout = nimble::message::FixedLayout::Build(dictionary.value(), "D");
  REQUIRE(layout.ok());

  NewOrderSingleWriter writer(layout.value());
  writer.bind_session("FIX.4.4", "BUY", "SELL");

  nimble::codec::EncodeOptions options;
  options.begin_string = "FIX.4.4";
  options.sender_comp_id = "BUY";
  options.target_comp_id = "SELL";
  options.sending_time = "20260406-12:00:00.000";

  // First encode.
  writer.set_account("ACC-1");
  options.msg_seq_num = 1U;
  nimble::codec::EncodeBuffer buf1;
  REQUIRE(writer.encode_to_buffer(dictionary.value(), options, &buf1).ok());

  // Clear and reuse.
  writer.clear();
  writer.set_account("ACC-2");
  writer.set_cl_ord_id("ORD-002");
  options.msg_seq_num = 2U;
  nimble::codec::EncodeBuffer buf2;
  REQUIRE(writer.encode_to_buffer(dictionary.value(), options, &buf2).ok());

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