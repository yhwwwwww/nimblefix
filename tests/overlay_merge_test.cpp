#include <catch2/catch_test_macros.hpp>

#include "fastfix/codec/fix_tags.h"
#include "fastfix/profile/overlay.h"

using namespace fastfix::codec::tags;

TEST_CASE("overlay-merge", "[overlay-merge]")
{
  fastfix::profile::NormalizedDictionary baseline;
  baseline.profile_id = 9001U;
  baseline.schema_hash = 0x9001000000000001ULL;
  baseline.fields = {
    { kMsgType, "MsgType", fastfix::profile::ValueType::kString, 0U },
    { kSenderCompID, "SenderCompID", fastfix::profile::ValueType::kString, 0U },
    { kTargetCompID, "TargetCompID", fastfix::profile::ValueType::kString, 0U },
    { kNoPartyIDs, "NoPartyIDs", fastfix::profile::ValueType::kInt, 0U },
    { kPartyID, "PartyID", fastfix::profile::ValueType::kString, 0U },
    { kPartyIDSource, "PartyIDSource", fastfix::profile::ValueType::kChar, 0U },
    { kPartyRole, "PartyRole", fastfix::profile::ValueType::kInt, 0U },
  };
  baseline.messages = {
      fastfix::profile::MessageDef{
          .msg_type = "D",
          .name = "NewOrderSingle",
          .field_rules =
              {
                  {kMsgType, static_cast<std::uint32_t>(fastfix::profile::FieldRuleFlags::kRequired)},
                  {kSenderCompID, static_cast<std::uint32_t>(fastfix::profile::FieldRuleFlags::kRequired)},
                  {kTargetCompID, static_cast<std::uint32_t>(fastfix::profile::FieldRuleFlags::kRequired)},
                  {kNoPartyIDs, 0U},
              },
          .flags = 0U,
      },
  };
  baseline.groups = {
      fastfix::profile::GroupDef{
          .count_tag = kNoPartyIDs,
          .delimiter_tag = kPartyID,
          .name = "Parties",
          .field_rules =
              {
                  {kPartyID, static_cast<std::uint32_t>(fastfix::profile::FieldRuleFlags::kRequired)},
                  {kPartyIDSource, static_cast<std::uint32_t>(fastfix::profile::FieldRuleFlags::kRequired)},
                  {kPartyRole, static_cast<std::uint32_t>(fastfix::profile::FieldRuleFlags::kRequired)},
              },
          .flags = 0U,
      },
  };

  fastfix::profile::NormalizedDictionary overlay;
  overlay.fields.push_back({ 5001U,
                             "VenueOrderType",
                             fastfix::profile::ValueType::kString,
                             static_cast<std::uint32_t>(fastfix::profile::FieldFlags::kCustom) });
  overlay.fields.push_back({ 5002U,
                             "VenueAccount",
                             fastfix::profile::ValueType::kString,
                             static_cast<std::uint32_t>(fastfix::profile::FieldFlags::kCustom) });
  overlay.fields.push_back({ 6001U,
                             "PartyDesk",
                             fastfix::profile::ValueType::kString,
                             static_cast<std::uint32_t>(fastfix::profile::FieldFlags::kCustom) });
  overlay.messages.push_back(fastfix::profile::MessageDef{
      .msg_type = "D",
      .name = "",
      .field_rules =
          {
              {5001U, static_cast<std::uint32_t>(fastfix::profile::FieldRuleFlags::kRequired)},
              {5002U, 0U},
              {kNoPartyIDs, 0U},
          },
      .flags = 0U,
  });
  overlay.groups.push_back(fastfix::profile::GroupDef{
      .count_tag = kNoPartyIDs,
      .delimiter_tag = kPartyID,
      .name = "Parties",
      .field_rules =
          {
              {6001U, 0U},
              {kPartyRole, static_cast<std::uint32_t>(fastfix::profile::FieldRuleFlags::kRequired)},
          },
      .flags = 0U,
  });

  auto merged = fastfix::profile::ApplyOverlay(baseline, overlay);
  REQUIRE(merged.ok());
  REQUIRE(merged.value().fields.size() == 10U);
  REQUIRE(merged.value().messages.size() == 1U);
  REQUIRE(merged.value().groups.size() == 1U);
  REQUIRE(merged.value().messages.front().field_rules.size() == 6U);
  REQUIRE(merged.value().groups.front().count_tag == kNoPartyIDs);
  REQUIRE(merged.value().fields.back().tag == 6001U);
  REQUIRE(merged.value().messages.front().field_rules[0].tag == kMsgType);
  REQUIRE(merged.value().messages.front().field_rules[1].tag == kSenderCompID);
  REQUIRE(merged.value().messages.front().field_rules[2].tag == kTargetCompID);
  REQUIRE(merged.value().messages.front().field_rules[3].tag == 5001U);
  REQUIRE(merged.value().messages.front().field_rules[4].tag == 5002U);
  REQUIRE(merged.value().messages.front().field_rules[5].tag == kNoPartyIDs);
  REQUIRE(merged.value().groups.front().field_rules.size() == 4U);
  REQUIRE(merged.value().groups.front().field_rules[0].tag == kPartyID);
  REQUIRE(merged.value().groups.front().field_rules[1].tag == kPartyIDSource);
  REQUIRE(merged.value().groups.front().field_rules[2].tag == 6001U);
  REQUIRE(merged.value().groups.front().field_rules[3].tag == kPartyRole);

  fastfix::profile::NormalizedDictionary delimiter_reject_overlay;
  delimiter_reject_overlay.groups.push_back(fastfix::profile::GroupDef{
    .count_tag = kNoPartyIDs,
    .delimiter_tag = 999U,
    .name = "",
    .field_rules = {},
    .flags = 0U,
  });
  auto rejected = fastfix::profile::ApplyOverlay(baseline, delimiter_reject_overlay);
  REQUIRE(!rejected.ok());

  fastfix::profile::NormalizedDictionary delimiter_allow_overlay;
  delimiter_allow_overlay.groups.push_back(fastfix::profile::GroupDef{
    .count_tag = kNoPartyIDs,
    .delimiter_tag = 999U,
    .name = "",
    .field_rules = {},
    .flags = static_cast<std::uint32_t>(fastfix::profile::GroupFlags::kAllowDelimiterOverride),
  });
  auto allowed = fastfix::profile::ApplyOverlay(baseline, delimiter_allow_overlay);
  REQUIRE(allowed.ok());
  REQUIRE(allowed.value().groups.front().delimiter_tag == 999U);
}

