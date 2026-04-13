#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "fastfix/codec/raw_passthrough.h"
#include "test_support.h"

namespace {

auto ToStringView(std::span<const std::byte> bytes) -> std::string_view {
    return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

}  // namespace

TEST_CASE("raw-passthrough decode extracts session fields and raw body", "[raw-passthrough]") {
    // Build a FIX message with known session fields and application body.
    // Body fields: 11=ORD-1, 55=AAPL, 54=1, 38=100
    const auto frame = fastfix::tests::EncodeFixFrame(
        "35=D|49=SENDER|56=TARGET|34=42|52=20260413-10:00:00.000|"
        "11=ORD-1|55=AAPL|54=1|38=100|");

    auto result = fastfix::codec::DecodeRawPassThrough(
        std::span<const std::byte>(frame.data(), frame.size()));
    REQUIRE(result.ok());

    const auto& view = result.value();
    CHECK(view.valid);
    CHECK(view.begin_string == "FIX.4.4");
    CHECK(view.msg_type == "D");
    CHECK(view.msg_seq_num == 42U);
    CHECK(view.sender_comp_id == "SENDER");
    CHECK(view.target_comp_id == "TARGET");
    CHECK(view.sending_time == "20260413-10:00:00.000");

    // The raw_body should contain the application fields: 11=ORD-1|55=AAPL|54=1|38=100|
    auto body_sv = ToStringView(view.raw_body);
    CHECK(body_sv.find("11=ORD-1") != std::string_view::npos);
    CHECK(body_sv.find("55=AAPL") != std::string_view::npos);
    CHECK(body_sv.find("54=1") != std::string_view::npos);
    CHECK(body_sv.find("38=100") != std::string_view::npos);

    // Session fields should NOT be in raw_body
    CHECK(body_sv.find("35=D") == std::string_view::npos);
    CHECK(body_sv.find("49=SENDER") == std::string_view::npos);
    CHECK(body_sv.find("56=TARGET") == std::string_view::npos);
    CHECK(body_sv.find("34=42") == std::string_view::npos);
}

TEST_CASE("raw-passthrough forwarding rewrites header and preserves body", "[raw-passthrough]") {
    const auto frame = fastfix::tests::EncodeFixFrame(
        "35=D|49=ORIG_SENDER|56=ORIG_TARGET|34=10|52=20260413-10:00:00.000|"
        "11=ORD-1|55=MSFT|54=2|38=200|44=150.25|");

    auto decoded = fastfix::codec::DecodeRawPassThrough(
        std::span<const std::byte>(frame.data(), frame.size()));
    REQUIRE(decoded.ok());

    fastfix::codec::ForwardingOptions opts;
    opts.sender_comp_id = "DOWNSTREAM_S";
    opts.target_comp_id = "DOWNSTREAM_T";
    opts.msg_seq_num = 77;
    opts.sending_time = "20260413-11:00:00.000";

    fastfix::codec::EncodeBuffer buffer;
    auto status = fastfix::codec::EncodeForwarded(decoded.value(), opts, &buffer);
    REQUIRE(status.ok());

    // Now decode the forwarded message using raw-passthrough again to verify it
    auto fwd_data = buffer.bytes();
    auto fwd_decoded = fastfix::codec::DecodeRawPassThrough(fwd_data);
    REQUIRE(fwd_decoded.ok());

    const auto& fwd = fwd_decoded.value();
    CHECK(fwd.valid);
    CHECK(fwd.begin_string == "FIX.4.4");
    CHECK(fwd.msg_type == "D");
    CHECK(fwd.msg_seq_num == 77U);
    CHECK(fwd.sender_comp_id == "DOWNSTREAM_S");
    CHECK(fwd.target_comp_id == "DOWNSTREAM_T");
    CHECK(fwd.sending_time == "20260413-11:00:00.000");

    // Body fields should be preserved exactly
    auto body_sv = ToStringView(fwd.raw_body);
    CHECK(body_sv.find("11=ORD-1") != std::string_view::npos);
    CHECK(body_sv.find("55=MSFT") != std::string_view::npos);
    CHECK(body_sv.find("54=2") != std::string_view::npos);
    CHECK(body_sv.find("38=200") != std::string_view::npos);
    CHECK(body_sv.find("44=150.25") != std::string_view::npos);
}

TEST_CASE("raw-passthrough forwarding with routing fields", "[raw-passthrough]") {
    const auto frame = fastfix::tests::EncodeFixFrame(
        "35=D|49=SENDER|56=TARGET|34=1|52=20260413-10:00:00.000|"
        "11=ORD-1|55=GOOG|");

    auto decoded = fastfix::codec::DecodeRawPassThrough(
        std::span<const std::byte>(frame.data(), frame.size()));
    REQUIRE(decoded.ok());

    fastfix::codec::ForwardingOptions opts;
    opts.sender_comp_id = "GW";
    opts.target_comp_id = "EXCH";
    opts.msg_seq_num = 5;
    opts.sending_time = "20260413-12:00:00.000";
    opts.on_behalf_of_comp_id = "SENDER";
    opts.deliver_to_comp_id = "TARGET";

    fastfix::codec::EncodeBuffer buffer;
    auto status = fastfix::codec::EncodeForwarded(decoded.value(), opts, &buffer);
    REQUIRE(status.ok());

    // Check that the routing fields are present in the output
    auto text = buffer.text();
    CHECK(text.find("115=SENDER") != std::string_view::npos);
    CHECK(text.find("128=TARGET") != std::string_view::npos);

    // Verify it's still a valid FIX message
    auto fwd_decoded = fastfix::codec::DecodeRawPassThrough(buffer.bytes());
    REQUIRE(fwd_decoded.ok());
    CHECK(fwd_decoded.value().sender_comp_id == "GW");
    CHECK(fwd_decoded.value().target_comp_id == "EXCH");
}

TEST_CASE("raw-passthrough preserves unknown application fields exactly", "[raw-passthrough]") {
    // Use non-standard tags in the body to simulate unknown/proprietary fields
    const auto frame = fastfix::tests::EncodeFixFrame(
        "35=D|49=S|56=T|34=1|52=20260413-10:00:00.000|"
        "11=ORD-1|9999=CUSTOM_VALUE|5555=ANOTHER|");

    auto decoded = fastfix::codec::DecodeRawPassThrough(
        std::span<const std::byte>(frame.data(), frame.size()));
    REQUIRE(decoded.ok());

    // Forward with new session identity
    fastfix::codec::ForwardingOptions opts;
    opts.sender_comp_id = "A";
    opts.target_comp_id = "B";
    opts.msg_seq_num = 1;
    opts.sending_time = "20260413-13:00:00.000";

    fastfix::codec::EncodeBuffer buffer;
    auto status = fastfix::codec::EncodeForwarded(decoded.value(), opts, &buffer);
    REQUIRE(status.ok());

    // The body should contain the proprietary fields verbatim
    auto fwd_decoded = fastfix::codec::DecodeRawPassThrough(buffer.bytes());
    REQUIRE(fwd_decoded.ok());

    auto body_sv = ToStringView(fwd_decoded.value().raw_body);
    CHECK(body_sv.find("9999=CUSTOM_VALUE") != std::string_view::npos);
    CHECK(body_sv.find("5555=ANOTHER") != std::string_view::npos);
    CHECK(body_sv.find("11=ORD-1") != std::string_view::npos);
}

TEST_CASE("raw-passthrough forwarding with overridden BeginString", "[raw-passthrough]") {
    const auto frame = fastfix::tests::EncodeFixFrame(
        "35=D|49=S|56=T|34=1|52=20260413-10:00:00.000|11=X|",
        "FIX.4.4");

    auto decoded = fastfix::codec::DecodeRawPassThrough(
        std::span<const std::byte>(frame.data(), frame.size()));
    REQUIRE(decoded.ok());
    CHECK(decoded.value().begin_string == "FIX.4.4");

    fastfix::codec::ForwardingOptions opts;
    opts.sender_comp_id = "A";
    opts.target_comp_id = "B";
    opts.msg_seq_num = 1;
    opts.sending_time = "20260413-10:00:00.000";
    opts.begin_string = "FIX.4.2";

    fastfix::codec::EncodeBuffer buffer;
    auto status = fastfix::codec::EncodeForwarded(decoded.value(), opts, &buffer);
    REQUIRE(status.ok());

    auto fwd = fastfix::codec::DecodeRawPassThrough(buffer.bytes());
    REQUIRE(fwd.ok());
    CHECK(fwd.value().begin_string == "FIX.4.2");
}

TEST_CASE("raw-passthrough decode rejects bad checksum", "[raw-passthrough]") {
    auto frame = fastfix::tests::EncodeFixFrame(
        "35=D|49=S|56=T|34=1|52=20260413-10:00:00.000|11=X|");

    // Corrupt a byte in the body to invalidate checksum
    frame[frame.size() - 5] = static_cast<std::byte>('9');

    auto result = fastfix::codec::DecodeRawPassThrough(
        std::span<const std::byte>(frame.data(), frame.size()));
    CHECK(!result.ok());
}

TEST_CASE("raw-passthrough decode rejects missing tag 8", "[raw-passthrough]") {
    auto bytes = fastfix::tests::Bytes("9=5\x01" "35=D\x01" "10=000\x01");
    auto result = fastfix::codec::DecodeRawPassThrough(
        std::span<const std::byte>(bytes.data(), bytes.size()));
    CHECK(!result.ok());
}
