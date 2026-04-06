#include <filesystem>
#include <new>
#include <optional>
#include <string>
#include <string_view>

#include "bench_support.h"

#include "DataDictionary.h"
#include "Field.h"
#include "Message.h"
#include "fix44/NewOrderSingle.h"

namespace {

using bench_support::BenchmarkMeasurement;
using bench_support::BenchmarkResult;
using bench_support::BuildFix44BusinessOrder;
using bench_support::Fix44BusinessOrder;
using bench_support::ReportMetric;

auto PrintUsage() -> void {
    std::cout << "usage: quickfix-cpp-bench --xml <FIX44.xml> [--iterations <count>]\n";
}

auto ToQuickFixTimestamp(const bench_support::BenchmarkTimestamp& timestamp) -> FIX::UtcTimeStamp {
    return FIX::UtcTimeStamp(
        timestamp.hour,
        timestamp.minute,
        timestamp.second,
        timestamp.millisecond,
        timestamp.day,
        timestamp.month,
        timestamp.year);
}

auto BuildOrderFromBusinessObject(const Fix44BusinessOrder& order_request) -> FIX44::NewOrderSingle {
    FIX44::NewOrderSingle order(
        FIX::ClOrdID(std::string(order_request.cl_ord_id)),
        FIX::Side(order_request.side),
        FIX::TransactTime(ToQuickFixTimestamp(order_request.transact_time)),
        FIX::OrdType(order_request.ord_type));
    order.set(FIX::Symbol(std::string(order_request.symbol)));
    order.set(FIX::OrderQty(static_cast<double>(order_request.order_qty)));
    if (order_request.price.has_value()) {
        order.set(FIX::Price(order_request.price.value()));
    }

    FIX44::NewOrderSingle::NoPartyIDs party;
    party.set(FIX::PartyID(std::string(order_request.party_id)));
    party.set(FIX::PartyIDSource(order_request.party_id_source));
    party.set(FIX::PartyRole(static_cast<int>(order_request.party_role)));
    order.addGroup(party);
    return order;
}

auto ApplyStaticHeader(FIX44::NewOrderSingle* order) -> void {
    if (order == nullptr) {
        return;
    }
    order->getHeader().setField(FIX::BeginString(std::string(bench_support::kDefaultBeginString)));
    order->getHeader().setField(FIX::SenderCompID(std::string(bench_support::kDefaultSenderCompId)));
    order->getHeader().setField(FIX::TargetCompID(std::string(bench_support::kDefaultTargetCompId)));
}

auto ApplyBenchmarkHeader(
    FIX44::NewOrderSingle* order,
    std::uint32_t msg_seq_num,
    const FIX::UtcTimeStamp& sending_time) -> void {
    ApplyStaticHeader(order);
    if (order == nullptr) {
        return;
    }
    order->getHeader().setField(FIX::MsgSeqNum(static_cast<int>(msg_seq_num)));
    order->getHeader().setField(FIX::SendingTime(sending_time));
}

auto BuildSampleFrame(const Fix44BusinessOrder& business_order, const FIX::UtcTimeStamp& sending_time) -> std::string {
    auto sample = BuildOrderFromBusinessObject(business_order);
    ApplyBenchmarkHeader(&sample, 1U, sending_time);
    return sample.toString();
}

auto ReportQuickFixMetric(const std::string& label, const BenchmarkResult& result) -> void {
    ReportMetric(label, result, 44);
}

auto RunParseBenchmark(const FIX::DataDictionary& dictionary, const std::string& sample_frame, std::uint32_t iterations)
    -> BenchmarkResult {
    BenchmarkResult result;
    result.samples_ns.reserve(iterations);
    BenchmarkMeasurement measurement;
    for (std::uint32_t index = 0; index < iterations; ++index) {
        const auto started = std::chrono::steady_clock::now();
        FIX44::NewOrderSingle parsed;
        parsed.setString(sample_frame, true, &dictionary, &dictionary);
        result.samples_ns.push_back(bench_support::DurationNs(started, std::chrono::steady_clock::now()));
    }
    measurement.Finish(result);
    return result;
}

auto RunEncodeBenchmark(
    const Fix44BusinessOrder& business_order,
    const FIX::UtcTimeStamp& sending_time,
    std::uint32_t iterations) -> BenchmarkResult {
    BenchmarkResult result;
    result.samples_ns.reserve(iterations);
    BenchmarkMeasurement measurement;
    for (std::uint32_t index = 0; index < iterations; ++index) {
        const auto started = std::chrono::steady_clock::now();
        auto order = BuildOrderFromBusinessObject(business_order);
        ApplyBenchmarkHeader(&order, index + 1U, sending_time);
        const auto encoded = order.toString();
        static_cast<void>(encoded);
        result.samples_ns.push_back(bench_support::DurationNs(started, std::chrono::steady_clock::now()));
    }
    measurement.Finish(result);
    return result;
}

auto RunEncodeBufferBenchmark(
    const Fix44BusinessOrder& business_order,
    const FIX::UtcTimeStamp& sending_time,
    std::uint32_t iterations) -> BenchmarkResult {
    BenchmarkResult result;
    result.samples_ns.reserve(iterations);
    std::string buffer;
    BenchmarkMeasurement measurement;
    for (std::uint32_t index = 0; index < iterations; ++index) {
        const auto started = std::chrono::steady_clock::now();
        auto order = BuildOrderFromBusinessObject(business_order);
        ApplyBenchmarkHeader(&order, index + 1U, sending_time);
        order.toString(buffer);
        result.samples_ns.push_back(bench_support::DurationNs(started, std::chrono::steady_clock::now()));
    }
    measurement.Finish(result);
    return result;
}

}  // namespace

