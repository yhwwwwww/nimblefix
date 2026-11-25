#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <type_traits>
#include <utility>
#include <vector>

#include <nimblefix/codec.hpp>
#include <nimblefix/dynamic_message.hpp>
#include <nimblefix/fix44.hpp>
#include "perf_counters.hpp"

namespace nf = nimblefix;

namespace {

bool track_allocations = false;
std::size_t allocation_calls = 0;
std::size_t allocation_bytes = 0;
std::uint64_t result_sink = 0;

constexpr nf::SessionHeader header{"BUY", "SELL", 1, "20261031-12:00:00.000"};
constexpr std::string_view sending_time = "20261031-12:00:00.000";
constexpr std::string_view symbol = "AAPL";
constexpr std::string_view hot_cl_ord_id = "BENCH-000000";
constexpr std::size_t max_frame_bytes = 64 * 1024;

[[noreturn]] void fail(std::string_view message) {
    std::cerr << "schema benchmark failed: " << message << '\n';
    std::exit(1);
}

void require(bool condition, std::string_view message = "operation rejected") {
    if (!condition) fail(message);
}

void consume(std::uint64_t value) noexcept {
    result_sink = (result_sink * 0x9e3779b185ebca87ULL) ^ (value + 0x517cc1b727220a95ULL);
    asm volatile("" : : "r"(result_sink) : "memory");
}

struct CaseMeta {
    std::size_t wire_bytes{};
    std::size_t instance_bytes{};
    std::size_t initialization_bytes{};
    std::size_t visited_slots{};
    std::size_t active_indices{};
    std::size_t sort_comparisons{};
};

struct Sample {
    std::uint64_t ns{};
    std::size_t allocations{};
    std::size_t allocated_bytes{};
};

template <class Function>
void measure(std::string_view name, std::size_t iterations, CaseMeta meta, Function&& operation) {
    std::vector<Sample> samples(iterations);
    constexpr std::size_t warmup = 1000;
    for (std::size_t i = 0; i < warmup; ++i) operation(i);

    rusage before{}, after{};
    getrusage(RUSAGE_SELF, &before);
    const auto wall_start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        allocation_calls = 0;
        allocation_bytes = 0;
        track_allocations = true;
        const auto start = std::chrono::steady_clock::now();
        operation(i);
        const auto stop = std::chrono::steady_clock::now();
        track_allocations = false;
        samples[i] = {
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count()),
            allocation_calls, allocation_bytes};
    }
    const auto wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - wall_start).count();
    getrusage(RUSAGE_SELF, &after);

    for (std::size_t i = 0; i < samples.size(); ++i) {
        const auto& sample = samples[i];
        std::cout << name << ',' << i << ',' << sample.ns << ',' << sample.allocations << ','
                  << sample.allocated_bytes << ',' << meta.wire_bytes << ',' << meta.instance_bytes << ','
                  << meta.initialization_bytes << ',' << meta.visited_slots << ',' << meta.active_indices << ','
                  << meta.sort_comparisons << '\n';
    }
    std::cerr << "{\"case\":\"" << name << "\",\"samples\":" << iterations
              << ",\"warmup\":" << warmup << ",\"wall_ns\":" << wall_ns
              << ",\"minor_faults\":" << after.ru_minflt - before.ru_minflt
              << ",\"major_faults\":" << after.ru_majflt - before.ru_majflt << "}\n";
}

[[nodiscard]] nf::Result<std::size_t> encode_order_result(std::string_view cl_ord_id,
    std::int64_t quantity, char side_value, std::span<char> output) noexcept {
    return nf::encode_fix44_stream<nf::fix44::NewOrderSingle>(header, output, [&](auto& w) {
        w.cl_ord_id(cl_ord_id)
         .handl_inst('1')
         .symbol(symbol)
         .side(side_value)
         .transact_time(sending_time)
         .order_qty(quantity)
         .ord_type('1');
    });
}

[[nodiscard]] std::size_t encode_order(std::string_view cl_ord_id, std::int64_t quantity,
    std::span<char> output) {
    const auto encoded = encode_order_result(cl_ord_id, quantity, '1', output);
    require(encoded.ok(), "generated FIX44 NewOrderSingle failed to encode");
    return encoded.value();
}

[[nodiscard]] nf::DynamicMessage make_dynamic_order() {
    auto message = nf::dynamic_message("D");
    require(message.set(11, hot_cl_ord_id).ok());
    require(message.set(21, '1').ok());
    require(message.set(55, symbol).ok());
    require(message.set(54, '1').ok());
    require(message.set(60, sending_time).ok());
    require(message.set(38, 100).ok());
    require(message.set(40, '1').ok());
    return message;
}

