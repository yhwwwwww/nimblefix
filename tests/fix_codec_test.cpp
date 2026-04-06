#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <vector>

#include "fastfix/codec/fix_codec.h"
#include "fastfix/codec/simd_scan.h"
#include "fastfix/profile/artifact_builder.h"
#include "fastfix/profile/normalized_dictionary.h"
#include "fastfix/profile/profile_loader.h"

#include "test_support.h"

namespace {

auto LoadCodecDictionary() -> fastfix::base::Result<fastfix::profile::NormalizedDictionaryView> {
    fastfix::profile::NormalizedDictionary dictionary;
    dictionary.profile_id = 7001U;
    dictionary.schema_hash = 0x7001700170017001ULL;
    dictionary.fields = {
        {35U, "MsgType", fastfix::profile::ValueType::kString, 0U},
        {49U, "SenderCompID", fastfix::profile::ValueType::kString, 0U},
        {56U, "TargetCompID", fastfix::profile::ValueType::kString, 0U},
        {11U, "ClOrdID", fastfix::profile::ValueType::kString, 0U},
        {55U, "Symbol", fastfix::profile::ValueType::kString, 0U},
        {552U, "NoSides", fastfix::profile::ValueType::kInt, 0U},
        {54U, "Side", fastfix::profile::ValueType::kChar, 0U},
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
                {11U, static_cast<std::uint32_t>(fastfix::profile::FieldRuleFlags::kRequired)},
                {55U, static_cast<std::uint32_t>(fastfix::profile::FieldRuleFlags::kRequired)},
                {552U, static_cast<std::uint32_t>(fastfix::profile::FieldRuleFlags::kRequired)},
            },
            .flags = 0U,
        },
    };
    dictionary.groups = {
        fastfix::profile::GroupDef{
            .count_tag = 552U,
            .delimiter_tag = 54U,
            .name = "Sides",
            .field_rules = {
                {54U, static_cast<std::uint32_t>(fastfix::profile::FieldRuleFlags::kRequired)},
                {453U, 0U},
            },
            .flags = 0U,
        },
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
    if (!artifact.ok()) {
        return artifact.status();
    }
    const auto artifact_path = std::filesystem::temp_directory_path() / "fastfix-codec-test.art";
    auto write_status = fastfix::profile::WriteProfileArtifact(artifact_path, artifact.value());
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

TEST_CASE("fix-codec", "[fix-codec]") {
    const auto ToReadableFrame = [](const std::vector<std::byte>& bytes) {
        std::string text;
        text.reserve(bytes.size());
        for (const auto value : bytes) {
            const auto ch = static_cast<char>(std::to_integer<unsigned char>(value));
            text.push_back(ch == '\x01' ? '|' : ch);
        }
        return text;
    };

    auto dictionary = LoadCodecDictionary();
    REQUIRE(dictionary.ok());

    fastfix::message::MessageBuilder builder("D");
    builder.set_string(35U, "D")
        .set_string(49U, "BUY")
        .set_string(56U, "SELL")
        .set_string(11U, "ORD-1")
        .set_string(55U, "AAPL");
    auto side = builder.add_group_entry(552U);
    side.set_char(54U, '1');
    auto party = side.add_group_entry(453U);
    party.set_string(448U, "PARTY-1");

    const auto message = std::move(builder).build();
    fastfix::codec::EncodeOptions options;
    options.begin_string = "FIX.4.4";
    options.sender_comp_id = "BUY";
    options.target_comp_id = "SELL";
    options.msg_seq_num = 1U;
    options.sending_time = "20260402-12:00:00.000";

    auto encoded = fastfix::codec::EncodeFixMessage(message, dictionary.value(), options);
    REQUIRE(encoded.ok());
    REQUIRE(!encoded.value().empty());

    fastfix::codec::EncodeBuffer reusable_buffer;
    auto buffered_status = fastfix::codec::EncodeFixMessageToBuffer(
        message,
        dictionary.value(),
        options,
        &reusable_buffer);
    REQUIRE(buffered_status.ok());
    REQUIRE(reusable_buffer.size() == encoded.value().size());
    REQUIRE(std::equal(
        reusable_buffer.bytes().begin(),
        reusable_buffer.bytes().end(),
        encoded.value().begin(),
        encoded.value().end()));

    auto compiled_template = fastfix::codec::CompileFrameEncodeTemplate(
        dictionary.value(),
        "D",
        fastfix::codec::EncodeTemplateConfig{
            .begin_string = "FIX.4.4",
            .sender_comp_id = "BUY",
            .target_comp_id = "SELL",
            .delimiter = fastfix::codec::kFixSoh,
        });
    REQUIRE(compiled_template.ok());

    auto template_encoded = compiled_template.value().Encode(message, options);
    REQUIRE(template_encoded.ok());
    REQUIRE(template_encoded.value() == encoded.value());

    auto template_buffer_status = compiled_template.value().EncodeToBuffer(message, options, &reusable_buffer);
    REQUIRE(template_buffer_status.ok());
    REQUIRE(reusable_buffer.size() == template_encoded.value().size());
    REQUIRE(std::equal(
        reusable_buffer.bytes().begin(),
        reusable_buffer.bytes().end(),
        template_encoded.value().begin(),
        template_encoded.value().end()));

    auto decoded = fastfix::codec::DecodeFixMessage(encoded.value(), dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "D");
    REQUIRE(decoded.value().header.msg_seq_num == 1U);
    REQUIRE(!decoded.value().validation_issue.present());
    REQUIRE(decoded.value().message.view().group(552U).has_value());
    REQUIRE(decoded.value().message.view().group(552U)->size() == 1U);
    REQUIRE((*decoded.value().message.view().group(552U))[0].get_char(54U).value() == '1');
    REQUIRE((*decoded.value().message.view().group(552U))[0].group(453U).has_value());
    REQUIRE((*(*decoded.value().message.view().group(552U))[0].group(453U))[0].get_string(448U).value() == "PARTY-1");

    auto decoded_view = fastfix::codec::DecodeFixMessageView(encoded.value(), dictionary.value());
    REQUIRE(decoded_view.ok());
    REQUIRE(decoded_view.value().header.msg_type == "D");
    REQUIRE(decoded_view.value().header.msg_seq_num == 1U);
    REQUIRE(decoded_view.value().header.sending_time == "20260402-12:00:00.000");
    REQUIRE(decoded_view.value().raw.size() == encoded.value().size());
    REQUIRE(std::equal(
        decoded_view.value().raw.begin(),
        decoded_view.value().raw.end(),
        encoded.value().begin(),
        encoded.value().end()));
    const auto raw_sides = decoded_view.value().message.view().raw_group(552U);
    REQUIRE(raw_sides.has_value());
    REQUIRE(raw_sides->size() == 1U);
    REQUIRE((*raw_sides)[0].field(54U).value() == "1");
    REQUIRE((*raw_sides)[0].field_at(0U).value().tag == 54U);
    REQUIRE((*raw_sides)[0].field_at(0U).value().value == "1");
    const auto raw_parties = (*raw_sides)[0].group(453U);
    REQUIRE(raw_parties.has_value());
    REQUIRE(raw_parties->size() == 1U);
    REQUIRE((*raw_parties)[0].field(448U).value() == "PARTY-1");
    REQUIRE(!message.view().raw_group(552U).has_value());

    fastfix::message::MessageRef parsed_owned_ref;
    {
        auto decoded_for_ref = fastfix::codec::DecodeFixMessageView(encoded.value(), dictionary.value());
        REQUIRE(decoded_for_ref.ok());
        parsed_owned_ref = fastfix::message::MessageRef::OwnParsed(
            std::move(decoded_for_ref.value().message),
            decoded_for_ref.value().raw);
    }
    REQUIRE(parsed_owned_ref.valid());
    REQUIRE(parsed_owned_ref.owns_message());
    const auto parsed_owned_group = parsed_owned_ref.view().group(552U);
    REQUIRE(parsed_owned_group.has_value());
    REQUIRE(parsed_owned_group->size() == 1U);
    REQUIRE((*parsed_owned_group)[0].get_char(54U).value() == '1');
    REQUIRE((*(*parsed_owned_group)[0].group(453U))[0].get_string(448U).value() == "PARTY-1");

    auto peeked_view = fastfix::codec::PeekSessionHeaderView(encoded.value());
    REQUIRE(peeked_view.ok());
    REQUIRE(peeked_view.value().begin_string == "FIX.4.4");
    REQUIRE(peeked_view.value().msg_type == "D");
    REQUIRE(peeked_view.value().sender_comp_id == "BUY");
    REQUIRE(peeked_view.value().target_comp_id == "SELL");

    auto unknown = fastfix::codec::DecodeFixMessage(
        ::fastfix::tests::EncodeFixFrame(
            "35=D|34=1|49=BUY|56=SELL|52=20260402-12:00:00.000|11=ORD-1|55=AAPL|9999=BAD|"),
        dictionary.value());
    REQUIRE(unknown.ok());
    REQUIRE(unknown.value().validation_issue.kind == fastfix::codec::ValidationIssueKind::kUnknownField);
    REQUIRE(unknown.value().validation_issue.tag == 9999U);

    auto disallowed = fastfix::codec::DecodeFixMessage(
        ::fastfix::tests::EncodeFixFrame(
            "35=D|34=1|49=BUY|56=SELL|52=20260402-12:00:00.000|11=ORD-1|55=AAPL|448=ORPHAN|"),
        dictionary.value());
    REQUIRE(disallowed.ok());
    REQUIRE(disallowed.value().validation_issue.kind == fastfix::codec::ValidationIssueKind::kFieldNotAllowed);
    REQUIRE(disallowed.value().validation_issue.tag == 448U);

    auto duplicate = fastfix::codec::DecodeFixMessage(
        ::fastfix::tests::EncodeFixFrame(
            "35=D|34=1|49=BUY|56=SELL|52=20260402-12:00:00.000|11=ORD-1|55=AAPL|55=MSFT|"),
        dictionary.value());
    REQUIRE(duplicate.ok());
    REQUIRE(duplicate.value().validation_issue.kind == fastfix::codec::ValidationIssueKind::kDuplicateField);
    REQUIRE(duplicate.value().validation_issue.tag == 55U);

    auto out_of_order = fastfix::codec::DecodeFixMessage(
        ::fastfix::tests::EncodeFixFrame(
            "35=D|34=1|49=BUY|56=SELL|52=20260402-12:00:00.000|55=AAPL|11=ORD-1|"),
        dictionary.value());
    REQUIRE(out_of_order.ok());
    REQUIRE(out_of_order.value().validation_issue.kind == fastfix::codec::ValidationIssueKind::kFieldOutOfOrder);
    REQUIRE(out_of_order.value().validation_issue.tag == 11U);

    auto invalid_group = fastfix::codec::DecodeFixMessage(
        ::fastfix::tests::EncodeFixFrame(
            "35=D|34=1|49=BUY|56=SELL|52=20260402-12:00:00.000|11=ORD-1|55=AAPL|552=1|"),
        dictionary.value());
    REQUIRE(invalid_group.ok());
    REQUIRE(invalid_group.value().validation_issue.kind == fastfix::codec::ValidationIssueKind::kIncorrectNumInGroupCount);
    REQUIRE(invalid_group.value().validation_issue.tag == 552U);

    fastfix::message::MessageBuilder reordered_builder("D");
    reordered_builder.set_string(35U, "D")
        .set_string(49U, "BUY")
        .set_string(56U, "SELL")
        .set_string(55U, "MSFT")
        .set_string(11U, "ORD-2")
        .set_string(9999U, "EXTRA");
    auto reordered_side = reordered_builder.add_group_entry(552U);
    reordered_side.set_char(54U, '2');
    auto reordered_party = reordered_side.add_group_entry(453U);
    reordered_party.set_string(448U, "PARTY-2");

    options.msg_seq_num = 7U;
    options.sending_time = "20260402-12:00:01.000";
    const auto reordered_message = std::move(reordered_builder).build();
    auto reordered_encoded = fastfix::codec::EncodeFixMessage(
        reordered_message,
        dictionary.value(),
        options);
    REQUIRE(reordered_encoded.ok());

    auto reordered_buffer_status = fastfix::codec::EncodeFixMessageToBuffer(
        reordered_message,
        dictionary.value(),
        options,
        &reusable_buffer);
    REQUIRE(reordered_buffer_status.ok());
    REQUIRE(reusable_buffer.size() == reordered_encoded.value().size());
    REQUIRE(std::equal(
        reusable_buffer.bytes().begin(),
        reusable_buffer.bytes().end(),
        reordered_encoded.value().begin(),
        reordered_encoded.value().end()));

    const auto readable = ToReadableFrame(reordered_encoded.value());
    const auto cl_ord_id = readable.find("|11=ORD-2|");
    const auto symbol = readable.find("|55=MSFT|");
    const auto sides = readable.find("|552=1|");
    const auto extra = readable.find("|9999=EXTRA|");
    REQUIRE(cl_ord_id != std::string::npos);
    REQUIRE(symbol != std::string::npos);
    REQUIRE(sides != std::string::npos);
    REQUIRE(extra != std::string::npos);
    REQUIRE(cl_ord_id < symbol);
    REQUIRE(symbol < sides);
    REQUIRE(sides < extra);

    fastfix::codec::UtcTimestampBuffer timestamp_buffer;
    const auto current_timestamp = fastfix::codec::CurrentUtcTimestamp(&timestamp_buffer);
    REQUIRE(current_timestamp.size() == fastfix::codec::kUtcTimestampLength);
    REQUIRE(current_timestamp[8] == '-');
    REQUIRE(current_timestamp[11] == ':');
    REQUIRE(current_timestamp[14] == ':');
    REQUIRE(current_timestamp[17] == '.');

    auto auto_time_options = options;
    auto_time_options.sending_time = {};
    auto auto_time_encoded = fastfix::codec::EncodeFixMessage(
        reordered_message,
        dictionary.value(),
        auto_time_options);
    REQUIRE(auto_time_encoded.ok());
    auto auto_time_decoded = fastfix::codec::DecodeFixMessageView(auto_time_encoded.value(), dictionary.value());
    REQUIRE(auto_time_decoded.ok());
    REQUIRE(auto_time_decoded.value().header.sending_time.size() == fastfix::codec::kUtcTimestampLength);
    REQUIRE(auto_time_decoded.value().header.sending_time[8] == '-');
    REQUIRE(auto_time_decoded.value().header.sending_time[17] == '.');

    auto reordered_decoded = fastfix::codec::DecodeFixMessage(reordered_encoded.value(), dictionary.value());
    REQUIRE(reordered_decoded.ok());
    REQUIRE(reordered_decoded.value().validation_issue.kind == fastfix::codec::ValidationIssueKind::kUnknownField);
    REQUIRE(reordered_decoded.value().validation_issue.tag == 9999U);

    fastfix::message::MessageBuilder spill_builder("D");
    spill_builder.set_string(35U, "D").set_string(49U, "BUY").set_string(56U, "SELL");
    for (int index = 0; index < 10; ++index) {
        auto spill_side = spill_builder.add_group_entry(552U);
        spill_side.set_char(54U, (index % 2) == 0 ? '1' : '2');
        auto spill_party = spill_side.add_group_entry(453U);
        spill_party.set_string(448U, "PARTY-" + std::to_string(index));
    }

    options.msg_seq_num = 11U;
    options.sending_time = "20260402-12:00:02.000";
    auto spill_encoded = fastfix::codec::EncodeFixMessage(
        std::move(spill_builder).build(),
        dictionary.value(),
        options);
    REQUIRE(spill_encoded.ok());

    auto spill_decoded = fastfix::codec::DecodeFixMessageView(spill_encoded.value(), dictionary.value());
    REQUIRE(spill_decoded.ok());
    const auto spill_sides = spill_decoded.value().message.view().group(552U);
    REQUIRE(spill_sides.has_value());
    REQUIRE(spill_sides->size() == 10U);
    REQUIRE((*spill_sides)[0].group(453U).has_value());
    REQUIRE((*spill_sides)[9].group(453U).has_value());
    REQUIRE((*(*spill_sides)[0].group(453U))[0].get_string(448U).value() == "PARTY-0");
    REQUIRE((*(*spill_sides)[9].group(453U))[0].get_string(448U).value() == "PARTY-9");
}

TEST_CASE("SIMD SOH scan correctness", "[simd-scan]") {
    // Build a buffer with SOH at known positions
    std::vector<std::byte> buf(64, static_cast<std::byte>('A'));
    buf[7] = static_cast<std::byte>(0x01);
    buf[23] = static_cast<std::byte>(0x01);
    buf[63] = static_cast<std::byte>(0x01);

    const auto needle = static_cast<std::byte>(0x01);
    const auto* first = fastfix::codec::FindByte(buf.data(), buf.size(), needle);
    REQUIRE(first == buf.data() + 7);

    const auto* second = fastfix::codec::FindByte(first + 1, buf.size() - 8, needle);
    REQUIRE(second == buf.data() + 23);

    const auto* third = fastfix::codec::FindByte(second + 1, buf.size() - 24, needle);
    REQUIRE(third == buf.data() + 63);
}

TEST_CASE("SIMD equals scan correctness", "[simd-scan]") {
    std::vector<std::byte> buf(48, static_cast<std::byte>('X'));
    buf[3] = static_cast<std::byte>('=');
    buf[19] = static_cast<std::byte>('=');
    buf[47] = static_cast<std::byte>('=');

    const auto needle = static_cast<std::byte>('=');
    const auto* first = fastfix::codec::FindByte(buf.data(), buf.size(), needle);
    REQUIRE(first == buf.data() + 3);

    const auto* second = fastfix::codec::FindByte(first + 1, buf.size() - 4, needle);
    REQUIRE(second == buf.data() + 19);

    const auto* third = fastfix::codec::FindByte(second + 1, buf.size() - 20, needle);
    REQUIRE(third == buf.data() + 47);
}

TEST_CASE("SIMD scan with unaligned data", "[simd-scan]") {
    // Allocate an oversized buffer and test with various alignment offsets
    std::vector<std::byte> storage(80, static_cast<std::byte>('Z'));
    const auto needle = static_cast<std::byte>(0x01);

    for (std::size_t offset = 0; offset < 16; ++offset) {
        // Reset
        std::fill(storage.begin(), storage.end(), static_cast<std::byte>('Z'));

        const auto* base = storage.data() + offset;
        const auto len = storage.size() - offset - 1; // leave room

        // Place SOH at a position past the first 16-byte chunk
        const auto target_pos = std::min<std::size_t>(17, len - 1);
        const_cast<std::byte*>(base)[target_pos] = needle;

        const auto* result = fastfix::codec::FindByte(base, len, needle);
        REQUIRE(result == base + target_pos);
    }
}

TEST_CASE("SIMD scan no match", "[simd-scan]") {
    std::vector<std::byte> buf(100, static_cast<std::byte>('B'));
    const auto needle = static_cast<std::byte>(0x01);

    const auto* result = fastfix::codec::FindByte(buf.data(), buf.size(), needle);
    REQUIRE(result == buf.data() + buf.size());
}

TEST_CASE("SIMD scan short buffer", "[simd-scan]") {
    // Test buffers shorter than 16 bytes to exercise scalar fallback
    for (std::size_t len = 0; len <= 15; ++len) {
        std::vector<std::byte> buf(len, static_cast<std::byte>('C'));
        const auto needle = static_cast<std::byte>('C');

        if (len == 0) {
            const auto* result = fastfix::codec::FindByte(buf.data(), 0, needle);
            REQUIRE(result == buf.data());
            continue;
        }

        // Should find the first byte
        const auto* result = fastfix::codec::FindByte(buf.data(), buf.size(), needle);
        REQUIRE(result == buf.data());

        // Place target at the last position only
        std::fill(buf.begin(), buf.end(), static_cast<std::byte>('D'));
        buf.back() = needle;
        // needle is 'C', buf.back() is now 'C'
        const auto* last = fastfix::codec::FindByte(buf.data(), buf.size(), needle);
        REQUIRE(last == buf.data() + len - 1);
    }
}

// ---------------------------------------------------------------------------
// Codec negative / edge-case tests
// ---------------------------------------------------------------------------

namespace {

auto BuildFrameWithBodyLength(
    std::string_view body_fields,
    std::uint32_t declared_body_length,
    std::string_view begin_string = "FIX.4.4") -> std::vector<std::byte> {
    std::string body(body_fields);
    for (auto& ch : body) {
        if (ch == '|') {
            ch = '\x01';
        }
    }

    std::string full;
    full.append("8=");
    full.append(begin_string);
    full.push_back('\x01');
    full.append("9=");
    full.append(std::to_string(declared_body_length));
    full.push_back('\x01');
    full.append(body);

    std::uint32_t checksum = 0;
    for (const auto ch : full) {
        checksum += static_cast<unsigned char>(ch);
    }
    checksum %= 256U;

    std::ostringstream stream;
    stream << "10=" << std::setw(3) << std::setfill('0') << checksum << '\x01';
    full.append(stream.str());
    return fastfix::tests::Bytes(full);
}

}  // namespace

TEST_CASE("codec negative: bad checksum value", "[fix-codec][negative]") {
    auto dictionary = LoadCodecDictionary();
    REQUIRE(dictionary.ok());

    auto frame = fastfix::tests::EncodeFixFrame(
        "35=D|34=1|49=BUY|56=SELL|52=20260402-12:00:00.000|11=ORD-1|55=AAPL|");
    std::string text(reinterpret_cast<const char*>(frame.data()), frame.size());
    auto pos = text.rfind("10=");
    REQUIRE(pos != std::string::npos);
    // Replace checksum with 999 (guaranteed mismatch; valid range is 0-255)
    text[pos + 3] = '9';
    text[pos + 4] = '9';
    text[pos + 5] = '9';
    auto tampered = fastfix::tests::Bytes(text);

    auto result = fastfix::codec::DecodeFixMessage(tampered, dictionary.value());
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.status().code() == fastfix::base::ErrorCode::kFormatError);

    auto peek = fastfix::codec::PeekSessionHeader(tampered);
    REQUIRE_FALSE(peek.ok());
    REQUIRE(peek.status().code() == fastfix::base::ErrorCode::kFormatError);
}

