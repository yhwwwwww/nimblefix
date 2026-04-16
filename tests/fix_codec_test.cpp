#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <vector>

#include "fastfix/codec/fix_tags.h"
#include "fastfix/codec/fix_codec.h"
#include "fastfix/codec/simd_scan.h"

#include "test_support.h"

using namespace fastfix::codec::tags;

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

    auto dictionary = fastfix::tests::LoadFix44DictionaryView();
    if (!dictionary.ok()) {
        SKIP("FIX44 artifact not available: " << dictionary.status().message());
    }

    fastfix::message::MessageBuilder builder("D");
    builder.set_string(kMsgType, "D")
        .set_string(kSenderCompID, "BUY")
        .set_string(kTargetCompID, "SELL")
        .set_string(kClOrdID, "ORD-1")
        .set_string(kSymbol, "AAPL");
    auto party = builder.add_group_entry(kNoPartyIDs);
    party.set_string(kPartyID, "PARTY-1").set_char(kPartyIDSource, 'D').set_int(kPartyRole, 3);

    const auto message = std::move(builder).build();
    fastfix::codec::EncodeOptions options;
    options.begin_string = "FIX.4.4";
    options.sender_comp_id = "BUY";
    options.sender_sub_id = "DESK-1";
    options.target_comp_id = "SELL";
    options.target_sub_id = "ROUTE-2";
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
    REQUIRE(decoded.value().header.sender_sub_id == "DESK-1");
    REQUIRE(decoded.value().header.target_sub_id == "ROUTE-2");
    REQUIRE(!decoded.value().validation_issue.present());
    REQUIRE(decoded.value().message.view().group(kNoPartyIDs).has_value());
    REQUIRE(decoded.value().message.view().group(kNoPartyIDs)->size() == 1U);
    REQUIRE((*decoded.value().message.view().group(kNoPartyIDs))[0].get_string(kPartyID).value() == "PARTY-1");

    auto decoded_view = fastfix::codec::DecodeFixMessageView(encoded.value(), dictionary.value());
    REQUIRE(decoded_view.ok());
    REQUIRE(decoded_view.value().header.msg_type == "D");
    REQUIRE(decoded_view.value().header.msg_seq_num == 1U);
    REQUIRE(decoded_view.value().header.sender_sub_id == "DESK-1");
    REQUIRE(decoded_view.value().header.target_sub_id == "ROUTE-2");
    REQUIRE(decoded_view.value().header.sending_time == "20260402-12:00:00.000");
    REQUIRE(decoded_view.value().raw.size() == encoded.value().size());
    REQUIRE(std::equal(
        decoded_view.value().raw.begin(),
        decoded_view.value().raw.end(),
        encoded.value().begin(),
        encoded.value().end()));
    const auto raw_parties = decoded_view.value().message.view().raw_group(kNoPartyIDs);
    REQUIRE(raw_parties.has_value());
    REQUIRE(raw_parties->size() == 1U);
    REQUIRE((*raw_parties)[0].field(kPartyID).value() == "PARTY-1");
    REQUIRE((*raw_parties)[0].field_at(0U).value().tag == kPartyID);
    REQUIRE((*raw_parties)[0].field_at(0U).value().value == "PARTY-1");
    REQUIRE(!message.view().raw_group(kNoPartyIDs).has_value());

    fastfix::codec::DecodedMessageView reusable_decoded;
    auto reusable_status = fastfix::codec::DecodeFixMessageView(
        encoded.value(),
        dictionary.value(),
        &reusable_decoded);
    REQUIRE(reusable_status.ok());
    REQUIRE(reusable_decoded.header.msg_type == "D");
    REQUIRE(reusable_decoded.message.view().group(kNoPartyIDs).has_value());

    fastfix::message::MessageBuilder cancel_builder("F");
    cancel_builder.set_string(kMsgType, "F")
        .set_string(kSenderCompID, "BUY")
        .set_string(kTargetCompID, "SELL")
        .set_string(kClOrdID, "ORD-2")
        .set_string(41U, "ORD-1")
        .set_string(kSymbol, "MSFT");
    options.msg_seq_num = 2U;
    options.sending_time = "20260402-12:00:00.001";
    auto cancel_encoded = fastfix::codec::EncodeFixMessage(
        std::move(cancel_builder).build(),
        dictionary.value(),
        options);
    REQUIRE(cancel_encoded.ok());

    reusable_status = fastfix::codec::DecodeFixMessageView(
        cancel_encoded.value(),
        dictionary.value(),
        &reusable_decoded);
    REQUIRE(reusable_status.ok());
    REQUIRE(reusable_decoded.header.msg_type == "F");
    REQUIRE(reusable_decoded.header.msg_seq_num == 2U);
    REQUIRE(reusable_decoded.message.view().msg_type() == "F");
    REQUIRE(reusable_decoded.message.view().get_string(41U).value() == "ORD-1");
    REQUIRE(!reusable_decoded.message.view().group(kNoPartyIDs).has_value());
    REQUIRE(reusable_decoded.raw.size() == cancel_encoded.value().size());

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
    const auto parsed_owned_group = parsed_owned_ref.view().group(kNoPartyIDs);
    REQUIRE(parsed_owned_group.has_value());
    REQUIRE(parsed_owned_group->size() == 1U);
    REQUIRE((*parsed_owned_group)[0].get_string(kPartyID).value() == "PARTY-1");

    auto peeked_view = fastfix::codec::PeekSessionHeaderView(encoded.value());
    REQUIRE(peeked_view.ok());
    REQUIRE(peeked_view.value().begin_string == "FIX.4.4");
    REQUIRE(peeked_view.value().msg_type == "D");
    REQUIRE(peeked_view.value().sender_comp_id == "BUY");
    REQUIRE(peeked_view.value().sender_sub_id == "DESK-1");
    REQUIRE(peeked_view.value().target_comp_id == "SELL");
    REQUIRE(peeked_view.value().target_sub_id == "ROUTE-2");

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
    REQUIRE(disallowed.value().validation_issue.tag == kPartyID);

    auto duplicate = fastfix::codec::DecodeFixMessage(
        ::fastfix::tests::EncodeFixFrame(
            "35=D|34=1|49=BUY|56=SELL|52=20260402-12:00:00.000|11=ORD-1|55=AAPL|55=MSFT|"),
        dictionary.value());
    REQUIRE(duplicate.ok());
    REQUIRE(duplicate.value().validation_issue.kind == fastfix::codec::ValidationIssueKind::kDuplicateField);
    REQUIRE(duplicate.value().validation_issue.tag == kSymbol);

    auto out_of_order = fastfix::codec::DecodeFixMessage(
        ::fastfix::tests::EncodeFixFrame(
            "35=D|34=1|49=BUY|56=SELL|52=20260402-12:00:00.000|55=AAPL|11=ORD-1|"),
        dictionary.value());
    REQUIRE(out_of_order.ok());
    // Ordering validation removed — out-of-order fields are no longer flagged.
    REQUIRE(!out_of_order.value().validation_issue.present());

    auto invalid_group = fastfix::codec::DecodeFixMessage(
        ::fastfix::tests::EncodeFixFrame(
            "35=D|34=1|49=BUY|56=SELL|52=20260402-12:00:00.000|11=ORD-1|55=AAPL|453=1|"),
        dictionary.value());
    REQUIRE(invalid_group.ok());
    REQUIRE(invalid_group.value().validation_issue.present());
    REQUIRE(invalid_group.value().validation_issue.tag == kNoPartyIDs);

    fastfix::message::MessageBuilder reordered_builder("D");
    reordered_builder.set_string(kMsgType, "D")
        .set_string(kSenderCompID, "BUY")
        .set_string(kTargetCompID, "SELL")
        .set_string(kSymbol, "MSFT")
        .set_string(kClOrdID, "ORD-2")
        .set_string(9999U, "EXTRA");
    auto reordered_party = reordered_builder.add_group_entry(kNoPartyIDs);
    reordered_party.set_string(kPartyID, "PARTY-2").set_char(kPartyIDSource, 'D').set_int(kPartyRole, 3);

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
    const auto parties = readable.find("|453=1|");
    const auto extra = readable.find("|9999=EXTRA|");
    REQUIRE(cl_ord_id != std::string::npos);
    REQUIRE(symbol != std::string::npos);
    REQUIRE(parties != std::string::npos);
    REQUIRE(extra != std::string::npos);
    // Generic encode uses dictionary rule order: ClOrdID < Parties < Symbol, extras last.
    REQUIRE(cl_ord_id < parties);
    REQUIRE(parties < symbol);
    REQUIRE(symbol < extra);

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
    spill_builder.set_string(kMsgType, "D").set_string(kSenderCompID, "BUY").set_string(kTargetCompID, "SELL");
    for (int index = 0; index < 10; ++index) {
        auto spill_party = spill_builder.add_group_entry(kNoPartyIDs);
        spill_party.set_string(kPartyID, "PARTY-" + std::to_string(index))
            .set_char(kPartyIDSource, 'D')
            .set_int(kPartyRole, index % 10);
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
    const auto spill_parties = spill_decoded.value().message.view().group(kNoPartyIDs);
    REQUIRE(spill_parties.has_value());
    REQUIRE(spill_parties->size() == 10U);
    REQUIRE((*spill_parties)[0].get_string(kPartyID).value() == "PARTY-0");
    REQUIRE((*spill_parties)[9].get_string(kPartyID).value() == "PARTY-9");
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
    auto dictionary = fastfix::tests::LoadFix44DictionaryView();
    if (!dictionary.ok()) {
        SKIP("FIX44 artifact not available: " << dictionary.status().message());
    }

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
    auto dictionary = fastfix::tests::LoadFix44DictionaryView();
    if (!dictionary.ok()) {
        SKIP("FIX44 artifact not available: " << dictionary.status().message());
    }

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
    auto dictionary = fastfix::tests::LoadFix44DictionaryView();
    if (!dictionary.ok()) {
        SKIP("FIX44 artifact not available: " << dictionary.status().message());
    }

    // Group count is 0 but a group delimiter tag follows — tag 448 should be
    // treated as a misplaced field outside any group context.
    auto frame = fastfix::tests::EncodeFixFrame(
        "35=D|34=1|49=BUY|56=SELL|52=20260402-12:00:00.000|11=ORD-1|55=AAPL|453=0|448=P1|");
    auto result = fastfix::codec::DecodeFixMessage(frame, dictionary.value());
    REQUIRE(result.ok());
    REQUIRE(result.value().validation_issue.present());
}