TEST_CASE("overlay-merge-header-trailer", "[overlay-merge]")
{
  fastfix::profile::NormalizedDictionary baseline;
  baseline.profile_id = 9002U;
  baseline.schema_hash = 0x9002000000000001ULL;
  baseline.fields = {
    { kMsgType, "MsgType", fastfix::profile::ValueType::kString, 0U },
    { kSenderCompID, "SenderCompID", fastfix::profile::ValueType::kString, 0U },
    { kTargetCompID, "TargetCompID", fastfix::profile::ValueType::kString, 0U },
    { kOnBehalfOfCompID, "OnBehalfOfCompID", fastfix::profile::ValueType::kString, 0U },
    { kSignature, "Signature", fastfix::profile::ValueType::kString, 0U },
  };
  baseline.header_fields = {
    { kOnBehalfOfCompID, 0U },
  };
  baseline.trailer_fields = {
    { kSignature, 0U },
  };

  // Overlay adds custom venue header/trailer fields.
  fastfix::profile::NormalizedDictionary overlay;
  overlay.fields.push_back({ 5500U,
                             "VenueSessionToken",
                             fastfix::profile::ValueType::kString,
                             static_cast<std::uint32_t>(fastfix::profile::FieldFlags::kCustom) });
  overlay.fields.push_back({ 5501U,
                             "VenueTrailerCheck",
                             fastfix::profile::ValueType::kString,
                             static_cast<std::uint32_t>(fastfix::profile::FieldFlags::kCustom) });
  overlay.header_fields = {
    { 5500U, static_cast<std::uint32_t>(fastfix::profile::FieldRuleFlags::kRequired) },
  };
  overlay.trailer_fields = {
    { 5501U, 0U },
  };

  auto merged = fastfix::profile::ApplyOverlay(baseline, overlay);
  REQUIRE(merged.ok());

  // Header should have baseline (115) + overlay (5500).
  REQUIRE(merged.value().header_fields.size() == 2U);
  REQUIRE(merged.value().header_fields[0].tag == kOnBehalfOfCompID);
  REQUIRE(merged.value().header_fields[1].tag == 5500U);
  REQUIRE(merged.value().header_fields[1].required());

  // Trailer should have baseline (89) + overlay (5501).
  REQUIRE(merged.value().trailer_fields.size() == 2U);
  REQUIRE(merged.value().trailer_fields[0].tag == kSignature);
  REQUIRE(merged.value().trailer_fields[1].tag == 5501U);
  REQUIRE(!merged.value().trailer_fields[1].required());
}

TEST_CASE("overlay-merge-header-rejects-core-session-tags", "[overlay-merge]")
{
  fastfix::profile::NormalizedDictionary baseline;
  baseline.profile_id = 9003U;
  baseline.schema_hash = 0x9003000000000001ULL;

  // Overlay tries to add MsgType to header — should be rejected.
  {
    fastfix::profile::NormalizedDictionary overlay;
    overlay.header_fields = {
      { kMsgType, static_cast<std::uint32_t>(fastfix::profile::FieldRuleFlags::kRequired) },
    };
    auto result = fastfix::profile::ApplyOverlay(baseline, overlay);
    REQUIRE(!result.ok());
  }

  // Overlay tries to add CheckSum to trailer — should be rejected.
  {
    fastfix::profile::NormalizedDictionary overlay;
    overlay.trailer_fields = {
      { kCheckSum, 0U },
    };
    auto result = fastfix::profile::ApplyOverlay(baseline, overlay);
    REQUIRE(!result.ok());
  }

  // Overlay tries to add SenderCompID to header — should be rejected.
  {
    fastfix::profile::NormalizedDictionary overlay;
    overlay.header_fields = {
      { kSenderCompID, 0U },
    };
    auto result = fastfix::profile::ApplyOverlay(baseline, overlay);
    REQUIRE(!result.ok());
  }
}