TEST_CASE("codec negative: incorrect BodyLength", "[fix-codec][negative]") {
    auto dictionary = LoadCodecDictionary();
    REQUIRE(dictionary.ok());

    SECTION("BodyLength too short") {
        auto frame = BuildFrameWithBodyLength(
            "35=D|34=1|49=BUY|56=SELL|52=20260402-12:00:00.000|11=ORD-1|55=AAPL|",
            5U);
        auto result = fastfix::codec::DecodeFixMessage(frame, dictionary.value());
        REQUIRE_FALSE(result.ok());
        REQUIRE(result.status().code() == fastfix::base::ErrorCode::kFormatError);
    }

    SECTION("BodyLength too long") {
        auto frame = BuildFrameWithBodyLength(
            "35=D|34=1|49=BUY|56=SELL|52=20260402-12:00:00.000|11=ORD-1|55=AAPL|",
            99999U);
        auto result = fastfix::codec::DecodeFixMessage(frame, dictionary.value());
        REQUIRE_FALSE(result.ok());
        REQUIRE(result.status().code() == fastfix::base::ErrorCode::kFormatError);
    }
}

TEST_CASE("codec negative: repeating group count=0", "[fix-codec][negative]") {
    auto dictionary = LoadCodecDictionary();
    REQUIRE(dictionary.ok());

    // Group count is 0 but a group delimiter tag follows — tag 54 should be
    // treated as a misplaced body field, not a group entry.
    auto frame = fastfix::tests::EncodeFixFrame(
        "35=D|34=1|49=BUY|56=SELL|52=20260402-12:00:00.000|11=ORD-1|55=AAPL|552=0|54=1|");
    auto result = fastfix::codec::DecodeFixMessage(frame, dictionary.value());
    REQUIRE(result.ok());
    REQUIRE(result.value().validation_issue.present());
    REQUIRE(result.value().validation_issue.kind == fastfix::codec::ValidationIssueKind::kFieldNotAllowed);
    REQUIRE(result.value().validation_issue.tag == 54U);
}

