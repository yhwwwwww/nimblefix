#include <nimblefix/codec.hpp>
#include <nimblefix/decimal.hpp>
#include <nimblefix/message.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void check(bool condition, const char* name) {
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", name);
    }
}

template <std::size_t N>
std::string_view as_view(const std::array<char, N>& output, std::size_t size) {
    return {output.data(), size};
}

void test_decimal() {
    using namespace nimblefix::literals;
    constexpr auto price = "150.250"_dec;
    static_assert(price.coefficient() == 150250);
    static_assert(price.scale() == 3);

    std::array<char, 32> text{};
    std::size_t size = 0;
    check(price.format(text, size).ok(), "decimal literal formats");
    check(std::string_view{text.data(), size} == "150.250", "decimal keeps scale");

    const auto check_decimal = [&](nimblefix::Decimal value, std::string_view expected, const char* name) {
        check(value.format(text, size).ok(), name);
        check(std::string_view{text.data(), size} == expected, name);
    };
    check_decimal("150.25"_dec, "150.25", "integer digits precede fraction");
    check_decimal("12345.678"_dec, "12345.678", "multiple integer and fraction digits");
    check_decimal("0.001"_dec, "0.001", "fraction below one retains leading zeroes");
    check_decimal("-0.010"_dec, "-0.010", "negative fractional trailing zeroes are retained");
    check_decimal("0.00"_dec, "0.00", "zero scale is retained");

    auto parsed = nimblefix::Decimal::parse("-9223372036854775808");
    check(parsed.ok(), "int64 minimum parses");
    if (parsed.ok()) {
        check(parsed.value().format(text, size).ok(), "int64 minimum formats");
        check(std::string_view{text.data(), size} == "-9223372036854775808", "int64 minimum is exact");
    }
    check(!nimblefix::Decimal::parse("1.2.3").ok(), "invalid decimal is rejected");
}

void test_fluent_encode() {
    using namespace nimblefix;
    using namespace nimblefix::literals;

    std::string input{"client=one"};
    auto message = nimblefix::message<"D">()
        .set<55>(input)
        .set<11>("second")
        .set<38>(100)
        .set<44>("150.25"_dec);
    input.assign("mutated");

    static_assert(decltype(message)::slot_count == 4);
    static_assert(decltype(message)::slot_storage_bytes == 4 * sizeof(detail::SlotValue));
    constexpr auto order = decltype(message)::wire_order;
    static_assert(order[0] == 55 && order[1] == 11 && order[2] == 38 && order[3] == 44);

    std::array<char, 512> output{};
    const auto encoded = encode_fix44(message, SessionHeader{
        .sender_comp_id = "BUY",
        .target_comp_id = "SELL",
        .msg_seq_num = 7,
        .sending_time = "20261026-12:30:45.123",
    }, output);
    check(encoded.ok(), "fluent message encodes");
    if (!encoded.ok()) return;

    const auto frame = as_view(output, encoded.value());
    check(frame.find("55=client=one\x01") != std::string_view::npos, "string input is owned before setter returns");
    check(frame.find("11=second\x01") != std::string_view::npos, "append order is retained without a schema");
    check(frame.find("38=100\x01") != std::string_view::npos, "integer is formatted directly");
    check(frame.find("44=150.25\x01") != std::string_view::npos, "exact decimal is formatted");
    check(frame.find("client=one") != std::string_view::npos, "equals is legal within a field value");

    const auto body_length_start = frame.find("9=") + 2;
    const auto body_length_end = frame.find('\x01', body_length_start);
    const auto checksum_start = frame.rfind("10=");
    std::size_t advertised_body_length = 0;
    for (std::size_t i = body_length_start; i < body_length_end; ++i) {
        advertised_body_length = advertised_body_length * 10 + static_cast<std::size_t>(frame[i] - '0');
    }
    check(advertised_body_length == checksum_start - (body_length_end + 1), "BodyLength counts body bytes");
    unsigned checksum = 0;
    for (std::size_t i = 0; i < checksum_start; ++i) checksum = (checksum + static_cast<unsigned char>(frame[i])) & 0xffU;
    const auto expected_checksum = static_cast<unsigned>((frame[checksum_start + 3] - '0') * 100 +
        (frame[checksum_start + 4] - '0') * 10 + (frame[checksum_start + 5] - '0'));
    check(checksum == expected_checksum, "CheckSum covers bytes before tag 10");

    std::array<char, 8> too_small;
    too_small.fill('Q');
    const auto failed = encode_fix44(message, SessionHeader{"BUY", "SELL", 7, "20261026-12:30:45.123"}, too_small);
    check(!failed.ok() && failed.status().code == ErrorCode::capacity_exceeded, "short output span is rejected");
    check(too_small[0] == 'Q' && too_small.back() == 'Q', "failed encode leaves output unchanged");
}