[[nodiscard]] nf::Status parse_frame(std::string_view wire, std::span<nf::FieldView> scratch,
    nf::FlatMessageView& flat) noexcept {
    nf::FrameView frame{};
    if (const auto status = nf::inspect_frame(wire, frame); !status.ok()) return status;
    return nf::parse_flat(frame, scratch, flat);
}

void verify_error_boundaries(std::string_view valid_wire) {
    std::array<char, 8> short_output{};
    const auto too_small = encode_order_result(hot_cl_ord_id, 100, '1', short_output);
    require(!too_small.ok() && too_small.status().code == nf::ErrorCode::capacity_exceeded,
        "real encoder did not reject a short output span");

    std::array<char, 1024> output{};
    const auto missing_result = nf::encode_fix44_stream<nf::fix44::NewOrderSingle>(header, output, [](auto& w) {
        w.cl_ord_id("MISSING");
    });
    require(!missing_result.ok() && missing_result.status().code == nf::ErrorCode::required_field_missing,
        "real encoder did not enforce required FIX44 fields");

    const auto enum_result = encode_order_result(hot_cl_ord_id, 100, 'X', output);
    require(!enum_result.ok(), "real encoder accepted an invalid FIX44 enum value");

    std::array<char, max_frame_bytes> corrupted{};
    std::memcpy(corrupted.data(), valid_wire.data(), valid_wire.size());
    corrupted[valid_wire.size() - 3] = corrupted[valid_wire.size() - 3] == '0' ? '1' : '0';
    nf::FrameView frame{};
    const auto bad_checksum = nf::inspect_frame({corrupted.data(), valid_wire.size()}, frame);
    require(!bad_checksum.ok(), "frame parser accepted a changed checksum");
}

struct DirectValues {
    std::string_view cl_ord_id;
    std::string_view symbol;
    std::string_view sending_time;
    std::int64_t order_qty;
};

[[nodiscard]] constexpr std::size_t decimal_digits(std::size_t value) noexcept {
    std::size_t digits = 1;
    while (value >= 10) { value /= 10; ++digits; }
    return digits;
}

class DirectWriter {
public:
    explicit DirectWriter(std::span<char> output) noexcept : output_(output) {}

    bool append(std::string_view text) noexcept {
        if (text.size() > output_.size() - position_) return false;
        for (char ch : text) append(ch);
        return true;
    }

    bool append(char ch) noexcept {
        if (position_ == output_.size()) return false;
        output_[position_++] = ch;
        checksum_ = (checksum_ + static_cast<unsigned char>(ch)) & 255U;
        return true;
    }

    bool append_number(std::uint64_t value) noexcept {
        char buffer[24];
        const auto converted = std::to_chars(std::begin(buffer), std::end(buffer), value);
        return converted.ec == std::errc{} && append({buffer, static_cast<std::size_t>(converted.ptr - buffer)});
    }

    [[nodiscard]] std::size_t size() const noexcept { return position_; }
    [[nodiscard]] unsigned checksum() const noexcept { return checksum_; }

private:
    std::span<char> output_;
    std::size_t position_{};
    unsigned checksum_{};
};