TEST_CASE("codec negative: empty message body", "[fix-codec][negative]") {
    auto dictionary = LoadCodecDictionary();
    REQUIRE(dictionary.ok());

    // Only standard header fields, no application-level body fields.
    auto frame = fastfix::tests::EncodeFixFrame(
        "35=D|34=1|49=BUY|56=SELL|52=20260402-12:00:00.000|");
    auto result = fastfix::codec::DecodeFixMessage(frame, dictionary.value());
    REQUIRE(result.ok());
    REQUIRE(result.value().header.msg_type == "D");
    REQUIRE(result.value().header.msg_seq_num == 1U);
}

TEST_CASE("codec negative: oversized message >64KB", "[fix-codec][negative]") {
    auto dictionary = LoadCodecDictionary();
    REQUIRE(dictionary.ok());

    std::string big_value(70000, 'X');
    std::string body = "35=D|34=1|49=BUY|56=SELL|52=20260402-12:00:00.000|11=" +
                        big_value + "|55=AAPL|";
    auto frame = fastfix::tests::EncodeFixFrame(body);
    REQUIRE(frame.size() > 65536U);

    // Must not crash — either succeeds or returns an explicit error.
    auto result = fastfix::codec::DecodeFixMessage(frame, dictionary.value());
    if (result.ok()) {
        REQUIRE(result.value().header.msg_type == "D");
    } else {
        REQUIRE(result.status().code() != fastfix::base::ErrorCode::kOk);
    }

    auto peek = fastfix::codec::PeekSessionHeader(frame);
    REQUIRE(peek.ok());
    REQUIRE(peek.value().msg_type == "D");
}