TEST_CASE("codec negative: empty message body", "[fix-codec][negative]") {
    auto dictionary = fastfix::tests::LoadFix44DictionaryView();
    if (!dictionary.ok()) {
        SKIP("FIX44 artifact not available: " << dictionary.status().message());
    }

    // Only standard header fields, no application-level body fields.
    auto frame = fastfix::tests::EncodeFixFrame(
        "35=D|34=1|49=BUY|56=SELL|52=20260402-12:00:00.000|");
    auto result = fastfix::codec::DecodeFixMessage(frame, dictionary.value());
    REQUIRE(result.ok());
    REQUIRE(result.value().header.msg_type == "D");
    REQUIRE(result.value().header.msg_seq_num == 1U);
}

TEST_CASE("codec negative: oversized message >64KB", "[fix-codec][negative]") {
    auto dictionary = fastfix::tests::LoadFix44DictionaryView();
    if (!dictionary.ok()) {
        SKIP("FIX44 artifact not available: " << dictionary.status().message());
    }

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
    auto dictionary = fastfix::tests::LoadFix44DictionaryView();
    if (!dictionary.ok()) {
        SKIP("FIX44 artifact not available: " << dictionary.status().message());
    }

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
    auto dictionary = fastfix::tests::LoadFix44DictionaryView();
    if (!dictionary.ok()) {
        SKIP("FIX44 artifact not available: " << dictionary.status().message());
    }

    auto frame = fastfix::tests::EncodeFixFrame(
        "35=ZZ|34=1|49=BUY|56=SELL|52=20260402-12:00:00.000|11=ORD-1|55=AAPL|");
    auto result = fastfix::codec::DecodeFixMessage(frame, dictionary.value());
    // Unrecognized MsgType is accepted gracefully — no crash, no hard error.
    REQUIRE(result.ok());
    REQUIRE(result.value().header.msg_type == "ZZ");
}

