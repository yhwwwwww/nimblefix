#include <nimblefix/codec.hpp>
#include <nimblefix/decimal.hpp>
#include <nimblefix/fix44_sample.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace {

int failures = 0;

void check(bool condition) {
    if (!condition) ++failures;
}

void test_generated_and_tagged_builders_match() {
    using namespace nimblefix;
    using namespace nimblefix::literals;
    namespace fix44 = nimblefix::fix44_sample;

    auto named = fix44::NewOrderSingle::make()
        .price("150.25"_dec)
        .cl_ord_id("client-1")
        .symbol("AAPL")
        .side('1')
        .transact_time("20261026-12:30:45")
        .order_qty(100)
        .ord_type('2');
    auto tagged = fix44::NewOrderSingle::make()
        .set<44>("150.25"_dec)
        .set<11>("client-1")
        .set<55>("AAPL")
        .set<54>('1')
        .set<60>("20261026-12:30:45")
        .set<38>(100)
        .set<40>('2');
    auto reordered = fix44::NewOrderSingle::make()
        .order_qty(100)
        .cl_ord_id("client-1")
        .side('1')
        .transact_time("20261026-12:30:45")
        .price("150.25"_dec)
        .ord_type('2')
        .symbol("AAPL");

    using Named = decltype(named);
    using Tagged = decltype(tagged);
    using Reordered = decltype(reordered);
    static_assert(Named::slot_count == 7);
    static_assert(Named::slot_storage_bytes == 7 * sizeof(detail::SlotValue));
    static_assert(Tagged::slot_count == Named::slot_count);
    static_assert(Named::wire_order == Tagged::wire_order);
    static_assert(Named::wire_order == std::array<std::uint32_t, 7>{11, 55, 54, 60, 38, 40, 44});
    static_assert(std::is_same_v<Named::plan_type, Reordered::plan_type>);
    static_assert(&Named::encoding_plan == &Reordered::encoding_plan);
    static_assert(Named::wire_order == Reordered::wire_order);
    static_assert(Named::physical_slot_map == std::array<std::uint32_t, 7>{1, 2, 3, 4, 5, 6, 0});
    static_assert(Reordered::physical_slot_map == std::array<std::uint32_t, 7>{1, 6, 2, 3, 0, 5, 4});
    static_assert(Named::physical_slot_map != Reordered::physical_slot_map);
    static_assert(fix44::NewOrderSinglePolicy::allowed_tags.size() == 7);
    static_assert(fix44::NewOrderSinglePolicy::required_tags.size() == 6);
    static_assert(fix44::Schema::field_count == 14);
    static_assert(fix44::Schema::enum_value_count == 12);
    static_assert(FieldWireType<fix44::NewOrderSinglePolicy, 11>::value == WireType::string);
    static_assert(FieldWireType<fix44::NewOrderSinglePolicy, 54>::value == WireType::character);
    static_assert(FieldWireType<fix44::NewOrderSinglePolicy, 38>::value == WireType::decimal);
    static_assert(FieldWireType<fix44::NewOrderSinglePolicy, 60>::value == WireType::utc_timestamp);

    std::array<char, 512> named_output{};
    std::array<char, 512> tagged_output{};
    std::array<char, 512> reordered_output{};
    const SessionHeader header{
        .sender_comp_id = "BUY",
        .target_comp_id = "SELL",
        .msg_seq_num = 7,
        .sending_time = "20261026-12:30:45",
    };
    const auto named_size = encode_fix44(named, header, named_output);
    const auto tagged_size = encode_fix44(tagged, header, tagged_output);
    const auto reordered_size = encode_fix44(reordered, header, reordered_output);
    check(named_size.ok() && tagged_size.ok() && reordered_size.ok());
    if (!named_size.ok() || !tagged_size.ok() || !reordered_size.ok()) return;

    const std::string_view named_frame{named_output.data(), named_size.value()};
    const std::string_view tagged_frame{tagged_output.data(), tagged_size.value()};
    const std::string_view reordered_frame{reordered_output.data(), reordered_size.value()};
    check(named_frame == tagged_frame);
    check(named_frame == reordered_frame);
    check(named.view().plan == reordered.view().plan);

    const auto cl_ord_id = named_frame.find("11=client-1\x01");
    const auto symbol = named_frame.find("55=AAPL\x01");
    const auto side = named_frame.find("54=1\x01");
    const auto transact_time = named_frame.find("60=20261026-12:30:45\x01");
    const auto order_qty = named_frame.find("38=100\x01");
    const auto ord_type = named_frame.find("40=2\x01");
    const auto price = named_frame.find("44=150.25\x01");
    check(cl_ord_id < symbol && symbol < side && side < transact_time &&
        transact_time < order_qty && order_qty < ord_type && ord_type < price);

    auto bad_enum = fix44::NewOrderSingle::make()
        .cl_ord_id("client-1")
        .symbol("AAPL")
        .side('X')
        .transact_time("20261026-12:30:45")
        .order_qty(100)
        .ord_type('2')
        .price("150.25"_dec);
    std::array<char, 512> invalid_output{};
    const auto invalid = encode_fix44(bad_enum, header, invalid_output);
    check(!invalid.ok() && invalid.status().code == ErrorCode::invalid_value &&
        invalid.status().tag == 54);

    auto missing_required = fix44::NewOrderSingle::make()
        .cl_ord_id("client-1")
        .symbol("AAPL")
        .side('1')
        .transact_time("20261026-12:30:45")
        .order_qty(100)
        .price("150.25"_dec);
    const auto missing = encode_fix44(missing_required, header, invalid_output);
    check(!missing.ok() && missing.status().code == ErrorCode::required_field_missing &&
        missing.status().tag == 40);

    auto bad_timestamp = fix44::NewOrderSingle::make()
        .cl_ord_id("client-1")
        .symbol("AAPL")
        .side('1')
        .transact_time("bad-time")
        .order_qty(100)
        .ord_type('2');
    const auto invalid_time = encode_fix44(bad_timestamp, header, invalid_output);
    check(!invalid_time.ok() && invalid_time.status().code == ErrorCode::invalid_timestamp &&
        invalid_time.status().tag == 60);

    auto bad_tagged_timestamp = fix44::NewOrderSingle::make()
        .set<11>("client-1")
        .set<55>("AAPL")
        .set<54>('1')
        .set<60>("bad-time")
        .set<38>(100)
        .set<40>('2');
    const auto invalid_tag_time = encode_fix44(bad_tagged_timestamp, header, invalid_output);
    check(!invalid_tag_time.ok() && invalid_tag_time.status().code == ErrorCode::invalid_timestamp &&
        invalid_tag_time.status().tag == 60);
}

} // namespace

int main() {
    test_generated_and_tagged_builders_match();
    return failures == 0 ? 0 : 1;
}
