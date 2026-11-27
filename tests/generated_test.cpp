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

void test_generated_stream_and_fixed() {
    using namespace nimblefix;
    using namespace nimblefix::literals;
    namespace fix44 = nimblefix::fix44_sample;

    static_assert(fix44::NewOrderSinglePolicy::allowed_tags.size() == 7);
    static_assert(fix44::NewOrderSinglePolicy::required_tags.size() == 6);
    static_assert(fix44::Schema::field_count == 14);
    static_assert(fix44::Schema::enum_value_count == 12);
    static_assert(FieldWireType<fix44::NewOrderSinglePolicy, 11>::value == WireType::string);
    static_assert(FieldWireType<fix44::NewOrderSinglePolicy, 54>::value == WireType::character);
    static_assert(FieldWireType<fix44::NewOrderSinglePolicy, 38>::value == WireType::decimal);
    static_assert(FieldWireType<fix44::NewOrderSinglePolicy, 60>::value == WireType::utc_timestamp);

    using OrderFixed = fix44::NewOrderSingle::Fixed<
        128,
        fix44::field::Price,
        fix44::field::ClOrdID,
        fix44::field::Symbol,
        fix44::field::Side,
        fix44::field::TransactTime,
        fix44::field::OrderQty,
        fix44::field::OrdType>;
    static_assert(OrderFixed::slot_count == 7);
    static_assert(OrderFixed::slot_storage_bytes == 7 * sizeof(detail::SlotValue));
    static_assert(OrderFixed::encoding_plan[0].tag == 11 &&
                  OrderFixed::encoding_plan[1].tag == 55 &&
                  OrderFixed::encoding_plan[2].tag == 54 &&
                  OrderFixed::encoding_plan[3].tag == 60 &&
                  OrderFixed::encoding_plan[4].tag == 38 &&
                  OrderFixed::encoding_plan[5].tag == 40 &&
                  OrderFixed::encoding_plan[6].tag == 44);

    OrderFixed fixed;
    check(fixed.price("150.25"_dec).ok());
    check(fixed.cl_ord_id("client-1").ok());
    check(fixed.symbol("AAPL").ok());
    check(fixed.side('1').ok());
    check(fixed.transact_time("20261026-12:30:45").ok());
    check(fixed.order_qty(100).ok());
    check(fixed.ord_type('2').ok());

    std::array<char, 512> named_output{};
    std::array<char, 512> tagged_output{};
    std::array<char, 512> fixed_output{};
    const SessionHeader header{
        .sender_comp_id = "BUY",
        .target_comp_id = "SELL",
        .msg_seq_num = 7,
        .sending_time = "20261026-12:30:45",
    };

    const auto named_size = encode_fix44_stream<fix44::NewOrderSingle>(header, named_output, [](auto& w) {
        w.cl_ord_id("client-1")
         .symbol("AAPL")
         .side('1')
         .transact_time("20261026-12:30:45")
         .order_qty(100)
         .ord_type('2')
         .price("150.25"_dec);
    });
    const auto tagged_size = encode_fix44_stream<fix44::NewOrderSingle>(header, tagged_output,
        [](fix44::NewOrderSingle::Writer& w) {
            w.set<11>("client-1")
             .set<55>("AAPL")
             .set<54>('1')
             .set<60>("20261026-12:30:45")
             .set<38>(100)
             .set<40>('2')
             .set<44>("150.25"_dec);
        });
    const auto fixed_size = encode_fix44(fixed, header, fixed_output);
    check(named_size.ok() && tagged_size.ok() && fixed_size.ok());
    if (!named_size.ok() || !tagged_size.ok() || !fixed_size.ok()) return;

    const std::string_view named_frame{named_output.data(), named_size.value()};
    const std::string_view tagged_frame{tagged_output.data(), tagged_size.value()};
    const std::string_view fixed_frame{fixed_output.data(), fixed_size.value()};
    check(named_frame == tagged_frame);
    check(named_frame == fixed_frame);

    const auto cl_ord_id = named_frame.find("11=client-1\x01");
    const auto symbol = named_frame.find("55=AAPL\x01");
    const auto side = named_frame.find("54=1\x01");
    const auto transact_time = named_frame.find("60=20261026-12:30:45\x01");
    const auto order_qty = named_frame.find("38=100\x01");
    const auto ord_type = named_frame.find("40=2\x01");
    const auto price = named_frame.find("44=150.25\x01");
    check(cl_ord_id < symbol && symbol < side && side < transact_time &&
        transact_time < order_qty && order_qty < ord_type && ord_type < price);

    // Optional fields: nullopt omitted, engaged optional emitted.
    std::array<char, 512> opt_output{};
    const std::optional<Decimal> no_px{std::nullopt};
    const auto opt_size = encode_fix44_stream<fix44::NewOrderSingle>(header, opt_output, [&](auto& w) {
        w.cl_ord_id("client-opt")
         .symbol("AAPL")
         .side('1')
         .transact_time("20261026-12:30:45")
         .order_qty(100)
         .ord_type('1')
         .price(no_px);
    });
    check(opt_size.ok());
    if (opt_size.ok()) {
        const std::string_view opt_frame{opt_output.data(), opt_size.value()};
        check(opt_frame.find("44=") == std::string_view::npos);
    }

    // Dynamic per-message standard header fields (50=SenderSubID, 57=TargetSubID) before body.
    std::array<char, 512> hdr_output{};
    const auto hdr_size = encode_fix44_stream<fix44::NewOrderSingle>(header, hdr_output, [](auto& w) {
        w.header().sender_sub_id("DESK-A").target_sub_id("EXEC-1");
        w.cl_ord_id("client-hdr")
         .symbol("AAPL")
         .side('1')
         .transact_time("20261026-12:30:45")
         .order_qty(100)
         .ord_type('1');
    });
    check(hdr_size.ok());
    if (hdr_size.ok()) {
        const std::string_view hf{hdr_output.data(), hdr_size.value()};
        const auto pos52 = hf.find("52=20261026-12:30:45\x01");
        const auto pos50 = hf.find("50=DESK-A\x01");
        const auto pos57 = hf.find("57=EXEC-1\x01");
        const auto pos11 = hf.find("11=client-hdr\x01");
        check(pos52 != std::string_view::npos && pos50 > pos52 && pos57 > pos50 && pos11 > pos57);
    }

    // Writing standard header field AFTER a body field must fail with invalid_header.
    std::array<char, 512> invalid_output{};
    const auto late_hdr = encode_fix44_stream<fix44::NewOrderSingle>(header, invalid_output, [](auto& w) {
        w.cl_ord_id("client-late");
        w.header().sender_sub_id("DESK-LATE");
        w.symbol("AAPL").side('1').transact_time("20261026-12:30:45").order_qty(100).ord_type('1');
    });
    check(!late_hdr.ok() && late_hdr.status().code == ErrorCode::invalid_header && late_hdr.status().tag == 50);

    // Invalid enum rejected with exact tag.
    const auto invalid = encode_fix44_stream<fix44::NewOrderSingle>(header, invalid_output, [](auto& w) {
        w.cl_ord_id("client-1").symbol("AAPL").side('X')
         .transact_time("20261026-12:30:45").order_qty(100).ord_type('2').price("150.25"_dec);
    });
    check(!invalid.ok() && invalid.status().code == ErrorCode::invalid_value &&
        invalid.status().tag == 54);

    // Missing required field rejected with exact missing tag.
    const auto missing = encode_fix44_stream<fix44::NewOrderSingle>(header, invalid_output, [](auto& w) {
        w.cl_ord_id("client-1").symbol("AAPL").side('1')
         .transact_time("20261026-12:30:45").order_qty(100).price("150.25"_dec);
    });
    check(!missing.ok() && missing.status().code == ErrorCode::required_field_missing &&
        missing.status().tag == 40);

    // Invalid timestamp rejected (named setter & set<60>).
    const auto invalid_time = encode_fix44_stream<fix44::NewOrderSingle>(header, invalid_output, [](auto& w) {
        w.cl_ord_id("client-1").symbol("AAPL").side('1')
         .transact_time("bad-time").order_qty(100).ord_type('2');
    });
    check(!invalid_time.ok() && invalid_time.status().code == ErrorCode::invalid_timestamp &&
        invalid_time.status().tag == 60);

    const auto invalid_tag_time = encode_fix44_stream<fix44::NewOrderSingle>(header, invalid_output,
        [](fix44::NewOrderSingle::Writer& w) {
            w.set<11>("client-1").set<55>("AAPL").set<54>('1')
             .set<60>("bad-time").set<38>(100).set<40>('2');
        });
    check(!invalid_tag_time.ok() && invalid_tag_time.status().code == ErrorCode::invalid_timestamp &&
        invalid_tag_time.status().tag == 60);

    // Duplicate body field rejected via named setter and via set<Tag>.
    const auto dup_body = encode_stream<fix44::NewOrderSingle>(header, invalid_output, [](auto& w) {
        w.cl_ord_id("client-1").cl_ord_id("client-dup").symbol("AAPL").side('1')
         .transact_time("20261026-12:30:45").order_qty(100).ord_type('2');
    });
    check(!dup_body.ok() && dup_body.status().code == ErrorCode::duplicate_field &&
        dup_body.status().tag == 11);

    const auto dup_mixed = encode_stream<fix44::NewOrderSingle>(header, invalid_output, [](auto& w) {
        w.cl_ord_id("client-1").symbol("AAPL").side('1')
         .transact_time("20261026-12:30:45").order_qty(100).ord_type('2')
         .price("150.25"_dec).template set<44>("151.00"_dec);
    });
    check(!dup_mixed.ok() && dup_mixed.status().code == ErrorCode::duplicate_field &&
        dup_mixed.status().tag == 44);

    // Disengaged optional followed by engaged value must NOT trigger duplicate_field.
    std::array<char, 512> opt_then_val_output{};
    const auto opt_then_val = encode_stream<fix44::NewOrderSingle>(header, opt_then_val_output, [&](auto& w) {
        const std::optional<std::string_view> empty_sub{};
        w.header().sender_sub_id(empty_sub).sender_sub_id("DESK-OK");
        w.cl_ord_id("client-opt-ok").symbol("AAPL").side('1')
         .transact_time("20261026-12:30:45").order_qty(100).ord_type('2')
         .price(no_px).price("150.25"_dec);
    });
    check(opt_then_val.ok());

    // Duplicate standard header field rejected with duplicate_field and exact tag.
    const auto dup_hdr = encode_stream<fix44::NewOrderSingle>(header, invalid_output, [](auto& w) {
        w.header().sender_sub_id("DESK-1").sender_sub_id("DESK-2");
        w.cl_ord_id("client-1").symbol("AAPL").side('1')
         .transact_time("20261026-12:30:45").order_qty(100).ord_type('1');
    });
    check(!dup_hdr.ok() && dup_hdr.status().code == ErrorCode::duplicate_field &&
        dup_hdr.status().tag == 50);
}

} // namespace

int main() {
    test_generated_stream_and_fixed();
    return failures == 0 ? 0 : 1;
}
