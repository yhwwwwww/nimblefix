#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include "fastfix/codec/fix_codec.h"
#include "fastfix/message/typed_message.h"
#include "fastfix/profile/artifact_builder.h"
#include "fastfix/profile/dictgen_input.h"
#include "fastfix/profile/normalized_dictionary.h"
#include "fastfix/profile/overlay.h"
#include "fastfix/profile/profile_loader.h"

#include "test_support.h"

namespace {

auto LoadSampleDictionaryView() -> fastfix::base::Result<fastfix::profile::NormalizedDictionaryView> {
    const auto project_root = std::filesystem::path(FASTFIX_PROJECT_DIR);
    auto dictionary = fastfix::profile::LoadNormalizedDictionaryFile(project_root / "samples" / "basic_profile.ffd");
    if (!dictionary.ok()) {
        return dictionary.status();
    }

    auto overlay = fastfix::profile::LoadNormalizedDictionaryFile(project_root / "samples" / "basic_overlay.ffd");
    if (!overlay.ok()) {
        return overlay.status();
    }

    auto merged = fastfix::profile::ApplyOverlay(dictionary.value(), overlay.value());
    if (!merged.ok()) {
        return merged.status();
    }

    auto artifact = fastfix::profile::BuildProfileArtifact(merged.value());
    if (!artifact.ok()) {
        return artifact.status();
    }

    const auto artifact_path = std::filesystem::temp_directory_path() / "fastfix-typed-message-test.art";
    const auto write_status = fastfix::profile::WriteProfileArtifact(artifact_path, artifact.value());
    if (!write_status.ok()) {
        return write_status;
    }

    auto loaded = fastfix::profile::LoadProfileArtifact(artifact_path);
    std::filesystem::remove(artifact_path);
    if (!loaded.ok()) {
        return loaded.status();
    }
    return fastfix::profile::NormalizedDictionaryView::FromProfile(std::move(loaded).value());
}

auto LoadDictionaryViewFromText(std::string_view text, std::string_view file_stub)
    -> fastfix::base::Result<fastfix::profile::NormalizedDictionaryView> {
    auto dictionary = fastfix::profile::LoadNormalizedDictionaryText(text);
    if (!dictionary.ok()) {
        return dictionary.status();
    }

    auto artifact = fastfix::profile::BuildProfileArtifact(dictionary.value());
    if (!artifact.ok()) {
        return artifact.status();
    }

    const auto artifact_path = std::filesystem::temp_directory_path() /
        (std::string("fastfix-") + std::string(file_stub) + ".art");
    const auto write_status = fastfix::profile::WriteProfileArtifact(artifact_path, artifact.value());
    if (!write_status.ok()) {
        return write_status;
    }

    auto loaded = fastfix::profile::LoadProfileArtifact(artifact_path);
    std::filesystem::remove(artifact_path);
    if (!loaded.ok()) {
        return loaded.status();
    }
    return fastfix::profile::NormalizedDictionaryView::FromProfile(std::move(loaded).value());
}

}  // namespace

TEST_CASE("typed-message-view", "[typed-message]") {
    auto dictionary = LoadSampleDictionaryView();
    REQUIRE(dictionary.ok());

    fastfix::message::MessageBuilder builder{"D"};
    builder.reserve_fields(4U).reserve_groups(1U).reserve_group_entries(453U, 1U);
    builder.set_string(49U, "BUY")
        .set_string(56U, "SELL")
        .set_string(5001U, "LIT")
        .set_string(5002U, "ACC-1");
    auto party = builder.add_group_entry(453U);
    party.set_string(448U, "PTY1")
        .set_char(447U, 'D')
        .set_int(452U, 7);

    auto message = std::move(builder).build();

    auto typed = fastfix::message::TypedMessageView::Bind(dictionary.value(), message.view());
    REQUIRE(typed.ok());
    REQUIRE(typed.value().validate_required_fields().ok());
    REQUIRE(typed.value().get_string(35U).value() == "D");
    REQUIRE(typed.value().get_string(5001U).value() == "LIT");

    const auto parties = typed.value().group(453U);
    REQUIRE(parties.has_value());
    REQUIRE(parties->size() == 1U);
    REQUIRE((*parties)[0].get_string(448U).value() == "PTY1");
    REQUIRE((*parties)[0].get_char(447U).value() == 'D');
    REQUIRE((*parties)[0].get_int(452U).value() == 7);

    // Validate required field check on incomplete message.
    fastfix::message::MessageBuilder empty_builder{"D"};
    auto empty_message = std::move(empty_builder).build();
    auto empty_typed = fastfix::message::TypedMessageView::Bind(dictionary.value(), empty_message.view());
    REQUIRE(empty_typed.ok());
    std::uint32_t missing_tag = 0U;
    REQUIRE(!empty_typed.value().validate_required_fields(&missing_tag).ok());
    REQUIRE(missing_tag == 49U);

    // Missing group required field.
    fastfix::message::MessageBuilder partial_builder{"D"};
    partial_builder.set_string(49U, "BUY").set_string(56U, "SELL");
    auto partial_party = partial_builder.add_group_entry(453U);
    partial_party.set_string(448U, "PTY2").set_int(452U, 9);
    auto partial_message = std::move(partial_builder).build();
    auto partial_typed = fastfix::message::TypedMessageView::Bind(dictionary.value(), partial_message.view());
    REQUIRE(partial_typed.ok());
    missing_tag = 0U;
    REQUIRE(!partial_typed.value().validate_required_fields(&missing_tag).ok());
    REQUIRE(missing_tag == 447U);
}

TEST_CASE("typed-message-view-timestamp", "[typed-message]") {
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

    // Build the message using plain MessageBuilder, then encode+decode to get a view.
    fastfix::message::MessageBuilder builder{"Z"};
    builder.set_string(60U, "20260403-12:00:00.000");
    auto leg = builder.add_group_entry(555U);
    leg.set_string(600U, "IBM").set_string(601U, "20260403-12:00:01.000");

    fastfix::codec::EncodeOptions timestamp_options;
    timestamp_options.begin_string = "FIX.4.4";
    timestamp_options.sender_comp_id = "BUY";
    timestamp_options.target_comp_id = "SELL";
    timestamp_options.msg_seq_num = 2U;
    timestamp_options.sending_time = "20260403-12:00:02.000";

    auto message = std::move(builder).build();
    auto encoded = fastfix::codec::EncodeFixMessage(message, timestamp_dictionary.value(), timestamp_options);
    REQUIRE(encoded.ok());

    auto timestamp_decoded = fastfix::codec::DecodeFixMessageView(
        encoded.value(),
        timestamp_dictionary.value());
    REQUIRE(timestamp_decoded.ok());

    auto typed_timestamp = fastfix::message::TypedMessageView::Bind(
        timestamp_dictionary.value(),
        timestamp_decoded.value().message.view());
    REQUIRE(typed_timestamp.ok());
    REQUIRE(typed_timestamp.value().get_timestamp(60U).value() == "20260403-12:00:00.000");

    const auto legs = typed_timestamp.value().group(555U);
    REQUIRE(legs.has_value());
    REQUIRE(legs->size() == 1U);
    REQUIRE((*legs)[0].get_string(600U).value() == "IBM");
    REQUIRE((*legs)[0].get_timestamp(601U).value() == "20260403-12:00:01.000");
}