TEST_CASE("codec negative: missing BeginString", "[fix-codec][negative]") {
    auto dictionary = LoadCodecDictionary();
    REQUIRE(dictionary.ok());

    // Frame starts with tag 9 instead of tag 8.
    std::string body = "35=D" "\x01" "34=1" "\x01" "49=BUY" "\x01" "56=SELL" "\x01" "52=20260402-12:00:00.000" "\x01";
    std::string full;
    full.append("9=");
    full.append(std::to_string(body.size()));
    full.push_back('\x01');
    full.append(body);

    std::uint32_t checksum = 0;
    for (const auto ch : full) {
        checksum += static_cast<unsigned char>(ch);
    }
    checksum %= 256U;
    std::ostringstream stream;
    stream << "10=" << std::setw(3) << std::setfill('0') << checksum << '\x01';
    full.append(stream.str());

    auto bytes = fastfix::tests::Bytes(full);

    auto result = fastfix::codec::DecodeFixMessage(bytes, dictionary.value());
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.status().code() == fastfix::base::ErrorCode::kFormatError);

    auto peek = fastfix::codec::PeekSessionHeader(bytes);
    REQUIRE_FALSE(peek.ok());
    REQUIRE(peek.status().code() == fastfix::base::ErrorCode::kFormatError);
}

