#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>

#include "fastfix/codec/fix_codec.h"
#include "fastfix/message/typed_message.h"
#include "fastfix/profile/artifact_builder.h"
#include "fastfix/profile/dictgen_input.h"
#include "fastfix/profile/normalized_dictionary.h"
#include "fastfix/profile/overlay.h"
#include "fastfix/profile/profile_loader.h"

#include "test_support.h"

TEST_CASE("dictgen", "[dictgen]") {    const auto ToReadableFrame = [](std::span<const std::byte> bytes) {
        std::string text;
        text.reserve(bytes.size());
        for (const auto value : bytes) {
            const auto ch = static_cast<char>(std::to_integer<unsigned char>(value));
            text.push_back(ch == '\x01' ? '|' : ch);
        }
        return text;
    };

    const auto project_root = std::filesystem::path(FASTFIX_PROJECT_DIR);
    const auto profile_path = project_root / "samples" / "basic_profile.ffd";
    const auto overlay_path = project_root / "samples" / "basic_overlay.ffd";

    auto dictionary = fastfix::profile::LoadNormalizedDictionaryFile(profile_path);
    REQUIRE(dictionary.ok());
    REQUIRE(dictionary.value().profile_id == 1001U);
    REQUIRE(dictionary.value().fields.size() >= 7U);

    auto overlay = fastfix::profile::LoadNormalizedDictionaryFile(overlay_path);
    REQUIRE(overlay.ok());
    REQUIRE(overlay.value().fields.size() == 2U);

    auto merged = fastfix::profile::ApplyOverlay(dictionary.value(), overlay.value());
    REQUIRE(merged.ok());

    auto artifact = fastfix::profile::BuildProfileArtifact(merged.value());
    REQUIRE(artifact.ok());

    const auto artifact_path = std::filesystem::temp_directory_path() / "fastfix-dictgen-test.art";
    const auto write_status = fastfix::profile::WriteProfileArtifact(artifact_path, artifact.value());
    REQUIRE(write_status.ok());

    auto loaded = fastfix::profile::LoadProfileArtifact(artifact_path);
    REQUIRE(loaded.ok());
    auto view = fastfix::profile::NormalizedDictionaryView::FromProfile(std::move(loaded).value());
    REQUIRE(view.ok());
    REQUIRE(view.value().find_field(5001U) != nullptr);

    const auto* message = view.value().find_message("D");
    REQUIRE(message != nullptr);
    REQUIRE(view.value().message_field_rules(*message).size() >= 5U);

    auto ordered_overlay = fastfix::profile::LoadNormalizedDictionaryText(
        "field|5001|VenueOrderType|string|1\n"
        "field|5002|VenueAccount|string|1\n"
        "message|D|NewOrderSingle|0|5001:r,5002:o,453:o\n");
    REQUIRE(ordered_overlay.ok());

    auto ordered_merged = fastfix::profile::ApplyOverlay(dictionary.value(), ordered_overlay.value());
    REQUIRE(ordered_merged.ok());
    const auto ordered_message = std::find_if(
        ordered_merged.value().messages.begin(),
        ordered_merged.value().messages.end(),
        [](const auto& candidate) { return candidate.msg_type == "D"; });
    REQUIRE(ordered_message != ordered_merged.value().messages.end());
    REQUIRE(ordered_message->field_rules.size() == 6U);
    REQUIRE(ordered_message->field_rules[0].tag == 35U);
    REQUIRE(ordered_message->field_rules[1].tag == 49U);
    REQUIRE(ordered_message->field_rules[2].tag == 56U);
    REQUIRE(ordered_message->field_rules[3].tag == 5001U);
    REQUIRE(ordered_message->field_rules[4].tag == 5002U);
    REQUIRE(ordered_message->field_rules[5].tag == 453U);

    auto ordered_artifact = fastfix::profile::BuildProfileArtifact(ordered_merged.value());
    REQUIRE(ordered_artifact.ok());
    const auto ordered_artifact_path = std::filesystem::temp_directory_path() / "fastfix-dictgen-ordered-test.art";
    REQUIRE(fastfix::profile::WriteProfileArtifact(ordered_artifact_path, ordered_artifact.value()).ok());
    auto ordered_loaded = fastfix::profile::LoadProfileArtifact(ordered_artifact_path);
    REQUIRE(ordered_loaded.ok());
    auto ordered_view = fastfix::profile::NormalizedDictionaryView::FromProfile(std::move(ordered_loaded).value());
    REQUIRE(ordered_view.ok());
    const auto* ordered_message_view = ordered_view.value().find_message("D");
    REQUIRE(ordered_message_view != nullptr);
    REQUIRE(ordered_view.value().message_field_rules(*ordered_message_view).size() == 6U);
    REQUIRE(ordered_view.value().message_field_rules(*ordered_message_view)[3].tag == 5001U);
    REQUIRE(ordered_view.value().message_field_rules(*ordered_message_view)[4].tag == 5002U);
    REQUIRE(ordered_view.value().message_field_rules(*ordered_message_view)[5].tag == 453U);

    fastfix::codec::EncodeOptions options;
    options.begin_string = "FIX.4.4";
    options.sender_comp_id = "BUY";
    options.target_comp_id = "SELL";
    options.msg_seq_num = 1U;
    options.sending_time = "20260404-12:00:00.000";

    fastfix::message::MessageBuilder ordered_builder{"D"};
    ordered_builder.reserve_fields(4U).reserve_groups(1U).reserve_group_entries(453U, 1U);
    ordered_builder.set_string(49U, "BUY")
        .set_string(56U, "SELL")
        .set_string(5001U, "LIT")
        .set_string(5002U, "ACC-1");
    auto ordered_party = ordered_builder.add_group_entry(453U);
    ordered_party.set_string(448U, "PTY1")
        .set_char(447U, 'D')
        .set_int(452U, 7);

    fastfix::codec::EncodeBuffer ordered_buffer;
    REQUIRE(ordered_builder.encode_to_buffer(ordered_view.value(), options, &ordered_buffer).ok());
    const auto ordered_readable = ToReadableFrame(ordered_buffer.bytes());
    const auto venue_order_type = ordered_readable.find("|5001=LIT|");
    const auto venue_account = ordered_readable.find("|5002=ACC-1|");
    const auto parties = ordered_readable.find("|453=1|");
    REQUIRE(venue_order_type != std::string::npos);
    REQUIRE(venue_account != std::string::npos);
    REQUIRE(parties != std::string::npos);
    REQUIRE(venue_order_type < parties);
    REQUIRE(venue_account < parties);

    std::filesystem::remove(artifact_path);
    std::filesystem::remove(ordered_artifact_path);
}
