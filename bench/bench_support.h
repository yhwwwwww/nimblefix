#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <time.h>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace bench_support {

inline std::atomic<std::uint64_t> g_allocation_count{0};
inline std::atomic<std::uint64_t> g_allocated_bytes{0};

struct AllocationSnapshot {
    std::uint64_t count{0};
    std::uint64_t bytes{0};
};

struct BenchmarkTimestamp {
    std::string_view text;
    int year{0};
    int month{0};
    int day{0};
    int hour{0};
    int minute{0};
    int second{0};
    int millisecond{0};
};

struct Fix44BusinessOrder {
    std::string_view cl_ord_id;
    std::string_view symbol;
    char side{'1'};
    BenchmarkTimestamp transact_time{};
    std::int64_t order_qty{0};
    char ord_type{'2'};
    std::optional<double> price;
    std::string_view party_id;
    char party_id_source{'D'};
    std::int64_t party_role{0};
};

struct BenchmarkResult {
    std::uint64_t wall_total_ns{0};
    std::uint64_t cpu_total_ns{0};
    std::uint64_t allocation_count{0};
    std::uint64_t allocated_bytes{0};
    std::vector<std::uint64_t> samples_ns;
    std::string work_label;
    std::uint64_t work_count{0};
    std::optional<std::uint64_t> cache_miss_count;
    std::optional<std::uint64_t> branch_miss_count;
};

struct PercentileSummary {
    std::uint64_t min_ns{0};
    std::uint64_t p50_ns{0};
    std::uint64_t p95_ns{0};
    std::uint64_t p99_ns{0};
    std::uint64_t p999_ns{0};
    std::uint64_t max_ns{0};
};

inline constexpr std::string_view kDefaultBeginString = "FIX.4.4";
inline constexpr std::string_view kDefaultSenderCompId = "BUY";
inline constexpr std::string_view kDefaultTargetCompId = "SELL";
inline constexpr std::string_view kBenchmarkClOrdId = "BENCH-ORDER-0001";
inline constexpr std::string_view kBenchmarkSymbol = "BENCH";
inline constexpr std::string_view kBenchmarkPartyId = "BENCH-PARTY";
inline constexpr char kBenchmarkSide = '1';
inline constexpr char kBenchmarkOrdType = '2';
inline constexpr char kBenchmarkPartyIdSource = 'D';
inline constexpr int kBenchmarkPartyRole = 3;
inline constexpr int kBenchmarkOrderQty = 100;
inline constexpr double kBenchmarkPrice = 123.45;

inline constexpr BenchmarkTimestamp kBenchmarkTimestamp{
    .text = "20260406-12:34:56.789",
    .year = 2026,
    .month = 4,
    .day = 6,
    .hour = 12,
    .minute = 34,
    .second = 56,
    .millisecond = 789,
};

inline auto BuildFix44BusinessOrder(bool include_price = true) -> Fix44BusinessOrder {
    Fix44BusinessOrder order{
        .cl_ord_id = kBenchmarkClOrdId,
        .symbol = kBenchmarkSymbol,
        .side = kBenchmarkSide,
        .transact_time = kBenchmarkTimestamp,
        .order_qty = kBenchmarkOrderQty,
        .ord_type = kBenchmarkOrdType,
        .price = kBenchmarkPrice,
        .party_id = kBenchmarkPartyId,
        .party_id_source = kBenchmarkPartyIdSource,
        .party_role = kBenchmarkPartyRole,
    };
    if (!include_price) {
        order.price.reset();
    }
    return order;
}

inline auto NormalizeAllocationSize(std::size_t size) noexcept -> std::size_t {
    return size == 0U ? 1U : size;
}

inline auto AllocateRaw(std::size_t size, std::size_t alignment) noexcept -> void* {
    const auto normalized = NormalizeAllocationSize(size);
    if (alignment <= alignof(std::max_align_t)) {
        return std::malloc(normalized);
    }

    void* memory = nullptr;
    if (posix_memalign(&memory, alignment, normalized) != 0) {
        return nullptr;
    }
    return memory;
}

inline auto RecordAllocation(std::size_t size) noexcept -> void {
    const auto normalized = static_cast<std::uint64_t>(NormalizeAllocationSize(size));
    g_allocation_count.fetch_add(1U, std::memory_order_seq_cst);
    g_allocated_bytes.fetch_add(normalized, std::memory_order_seq_cst);
}