TEST_CASE("codec negative: illegal MsgType", "[fix-codec][negative]") {
    auto dictionary = LoadCodecDictionary();
    REQUIRE(dictionary.ok());

    auto frame = fastfix::tests::EncodeFixFrame(
        "35=ZZ|34=1|49=BUY|56=SELL|52=20260402-12:00:00.000|11=ORD-1|55=AAPL|");
    auto result = fastfix::codec::DecodeFixMessage(frame, dictionary.value());
    // Unrecognized MsgType is accepted gracefully — no crash, no hard error.
    REQUIRE(result.ok());
    REQUIRE(result.value().header.msg_type == "ZZ");
}

TEST_CASE("codec negative: truncated frame", "[fix-codec][negative]") {
    auto dictionary = LoadCodecDictionary();
    REQUIRE(dictionary.ok());

    auto full_frame = fastfix::tests::EncodeFixFrame(
        "35=D|34=1|49=BUY|56=SELL|52=20260402-12:00:00.000|11=ORD-1|55=AAPL|");
    REQUIRE(full_frame.size() > 20U);

    // Cut frame in the middle of a field.
    auto truncated = std::vector<std::byte>(
        full_frame.begin(),
        full_frame.begin() + static_cast<std::ptrdiff_t>(full_frame.size() / 2));

    auto result = fastfix::codec::DecodeFixMessage(truncated, dictionary.value());
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.status().code() == fastfix::base::ErrorCode::kFormatError);

    auto peek = fastfix::codec::PeekSessionHeader(truncated);
    REQUIRE_FALSE(peek.ok());
    REQUIRE(peek.status().code() == fastfix::base::ErrorCode::kFormatError);
}

