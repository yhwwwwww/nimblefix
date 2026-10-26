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
#include <string_view>
#include <sys/resource.h>
#include <vector>

#include <nimblefix/codec.hpp>
#include <nimblefix/fix44_sample.hpp>

namespace nf = nimblefix;
using namespace nf::literals;

namespace {
// Linux linker wrappers count this executable's direct malloc calls, including
// fluent storage, plus C++ new. Dynamic libraries' internal mallocs are excluded.
bool track_allocations = false;
std::size_t allocation_calls = 0;
std::size_t allocation_bytes = 0;
std::uint64_t result_sink = 0;

constexpr nf::SessionHeader header{"BUY", "SELL", 1, "20261026-12:00:00.000"};
using OrderShape = nf::Fields<
    nf::Field<11, nf::FieldKind::string>, nf::Field<55, nf::FieldKind::string>,
    nf::Field<54, nf::FieldKind::character>, nf::Field<60, nf::FieldKind::string>,
    nf::Field<38, nf::FieldKind::signed_integer>, nf::Field<40, nf::FieldKind::character>,
    nf::Field<44, nf::FieldKind::decimal>>;

void require(bool good) {
    if (!good) { std::cerr << "benchmark operation failed\n"; std::exit(1); }
}

void consume(const void* pointer, std::size_t size) {
    asm volatile("" : : "r"(pointer), "r"(size) : "memory");
    result_sink += size;
}

template <class Message>
void encode(const Message& value, std::span<char> buffer) {
    const auto encoded = nf::encode_fix44(value, header, buffer);
    require(encoded.ok());
    consume(buffer.data(), encoded.value());
}

struct Sample { std::uint64_t ns; std::size_t allocations; std::size_t bytes; };

template <class Function>
void measure(std::string_view name, std::size_t iterations, Function operation) {
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
        samples[i] = {static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count()),
            allocation_calls, allocation_bytes};
    }
    const auto wall_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - wall_start).count();
    getrusage(RUSAGE_SELF, &after);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        std::cout << name << ',' << i << ',' << samples[i].ns << ','
                  << samples[i].allocations << ',' << samples[i].bytes << '\n';
    }
    std::cerr << "{\"case\":\"" << name << "\",\"samples\":" << iterations
              << ",\"warmup\":" << warmup << ",\"wall_ns\":" << wall_ns
              << ",\"minor_faults\":" << after.ru_minflt - before.ru_minflt
              << ",\"major_faults\":" << after.ru_majflt - before.ru_majflt << "}\n";
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
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }

int main(int argc, char** argv) {
    std::size_t iterations = 10'000;
    if (argc > 2) { std::cerr << "usage: codec-bench [samples: 100..1000000]\n"; return 2; }
    if (argc == 2) {
        const std::string_view text{argv[1]};
        const auto converted = std::from_chars(text.data(), text.data() + text.size(), iterations);
        if (converted.ec != std::errc{} || converted.ptr != text.data() + text.size() || iterations < 100 || iterations > 1'000'000) return 2;
    }
    std::array<char, 1024> buffer;
    nf::FixedMessage<"D", OrderShape, 64> fixed;
    auto fill_fixed = [&](std::size_t i) {
        fixed.reset();
        require(fixed.set<11>("DEMO-001").ok());
        require(fixed.set<55>("AAPL").ok());
        require(fixed.set<54>('1').ok());
        require(fixed.set<60>("20261026-12:00:00.000").ok());
        require(fixed.set<38>(static_cast<int>(i % 1000 + 1)).ok());
        require(fixed.set<40>('2').ok());
        require(fixed.set<44>("150.25"_dec).ok());
    };
    fill_fixed(0);
    const auto encoded = nf::encode_fix44(fixed, header, buffer);
    require(encoded.ok());
    std::array<char, 1024> stable_frame = buffer;
    const std::string_view wire(stable_frame.data(), encoded.value());

    std::cout << "case,iteration,elapsed_ns,allocation_calls,allocation_bytes\n";
    measure("clock_baseline", iterations, [&](std::size_t i) { consume(&i, 0); });
    measure("null_copy", iterations, [&](std::size_t) {
        std::memcpy(buffer.data(), stable_frame.data(), wire.size()); consume(buffer.data(), wire.size());
    });
    measure("fixed_encode", iterations, [&](std::size_t) { encode(fixed, buffer); });
    measure("fixed_build_encode", iterations, [&](std::size_t i) { fill_fixed(i); encode(fixed, buffer); });
    measure("fluent_build_encode", iterations, [&](std::size_t i) {
        auto value = nf::message<"D">().set<11>("DEMO-001").set<55>("AAPL").set<54>('1')
            .set<60>("20261026-12:00:00.000").set<38>(static_cast<int>(i % 1000 + 1))
            .set<40>('2').set<44>("150.25"_dec);
        encode(value, buffer);
    });
    measure("generated_build_encode", iterations, [&](std::size_t i) {
        auto value = nf::fix44_sample::NewOrderSingle::make().cl_ord_id("DEMO-001")
            .symbol("AAPL").side('1').transact_time("20261026-12:00:00.000")
            .order_qty(static_cast<int>(i % 1000 + 1)).ord_type('2').price("150.25"_dec);
        encode(value, buffer);
    });
    measure("generated_tag_build_encode", iterations, [&](std::size_t i) {
        auto value = nf::fix44_sample::NewOrderSingle::make().set<11>("DEMO-001")
            .set<55>("AAPL").set<54>('1').set<60>("20261026-12:00:00.000")
            .set<38>(static_cast<int>(i % 1000 + 1)).set<40>('2').set<44>("150.25"_dec);
        encode(value, buffer);
    });
    std::array<nf::FieldView, 32> scratch;
    measure("frame_flat_parse", iterations, [&](std::size_t) {
        nf::FrameView frame;
        require(nf::inspect_frame(wire, frame).ok());
        nf::FlatMessageView view;
        require(nf::parse_flat(frame, scratch, view).ok());
        consume(scratch.data(), view.fields().size());
    });
    return result_sink == 0 ? 1 : 0;
}