void test_fixed_shape() {
    using namespace nimblefix;
    using OrderShape = Fields<
        Field<11, FieldKind::string>,
        Field<55, FieldKind::string>,
        Field<38, FieldKind::signed_integer>>;
    FixedMessage<"D", OrderShape, 32> message;
    std::string transient{"id-1"};
    check(message.set<11>(transient).ok(), "fixed field accepts string");
    transient.assign("changed");
    check(message.set<55>("IBM=US").ok(), "fixed shape allows equals in value");

    constexpr auto view = FixedMessage<"D", OrderShape, 32>::slot_count;
    static_assert(view == 3);
    static_assert(FixedMessage<"D", OrderShape, 32>::slot_storage_bytes == 3 * sizeof(detail::SlotValue));
    const auto message_view = message.view();
    check(message_view.field_count == 3, "fixed layout holds only its declared slots");
    check(detail::is_present(message_view, 0) && detail::is_present(message_view, 1), "presence tracks filled slots");
    check(!detail::is_present(message_view, 2), "optional slot remains absent");

    std::array<char, 256> output{};
    const auto encoded = encode_fix44(message, SessionHeader{"BUY", "SELL", 1, "20261026-12:30:45"}, output);
    check(encoded.ok(), "fixed shape encodes");
    if (encoded.ok()) {
        const auto frame = as_view(output, encoded.value());
        check(frame.find("11=id-1\x01") != std::string_view::npos, "fixed field owns source text");
        check(frame.find("55=IBM=US\x01") != std::string_view::npos, "fixed shape emits optional fields in plan order");
        check(frame.find("38=") == std::string_view::npos, "unset fixed slot is omitted");
    }

    using TinyShape = Fields<Field<58, FieldKind::string>>;
    FixedMessage<"D", TinyShape, 2> tiny;
    check(tiny.set<58>("too long").code == ErrorCode::capacity_exceeded, "fixed arena is bounded");
    check(!tiny.status().ok(), "fixed shape keeps first error sticky");

    FixedMessage<"D", TinyShape, 1> reusable;
    check(reusable.set<58>("x").ok(), "fixed string fits its arena");
    check(reusable.set<58>("y").ok(), "fixed string assignment reuses its slot storage");
    reusable.reset();
    check(reusable.status().ok(), "fixed shape reset clears its sticky status");
    check(!detail::is_present(reusable.view(), 0), "fixed shape reset clears presence");
    check(reusable.view().value_byte_count == 0, "fixed shape reset clears used arena length");

    using TwoStrings = Fields<Field<11, FieldKind::string>, Field<55, FieldKind::string>>;
    FixedMessage<"D", TwoStrings, 5> compacted;
    check(compacted.set<11>("ab").ok() && compacted.set<55>("cd").ok(),
        "fixed strings initially share one bounded arena");
    check(compacted.set<11>("abc").ok(), "fixed growth reclaims the next string's shifted space");
    const auto compacted_view = compacted.view();
    check(compacted_view.value_byte_count == 5, "fixed replacement stores only current string bytes");
    check(std::string_view{compacted_view.value_bytes + compacted_view.slots[0].offset,
        compacted_view.slots[0].size} == "abc", "fixed growth updates the replaced field");
    check(std::string_view{compacted_view.value_bytes + compacted_view.slots[1].offset,
        compacted_view.slots[1].size} == "cd", "fixed growth shifts later strings safely");
    check(compacted.set<55>("z").ok() && compacted.set<11>("a").ok(),
        "fixed shrinking updates compact the arena");
    check(compacted.view().value_byte_count == 2, "fixed shrink releases prior arena bytes");
    check(compacted.set<55>("wxyz").ok(), "fixed growth can reuse bytes released by shrink");

    FixedMessage<"D", TwoStrings, 8> aliased_growth;
    check(aliased_growth.set<11>("a").ok() && aliased_growth.set<55>("long").ok(),
        "fixed alias setup fits the bounded arena");
    const auto alias_view = aliased_growth.view();
    const std::string_view later_value{alias_view.value_bytes + alias_view.slots[1].offset,
        alias_view.slots[1].size};
    check(aliased_growth.set<11>(later_value).ok(), "fixed growth remaps an aliased later value");
    const auto alias_after = aliased_growth.view();
    check(std::string_view{alias_after.value_bytes + alias_after.slots[0].offset,
        alias_after.slots[0].size} == "long", "fixed alias survives arena shifting");
}