TEST_CASE("codec negative: field with no value", "[fix-codec][negative]") {
    // Construct a frame where tag 58 has an empty value: "58=\x01".
    std::string body = "35=D" "\x01" "34=1" "\x01" "49=BUY" "\x01" "56=SELL" "\x01" "52=20260402-12:00:00.000" "\x01" "58=" "\x01";

    std::string full;
    full.append("8=FIX.4.4\x01");
    full.append("9=");
    full.append(std::to_string(body.size()));
    full.push_back('\x01');
    full.append(body);

    std::uint32_t checksum = 0;
    for (const auto ch : full) {
        checksum += static_cast<unsigned char>(ch);
    }
    checksum %= 256U;
    std::ostringstream stream;
    stream << "10=" << std::setw(3) << std::setfill('0') << checksum << '\x01';
    full.append(stream.str());

    auto bytes = fastfix::tests::Bytes(full);
    auto dictionary = LoadCodecDictionary();
    REQUIRE(dictionary.ok());

    auto result = fastfix::codec::DecodeFixMessage(bytes, dictionary.value());
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.status().code() == fastfix::base::ErrorCode::kFormatError);
}

TEST_CASE("codec negative: duplicate tag in message", "[fix-codec][negative]") {
    auto dictionary = LoadCodecDictionary();
    REQUIRE(dictionary.ok());

    // Tag 11 (ClOrdID) appears twice outside of any repeating group.
    auto frame = fastfix::tests::EncodeFixFrame(
        "35=D|34=1|49=BUY|56=SELL|52=20260402-12:00:00.000|11=ORD-1|11=ORD-2|55=AAPL|");
    auto result = fastfix::codec::DecodeFixMessage(frame, dictionary.value());
    REQUIRE(result.ok());
    REQUIRE(result.value().validation_issue.present());
    REQUIRE(result.value().validation_issue.kind == fastfix::codec::ValidationIssueKind::kDuplicateField);
    REQUIRE(result.value().validation_issue.tag == 11U);
}