// Fixed canonical body order from the generated FIX44 NewOrderSingle plan:
// 11, 21, 55, 54, 60, 38, 40. This reference does no schema, enum, or
// required-field validation; its fixed input domain is checked separately.
[[nodiscard]] bool direct_ordered_writer(const DirectValues& values, std::span<char> output,
    std::size_t& written) noexcept {
    char qty_buffer[24];
    const auto qty = std::to_chars(std::begin(qty_buffer), std::end(qty_buffer), values.order_qty);
    if (qty.ec != std::errc{}) return false;
    const std::string_view qty_text{qty_buffer, static_cast<std::size_t>(qty.ptr - qty_buffer)};

    const std::size_t application_body_size =
        4 + values.cl_ord_id.size() + 4 + 1 +
        4 + values.symbol.size() + 4 + 1 +
        4 + values.sending_time.size() + 4 + qty_text.size() +
        4 + 1;
    const std::size_t body_size = application_body_size +
        (4 + 1) + (4 + 3) + (4 + 4) + (4 + 1) + (4 + values.sending_time.size());
    constexpr std::string_view prefix = "8=FIX.4.4\0019=";
    constexpr std::string_view fields = "\00135=D\00149=BUY\00156=SELL\00134=1\00152=";
    const std::size_t header_length = prefix.size() + decimal_digits(body_size) +
        fields.size() + values.sending_time.size() + 1;
    constexpr std::size_t checksum_suffix = 7;
    if (header_length > output.size() || body_size > output.size() - header_length ||
        checksum_suffix > output.size() - header_length - body_size) return false;

    DirectWriter writer{output};
    if (!writer.append(prefix) || !writer.append_number(body_size) || !writer.append(fields) ||
        !writer.append(values.sending_time) || !writer.append(nf::soh) ||
        !writer.append("11=") || !writer.append(values.cl_ord_id) || !writer.append(nf::soh) ||
        !writer.append("21=1") || !writer.append(nf::soh) ||
        !writer.append("55=") || !writer.append(values.symbol) || !writer.append(nf::soh) ||
        !writer.append("54=1") || !writer.append(nf::soh) ||
        !writer.append("60=") || !writer.append(values.sending_time) || !writer.append(nf::soh) ||
        !writer.append("38=") || !writer.append(qty_text) || !writer.append(nf::soh) ||
        !writer.append("40=1") || !writer.append(nf::soh)) return false;
    const auto checksum = writer.checksum();
    const char suffix[] = {
        '1', '0', '=', static_cast<char>('0' + checksum / 100),
        static_cast<char>('0' + (checksum / 10) % 10),
        static_cast<char>('0' + checksum % 10), nf::soh};
    for (const char ch : suffix) if (!writer.append(ch)) return false;
    written = writer.size();
    return true;
}

template <class Seq>
struct SyntheticShapeFromSeq;

template <std::size_t... I>
struct SyntheticShapeFromSeq<std::index_sequence<I...>> {
    using type = nf::Fields<nf::Field<1000 + I, nf::FieldKind::string>...>;
};

template <std::size_t S>
using SyntheticShape = typename SyntheticShapeFromSeq<std::make_index_sequence<S>>::type;

struct RefSlot {
    std::uint32_t tag{};
    std::string_view value{};
    bool present{};
};

template <std::size_t S>
struct ShapeLocalReference {
    std::array<RefSlot, S> slots{};
    void reset() noexcept { for (auto& slot : slots) slot = {}; }
    void set(std::size_t index, std::string_view value, std::uint32_t tag = 0) noexcept {
        slots[index] = {tag == 0 ? static_cast<std::uint32_t>(1000 + index) : tag, value, true};
    }
    template <class Function>
    void enumerate(Function&& function, std::size_t& visited) const noexcept {
        for (const auto& slot : slots) {
            ++visited;
            if (slot.present) function(slot.tag, slot.value);
        }
    }
};

template <std::size_t N>
struct FullSchemaDenseReference {
    std::array<RefSlot, N> slots{};
    void reset() noexcept { for (auto& slot : slots) slot = {}; }
    void set(std::size_t index, std::string_view value, std::uint32_t tag = 0) noexcept {
        slots[index] = {tag == 0 ? static_cast<std::uint32_t>(1000 + index) : tag, value, true};
    }
    template <class Function>
    void enumerate(Function&& function, std::size_t& visited) const noexcept {
        for (const auto& slot : slots) {
            ++visited;
            if (slot.present) function(slot.tag, slot.value);
        }
    }
};

template <std::size_t K>
struct ActiveIndexReference {
    std::array<RefSlot, K> slots{};
    std::array<std::size_t, K> active{};
    std::size_t count{};
    std::size_t comparisons{};

    void reset() noexcept { slots = {}; active = {}; count = comparisons = 0; }

    void insert_sorted(std::size_t index, std::string_view value, std::uint32_t tag = 0) noexcept {
        std::size_t at = count;
        while (at != 0) {
            ++comparisons;
            if (active[at - 1] < index) break;
            active[at] = active[at - 1];
            slots[at] = slots[at - 1];
            --at;
        }
        active[at] = index;
        slots[at] = {tag == 0 ? static_cast<std::uint32_t>(1000 + index) : tag, value, true};
        ++count;
    }

    template <class Function>
    void enumerate(Function&& function, std::size_t& visited) const noexcept {
        for (std::size_t i = 0; i < count; ++i) {
            ++visited;
            function(slots[i].tag, slots[i].value);
        }
    }
};

template <std::size_t S, std::size_t K, std::size_t B>
struct SparseCase {
    using Shape = SyntheticShape<S>;
    using Actual = nf::FixedMessage<"Z", Shape, K * B>;
    static_assert(K <= S && K > 0 && B > 0);
    static constexpr std::size_t dense_size = nf::fix44::Schema::field_count;

