#include <catch2/catch_test_macros.hpp>

#include "fastfix/profile/overlay.h"

#include "test_support.h"

TEST_CASE("overlay-merge", "[overlay-merge]") {
    fastfix::profile::NormalizedDictionary baseline;
    baseline.profile_id = 9001U;
    baseline.schema_hash = 0x9001000000000001ULL;
    baseline.fields = {
        {35U, "MsgType", fastfix::profile::ValueType::kString, 0U},
        {49U, "SenderCompID", fastfix::profile::ValueType::kString, 0U},
        {56U, "TargetCompID", fastfix::profile::ValueType::kString, 0U},
        {453U, "NoPartyIDs", fastfix::profile::ValueType::kInt, 0U},
        {448U, "PartyID", fastfix::profile::ValueType::kString, 0U},
        {447U, "PartyIDSource", fastfix::profile::ValueType::kChar, 0U},
        {452U, "PartyRole", fastfix::profile::ValueType::kInt, 0U},
    };
    baseline.messages = {
        fastfix::profile::MessageDef{
            .msg_type = "D",
            .name = "NewOrderSingle",
            .field_rules = {
                {35U, static_cast<std::uint32_t>(fastfix::profile::FieldRuleFlags::kRequired)},
                {49U, static_cast<std::uint32_t>(fastfix::profile::FieldRuleFlags::kRequired)},
                {56U, static_cast<std::uint32_t>(fastfix::profile::FieldRuleFlags::kRequired)},
                {453U, 0U},
            },
            .flags = 0U,
        },
    };
    baseline.groups = {
        fastfix::profile::GroupDef{
            .count_tag = 453U,
            .delimiter_tag = 448U,
            .name = "Parties",
            .field_rules = {
                {448U, static_cast<std::uint32_t>(fastfix::profile::FieldRuleFlags::kRequired)},
                {447U, static_cast<std::uint32_t>(fastfix::profile::FieldRuleFlags::kRequired)},
                {452U, static_cast<std::uint32_t>(fastfix::profile::FieldRuleFlags::kRequired)},
            },
            .flags = 0U,
        },
    };

    fastfix::profile::NormalizedDictionary overlay;
    overlay.fields.push_back(
        {5001U, "VenueOrderType", fastfix::profile::ValueType::kString,
                  static_cast<std::uint32_t>(fastfix::profile::FieldFlags::kCustom)});
    overlay.fields.push_back(
        {5002U, "VenueAccount", fastfix::profile::ValueType::kString,
                  static_cast<std::uint32_t>(fastfix::profile::FieldFlags::kCustom)});
    overlay.fields.push_back(
        {6001U, "PartyDesk", fastfix::profile::ValueType::kString,
                  static_cast<std::uint32_t>(fastfix::profile::FieldFlags::kCustom)});
    overlay.messages.push_back(fastfix::profile::MessageDef{
        .msg_type = "D",
        .name = "",
        .field_rules = {
            {5001U, static_cast<std::uint32_t>(fastfix::profile::FieldRuleFlags::kRequired)},
            {5002U, 0U},
            {453U, 0U},
        },
        .flags = 0U,
    });
    overlay.groups.push_back(fastfix::profile::GroupDef{
        .count_tag = 453U,
        .delimiter_tag = 448U,
        .name = "Parties",
        .field_rules = {
            {6001U, 0U},
            {452U, static_cast<std::uint32_t>(fastfix::profile::FieldRuleFlags::kRequired)},
        },
        .flags = 0U,
    });

    auto merged = fastfix::profile::ApplyOverlay(baseline, overlay);
    REQUIRE(merged.ok());
    REQUIRE(merged.value().fields.size() == 10U);
    REQUIRE(merged.value().messages.size() == 1U);
    REQUIRE(merged.value().groups.size() == 1U);
    REQUIRE(merged.value().messages.front().field_rules.size() == 6U);
    REQUIRE(merged.value().groups.front().count_tag == 453U);
    REQUIRE(merged.value().fields.back().tag == 6001U);
    REQUIRE(merged.value().messages.front().field_rules[0].tag == 35U);
    REQUIRE(merged.value().messages.front().field_rules[1].tag == 49U);
    REQUIRE(merged.value().messages.front().field_rules[2].tag == 56U);
    REQUIRE(merged.value().messages.front().field_rules[3].tag == 5001U);
    REQUIRE(merged.value().messages.front().field_rules[4].tag == 5002U);
    REQUIRE(merged.value().messages.front().field_rules[5].tag == 453U);
    REQUIRE(merged.value().groups.front().field_rules.size() == 4U);
    REQUIRE(merged.value().groups.front().field_rules[0].tag == 448U);
    REQUIRE(merged.value().groups.front().field_rules[1].tag == 447U);
    REQUIRE(merged.value().groups.front().field_rules[2].tag == 6001U);
    REQUIRE(merged.value().groups.front().field_rules[3].tag == 452U);

    fastfix::profile::NormalizedDictionary delimiter_reject_overlay;
    delimiter_reject_overlay.groups.push_back(fastfix::profile::GroupDef{
        .count_tag = 453U,
        .delimiter_tag = 999U,
        .name = "",
        .field_rules = {},
        .flags = 0U,
    });
    auto rejected = fastfix::profile::ApplyOverlay(baseline, delimiter_reject_overlay);
    REQUIRE(!rejected.ok());

    fastfix::profile::NormalizedDictionary delimiter_allow_overlay;
    delimiter_allow_overlay.groups.push_back(fastfix::profile::GroupDef{
        .count_tag = 453U,
        .delimiter_tag = 999U,
        .name = "",
        .field_rules = {},
        .flags = static_cast<std::uint32_t>(fastfix::profile::GroupFlags::kAllowDelimiterOverride),
    });
    auto allowed = fastfix::profile::ApplyOverlay(baseline, delimiter_allow_overlay);
    REQUIRE(allowed.ok());
    REQUIRE(allowed.value().groups.front().delimiter_tag == 999U);

}
