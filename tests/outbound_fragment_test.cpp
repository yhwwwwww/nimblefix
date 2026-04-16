#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <string_view>

#include "fix44_builders.h"
#include "fastfix/codec/fix_codec.h"
#include "fastfix/codec/fix_tags.h"
#include "fastfix/message/fixed_layout_writer.h"

#include "test_support.h"

namespace {

auto EncodedExtras(std::string_view header, std::string_view body) -> fastfix::codec::EncodedOutboundExtras {
    return fastfix::codec::EncodedOutboundExtras{
        .header_fragment = std::string(header),
        .body_fragment = std::string(body),
    };
}

auto ToReadableFrame(std::span<const std::byte> bytes) -> std::string {
    std::string text;
    text.reserve(bytes.size());
    for (const auto value : bytes) {
        const auto ch = static_cast<char>(std::to_integer<unsigned char>(value));
        text.push_back(ch == '\x01' ? '|' : ch);
    }
    return text;
}

}  // namespace

using namespace fastfix::codec::tags;

TEST_CASE("encoded fragments drive generic and template encoders end-to-end", "[fix-codec][outbound-fragment]") {
    auto dictionary = fastfix::tests::LoadFix44DictionaryView();
    if (!dictionary.ok()) {
        SKIP("FIX44 artifact not available: " << dictionary.status().message());
    }

    fastfix::message::MessageBuilder builder("D");
    builder.set_string(kMsgType, "D")
        .set_string(kClOrdID, "ORD-42")
        .set_string(kSymbol, "AAPL")
        .set_char(kSide, '1')
        .set_string(kTransactTime, "20260414-09:30:00.000")
        .set_int(kOrderQty, 100)
        .set_char(kOrdType, '2');
    const auto message = std::move(builder).build();

    const auto extras = EncodedExtras(
        "50=DESK-9\x01"
        "57=ROUTE-7\x01"
        "97=Y\x01"
        "115=CLIENT-A\x01"
        "128=VENUE-B\x01",
        "1=ACC-77\x01"
        "9999=TAIL\x01");

    fastfix::codec::EncodeOptions options;
    options.begin_string = "FIX.4.4";
    options.sender_comp_id = "BUY";
    options.target_comp_id = "SELL";
    options.msg_seq_num = 17U;
    options.sending_time = "20260414-09:30:01.000";

    auto encoded = fastfix::codec::EncodeFixMessage(message, dictionary.value(), options, extras.view());
    REQUIRE(encoded.ok());

    auto compiled = fastfix::codec::CompileFrameEncodeTemplate(
        dictionary.value(),
        "D",
        fastfix::codec::EncodeTemplateConfig{
            .begin_string = "FIX.4.4",
            .sender_comp_id = "BUY",
            .target_comp_id = "SELL",
            .delimiter = fastfix::codec::kFixSoh,
        });
    REQUIRE(compiled.ok());

    auto template_encoded = compiled.value().Encode(message, options, extras.view());
    REQUIRE(template_encoded.ok());
    CHECK(template_encoded.value() == encoded.value());

    auto decoded = fastfix::codec::DecodeFixMessageView(encoded.value(), dictionary.value());
    REQUIRE(decoded.ok());
    CHECK(decoded.value().header.msg_type == "D");
    CHECK(decoded.value().header.msg_seq_num == 17U);
    CHECK(decoded.value().header.sender_sub_id == "DESK-9");
    CHECK(decoded.value().header.target_sub_id == "ROUTE-7");
    CHECK(decoded.value().header.on_behalf_of_comp_id == "CLIENT-A");
    CHECK(decoded.value().header.deliver_to_comp_id == "VENUE-B");
    CHECK(decoded.value().header.poss_resend);
    REQUIRE(decoded.value().message.view().get_string(kAccount).has_value());
    CHECK(decoded.value().message.view().get_string(kAccount).value() == "ACC-77");

    auto peeked = fastfix::codec::PeekSessionHeaderView(encoded.value());
    REQUIRE(peeked.ok());
    CHECK(peeked.value().sender_sub_id == "DESK-9");
    CHECK(peeked.value().target_sub_id == "ROUTE-7");
    CHECK(peeked.value().on_behalf_of_comp_id == "CLIENT-A");
    CHECK(peeked.value().deliver_to_comp_id == "VENUE-B");
    CHECK(peeked.value().poss_resend);

    const auto text = ToReadableFrame(encoded.value());
    const auto sender_sub = text.find("|50=DESK-9|");
    const auto target_sub = text.find("|57=ROUTE-7|");
    const auto poss_resend = text.find("|97=Y|");
    const auto on_behalf = text.find("|115=CLIENT-A|");
    const auto deliver_to = text.find("|128=VENUE-B|");
    const auto cl_ord_id = text.find("|11=ORD-42|");
    const auto account = text.find("|1=ACC-77|");
    const auto tail = text.find("|9999=TAIL|");
    REQUIRE(sender_sub != std::string::npos);
    REQUIRE(target_sub != std::string::npos);
    REQUIRE(poss_resend != std::string::npos);
    REQUIRE(on_behalf != std::string::npos);
    REQUIRE(deliver_to != std::string::npos);
    REQUIRE(cl_ord_id != std::string::npos);
    REQUIRE(account != std::string::npos);
    REQUIRE(tail != std::string::npos);
    CHECK(sender_sub < cl_ord_id);
    CHECK(target_sub < cl_ord_id);
    CHECK(poss_resend < cl_ord_id);
    CHECK(on_behalf < cl_ord_id);
    CHECK(deliver_to < cl_ord_id);
    CHECK(cl_ord_id < account);
    CHECK(account < tail);
}