    std::array<char, B> value_buffer{};
    std::string_view value{};
    ShapeLocalReference<S> local{};
    FullSchemaDenseReference<dense_size> dense{};
    ActiveIndexReference<K> active{};
    std::array<char, max_frame_bytes> wire_buffer{};
    std::array<nf::FieldView, S + 16> parse_scratch{};
    std::array<nf::FieldView, K> actual_fields{};
    std::size_t wire_size{};

    SparseCase() {
        value_buffer.fill('x');
        value_buffer[0] = 'a';
        value = {value_buffer.data(), value_buffer.size()};
        build_actual();
        initialize_refs();
        cross_check_refs();
    }

    template <std::size_t... I>
    void set_actual(Actual& message, std::index_sequence<I...>) {
        ((I < K ? static_cast<void>(require(message.template set<1000 + I>(value).ok(),
            "test-only actual encoder rejected a matrix value")) : static_cast<void>(0)), ...);
    }

    void build_actual() {
        Actual message;
        set_actual(message, std::make_index_sequence<S>{});
        const auto encoded = nf::encode_fix44(message, header, wire_buffer);
        require(encoded.ok(), "test-only actual encoder failed matrix payload");
        wire_size = encoded.value();
        nf::FlatMessageView flat{};
        require(parse_frame({wire_buffer.data(), wire_size}, parse_scratch, flat).ok(),
            "test-only actual matrix payload did not parse");
        std::size_t count = 0;
        for (const auto& field : flat.fields()) {
            if (field.tag >= 1000 && field.tag < 1000 + S) {
                require(count < actual_fields.size(), "matrix parser produced too many application fields");
                actual_fields[count++] = field;
            }
        }
        require(count == K, "actual encoder matrix field count differs from K");
    }

    void initialize_refs() {
        local.reset(); dense.reset(); active.reset();
        for (std::size_t i = K; i != 0; --i) {
            const auto index = i - 1;
            local.set(index, value);
            dense.set(index, value);
            active.insert_sorted(index, value);
        }
    }

    template <class Candidate>
    void compare(const Candidate& candidate, std::string_view label) const {
        std::size_t found = 0, visited = 0;
        candidate.enumerate([&](std::uint32_t tag, std::string_view text) {
            if (found >= actual_fields.size() || actual_fields[found].tag != tag ||
                actual_fields[found].value != text) {
                std::cerr << "schema benchmark failed: " << label << " disagrees with actual encoder wire\n";
                std::exit(1);
            }
            ++found;
        }, visited);
        require(found == K, "reference and actual encoder field counts differ");
    }

    void cross_check_refs() const {
        compare(local, "shape-local");
        compare(dense, "full-schema dense");
        compare(active, "active-index sparse");
    }

    void run(std::size_t iterations, std::string_view suffix) {
        const std::string label{suffix};
        measure(std::string{"shape_local_fill_enumerate_"} + label, iterations,
            {wire_size, sizeof(local), sizeof(local.slots), S, 0, 0}, [&](std::size_t) {
                local.reset();
                for (std::size_t i = K; i != 0; --i) local.set(i - 1, value);
                std::size_t visited = 0;
                std::uint64_t sum = 0;
                local.enumerate([&](std::uint32_t tag, std::string_view text) { sum += tag + text.size(); }, visited);
                consume(sum + visited);
            });
        measure(std::string{"full_schema_dense_fill_enumerate_"} + label, iterations,
            {wire_size, sizeof(dense), sizeof(dense.slots), dense_size, 0, 0}, [&](std::size_t) {
                dense.reset();
                for (std::size_t i = K; i != 0; --i) dense.set(i - 1, value);
                std::size_t visited = 0;
                std::uint64_t sum = 0;
                dense.enumerate([&](std::uint32_t tag, std::string_view text) { sum += tag + text.size(); }, visited);
                consume(sum + visited);
            });
        measure(std::string{"active_index_sparse_fill_enumerate_"} + label, iterations,
            {wire_size, sizeof(active), sizeof(active.slots) + sizeof(active.active), K, K,
                K * (K - 1) / 2}, [&](std::size_t) {
                active.reset();
                for (std::size_t i = K; i != 0; --i) active.insert_sorted(i - 1, value);
                std::size_t visited = 0;
                std::uint64_t sum = 0;
                active.enumerate([&](std::uint32_t tag, std::string_view text) { sum += tag + text.size(); }, visited);
                consume(sum + visited + active.comparisons);
            });
    }
};