TEST_CASE("codec negative: truncated frame", "[fix-codec][negative]") {
    auto dictionary = fastfix::tests::LoadFix44DictionaryView();
    if (!dictionary.ok()) {
        SKIP("FIX44 artifact not available: " << dictionary.status().message());
    }

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
    auto dictionary = fastfix::tests::LoadFix44DictionaryView();
    if (!dictionary.ok()) {
        SKIP("FIX44 artifact not available: " << dictionary.status().message());
    }

    auto result = fastfix::codec::DecodeFixMessage(bytes, dictionary.value());
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.status().code() == fastfix::base::ErrorCode::kFormatError);
}

TEST_CASE("codec negative: duplicate tag in message", "[fix-codec][negative]") {
    auto dictionary = fastfix::tests::LoadFix44DictionaryView();
    if (!dictionary.ok()) {
        SKIP("FIX44 artifact not available: " << dictionary.status().message());
    }

    // Tag 11 (ClOrdID) appears twice outside of any repeating group.
    auto frame = fastfix::tests::EncodeFixFrame(
        "35=D|34=1|49=BUY|56=SELL|52=20260402-12:00:00.000|11=ORD-1|11=ORD-2|55=AAPL|");
    auto result = fastfix::codec::DecodeFixMessage(frame, dictionary.value());
    REQUIRE(result.ok());
    REQUIRE(result.value().validation_issue.present());
    REQUIRE(result.value().validation_issue.kind == fastfix::codec::ValidationIssueKind::kDuplicateField);
    REQUIRE(result.value().validation_issue.tag == kClOrdID);
}

