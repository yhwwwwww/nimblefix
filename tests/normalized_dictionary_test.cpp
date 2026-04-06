#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include "fastfix/profile/artifact_builder.h"
#include "fastfix/profile/normalized_dictionary.h"
#include "fastfix/profile/profile_loader.h"

#include "test_support.h"

TEST_CASE("normalized-dictionary", "[normalized-dictionary]") {
    fastfix::profile::NormalizedDictionary dictionary;
    dictionary.profile_id = 77U;
    dictionary.schema_hash = 0x7700770077007700ULL;
    dictionary.fields = {
        {35U, "MsgType", fastfix::profile::ValueType::kString, 0U},
        {49U, "SenderCompID", fastfix::profile::ValueType::kString, 0U},
        {56U, "TargetCompID", fastfix::profile::ValueType::kString, 0U},
        {453U, "NoPartyIDs", fastfix::profile::ValueType::kInt, 0U},
        {448U, "PartyID", fastfix::profile::ValueType::kString, 0U},
    };
    dictionary.messages = {
        fastfix::profile::MessageDef{
            .msg_type = "D",
            .name = "NewOrderSingle",
            .field_rules = {
                {35U, static_cast<std::uint32_t>(fastfix::profile::FieldRuleFlags::kRequired)},
                {49U, static_cast<std::uint32_t>(fastfix::profile::FieldRuleFlags::kRequired)},
                {56U, static_cast<std::uint32_t>(fastfix::profile::FieldRuleFlags::kRequired)},
            },
            .flags = 0U,
        },
    };
    dictionary.groups = {
        fastfix::profile::GroupDef{
            .count_tag = 453U,
            .delimiter_tag = 448U,
            .name = "Parties",
            .field_rules = {
                {448U, static_cast<std::uint32_t>(fastfix::profile::FieldRuleFlags::kRequired)},
            },
            .flags = 0U,
        },
    };

    auto artifact = fastfix::profile::BuildProfileArtifact(dictionary);
    REQUIRE(artifact.ok());

    const auto artifact_path = std::filesystem::temp_directory_path() / "fastfix-normalized-dictionary-test.art";
    const auto write_status = fastfix::profile::WriteProfileArtifact(artifact_path, artifact.value());
    REQUIRE(write_status.ok());

    auto loaded = fastfix::profile::LoadProfileArtifact(artifact_path);
    REQUIRE(loaded.ok());

    auto view = fastfix::profile::NormalizedDictionaryView::FromProfile(std::move(loaded).value());
    REQUIRE(view.ok());
    REQUIRE(view.value().field_count() == 5U);
    REQUIRE(view.value().message_count() == 1U);
    REQUIRE(view.value().group_count() == 1U);

    const auto* msg_type = view.value().find_field(35U);
    REQUIRE(msg_type != nullptr);
    REQUIRE(view.value().field_name(*msg_type).value() == "MsgType");

    const auto* message = view.value().find_message("D");
    REQUIRE(message != nullptr);
    REQUIRE(view.value().message_name(*message).value() == "NewOrderSingle");
    REQUIRE(view.value().message_field_rules(*message).size() == 3U);

    const auto* group = view.value().find_group(453U);
    REQUIRE(group != nullptr);
    REQUIRE(view.value().group_name(*group).value() == "Parties");
    REQUIRE(view.value().group_field_rules(*group).size() == 1U);
    REQUIRE(group->required_field_bitmap == 1U);
    REQUIRE((group->flags & static_cast<std::uint32_t>(fastfix::profile::GroupFlags::kRequiredBitmapOverflow)) == 0U);

    std::filesystem::remove(artifact_path);
}