TEST_CASE("encoded fragments drive fixed layout writer end-to-end", "[fix-codec][outbound-fragment]") {
    auto dictionary = fastfix::tests::LoadFix44DictionaryView();
    if (!dictionary.ok()) {
        SKIP("FIX44 artifact not available: " << dictionary.status().message());
    }

    auto layout = fastfix::message::FixedLayout::Build(dictionary.value(), "D");
    REQUIRE(layout.ok());

    const auto extras = EncodedExtras(
        "50=DESK-9\x01"
        "57=ROUTE-7\x01"
        "97=Y\x01"
        "115=CLIENT-A\x01"
        "128=VENUE-B\x01",
        "1=ACC-77\x01"
        "9999=TAIL\x01");

    fastfix::message::FixedLayoutWriter writer(layout.value());
    writer.bind_session("FIX.4.4", "BUY", "SELL");
    writer.set_string(kClOrdID, "ORD-42")
        .set_string(kSymbol, "AAPL")
        .set_char(kSide, '1')
        .set_string(kTransactTime, "20260414-09:30:00.000")
        .set_int(kOrderQty, 100)
        .set_char(kOrdType, '2');

    fastfix::codec::EncodeOptions options;
    options.begin_string = "FIX.4.4";
    options.sender_comp_id = "BUY";
    options.target_comp_id = "SELL";
    options.msg_seq_num = 17U;
    options.sending_time = "20260414-09:30:01.000";

    fastfix::codec::EncodeBuffer buffer;
    auto status = writer.encode_to_buffer(dictionary.value(), options, extras.view(), &buffer);
    REQUIRE(status.ok());

    auto decoded = fastfix::codec::DecodeFixMessageView(buffer.bytes(), dictionary.value());
    REQUIRE(decoded.ok());
    CHECK(decoded.value().header.sender_sub_id == "DESK-9");
    CHECK(decoded.value().header.target_sub_id == "ROUTE-7");
    CHECK(decoded.value().header.on_behalf_of_comp_id == "CLIENT-A");
    CHECK(decoded.value().header.deliver_to_comp_id == "VENUE-B");
    CHECK(decoded.value().header.poss_resend);
    REQUIRE(decoded.value().message.view().get_string(kAccount).has_value());
    CHECK(decoded.value().message.view().get_string(kAccount).value() == "ACC-77");

    const auto text = ToReadableFrame(buffer.bytes());
    const auto sender_sub = text.find("|50=DESK-9|");
    const auto target_sub = text.find("|57=ROUTE-7|");
    const auto cl_ord_id = text.find("|11=ORD-42|");
    const auto account = text.find("|1=ACC-77|");
    const auto tail = text.find("|9999=TAIL|");
    REQUIRE(sender_sub != std::string::npos);
    REQUIRE(target_sub != std::string::npos);
    REQUIRE(cl_ord_id != std::string::npos);
    REQUIRE(account != std::string::npos);
    REQUIRE(tail != std::string::npos);
    CHECK(sender_sub < cl_ord_id);
    CHECK(target_sub < cl_ord_id);
    CHECK(cl_ord_id < account);
    CHECK(account < tail);
}

TEST_CASE("encoded fragments drive generated writer end-to-end", "[fix-codec][outbound-fragment]") {
    auto dictionary = fastfix::tests::LoadFix44DictionaryView();
    if (!dictionary.ok()) {
        SKIP("FIX44 artifact not available: " << dictionary.status().message());
    }

    auto layout = fastfix::message::FixedLayout::Build(dictionary.value(), "D");
    REQUIRE(layout.ok());

    const auto extras = EncodedExtras(
        "50=DESK-9\x01"
        "57=ROUTE-7\x01"
        "97=Y\x01"
        "115=CLIENT-A\x01"
        "128=VENUE-B\x01",
        "1=ACC-77\x01"
        "9999=TAIL\x01");

    fastfix::generated::profile_4400::NewOrderSingleWriter writer(layout.value());
    writer.bind_session("FIX.4.4", "BUY", "SELL");
    writer.set_cl_ord_id("ORD-42")
        .set_symbol("AAPL")
        .set_side('1')
        .set_transact_time("20260414-09:30:00.000")
        .set_order_qty(100)
        .set_ord_type('2');

    fastfix::codec::EncodeOptions options;
    options.begin_string = "FIX.4.4";
    options.sender_comp_id = "BUY";
    options.target_comp_id = "SELL";
    options.msg_seq_num = 17U;
    options.sending_time = "20260414-09:30:01.000";

    fastfix::codec::EncodeBuffer buffer;
    auto status = writer.encode_to_buffer(dictionary.value(), options, extras.view(), &buffer);
    REQUIRE(status.ok());

    auto decoded = fastfix::codec::DecodeFixMessageView(buffer.bytes(), dictionary.value());
    REQUIRE(decoded.ok());
    CHECK(decoded.value().header.sender_sub_id == "DESK-9");
    CHECK(decoded.value().header.target_sub_id == "ROUTE-7");
    CHECK(decoded.value().header.on_behalf_of_comp_id == "CLIENT-A");
    CHECK(decoded.value().header.deliver_to_comp_id == "VENUE-B");
    CHECK(decoded.value().header.poss_resend);
    REQUIRE(decoded.value().message.view().get_string(kAccount).has_value());
    CHECK(decoded.value().message.view().get_string(kAccount).value() == "ACC-77");

    const auto text = ToReadableFrame(buffer.bytes());
    const auto sender_sub = text.find("|50=DESK-9|");
    const auto cl_ord_id = text.find("|11=ORD-42|");
    const auto account = text.find("|1=ACC-77|");
    REQUIRE(sender_sub != std::string::npos);
    REQUIRE(cl_ord_id != std::string::npos);
    REQUIRE(account != std::string::npos);
    CHECK(sender_sub < cl_ord_id);
    CHECK(cl_ord_id < account);
}