struct FixedOrderReferences {
    static constexpr std::array<std::uint32_t, 7> tags{11, 21, 55, 54, 60, 38, 40};
    static constexpr std::size_t dense_size = nf::fix44::Schema::field_count;
    std::array<nf::FieldView, tags.size()> expected{};
    ShapeLocalReference<tags.size()> local{};
    FullSchemaDenseReference<dense_size> dense{};
    ActiveIndexReference<tags.size()> active{};
    std::size_t wire_bytes{};
    std::size_t value_bytes{};

    FixedOrderReferences(const nf::FlatMessageView& parsed, std::size_t frame_bytes)
        : wire_bytes(frame_bytes) {
        std::size_t found = 0;
        for (const auto& field : parsed.fields()) {
            if (found < tags.size() && field.tag == tags[found]) expected[found++] = field;
        }
        require(found == tags.size(), "FIX44 reference fixture is missing a canonical NewOrderSingle field");
        for (const auto& field : expected) value_bytes += field.value.size();
        reset_and_fill();
        cross_check();
    }

    void reset_and_fill() {
        local.reset(); dense.reset(); active.reset();
        for (std::size_t i = tags.size(); i != 0; --i) {
            const auto index = i - 1;
            local.set(index, expected[index].value, expected[index].tag);
            dense.set(index, expected[index].value, expected[index].tag);
            active.insert_sorted(index, expected[index].value, expected[index].tag);
        }
    }

    template <class Candidate>
    void cross_check(const Candidate& candidate, std::string_view name) const {
        std::size_t count = 0, visits = 0;
        candidate.enumerate([&](std::uint32_t tag, std::string_view value) {
            if (count >= expected.size() || expected[count].tag != tag || expected[count].value != value) {
                std::cerr << "schema benchmark failed: " << name << " differs from fixed FIX44 payload\n";
                std::exit(1);
            }
            ++count;
        }, visits);
        require(count == expected.size(), "fixed-payload reference field count mismatch");
    }

    void cross_check() const {
        cross_check(local, "shape-local reference");
        cross_check(dense, "dense reference");
        cross_check(active, "active-index reference");
    }

    void run(std::size_t iterations) {
        reset_and_fill();
        const auto label = std::string{"_same_fix44_payload_S7_K7_B"} +
            std::to_string(value_bytes) + "_N" + std::to_string(dense_size);
        measure(std::string{"shape_local_fill_enumerate"} + label, iterations,
            {wire_bytes, sizeof(local), sizeof(local.slots), tags.size(), 0, 0}, [&](std::size_t) {
                local.reset();
                for (std::size_t i = tags.size(); i != 0; --i) {
                    const auto index = i - 1;
                    local.set(index, expected[index].value, expected[index].tag);
                }
                std::size_t visited = 0;
                std::uint64_t sum = 0;
                local.enumerate([&](std::uint32_t tag, std::string_view value) {
                    sum += tag + value.size();
                }, visited);
                consume(sum + visited);
            });
        measure(std::string{"full_schema_dense_fill_enumerate"} + label, iterations,
            {wire_bytes, sizeof(dense), sizeof(dense.slots), dense_size, 0, 0}, [&](std::size_t) {
                dense.reset();
                for (std::size_t i = tags.size(); i != 0; --i) {
                    const auto index = i - 1;
                    dense.set(index, expected[index].value, expected[index].tag);
                }
                std::size_t visited = 0;
                std::uint64_t sum = 0;
                dense.enumerate([&](std::uint32_t tag, std::string_view value) {
                    sum += tag + value.size();
                }, visited);
                consume(sum + visited);
            });
        measure(std::string{"active_index_sparse_fill_enumerate"} + label, iterations,
            {wire_bytes, sizeof(active), sizeof(active.slots) + sizeof(active.active),
                tags.size(), tags.size(), tags.size() * (tags.size() - 1) / 2}, [&](std::size_t) {
                active.reset();
                for (std::size_t i = tags.size(); i != 0; --i) {
                    const auto index = i - 1;
                    active.insert_sorted(index, expected[index].value, expected[index].tag);
                }
                std::size_t visited = 0;
                std::uint64_t sum = 0;
                active.enumerate([&](std::uint32_t tag, std::string_view value) {
                    sum += tag + value.size();
                }, visited);
                consume(sum + visited + active.comparisons);
            });
    }
};