void* operator new(std::size_t size) {
    auto* memory = bench_support::AllocateRaw(size, alignof(std::max_align_t));
    if (memory == nullptr) {
        throw std::bad_alloc();
    }
    bench_support::RecordAllocation(size);
    return memory;
}

void* operator new[](std::size_t size) {
    auto* memory = bench_support::AllocateRaw(size, alignof(std::max_align_t));
    if (memory == nullptr) {
        throw std::bad_alloc();
    }
    bench_support::RecordAllocation(size);
    return memory;
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    auto* memory = bench_support::AllocateRaw(size, alignof(std::max_align_t));
    if (memory != nullptr) {
        bench_support::RecordAllocation(size);
    }
    return memory;
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    auto* memory = bench_support::AllocateRaw(size, alignof(std::max_align_t));
    if (memory != nullptr) {
        bench_support::RecordAllocation(size);
    }
    return memory;
}

void* operator new(std::size_t size, std::align_val_t alignment) {
    auto* memory = bench_support::AllocateRaw(size, static_cast<std::size_t>(alignment));
    if (memory == nullptr) {
        throw std::bad_alloc();
    }
    bench_support::RecordAllocation(size);
    return memory;
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    auto* memory = bench_support::AllocateRaw(size, static_cast<std::size_t>(alignment));
    if (memory == nullptr) {
        throw std::bad_alloc();
    }
    bench_support::RecordAllocation(size);
    return memory;
}

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    auto* memory = bench_support::AllocateRaw(size, static_cast<std::size_t>(alignment));
    if (memory != nullptr) {
        bench_support::RecordAllocation(size);
    }
    return memory;
}

void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    auto* memory = bench_support::AllocateRaw(size, static_cast<std::size_t>(alignment));
    if (memory != nullptr) {
        bench_support::RecordAllocation(size);
    }
    return memory;
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete[](void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::align_val_t) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, std::align_val_t) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t, std::align_val_t) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept {
    std::free(memory);
}

int main(int argc, char** argv) {
    std::filesystem::path xml_path;
    std::uint32_t iterations = 100000U;

    for (int index = 1; index < argc; ++index) {
        const std::string_view arg(argv[index]);
        if (arg == "--xml" && index + 1 < argc) {
            xml_path = argv[++index];
            continue;
        }
        if (arg == "--iterations" && index + 1 < argc) {
            iterations = static_cast<std::uint32_t>(std::stoul(argv[++index]));
            continue;
        }
        PrintUsage();
        return 1;
    }

    if (xml_path.empty()) {
        PrintUsage();
        return 1;
    }

    FIX::DataDictionary dictionary(xml_path.string());
    const auto business_order = BuildFix44BusinessOrder();
    const auto fixed_sending_time = ToQuickFixTimestamp(business_order.transact_time);
    const auto sample_frame = BuildSampleFrame(business_order, fixed_sending_time);

    const auto parse_result = RunParseBenchmark(dictionary, sample_frame, iterations);
    ReportQuickFixMetric("quickfix-parse", parse_result);

    std::cout << "quickfix encode uses fixed SendingTime; quickfix-encode and quickfix-encode-buffer share the same serializer\n";

    const auto encode_result = RunEncodeBenchmark(business_order, fixed_sending_time, iterations);
    ReportQuickFixMetric("quickfix-encode", encode_result);

    const auto encode_buffer_result = RunEncodeBufferBenchmark(business_order, fixed_sending_time, iterations);
    ReportQuickFixMetric("quickfix-encode-buffer", encode_buffer_result);
    return 0;
}