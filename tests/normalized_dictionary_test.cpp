#include <catch2/catch_test_macros.hpp>

#include "nimblefix/codec/fix_tags.h"
#include "test_support.h"

TEST_CASE("normalized-dictionary", "[normalized-dictionary]")
{
  auto view_result = nimble::tests::LoadFix44DictionaryView();
  if (!view_result.ok()) {
    SKIP("FIX44 artifact not available: " << view_result.status().message());
  }
  auto& view = view_result.value();

  REQUIRE(view.field_count() > 100U);
  REQUIRE(view.message_count() > 50U);
  REQUIRE(view.group_count() > 0U);

  const auto* msg_type = view.find_field(nimble::codec::tags::kMsgType);
  REQUIRE(msg_type != nullptr);
  REQUIRE(view.field_name(*msg_type).value() == "MsgType");

  const auto* message = view.find_message("D");
  REQUIRE(message != nullptr);
  REQUIRE(view.message_name(*message).value() == "NewOrderSingle");
  REQUIRE(view.message_field_rules(*message).size() > 0U);

  const auto* group = view.find_group(nimble::codec::tags::kNoPartyIDs);
  REQUIRE(group != nullptr);
  REQUIRE(view.group_name(*group).value() == "NoPartyIDs");
  REQUIRE(view.group_field_rules(*group).size() > 0U);
}