TEST_CASE("PrecompiledTemplateTable build, find, and encode", "[fix-codec][precompiled-table]") {
    auto dictionary = LoadCodecDictionary();
    REQUIRE(dictionary.ok());

    fastfix::codec::EncodeTemplateConfig config{
        .begin_string = "FIX.4.4",
        .sender_comp_id = "BUY",
        .target_comp_id = "SELL",
    };

    auto table = fastfix::codec::PrecompiledTemplateTable::Build(dictionary.value(), config);
    REQUIRE(table.ok());
    REQUIRE_FALSE(table.value().empty());
    REQUIRE(table.value().size() >= 1U);

    // Known message type "D" should be found
    const auto* tmpl = table.value().find("D");
    REQUIRE(tmpl != nullptr);
    REQUIRE(tmpl->valid());
    REQUIRE(tmpl->msg_type() == "D");

    // Unknown message type should return nullptr
    REQUIRE(table.value().find("ZZ") == nullptr);

    // Build a message and encode via the precompiled table overload
    fastfix::message::MessageBuilder builder("D");
    builder.set_string(35U, "D")
        .set_string(49U, "BUY")
        .set_string(56U, "SELL")
        .set_string(11U, "ORD-1")
        .set_string(55U, "AAPL");
    auto side = builder.add_group_entry(552U);
    side.set_char(54U, '1');
    auto party = side.add_group_entry(453U);
    party.set_string(448U, "PARTY-1");
    const auto message = std::move(builder).build();

    fastfix::codec::EncodeOptions options;
    options.begin_string = "FIX.4.4";
    options.sender_comp_id = "BUY";
    options.target_comp_id = "SELL";
    options.msg_seq_num = 1U;
    options.sending_time = "20260402-12:00:00.000";

    // Encode via the global-cache path (precompiled = nullptr)
    fastfix::codec::EncodeBuffer buffer_global;
    auto status_global = fastfix::codec::EncodeFixMessageToBuffer(
        message, dictionary.value(), options, &buffer_global, nullptr);
    REQUIRE(status_global.ok());

    // Encode via the precompiled table path
    fastfix::codec::EncodeBuffer buffer_precompiled;
    auto status_precompiled = fastfix::codec::EncodeFixMessageToBuffer(
        message, dictionary.value(), options, &buffer_precompiled, &table.value());
    REQUIRE(status_precompiled.ok());

    // Both paths must produce identical output
    REQUIRE(buffer_global.size() == buffer_precompiled.size());
    REQUIRE(std::equal(
        buffer_global.bytes().begin(),
        buffer_global.bytes().end(),
        buffer_precompiled.bytes().begin(),
        buffer_precompiled.bytes().end()));

    // Also verify the template directly produces the same result
    fastfix::codec::EncodeBuffer buffer_direct;
    auto status_direct = tmpl->EncodeToBuffer(message, options, &buffer_direct);
    REQUIRE(status_direct.ok());
    REQUIRE(buffer_direct.size() == buffer_global.size());
    REQUIRE(std::equal(
        buffer_direct.bytes().begin(),
        buffer_direct.bytes().end(),
        buffer_global.bytes().begin(),
        buffer_global.bytes().end()));
}