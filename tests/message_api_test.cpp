#include <catch2/catch_test_macros.hpp>

#include "fastfix/codec/fix_codec.h"
#include "fastfix/message/fixed_layout_writer.h"
#include "fastfix/message/message.h"
#include "fastfix/profile/artifact_builder.h"
#include "fastfix/profile/dictgen_input.h"
#include "fastfix/profile/overlay.h"
#include "fastfix/profile/profile_loader.h"

#include "test_support.h"

namespace {

auto LoadMessageApiDictionary() -> fastfix::base::Result<fastfix::profile::NormalizedDictionaryView> {
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

    const auto artifact_path = std::filesystem::temp_directory_path() / "fastfix-message-api-test.art";
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

TEST_CASE("message-api", "[message-api]") {
    auto dictionary = LoadMessageApiDictionary();
    REQUIRE(dictionary.ok());

    fastfix::message::MessageBuilder builder("D");
    builder.set_string(35U, "D").set_string(49U, "BUY").set_string(56U, "SELL").set_int(38U, 1000);
    auto parties = builder.add_group_entry(453U);
    parties.set_string(448U, "PTY1").set_char(447U, 'D').set_int(452U, 1);

    fastfix::codec::EncodeOptions options;
    options.begin_string = "FIX.4.4";
    options.sender_comp_id = "BUY";
    options.target_comp_id = "SELL";
    options.msg_seq_num = 1U;
    options.sending_time = "20260403-13:00:00.000";

    fastfix::codec::EncodeBuffer buffer;
    REQUIRE(builder.encode_to_buffer(dictionary.value(), options, &buffer).ok());
    auto encoded = builder.encode(dictionary.value(), options);
    REQUIRE(encoded.ok());
    REQUIRE(buffer.size() == encoded.value().size());
    REQUIRE(std::equal(
        buffer.bytes().begin(),
        buffer.bytes().end(),
        encoded.value().begin(),
        encoded.value().end()));

    const auto message = std::move(builder).build();
    const auto view = message.view();

    auto encoded_owned = fastfix::codec::EncodeFixMessage(message, dictionary.value(), options);
    REQUIRE(encoded_owned.ok());
    REQUIRE(encoded_owned.value() == encoded.value());

    REQUIRE(view.msg_type() == "D");
    REQUIRE(view.get_string(49U).has_value());
    REQUIRE(view.get_string(49U).value() == "BUY");
    REQUIRE(view.get_int(38U).has_value());
    REQUIRE(view.get_int(38U).value() == 1000);

    const auto group = view.group(453U);
    REQUIRE(group.has_value());
    REQUIRE(group->size() == 1U);
    REQUIRE((*group)[0].get_string(448U).value() == "PTY1");
    REQUIRE((*group)[0].get_char(447U).value() == 'D');
    REQUIRE((*group)[0].get_int(452U).value() == 1);
}

TEST_CASE("fixed-layout-build", "[message-api][fixed-layout]") {
    auto dictionary = LoadMessageApiDictionary();
    REQUIRE(dictionary.ok());

    auto layout = fastfix::message::FixedLayout::Build(dictionary.value(), "D");
    REQUIRE(layout.ok());

    CHECK(layout.value().msg_type() == "D");
    CHECK(layout.value().field_count() > 0U);

    // Scalar tag 49 (SenderCompID) should have a valid slot.
    CHECK(layout.value().slot_index(49U) >= 0);
    // Tag 0 should not be in layout.
    CHECK(layout.value().slot_index(0U) == -1);
    // Group count_tag 453 should be in the group index.
    CHECK(layout.value().group_slot_index(453U) >= 0);
    // Non-existent group.
    CHECK(layout.value().group_slot_index(99999U) == -1);
}

TEST_CASE("fixed-layout-build-unknown-type", "[message-api][fixed-layout]") {
    auto dictionary = LoadMessageApiDictionary();
    REQUIRE(dictionary.ok());

    auto layout = fastfix::message::FixedLayout::Build(dictionary.value(), "ZZ_NONEXIST");
    REQUIRE_FALSE(layout.ok());
}

TEST_CASE("fixed-layout-writer-scalar-fields", "[message-api][fixed-layout]") {
    auto dictionary = LoadMessageApiDictionary();
    REQUIRE(dictionary.ok());

    auto layout = fastfix::message::FixedLayout::Build(dictionary.value(), "D");
    REQUIRE(layout.ok());

    fastfix::message::FixedLayoutWriter writer(layout.value());
    REQUIRE(writer.set_string(49U, "BUY"));
    REQUIRE(writer.set_string(56U, "SELL"));
    REQUIRE(writer.set_string(5001U, "LIT"));

    // Unknown tag should fail.
    CHECK_FALSE(writer.set_string(99999U, "NOPE"));

    // Encode + decode roundtrip to verify fields.
    fastfix::codec::EncodeOptions options;
    options.begin_string = "FIX.4.4";
    options.sender_comp_id = "BUY";
    options.target_comp_id = "SELL";
    options.msg_seq_num = 1U;
    options.sending_time = "20260406-12:00:00.000";

    fastfix::codec::EncodeBuffer buf;
    REQUIRE(writer.encode_to_buffer(dictionary.value(), options, &buf).ok());

    auto decoded = fastfix::codec::DecodeFixMessageView(buf.bytes(), dictionary.value());
    REQUIRE(decoded.ok());
    auto view = decoded.value().message.view();

    CHECK(view.msg_type() == "D");
    REQUIRE(view.get_string(49U).has_value());
    CHECK(view.get_string(49U).value() == "BUY");
    REQUIRE(view.get_string(56U).has_value());
    CHECK(view.get_string(56U).value() == "SELL");
    REQUIRE(view.get_string(5001U).has_value());
    CHECK(view.get_string(5001U).value() == "LIT");
}

TEST_CASE("fixed-layout-writer-with-groups", "[message-api][fixed-layout]") {
    auto dictionary = LoadMessageApiDictionary();
    REQUIRE(dictionary.ok());

    auto layout = fastfix::message::FixedLayout::Build(dictionary.value(), "D");
    REQUIRE(layout.ok());

    fastfix::message::FixedLayoutWriter writer(layout.value());
    writer.set_string(49U, "BUY");
    writer.set_string(56U, "SELL");
    writer.set_string(5001U, "LIT");
    auto party = writer.add_group_entry(453U);
    party.set_string(448U, "PTY1").set_char(447U, 'D').set_int(452U, 1);

    // Encode + decode roundtrip to verify group fields.
    fastfix::codec::EncodeOptions options;
    options.begin_string = "FIX.4.4";
    options.sender_comp_id = "BUY";
    options.target_comp_id = "SELL";
    options.msg_seq_num = 1U;
    options.sending_time = "20260406-12:00:00.000";

    fastfix::codec::EncodeBuffer buf;
    REQUIRE(writer.encode_to_buffer(dictionary.value(), options, &buf).ok());

    auto decoded = fastfix::codec::DecodeFixMessageView(buf.bytes(), dictionary.value());
    REQUIRE(decoded.ok());
    auto view = decoded.value().message.view();

    const auto group = view.group(453U);
    REQUIRE(group.has_value());
    REQUIRE(group->size() == 1U);
    CHECK((*group)[0].get_string(448U).value() == "PTY1");
    CHECK((*group)[0].get_char(447U).value() == 'D');
    CHECK((*group)[0].get_int(452U).value() == 1);
}

TEST_CASE("fixed-layout-writer-matches-message-builder", "[message-api][fixed-layout]") {
    auto dictionary = LoadMessageApiDictionary();
    REQUIRE(dictionary.ok());

    // Build via MessageBuilder (same tags as FixedLayoutWriter below).
    fastfix::message::MessageBuilder mb("D");
    mb.set_string(49U, "BUY").set_string(56U, "SELL").set_string(5001U, "LIT");
    auto mb_party = mb.add_group_entry(453U);
    mb_party.set_string(448U, "PTY1").set_char(447U, 'D').set_int(452U, 1);

    fastfix::codec::EncodeOptions options;
    options.begin_string = "FIX.4.4";
    options.sender_comp_id = "BUY";
    options.target_comp_id = "SELL";
    options.msg_seq_num = 1U;
    options.sending_time = "20260406-12:34:56.789";

    fastfix::codec::EncodeBuffer mb_buffer;
    REQUIRE(mb.encode_to_buffer(dictionary.value(), options, &mb_buffer).ok());

    // Build via FixedLayoutWriter.
    auto layout = fastfix::message::FixedLayout::Build(dictionary.value(), "D");
    REQUIRE(layout.ok());

    fastfix::message::FixedLayoutWriter writer(layout.value());
    writer.set_string(49U, "BUY");
    writer.set_string(56U, "SELL");
    writer.set_string(5001U, "LIT");
    auto fw_party = writer.add_group_entry(453U);
    fw_party.set_string(448U, "PTY1").set_char(447U, 'D').set_int(452U, 1);

    fastfix::codec::EncodeBuffer fw_buffer;
    REQUIRE(writer.encode_to_buffer(dictionary.value(), options, &fw_buffer).ok());

    // Encoded output should be identical.
    CHECK(mb_buffer.text() == fw_buffer.text());

    // Also verify decode roundtrip.
    auto decoded = fastfix::codec::DecodeFixMessageView(fw_buffer.bytes(), dictionary.value());
    REQUIRE(decoded.ok());
    auto parsed_view = decoded.value().message.view();
    CHECK(parsed_view.msg_type() == "D");
    REQUIRE(parsed_view.get_string(49U).has_value());
    CHECK(parsed_view.get_string(49U).value() == "BUY");
    auto parsed_group = parsed_view.group(453U);
    REQUIRE(parsed_group.has_value());
    REQUIRE(parsed_group->size() == 1U);
    CHECK((*parsed_group)[0].get_string(448U).value() == "PTY1");
}

TEST_CASE("fixed-layout-writer-all-value-types", "[message-api][fixed-layout]") {
    auto dictionary = LoadMessageApiDictionary();
    REQUIRE(dictionary.ok());

    auto layout = fastfix::message::FixedLayout::Build(dictionary.value(), "D");
    REQUIRE(layout.ok());

    fastfix::message::FixedLayoutWriter writer(layout.value());

    // All tags in this profile are string/int/char type: 49=string, 56=string, 5001=string, 5002=string.
    CHECK(writer.set_string(49U, "BUY"));
    CHECK(writer.set_string(56U, "SELL"));
    CHECK(writer.set_string(5001U, "LIT"));
    CHECK(writer.set_string(5002U, "ACC-1"));

    // Encode + decode roundtrip to verify all value types.
    fastfix::codec::EncodeOptions options;
    options.begin_string = "FIX.4.4";
    options.sender_comp_id = "BUY";
    options.target_comp_id = "SELL";
    options.msg_seq_num = 1U;
    options.sending_time = "20260406-12:00:00.000";

    fastfix::codec::EncodeBuffer buf;
    REQUIRE(writer.encode_to_buffer(dictionary.value(), options, &buf).ok());

    auto decoded = fastfix::codec::DecodeFixMessageView(buf.bytes(), dictionary.value());
    REQUIRE(decoded.ok());
    auto view = decoded.value().message.view();
    REQUIRE(view.get_string(49U).has_value());
    CHECK(view.get_string(49U).value() == "BUY");
    REQUIRE(view.get_string(56U).has_value());
    CHECK(view.get_string(56U).value() == "SELL");
    REQUIRE(view.get_string(5001U).has_value());
    CHECK(view.get_string(5001U).value() == "LIT");
    REQUIRE(view.get_string(5002U).has_value());
    CHECK(view.get_string(5002U).value() == "ACC-1");
}

TEST_CASE("fixed-layout-writer-encode-roundtrip", "[message-api][fixed-layout]") {
    auto dictionary = LoadMessageApiDictionary();
    REQUIRE(dictionary.ok());

    auto layout = fastfix::message::FixedLayout::Build(dictionary.value(), "D");
    REQUIRE(layout.ok());

    fastfix::message::FixedLayoutWriter writer(layout.value());
    writer.set_string(49U, "SENDER");
    writer.set_string(56U, "TARGET");
    writer.set_string(5001U, "LIT");
    writer.set_string(5002U, "ACC-1");
    auto party = writer.add_group_entry(453U);
    party.set_string(448U, "PTY1").set_char(447U, 'D').set_int(452U, 3);

    fastfix::codec::EncodeOptions options;
    options.begin_string = "FIX.4.4";
    options.sender_comp_id = "SENDER";
    options.target_comp_id = "TARGET";
    options.msg_seq_num = 7U;
    options.sending_time = "20260406-12:00:00.000";

    auto encoded = writer.encode(dictionary.value(), options);
    REQUIRE(encoded.ok());
    REQUIRE(!encoded.value().empty());

    // Decode and verify.
    auto decoded = fastfix::codec::DecodeFixMessageView(encoded.value(), dictionary.value());
    REQUIRE(decoded.ok());
    auto view = decoded.value().message.view();
    CHECK(view.msg_type() == "D");
    REQUIRE(view.get_string(49U).has_value());
    CHECK(view.get_string(49U).value() == "SENDER");
    REQUIRE(view.get_string(5001U).has_value());
    CHECK(view.get_string(5001U).value() == "LIT");
    auto group = view.group(453U);
    REQUIRE(group.has_value());
    REQUIRE(group->size() == 1U);
    CHECK((*group)[0].get_string(448U).value() == "PTY1");
}

TEST_CASE("fixed-layout-writer-extra-fields-hybrid-path", "[message-api][fixed-layout]") {
    auto dictionary = LoadMessageApiDictionary();
    REQUIRE(dictionary.ok());

    auto layout = fastfix::message::FixedLayout::Build(dictionary.value(), "D");
    REQUIRE(layout.ok());

    fastfix::message::FixedLayoutWriter writer(layout.value());

    // Set known layout fields via normal O(1) setters.
    REQUIRE(writer.set_string(49U, "SENDER"));
    REQUIRE(writer.set_string(56U, "TARGET"));
    REQUIRE(writer.set_string(5001U, "LIT"));

    // Set extra fields NOT in the layout via hybrid path.
    writer.set_extra_string(9999U, "CUSTOM_VAL");
    writer.set_extra_int(9998U, 42);
    writer.set_extra_char(9997U, 'Z');
    writer.set_extra_boolean(9996U, true);

    // Encode to buffer and decode — extra fields should survive roundtrip.
    fastfix::codec::EncodeOptions options;
    options.begin_string = "FIX.4.4";
    options.sender_comp_id = "SENDER";
    options.target_comp_id = "TARGET";
    options.msg_seq_num = 1U;
    options.sending_time = "20260406-12:00:00.000";

    fastfix::codec::EncodeBuffer buf;
    REQUIRE(writer.encode_to_buffer(dictionary.value(), options, &buf).ok());

    // The raw wire should contain our extra tag.
    const auto wire = buf.text();
    CHECK(wire.find("9999=CUSTOM_VAL") != std::string_view::npos);
    CHECK(wire.find("9998=42") != std::string_view::npos);
    CHECK(wire.find("9997=Z") != std::string_view::npos);
    CHECK(wire.find("9996=Y") != std::string_view::npos);

    // Decode and verify the extra field is accessible.
    auto decoded = fastfix::codec::DecodeFixMessageView(buf.bytes(), dictionary.value());
    REQUIRE(decoded.ok());
    auto parsed = decoded.value().message.view();
    REQUIRE(parsed.get_string(9999U).has_value());
    CHECK(parsed.get_string(9999U).value() == "CUSTOM_VAL");

    // Second writer: extra fields should survive encode roundtrip.
    fastfix::message::FixedLayoutWriter writer2(layout.value());
    writer2.set_string(49U, "S");
    writer2.set_extra_string(8888U, "EXT");
    fastfix::codec::EncodeBuffer buf2;
    REQUIRE(writer2.encode_to_buffer(dictionary.value(), options, &buf2).ok());
    const auto wire2 = buf2.text();
    CHECK(wire2.find("8888=EXT") != std::string_view::npos);
}