inline auto CurrentAllocationSnapshot() -> AllocationSnapshot {
    return AllocationSnapshot{
        .count = g_allocation_count.load(std::memory_order_seq_cst),
        .bytes = g_allocated_bytes.load(std::memory_order_seq_cst),
    };
}

inline auto NowNs() -> std::uint64_t {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
                                              .count());
}

#if defined(__linux__)
class PerfEventCounter {
  public:
    PerfEventCounter() = default;

    explicit PerfEventCounter(std::uint64_t config) {
        Open(config);
    }

    PerfEventCounter(const PerfEventCounter&) = delete;
    auto operator=(const PerfEventCounter&) -> PerfEventCounter& = delete;

    PerfEventCounter(PerfEventCounter&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)) {
    }

    auto operator=(PerfEventCounter&& other) noexcept -> PerfEventCounter& {
        if (this != &other) {
            Close();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    ~PerfEventCounter() {
        Close();
    }

    auto Start() -> bool {
        if (fd_ < 0) {
            return false;
        }
        if (ioctl(fd_, PERF_EVENT_IOC_RESET, 0) != 0) {
            return false;
        }
        return ioctl(fd_, PERF_EVENT_IOC_ENABLE, 0) == 0;
    }

    auto StopAndRead() -> std::optional<std::uint64_t> {
        if (fd_ < 0) {
            return std::nullopt;
        }
        static_cast<void>(ioctl(fd_, PERF_EVENT_IOC_DISABLE, 0));

        std::uint64_t value = 0;
        if (::read(fd_, &value, sizeof(value)) != static_cast<ssize_t>(sizeof(value))) {
            return std::nullopt;
        }
        return value;
    }

  private:
    auto Open(std::uint64_t config) -> void {
        perf_event_attr attr{};
        attr.type = PERF_TYPE_HARDWARE;
        attr.size = sizeof(attr);
        attr.config = config;
        attr.disabled = 1;
        attr.exclude_kernel = 1;
        attr.exclude_hv = 1;
        attr.exclude_idle = 1;

        fd_ = static_cast<int>(::syscall(__NR_perf_event_open, &attr, 0, -1, -1, 0));
    }

    auto Close() -> void {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    int fd_{-1};
};
#endif

class PerfCounterScope {
  public:
    PerfCounterScope() = default;

    auto Start() -> void {
#if defined(__linux__)
        cache_started_ = cache_misses_.Start();
        branch_started_ = branch_misses_.Start();
#endif
    }

    auto Finish(std::optional<std::uint64_t>& cache_miss_count, std::optional<std::uint64_t>& branch_miss_count)
        -> void {
#if defined(__linux__)
        if (cache_started_) {
            cache_miss_count = cache_misses_.StopAndRead();
        }
        if (branch_started_) {
            branch_miss_count = branch_misses_.StopAndRead();
        }
#else
        (void)cache_miss_count;
        (void)branch_miss_count;
#endif
    }

  private:
#if defined(__linux__)
    PerfEventCounter cache_misses_{PERF_COUNT_HW_CACHE_MISSES};
    PerfEventCounter branch_misses_{PERF_COUNT_HW_BRANCH_MISSES};
    bool cache_started_{false};
    bool branch_started_{false};
#endif
};

inline auto DurationNs(std::chrono::steady_clock::time_point started, std::chrono::steady_clock::time_point finished)
    -> std::uint64_t {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count());
}

inline auto ProcessCpuNs() -> std::uint64_t {
    timespec timestamp{};
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &timestamp) != 0) {
        return 0U;
    }
    return (static_cast<std::uint64_t>(timestamp.tv_sec) * 1'000'000'000ULL) +
           static_cast<std::uint64_t>(timestamp.tv_nsec);
}

class BenchmarkMeasurement {
  public:
    BenchmarkMeasurement() {
        perf_.Start();
        wall_started_ = std::chrono::steady_clock::now();
        cpu_started_ = ProcessCpuNs();
        allocation_started_ = CurrentAllocationSnapshot();
    }

