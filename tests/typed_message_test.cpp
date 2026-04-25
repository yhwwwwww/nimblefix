#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include "nimblefix/codec/fix_codec.h"
#include "nimblefix/codec/fix_tags.h"
#include "nimblefix/message/message_builder.h"
#include "nimblefix/message/typed_message_view.h"
#include "nimblefix/profile/artifact_builder.h"
#include "nimblefix/profile/dictgen_input.h"
#include "nimblefix/profile/normalized_dictionary.h"
#include "nimblefix/profile/profile_loader.h"

#include "test_support.h"

namespace {

using namespace nimble::codec::tags;

auto
LoadDictionaryViewFromText(std::string_view text, std::string_view file_stub)
  -> nimble::base::Result<nimble::profile::NormalizedDictionaryView>
{
  auto dictionary = nimble::profile::LoadNormalizedDictionaryText(text);
  if (!dictionary.ok()) {
    return dictionary.status();
  }

  auto artifact = nimble::profile::BuildProfileArtifact(dictionary.value());
  if (!artifact.ok()) {
    return artifact.status();
  }

  const auto artifact_path =
    std::filesystem::temp_directory_path() / (std::string("nimblefix-") + std::string(file_stub) + ".nfa");
  const auto write_status = nimble::profile::WriteProfileArtifact(artifact_path, artifact.value());
  if (!write_status.ok()) {
    return write_status;
  }

  auto loaded = nimble::profile::LoadProfileArtifact(artifact_path);
  std::filesystem::remove(artifact_path);
  if (!loaded.ok()) {
    return loaded.status();
  }
  return nimble::profile::NormalizedDictionaryView::FromProfile(std::move(loaded).value());
}

} // namespace

TEST_CASE("typed-message-view", "[typed-message]")
{
  auto dictionary_view = nimble::tests::LoadFix44DictionaryViewOrSkip();
  auto dictionary = nimble::base::Result<nimble::profile::NormalizedDictionaryView>(std::move(dictionary_view));

  nimble::message::MessageBuilder builder{ "D" };
  builder.reserve_fields(11U).reserve_groups(1U).reserve_group_entries(kNoPartyIDs, 1U);
  builder.set_string(kSenderCompID, "BUY")
    .set_string(kTargetCompID, "SELL")
    .set_string(kClOrdID, "ORD-001")
    .set_string(kSymbol, "AAPL")
    .set_char(kSide, '1')
    .set_string(kTransactTime, "20260406-12:00:00.000")
    .set_int(kOrderQty, 100)
    .set_char(kOrdType, '2')
    .set_string(kAccount, "ACC-1");
  auto party = builder.add_group_entry(kNoPartyIDs);
  party.set_string(kPartyID, "PTY1").set_char(kPartyIDSource, 'D').set_int(kPartyRole, 7);

  auto message = std::move(builder).build();

  auto typed = nimble::message::TypedMessageView::Bind(dictionary.value(), message.view());
  REQUIRE(typed.ok());
  REQUIRE(typed.value().validate_required_fields().ok());
  REQUIRE(typed.value().get_string(kMsgType).value() == "D");
  REQUIRE(typed.value().get_string(kAccount).value() == "ACC-1");

  const auto parties = typed.value().group(kNoPartyIDs);
  REQUIRE(parties.has_value());
  REQUIRE(parties->size() == 1U);
  REQUIRE((*parties)[0].get_string(kPartyID).value() == "PTY1");
  REQUIRE((*parties)[0].get_char(kPartyIDSource).value() == 'D');
  REQUIRE((*parties)[0].get_int(kPartyRole).value() == 7);

  // Validate required field check on incomplete message.
  nimble::message::MessageBuilder empty_builder{ "D" };
  auto empty_message = std::move(empty_builder).build();
  auto empty_typed = nimble::message::TypedMessageView::Bind(dictionary.value(), empty_message.view());
  REQUIRE(empty_typed.ok());
  std::uint32_t missing_tag = 0U;
  REQUIRE(!empty_typed.value().validate_required_fields(&missing_tag).ok());
  REQUIRE(missing_tag == kClOrdID);

  // Missing group required field — NoPartyIDs fields are all optional in FIX44,
  // so we test with a message that has a different required field missing
  // instead. Verify that omitting ClOrdID from a NewOrderSingle triggers
  // validation failure.
  nimble::message::MessageBuilder partial_builder{ "D" };
  partial_builder.set_string(kSenderCompID, "BUY")
    .set_string(kTargetCompID, "SELL")
    .set_string(kSymbol, "AAPL")
    .set_char(kSide, '1')
    .set_string(kTransactTime, "20260406-12:00:00.000")
    .set_int(kOrderQty, 100)
    .set_char(kOrdType, '2')
    .set_string(kAccount, "ACC-1");
  auto partial_party = partial_builder.add_group_entry(kNoPartyIDs);
  partial_party.set_string(kPartyID, "PTY2").set_int(kPartyRole, 9);
  auto partial_message = std::move(partial_builder).build();
  auto partial_typed = nimble::message::TypedMessageView::Bind(dictionary.value(), partial_message.view());
  REQUIRE(partial_typed.ok());
  missing_tag = 0U;
  REQUIRE(!partial_typed.value().validate_required_fields(&missing_tag).ok());
  REQUIRE(missing_tag == kClOrdID);
}