template <std::size_t S, std::size_t K, std::size_t B>
void run_sparse_case(std::size_t iterations) {
    SparseCase<S, K, B> test;
    char label[64];
    const auto result = std::snprintf(label, sizeof(label), "S%zu_K%zu_B%zu_N%zu", S, K, B,
        nf::fix44::Schema::field_count);
    require(result > 0 && static_cast<std::size_t>(result) < sizeof(label), "case label formatting failed");
    test.run(iterations, label);
}

struct OrderInput {
    std::array<char, 16> cl_ord_id{};
    std::int64_t quantity{100};
};

[[nodiscard]] std::vector<OrderInput> make_workset(std::size_t count) {
    std::vector<OrderInput> result(count);
    for (std::size_t i = 0; i < result.size(); ++i) {
        constexpr std::string_view prefix = "BENCH-";
        std::memcpy(result[i].cl_ord_id.data(), prefix.data(), prefix.size());
        auto numeric = i % 1'000'000;
        for (std::size_t digit = prefix.size() + 6; digit != prefix.size(); --digit) {
            result[i].cl_ord_id[digit - 1] = static_cast<char>('0' + numeric % 10);
            numeric /= 10;
        }
        result[i].cl_ord_id[prefix.size() + 6] = '\0';
        result[i].quantity = static_cast<std::int64_t>(100 + i % 900);
    }
    return result;
}

void verify_order(std::string_view wire, std::string_view expected_id) {
    std::array<nf::FieldView, 128> scratch{};
    nf::FlatMessageView flat{};
    require(parse_frame(wire, scratch, flat).ok(), "generated order wire failed flat parse");
    require(nf::validate_schema_message(nf::fix44::Schema::descriptor(), flat, nf::ValidationLevel::strict).ok(),
        "generated order wire failed strict FIX44 schema validation");
    nf::fix44::NewOrderSingleView typed{flat};
    const auto id = typed.cl_ord_id();
    const auto qty = typed.order_qty();
    require(id.ok() && id.value() == expected_id && qty.ok(), "generated typed FIX44 projection mismatch");
}

void verify_group_roundtrip(std::span<char> output, std::size_t& wire_size) {
    const auto encoded = nf::encode_fix44_stream<nf::fix44::ExecutionReport>(header, output, [](auto& w) {
        w.order_id("ORDER-1")
         .exec_id("EXEC-1")
         .exec_type('0')
         .ord_status('0')
         .side('1')
         .leaves_qty(100)
         .cum_qty(0)
         .avg_px(0)
         .no_party_i_ds(3, [](auto& g) {
             for (std::size_t index = 0; index < 3; ++index) {
                 char id[7] = {'P', 'A', 'R', 'T', 'Y', static_cast<char>('0' + index), '\0'};
                 g.entry()
                     .party_id(std::string_view{id, 6})
                     .party_id_source('D')
                     .party_role(static_cast<std::int64_t>(index + 1));
             }
         });
    });
    require(encoded.ok(), "FIX44 ExecutionReport with PartyIDs failed to encode");
    wire_size = encoded.value();

    std::array<nf::FieldView, 256> scratch;
    nf::FlatMessageView flat{};
    require(parse_frame({output.data(), wire_size}, scratch, flat).ok(), "group roundtrip frame failed flat parse");
    require(nf::validate_schema_message(nf::fix44::Schema::descriptor(), flat,
        nf::ValidationLevel::strict).ok(), "group roundtrip failed strict schema validation");
    nf::fix44::ExecutionReportView typed{flat};
    const auto parsed_group = typed.no_party_i_ds();
    require(parsed_group.ok() && parsed_group.value().size() == 3, "typed group count mismatch");
    for (std::size_t index = 0; index < 3; ++index) {
        const auto entry = parsed_group.value().at(index);
        const auto id = entry.template get<448>();
        const auto source = entry.template get<447>();
        const auto role = entry.template get<452>();
        require(entry.status().ok() && id.ok() && source.ok() && role.ok(), "typed group entry projection failed");
        const std::string_view expected = index == 0 ? "PARTY0" : index == 1 ? "PARTY1" : "PARTY2";
        require(id.value() == expected && source.value() == 'D' &&
            role.value() == static_cast<std::int64_t>(index + 1), "typed group data mismatch");
    }
}

} // namespace

extern "C" void* __real_malloc(std::size_t);
extern "C" void* __wrap_malloc(std::size_t size) {
    if (track_allocations) { ++allocation_calls; allocation_bytes += size; }
    return __real_malloc(size);
}