    auto Finish(BenchmarkResult& result) -> void {
        const auto wall_finished = std::chrono::steady_clock::now();
        const auto cpu_finished = ProcessCpuNs();
        const auto allocation_finished = CurrentAllocationSnapshot();

        result.wall_total_ns = DurationNs(wall_started_, wall_finished);
        result.cpu_total_ns = cpu_finished - cpu_started_;
        result.allocation_count = allocation_finished.count - allocation_started_.count;
        result.allocated_bytes = allocation_finished.bytes - allocation_started_.bytes;
        perf_.Finish(result.cache_miss_count, result.branch_miss_count);
    }

  private:
    PerfCounterScope perf_;
    std::chrono::steady_clock::time_point wall_started_{};
    std::uint64_t cpu_started_{0};
    AllocationSnapshot allocation_started_{};
};

inline auto PercentileValue(const std::vector<std::uint64_t>& sorted_samples, double fraction) -> std::uint64_t {
    if (sorted_samples.empty()) {
        return 0;
    }

    const auto clamped = std::clamp(fraction, 0.0, 1.0);
    const auto rank = static_cast<std::size_t>(
        std::ceil(clamped * static_cast<double>(sorted_samples.size())));
    const auto index = rank == 0U ? 0U : std::min(rank - 1U, sorted_samples.size() - 1U);
    return sorted_samples[index];
}

inline auto SummarizePercentiles(const std::vector<std::uint64_t>& samples) -> PercentileSummary {
    if (samples.empty()) {
        return {};
    }

    auto sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    return PercentileSummary{
        .min_ns = sorted.front(),
        .p50_ns = PercentileValue(sorted, 0.50),
        .p95_ns = PercentileValue(sorted, 0.95),
        .p99_ns = PercentileValue(sorted, 0.99),
        .p999_ns = PercentileValue(sorted, 0.999),
        .max_ns = sorted.back(),
    };
}

inline auto PrintOptionalCounter(std::string_view label, const std::optional<std::uint64_t>& value) -> void {
    std::cout << ' ' << label << '=';
    if (value.has_value()) {
        std::cout << value.value();
        return;
    }
    std::cout << "n/a";
}

inline auto ReportMetric(const std::string& label, const BenchmarkResult& result, int label_width = 24) -> void {
    const auto percentiles = SummarizePercentiles(result.samples_ns);
    const auto iterations = static_cast<std::uint64_t>(result.samples_ns.size());
    const double ns_per_op = iterations == 0U
        ? 0.0
        : static_cast<double>(result.wall_total_ns) / static_cast<double>(iterations);
    const double ops_per_second = result.wall_total_ns == 0U
        ? 0.0
        : (static_cast<double>(iterations) * 1'000'000'000.0) / static_cast<double>(result.wall_total_ns);
    const double cpu_percent = result.wall_total_ns == 0U
        ? 0.0
        : (static_cast<double>(result.cpu_total_ns) * 100.0) / static_cast<double>(result.wall_total_ns);

    std::cout << std::left << std::setw(label_width) << label
              << " count=" << iterations
              << " total_ns=" << result.wall_total_ns
              << " cpu_ns=" << result.cpu_total_ns
              << " cpu_pct=" << std::fixed << std::setprecision(2) << cpu_percent
              << " allocs=" << result.allocation_count
              << " alloc_bytes=" << result.allocated_bytes
              << " avg_ns=" << std::fixed << std::setprecision(2) << ns_per_op
              << " min_ns=" << percentiles.min_ns
              << " p50_ns=" << percentiles.p50_ns
              << " p95_ns=" << percentiles.p95_ns
              << " p99_ns=" << percentiles.p99_ns
              << " p999_ns=" << percentiles.p999_ns
              << " max_ns=" << percentiles.max_ns
              << " ops_per_sec=" << std::fixed << std::setprecision(2) << ops_per_second;
    PrintOptionalCounter("cache_miss", result.cache_miss_count);
    PrintOptionalCounter("branch_miss", result.branch_miss_count);
    if (!result.work_label.empty()) {
        const double work_per_second = result.wall_total_ns == 0U
            ? 0.0
            : (static_cast<double>(result.work_count) * 1'000'000'000.0) /
                  static_cast<double>(result.wall_total_ns);
        std::cout << ' ' << result.work_label << '=' << result.work_count
                  << ' ' << result.work_label << "_per_sec=" << std::fixed << std::setprecision(2)
                  << work_per_second;
    }
    std::cout << '\n';
}

}  // namespace bench_support