TEST_CASE("PrecompiledTemplateTable build, find, and encode", "[fix-codec][precompiled-table]") {
    auto dictionary = fastfix::tests::LoadFix44DictionaryView();
    if (!dictionary.ok()) {
        SKIP("FIX44 artifact not available: " << dictionary.status().message());
    }

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
    builder.set_string(kMsgType, "D")
        .set_string(kSenderCompID, "BUY")
        .set_string(kTargetCompID, "SELL")
        .set_string(kClOrdID, "ORD-1")
        .set_string(kSymbol, "AAPL");
    auto side = builder.add_group_entry(kNoSides);
    side.set_char(kSide, '1');
    auto party = side.add_group_entry(kNoPartyIDs);
    party.set_string(kPartyID, "PARTY-1");
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

// ===========================================================================
// Malicious input resource exhaustion tests
// ===========================================================================

TEST_CASE("fix-codec: group count exceeding kMaxGroupEntryCount", "[fix-codec][resource-exhaustion]") {
    auto dictionary = fastfix::tests::LoadFix44DictionaryView();
    if (!dictionary.ok()) {
        SKIP("FIX44 artifact not available: " << dictionary.status().message());
    }

    // Build a FIX frame with NoPartyIDs=99999, exceeding kMaxGroupEntryCount (10000)
    // Just the count tag without any actual entries
    auto wire = ::fastfix::tests::EncodeFixFrame(
        "35=D|34=1|49=BUY|56=SELL|52=20260402-12:00:00.000|11=ORD-1|55=AAPL|453=99999|448=P1|");

    auto decoded = fastfix::codec::DecodeFixMessage(wire, dictionary.value());
    REQUIRE(!decoded.ok());  // Must reject, not OOM

    auto decoded_view = fastfix::codec::DecodeFixMessageView(wire, dictionary.value());
    REQUIRE(!decoded_view.ok());  // View path must also reject
}

TEST_CASE("fix-codec: group count at exact boundary", "[fix-codec][resource-exhaustion]") {
    auto dictionary = fastfix::tests::LoadFix44DictionaryView();
    if (!dictionary.ok()) {
        SKIP("FIX44 artifact not available: " << dictionary.status().message());
    }

    // Test at kMaxGroupEntryCount + 1 = 10001
    auto wire = ::fastfix::tests::EncodeFixFrame(
        "35=D|34=1|49=BUY|56=SELL|52=20260402-12:00:00.000|11=ORD-1|55=AAPL|453=10001|448=P1|");

    auto decoded = fastfix::codec::DecodeFixMessage(wire, dictionary.value());
    REQUIRE(!decoded.ok());

    auto decoded_view = fastfix::codec::DecodeFixMessageView(wire, dictionary.value());
    REQUIRE(!decoded_view.ok());
}

TEST_CASE("fix-codec: very long message body", "[fix-codec][resource-exhaustion]") {
    auto dictionary = fastfix::tests::LoadFix44DictionaryView();
    if (!dictionary.ok()) {
        SKIP("FIX44 artifact not available: " << dictionary.status().message());
    }

    // Build a message with a very large field value (1MB of padding)
    std::string big_value(1'000'000, 'X');
    std::string body = "35=D|34=1|49=BUY|56=SELL|52=20260402-12:00:00.000|11=ORD-1|55=" + big_value + "|";
    auto wire = ::fastfix::tests::EncodeFixFrame(body);

    // Should not crash or OOM — may succeed or return a validation issue
    auto decoded = fastfix::codec::DecodeFixMessage(wire, dictionary.value());
    // We don't care about the result value, just that it didn't crash/hang
    (void)decoded;

    auto decoded_view = fastfix::codec::DecodeFixMessageView(wire, dictionary.value());
    (void)decoded_view;

    // Peek should also handle it gracefully
    auto peeked = fastfix::codec::PeekSessionHeaderView(wire);
    (void)peeked;
}

TEST_CASE("fix-codec: deeply nested groups beyond kMaxGroupNestingDepth", "[fix-codec][resource-exhaustion]") {
    // The FIX44 dictionary has Parties(453) as a flat group.
    // kMaxGroupNestingDepth=16 is checked at parse time.
    // We can't easily build 16+ levels with only 1 group type,
    // but we can verify the limit enforcement works by testing
    // with the existing 1-level nesting and by constructing a
    // dictionary with deeper nesting capability.

    // First, verify single-level group nesting works fine (below limit)
    auto dictionary = fastfix::tests::LoadFix44DictionaryView();
    if (!dictionary.ok()) {
        SKIP("FIX44 artifact not available: " << dictionary.status().message());
    }

    auto wire = ::fastfix::tests::EncodeFixFrame(
        "35=D|34=1|49=BUY|56=SELL|52=20260402-12:00:00.000|11=ORD-1|55=AAPL|453=1|448=P1|447=D|452=3|");
    auto decoded = fastfix::codec::DecodeFixMessage(wire, dictionary.value());
    REQUIRE(decoded.ok());  // 1-level nesting is fine

    // Now build a dictionary with many self-referencing-like group layers
    // to exceed kMaxGroupNestingDepth (16)
    fastfix::profile::NormalizedDictionary deep_dict;
    deep_dict.profile_id = 7099U;
    deep_dict.schema_hash = 0x7099709970997099ULL;

    // We need 17 group levels. Create tags: 5001..5034 for count/delimiter pairs
    // Level 0: count=5001, delimiter=5002
    // Level 1: count=5003, delimiter=5004
    // ...
    // Level 16: count=5033, delimiter=5034

    std::vector<fastfix::profile::FieldDef> fields = {
        {kMsgType, "MsgType", fastfix::profile::ValueType::kString, 0U},
        {kSenderCompID, "SenderCompID", fastfix::profile::ValueType::kString, 0U},
        {kTargetCompID, "TargetCompID", fastfix::profile::ValueType::kString, 0U},
    };
    for (std::uint32_t level = 0; level <= 16; ++level) {
        std::uint32_t count_tag = 5001U + level * 2U;
        std::uint32_t delim_tag = 5002U + level * 2U;
        fields.push_back({count_tag, "Count" + std::to_string(level), fastfix::profile::ValueType::kInt, 0U});
        fields.push_back({delim_tag, "Delim" + std::to_string(level), fastfix::profile::ValueType::kString, 0U});
    }
    deep_dict.fields = std::move(fields);

    // Top-level message uses level-0 group
    deep_dict.messages = {
        fastfix::profile::MessageDef{
            .msg_type = "U1",
            .name = "DeepTest",
            .field_rules = {
                {kMsgType, static_cast<std::uint32_t>(fastfix::profile::FieldRuleFlags::kRequired)},
                {5001U, 0U},
            },
            .flags = 0U,
        },
    };

    // Each group at level N contains the count tag for level N+1
    std::vector<fastfix::profile::GroupDef> groups;
    for (std::uint32_t level = 0; level <= 16; ++level) {
        std::uint32_t count_tag = 5001U + level * 2U;
        std::uint32_t delim_tag = 5002U + level * 2U;
        std::vector<fastfix::profile::FieldRule> group_field_rules = {
            {delim_tag, static_cast<std::uint32_t>(fastfix::profile::FieldRuleFlags::kRequired)},
        };
        // Add nested group count tag if not at the deepest level
        if (level < 16U) {
            std::uint32_t next_count_tag = 5003U + level * 2U;
            group_field_rules.push_back({next_count_tag, 0U});
        }
        groups.push_back(fastfix::profile::GroupDef{
            .count_tag = count_tag,
            .delimiter_tag = delim_tag,
            .name = "Group" + std::to_string(level),
            .field_rules = std::move(group_field_rules),
            .flags = 0U,
        });
    }
    deep_dict.groups = std::move(groups);

    auto artifact = fastfix::profile::BuildProfileArtifact(deep_dict);
    REQUIRE(artifact.ok());
    const auto artifact_path = std::filesystem::temp_directory_path() / "fastfix-deep-nest-test.art";
    auto write_status = fastfix::profile::WriteProfileArtifact(artifact_path, artifact.value());
    REQUIRE(write_status.ok());
    auto loaded = fastfix::profile::LoadProfileArtifact(artifact_path);
    std::filesystem::remove(artifact_path);
    REQUIRE(loaded.ok());
    auto deep_dictionary = fastfix::profile::NormalizedDictionaryView::FromProfile(std::move(loaded).value());
    REQUIRE(deep_dictionary.ok());

    // Build a wire message with 17 nesting levels: each level has count=1, delimiter=value
    std::string body = "35=U1|34=1|49=BUY|56=SELL|52=20260402-12:00:00.000|";
    for (std::uint32_t level = 0; level <= 16; ++level) {
        std::uint32_t count_tag = 5001U + level * 2U;
        std::uint32_t delim_tag = 5002U + level * 2U;
        body += std::to_string(count_tag) + "=1|";
        body += std::to_string(delim_tag) + "=V" + std::to_string(level) + "|";
    }

    auto deep_wire = ::fastfix::tests::EncodeFixFrame(body);
    auto deep_decoded = fastfix::codec::DecodeFixMessage(deep_wire, deep_dictionary.value());
    REQUIRE(!deep_decoded.ok());  // Must reject: depth 17 > kMaxGroupNestingDepth(16)

    auto deep_decoded_view = fastfix::codec::DecodeFixMessageView(deep_wire, deep_dictionary.value());
    REQUIRE(!deep_decoded_view.ok());  // View path must also reject
}