TEST_CASE("typed-message-view-timestamp", "[typed-message]")
{
  constexpr std::string_view kTimestampDictionary = R"(profile_id=2002
schema_hash=0x2002

field|35|MsgType|string|0
field|60|TransactTime|timestamp|0
field|555|NoLegs|int|0
field|600|LegSymbol|string|0
field|601|LegTransactTime|timestamp|0

message|Z|TimestampMessage|0|35:r,60:r,555:o
group|555|600|Legs|0|600:r,601:r
)";

  auto timestamp_dictionary = LoadDictionaryViewFromText(kTimestampDictionary, "typed-message-timestamp");
  REQUIRE(timestamp_dictionary.ok());

  // Build the message using plain MessageBuilder, then encode+decode to get a
  // view.
  nimble::message::MessageBuilder builder{ "Z" };
  builder.set_string(kTransactTime, "20260403-12:00:00.000");
  auto leg = builder.add_group_entry(kNoLegs);
  leg.set_string(kLegSymbol, "IBM").set_string(kLegTransactTime, "20260403-12:00:01.000");

  nimble::codec::EncodeOptions timestamp_options;
  timestamp_options.begin_string = "FIX.4.4";
  timestamp_options.sender_comp_id = "BUY";
  timestamp_options.target_comp_id = "SELL";
  timestamp_options.msg_seq_num = 2U;
  timestamp_options.sending_time = "20260403-12:00:02.000";

  auto message = std::move(builder).build();
  auto encoded = nimble::codec::EncodeFixMessage(message, timestamp_dictionary.value(), timestamp_options);
  REQUIRE(encoded.ok());

  auto timestamp_decoded = nimble::codec::DecodeFixMessageView(encoded.value(), timestamp_dictionary.value());
  REQUIRE(timestamp_decoded.ok());

  auto typed_timestamp =
    nimble::message::TypedMessageView::Bind(timestamp_dictionary.value(), timestamp_decoded.value().message.view());
  REQUIRE(typed_timestamp.ok());
  REQUIRE(typed_timestamp.value().get_timestamp(kTransactTime).value() == "20260403-12:00:00.000");

  const auto legs = typed_timestamp.value().group(kNoLegs);
  REQUIRE(legs.has_value());
  REQUIRE(legs->size() == 1U);
  REQUIRE((*legs)[0].get_string(kLegSymbol).value() == "IBM");
  REQUIRE((*legs)[0].get_timestamp(kLegTransactTime).value() == "20260403-12:00:01.000");
}