void test_fluent_limits_and_delimiters() {
    using namespace nimblefix;
    auto too_many = message<"D">(MessageLimits{1, 4}).set<11>("id").set<55>("IBM");
    check(too_many.status().code == ErrorCode::capacity_exceeded, "fluent slot count is bounded");

    auto with_owned_values = message<"D">().set<11>("kept").set<55>("next");
    const auto borrowed_view = with_owned_values.view();
    const std::string_view borrowed{borrowed_view.value_bytes + borrowed_view.slots[0].offset,
        borrowed_view.slots[0].size};
    auto rebased = std::move(with_owned_values).set<38>(borrowed); // the third slot grows the backing block
    std::array<char, 256> output{};
    const auto rebased_wire = encode_fix44(rebased, SessionHeader{"BUY", "SELL", 1, "20261026-12:30:45"}, output);
    check(rebased_wire.ok(), "self-referenced value survives a growth relocation");
    if (rebased_wire.ok()) {
        const auto frame = as_view(output, rebased_wire.value());
        check(frame.find("38=kept\x01") != std::string_view::npos, "rebased arena text remains owned");
    }

    auto bad_value = message<"D">().set<11>(std::string_view{"a\x01" "b", 3});
    check(bad_value.status().code == ErrorCode::invalid_value, "SOH in a regular value is rejected");
    auto empty_value = message<"D">().set<11>("");
    check(empty_value.status().code == ErrorCode::invalid_value, "empty ordinary field values are rejected");

    output.fill('\0');
    auto scalar_values = message<"D">().set<21>('1').set<40>(true);
    const auto scalars = encode_fix44(scalar_values, SessionHeader{"BUY", "SELL", 1, "20261026-12:30:45"}, output);
    check(scalars.ok(), "character and boolean values encode");
    if (scalars.ok()) {
        const auto frame = as_view(output, scalars.value());
        check(frame.find("21=1\x01") != std::string_view::npos, "character field is emitted as one byte");
        check(frame.find("40=1\x01") != std::string_view::npos, "boolean field uses FIX numeric true");
    }

    const auto bad_header = encode_fix44(message<"D">().set<11>("id"),
        SessionHeader{"BUY\x01" "BAD", "SELL", 1, "20261026-12:30:45"}, output);
    check(!bad_header.ok() && bad_header.status().code == ErrorCode::invalid_header,
        "managed header values are validated");
    const auto bad_time = encode_fix44(message<"D">().set<11>("id"),
        SessionHeader{"BUY", "SELL", 1, "20261326-12:30:45"}, output);
    check(!bad_time.ok() && bad_time.status().code == ErrorCode::invalid_timestamp,
        "SendingTime must be a valid UTC timestamp");
}

} // namespace

int main() {
    test_decimal();
    test_fluent_encode();
    test_fixed_shape();
    test_fluent_limits_and_delimiters();
    return failures == 0 ? 0 : 1;
}
