#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

#include <nimblefix/codec.hpp>
#include <nimblefix/fix44_sample.hpp>

namespace nf = nimblefix;
using namespace nf::literals;

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << __FILE__ << ':' << __LINE__ << ": " #condition "\n"; std::exit(1); \
} } while (false)

namespace {
constexpr std::string_view expected =
    "8=FIX.4.4\0019=122\00135=D\00149=BUY\00156=SELL\00134=1\001"
    "52=20261026-12:00:00.000\00111=DEMO-001\00155=AAPL\00154=1\001"
    "60=20261026-12:00:00.000\00138=100\00140=2\00144=150.25\00110=212\001";
constexpr nf::SessionHeader header{"BUY", "SELL", 1, "20261026-12:00:00.000"};

template <class Message>
void check_wire(const Message& message) {
    std::array<char, 1024> output;
    output.fill('!');
    const auto result = nf::encode_fix44(message, header, output);
    CHECK(result.ok());
    CHECK(std::string_view(output.data(), result.value()) == expected);
    CHECK(output[result.value()] == '!');
    nf::FrameView frame;
    CHECK(nf::inspect_frame({output.data(), result.value()}, frame).ok());
    std::array<nf::FieldView, 32> fields;
    nf::FlatMessageView view;
    CHECK(nf::parse_flat(frame, fields, view).ok());
    CHECK(view.find_unique(44).value().value == "150.25");
    CHECK(view.find_unique(11).value().value == "DEMO-001");
    CHECK(view.find_unique(900001).error().code == nf::ErrorCode::missing_field);
    for (std::size_t end = 0; end < expected.size(); ++end) {
        CHECK(nf::inspect_frame(expected.substr(0, end), frame).code == nf::ErrorCode::need_more);
        CHECK(frame.bytes.empty());
    }
    CHECK(nf::inspect_frame(std::string(expected) + std::string(expected), frame).ok());
    CHECK(frame.consumed == expected.size());
}

void independent_wire_fixture() {
    // BodyLength=122 and CheckSum=212 are fixed independently of this encoder.
    auto flat = nf::message<"D">().set<11>("DEMO-001").set<55>("AAPL")
        .set<54>('1').set<60>("20261026-12:00:00.000").set<38>(100)
        .set<40>('2').set<44>("150.25"_dec);
    check_wire(flat);

    // Streaming encoder must produce byte-identical frame (BodyLength=122, CheckSum=212).
    {
        std::array<char, 1024> output;
        output.fill('!');
        const auto streamed = nf::encode_fix44_stream<nf::fix44_sample::NewOrderSingle>(
            header, output, [](auto& w) {
                w.cl_ord_id("DEMO-001").symbol("AAPL").side('1')
                 .transact_time("20261026-12:00:00.000").order_qty(100)
                 .ord_type('2').price("150.25"_dec);
            });
        CHECK(streamed.ok());
        CHECK(std::string_view(output.data(), streamed.value()) == expected);
        CHECK(output[streamed.value()] == '!');
    }

    // Flat declaration order is significant and is not sorted by numeric tag.
    auto different = nf::message<"Z">().set<900001>("last").set<11>("first");
    std::array<char, 256> output;
    const auto result = nf::encode_fix44(different, header, output);
    CHECK(result.ok());
    const std::string_view wire(output.data(), result.value());
    CHECK(wire.find("900001=last") < wire.find("11=first"));
}

void sparse_and_owned() {
    using Near = nf::FixedMessage<"Z", nf::Fields<nf::Field<11, nf::FieldKind::string>, nf::Field<55, nf::FieldKind::string>>, 32>;
    using Far = nf::FixedMessage<"Z", nf::Fields<nf::Field<11, nf::FieldKind::string>, nf::Field<900001, nf::FieldKind::string>>, 32>;
    static_assert(Near::slot_count == 2 && Far::slot_count == 2);
    static_assert(sizeof(Near) == sizeof(Far));
    static_assert(Near::slot_storage_bytes == 32 && Far::slot_storage_bytes == 32);

    std::string changing = "temporary";
    auto value = nf::message<"Z">().set<11>(std::string_view{changing}).set<55>("A=B");
    changing.assign(50, 'x');
    std::array<char, 256> output;
    auto encoded = nf::encode_fix44(value, header, output);
    CHECK(encoded.ok());
    std::string_view wire(output.data(), encoded.value());
    CHECK(wire.find("11=temporary\00155=A=B\001") != std::string_view::npos);
    auto moved = std::move(value);
    CHECK(!value.status().ok());
    CHECK(nf::encode_fix44(moved, header, output).ok());

    auto failed = nf::message<"Z">({.max_fields = 2, .max_value_bytes = 2})
        .set<11>("too-long").set<55>("x");
    CHECK(failed.status().code == nf::ErrorCode::capacity_exceeded);
    CHECK(failed.status().tag == 11);
    output.fill('#');
    CHECK(!nf::encode_fix44(failed, header, output).ok());
    for (char ch : output) CHECK(ch == '#');
}

void boundaries() {
    auto message = nf::message<"Z">().set<11>("x");
    std::array<char, 256> output;
    output.fill('#');
    const auto result = nf::encode_fix44(message, header, output);
    CHECK(result.ok());
    const auto size = result.value();
    for (std::size_t limit = 0; limit < size; ++limit) {
        output.fill('#');
        CHECK(nf::encode_fix44(message, header, std::span{output}.first(limit)).error().code == nf::ErrorCode::capacity_exceeded);
        for (char ch : output) CHECK(ch == '#');
    }
    CHECK(nf::encode_fix44(message, {"BUY", "SELL", std::numeric_limits<std::uint64_t>::max(), header.sending_time}, output).ok());
    CHECK(!nf::encode_fix44(message, {"BUY", "SELL", 0, header.sending_time}, output).ok());
    CHECK(!nf::encode_fix44(message, {"B\001UY", "SELL", 1, header.sending_time}, output).ok());
}

void fixed_string_reference_model() {
    using Shape = nf::Fields<nf::Field<11, nf::FieldKind::string>,
        nf::Field<55, nf::FieldKind::string>, nf::Field<900001, nf::FieldKind::string>>;
    nf::FixedMessage<"Z", Shape, 48> message;
    std::array<std::string, 3> reference;
    std::uint32_t state = 123456789;
    auto random = [&]() { state = state * 1664525U + 1013904223U; return state; };
    constexpr std::array<std::uint32_t, 3> tags{11, 55, 900001};
    for (unsigned iteration = 0; iteration < 2000; ++iteration) {
        const std::size_t index = random() % 3;
        std::string value(1 + random() % 20, static_cast<char>('a' + random() % 26));
        std::string_view source = value;
        // Include substrings spanning multiple arena values, so an update
        // must preserve bytes that straddle its own compaction boundary.
        const auto arena = message.view();
        if (arena.value_byte_count != 0 && random() % 2 != 0) {
            const auto offset = random() % arena.value_byte_count;
            const auto count = 1 + random() % (arena.value_byte_count - offset);
            source = {arena.value_bytes + offset, count};
            value = std::string(source);
        }
        std::size_t expected_bytes = value.size();
        for (std::size_t i = 0; i < reference.size(); ++i)
            if (i != index) expected_bytes += reference[i].size();
        nf::Status status;
        if (index == 0) status = message.set<11>(source);
        else if (index == 1) status = message.set<55>(source);
        else status = message.set<900001>(source);
        if (expected_bytes > 48) {
            CHECK(status.code == nf::ErrorCode::capacity_exceeded);
            message.reset();
            reference.fill({});
            continue;
        }
        CHECK(status.ok());
        reference[index] = value;
        std::array<char, 512> output;
        const auto encoded = nf::encode_fix44(message, header, output);
        CHECK(encoded.ok());
        nf::FrameView frame;
        CHECK(nf::inspect_frame({output.data(), encoded.value()}, frame).ok());
        nf::FlatMessageView view;
        std::array<nf::FieldView, 16> scratch;
        CHECK(nf::parse_flat(frame, scratch, view).ok());
        for (std::size_t i = 0; i < reference.size(); ++i) {
            const auto found = view.find_unique(tags[i]);
            if (reference[i].empty()) CHECK(found.error().code == nf::ErrorCode::missing_field);
            else { CHECK(found.ok()); CHECK(found.value().value == reference[i]); }
        }
    }
}
} // namespace

int main() {
    independent_wire_fixture();
    sparse_and_owned();
    boundaries();
    fixed_string_reference_model();
    std::cout << "public codec contracts passed\n";
}