void* operator new(std::size_t size) {
    if (auto* result = std::malloc(size == 0 ? 1 : size)) return result;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
[[gnu::noinline]] void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

int main(int argc, char** argv) {
    std::size_t iterations = 5'000;
    std::size_t workset_size = 1024;
    if (argc > 4) {
        std::cerr << "usage: schema-bench [samples: 100..100000] [workset: 1..8192] [--pmu]\n";
        return 2;
    }
    if (argc >= 2) {
        const std::string_view value{argv[1]};
        const auto converted = std::from_chars(value.data(), value.data() + value.size(), iterations);
        if (converted.ec != std::errc{} || converted.ptr != value.data() + value.size() ||
            iterations < 100 || iterations > 100'000) return 2;
    }
    if (argc >= 3) {
        const std::string_view value{argv[2]};
        const auto converted = std::from_chars(value.data(), value.data() + value.size(), workset_size);
        if (converted.ec != std::errc{} || converted.ptr != value.data() + value.size() ||
            workset_size < 1 || workset_size > 8192) return 2;
    }
    const bool collect_pmu = argc == 4 && std::string_view{argv[3]} == "--pmu";
    if (argc == 4 && !collect_pmu) {
        std::cerr << "usage: schema-bench [samples: 100..100000] [workset: 1..8192] [--pmu]\n";
        return 2;
    }
    nf_bench::PerfCounters perf_counters;
    if (collect_pmu) perf_counters.start();

    std::array<char, max_frame_bytes> buffer{};
    const auto encoded_size = encode_order(hot_cl_ord_id, 100, buffer);
    std::array<char, max_frame_bytes> direct_buffer{};
    std::size_t direct_size = 0;
    const DirectValues direct_values{hot_cl_ord_id, symbol, sending_time, 100};
    require(direct_ordered_writer(direct_values, direct_buffer, direct_size), "direct bounded writer failed");
    if (direct_size != encoded_size ||
        std::memcmp(direct_buffer.data(), buffer.data(), std::min(direct_size, encoded_size)) != 0) {
        std::cerr << "generated(" << encoded_size << ")=";
        std::cerr.write(buffer.data(), static_cast<std::streamsize>(encoded_size));
        std::cerr << "\ndirect(" << direct_size << ")=";
        std::cerr.write(direct_buffer.data(), static_cast<std::streamsize>(direct_size));
        std::cerr << '\n';
        fail("direct ordered writer differs from the generated FIX44 encoder");
    }
    std::array<char, 8> too_short{};
    std::size_t ignored_size = 0;
    require(!direct_ordered_writer(direct_values, too_short, ignored_size),
        "direct bounded writer accepted a too-small output span");
    verify_error_boundaries({buffer.data(), encoded_size});
    verify_order({buffer.data(), encoded_size}, hot_cl_ord_id);

    std::cout << "case,iteration,elapsed_ns,allocation_calls,allocation_bytes,wire_bytes,instance_bytes,"
                 "initialization_bytes,visited_slots,active_indices,sort_comparisons\n";
    const auto workset = make_workset(workset_size);
    const CaseMeta generated_meta{encoded_size, sizeof(nf::fix44::NewOrderSingle::Writer), 0, 7, 0, 0};
    measure("fix44_generated_build_encode_hot", iterations, generated_meta, [&](std::size_t) {
        const auto size = encode_order(hot_cl_ord_id, 100, buffer);
        consume(size + static_cast<unsigned char>(buffer[size - 1]));
    });
    measure("fix44_generated_build_encode_rotating", iterations, generated_meta, [&](std::size_t i) {
        const auto& input = workset[i % workset.size()];
        const std::string_view id{input.cl_ord_id.data(), std::strlen(input.cl_ord_id.data())};
        const auto size = encode_order(id, input.quantity, buffer);
        consume(size + static_cast<unsigned char>(buffer[size - 1]));
    });

    auto dynamic_order = make_dynamic_order();
    const auto dynamic_fixture = nf::encode_fix44(dynamic_order, header, direct_buffer);
    const auto static_fixture_size = encode_order(hot_cl_ord_id, 100, buffer);
    require(dynamic_fixture.ok() && dynamic_fixture.value() == static_fixture_size &&
        std::memcmp(buffer.data(), direct_buffer.data(), static_fixture_size) == 0,
        "dynamic and generated order wire differ");
    const CaseMeta dynamic_meta{encoded_size, sizeof(nf::DynamicMessage), 0, 7, 0, 0};
    measure("dynamic_runtime_tags_build_encode", iterations, dynamic_meta, [&](std::size_t) {
        auto message = make_dynamic_order();
        const auto result = nf::encode_fix44(message, header, direct_buffer);
        require(result.ok());
        consume(result.value() + static_cast<unsigned char>(direct_buffer[result.value() - 1]));
    });
    measure("dynamic_prebuilt_encode_only", iterations, dynamic_meta, [&](std::size_t) {
        const auto result = nf::encode_fix44(dynamic_order, header, direct_buffer);
        require(result.ok());
        consume(result.value() + static_cast<unsigned char>(direct_buffer[result.value() - 1]));
    });

    std::array<nf::FieldView, 128> flat_scratch{};
    nf::FlatMessageView parsed{};
    const auto fixture_size = encode_order(hot_cl_ord_id, 100, buffer);
    require(fixture_size == encoded_size, "rotating and hot orders produced different wire sizes");
    require(parse_frame({buffer.data(), fixture_size}, flat_scratch, parsed).ok(),
        "benchmark fixture parse failed");
    const auto parsed_field_count = parsed.fields().size();
    FixedOrderReferences same_payload_refs{parsed, fixture_size};
    measure("fix44_parse_flat", iterations, {encoded_size, sizeof(nf::FlatMessageView), 0,
        parsed_field_count, 0, 0}, [&](std::size_t) {
        std::array<nf::FieldView, 128> scratch;
        nf::FlatMessageView flat{};
        require(parse_frame({buffer.data(), encoded_size}, scratch, flat).ok(), "flat parse failed");
        consume(flat.fields().size() + flat.fields().back().tag);
    });
    measure("fix44_strict_validation_only", iterations, {encoded_size, sizeof(parsed), 0,
        parsed_field_count, 0, 0}, [&](std::size_t) {
        const auto status = nf::validate_schema_message(nf::fix44::Schema::descriptor(), parsed,
            nf::ValidationLevel::strict);
        require(status.ok(), "strict FIX44 validation failed");
        consume(parsed.fields().size());
    });
    measure("fix44_typed_parse_strict_get", iterations, {encoded_size, sizeof(nf::fix44::NewOrderSingleView), 0,
        parsed_field_count, 0, 0}, [&](std::size_t) {
        std::array<nf::FieldView, 128> scratch;
        nf::FlatMessageView flat{};
        require(parse_frame({buffer.data(), encoded_size}, scratch, flat).ok(), "typed parse flat stage failed");
        require(nf::validate_schema_message(nf::fix44::Schema::descriptor(), flat,
            nf::ValidationLevel::strict).ok(), "typed parse strict validation failed");
        nf::fix44::NewOrderSingleView typed{flat};
        const auto id = typed.cl_ord_id();
        const auto qty = typed.order_qty();
        require(id.ok() && qty.ok(), "typed FIX44 getters failed");
        consume(id.value().size() + static_cast<std::uint64_t>(qty.value().coefficient()));
    });
    measure("fix44_direct_bounded_ordered_writer", iterations, {encoded_size, sizeof(DirectValues), 0,
        7, 0, 0}, [&](std::size_t) {
        std::size_t size = 0;
        require(direct_ordered_writer(direct_values, direct_buffer, size), "direct writer rejected valid payload");
        consume(size + static_cast<unsigned char>(direct_buffer[size - 1]));
    });
    same_payload_refs.run(iterations);

    std::size_t group_wire_size = 0;
    verify_group_roundtrip(buffer, group_wire_size);
    measure("fix44_group_build_encode_parse_validate_project", iterations,
        {group_wire_size, sizeof(nf::fix44::ExecutionReport::Writer), 0, 17, 3, 0}, [&](std::size_t) {
        std::size_t size = 0;
        verify_group_roundtrip(buffer, size);
        consume(size + 3);
    });

    run_sparse_case<8, 2, 8>(iterations);
    run_sparse_case<8, 8, 8>(iterations);
    run_sparse_case<32, 2, 8>(iterations);
    run_sparse_case<32, 8, 64>(iterations);
    run_sparse_case<32, 32, 128>(iterations);

    if (result_sink == 0) fail("result sink remained zero");
    if (collect_pmu) {
        perf_counters.stop();
        std::cerr << "{\"pmu\":" << perf_counters.json() << "}\n";
    }
    std::cerr << "{\"schema\":\"FIX.4.4\",\"field_count\":" << nf::fix44::Schema::field_count
              << ",\"message_count\":" << nf::fix44::Schema::message_count
              << ",\"group_count\":" << nf::fix44::Schema::group_count
              << ",\"workset_size\":" << workset_size << "}\n";
    return 0;
}
