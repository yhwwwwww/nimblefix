#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <nimblefix/session.hpp>
#include <nimblefix/status.hpp>

namespace nimblefix {

enum class TelemetryStage : std::uint8_t {
    read = 0,
    parse = 1,
    validate = 2,
    journal = 3,
    write = 4,
    count = 5,
};

[[nodiscard]] constexpr std::string_view telemetry_stage_name(TelemetryStage stage) noexcept {
    switch (stage) {
    case TelemetryStage::read: return "read";
    case TelemetryStage::parse: return "parse";
    case TelemetryStage::validate: return "validate";
    case TelemetryStage::journal: return "journal";
    case TelemetryStage::write: return "write";
    case TelemetryStage::count: break;
    }
    return "unknown";
}

[[nodiscard]] inline std::uint64_t telemetry_now_ns() noexcept {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
           static_cast<std::uint64_t>(ts.tv_nsec);
}

struct StageLatencySummary {
    std::uint64_t count{};
    std::uint64_t min_ns{};
    std::uint64_t p50_ns{};
    std::uint64_t p90_ns{};
    std::uint64_t p99_ns{};
    std::uint64_t p999_ns{};
    std::uint64_t p9999_ns{};
    std::uint64_t max_ns{};
};

// Lock-free, single-writer HDR-style log-linear nanosecond histogram stored directly in mmap.
// - Buckets 0..15: linear 16 ns bins covering [0, 256 ns)
// - Buckets 16..431: 26 power-of-2 octaves x 16 sub-buckets (<=6.25% error) covering [256 ns, 17.1 s)
// - Bucket 432: overflow (>= 2^34 ns)
// Recording a sample takes ~2 CPU instructions (countl_zero + relaxed increment), zero locks, zero allocs.
struct alignas(64) TelemetryHistogram {
    static constexpr std::size_t sub_buckets = 16;
    static constexpr std::size_t octaves = 26;
    static constexpr std::size_t bucket_count = sub_buckets + octaves * sub_buckets + 1; // 433

    std::atomic<std::uint64_t> count{0};
    std::atomic<std::uint64_t> sum_ns{0};
    std::atomic<std::uint64_t> min_ns{std::numeric_limits<std::uint64_t>::max()};
    std::atomic<std::uint64_t> max_ns{0};
    std::atomic<std::uint64_t> buckets[bucket_count]{};

    [[nodiscard]] static constexpr std::size_t bucket_for(std::uint64_t ns) noexcept {
        if (ns < 256ULL) {
            return static_cast<std::size_t>(ns >> 4U);
        }
        const unsigned msb = 63U - static_cast<unsigned>(std::countl_zero(ns));
        const unsigned octave = msb - 8U;
        if (octave >= octaves) {
            return bucket_count - 1;
        }
        const unsigned sub = static_cast<unsigned>((ns >> (msb - 4U)) & 0xFU);
        return sub_buckets + static_cast<std::size_t>(octave) * sub_buckets + sub;
    }

    [[nodiscard]] static constexpr std::uint64_t bucket_upper_bound_ns(std::size_t idx) noexcept {
        if (idx < sub_buckets) {
            return (static_cast<std::uint64_t>(idx) + 1ULL) << 4U;
        }
        if (idx >= bucket_count - 1) {
            return 1ULL << (8U + octaves);
        }
        const std::size_t rel = idx - sub_buckets;
        const unsigned octave = static_cast<unsigned>(rel / sub_buckets);
        const unsigned sub = static_cast<unsigned>(rel % sub_buckets);
        const unsigned shift = octave + 4U; // (8 + octave) - 4
        return (static_cast<std::uint64_t>(16U + sub + 1U)) << shift;
    }

    // Single-writer hot-path recording: no RMW lock prefix (plain relaxed load+store).
    void record(std::uint64_t ns) noexcept {
        const std::size_t idx = bucket_for(ns);
        const auto cur_b = buckets[idx].load(std::memory_order_relaxed);
        buckets[idx].store(cur_b + 1ULL, std::memory_order_relaxed);
        const auto cur_c = count.load(std::memory_order_relaxed);
        count.store(cur_c + 1ULL, std::memory_order_relaxed);
        const auto cur_s = sum_ns.load(std::memory_order_relaxed);
        sum_ns.store(cur_s + ns, std::memory_order_relaxed);
        const auto cur_min = min_ns.load(std::memory_order_relaxed);
        if (ns < cur_min) min_ns.store(ns, std::memory_order_relaxed);
        const auto cur_max = max_ns.load(std::memory_order_relaxed);
        if (ns > cur_max) max_ns.store(ns, std::memory_order_relaxed);
    }

    [[nodiscard]] StageLatencySummary summarize() const noexcept {
        StageLatencySummary out{};
        std::array<std::uint64_t, bucket_count> snap{};
        std::uint64_t total = 0;
        for (std::size_t i = 0; i < bucket_count; ++i) {
            const auto c = buckets[i].load(std::memory_order_relaxed);
            snap[i] = c;
            total += c;
        }
        if (total == 0) return out;
        out.count = total;
        const auto raw_min = min_ns.load(std::memory_order_relaxed);
        const auto raw_max = max_ns.load(std::memory_order_relaxed);
        out.min_ns = raw_min == std::numeric_limits<std::uint64_t>::max() ? 0ULL : raw_min;
        out.max_ns = raw_max;

        auto quantile = [&](double q) noexcept -> std::uint64_t {
            const std::uint64_t target = std::max<std::uint64_t>(
                1ULL, static_cast<std::uint64_t>(static_cast<double>(total) * q + 0.999999));
            std::uint64_t acc = 0;
            for (std::size_t i = 0; i < bucket_count; ++i) {
                acc += snap[i];
                if (acc >= target) {
                    const auto hi = bucket_upper_bound_ns(i);
                    return raw_max != 0 ? std::min(hi, raw_max) : hi;
                }
            }
            return raw_max;
        };
        out.p50_ns = quantile(0.50);
        out.p90_ns = quantile(0.90);
        out.p99_ns = quantile(0.99);
        out.p999_ns = quantile(0.999);
        out.p9999_ns = quantile(0.9999);
        return out;
    }
};

struct alignas(64) TelemetrySessionSlot {
    std::atomic<std::uint8_t> occupied{0};
    std::atomic<std::uint8_t> state{0}; // SessionState
    std::uint8_t reserved_pad[6]{};
    char name[128]{};
    char local_comp_id[64]{};
    char remote_comp_id[64]{};

    std::atomic<std::uint64_t> generation{0};
    std::atomic<std::uint64_t> next_inbound_seq{1};
    std::atomic<std::uint64_t> next_outbound_seq{1};
    std::atomic<std::uint64_t> inbound_frames{0};
    std::atomic<std::uint64_t> outbound_frames{0};
    std::atomic<std::uint64_t> business_deliveries{0};
    std::atomic<std::uint64_t> inbound_bytes{0};
    std::atomic<std::uint64_t> outbound_bytes{0};
    std::atomic<std::uint64_t> error_count{0};
    std::atomic<std::uint64_t> active_endpoint_index{0};
    std::atomic<std::uint64_t> last_update_ns{0};

    TelemetryHistogram stages[static_cast<std::size_t>(TelemetryStage::count)]{};

    void record_stage(TelemetryStage stage, std::uint64_t elapsed_ns) noexcept {
        stages[static_cast<std::size_t>(stage)].record(elapsed_ns);
    }

    void inc_inbound(std::size_t bytes) noexcept {
        inbound_frames.store(inbound_frames.load(std::memory_order_relaxed) + 1ULL, std::memory_order_relaxed);
        inbound_bytes.store(inbound_bytes.load(std::memory_order_relaxed) + bytes, std::memory_order_relaxed);
    }

    void inc_outbound(std::size_t bytes) noexcept {
        outbound_frames.store(outbound_frames.load(std::memory_order_relaxed) + 1ULL, std::memory_order_relaxed);
        outbound_bytes.store(outbound_bytes.load(std::memory_order_relaxed) + bytes, std::memory_order_relaxed);
    }

    void inc_delivery() noexcept {
        business_deliveries.store(
            business_deliveries.load(std::memory_order_relaxed) + 1ULL, std::memory_order_relaxed);
    }

    void inc_error() noexcept {
        error_count.store(error_count.load(std::memory_order_relaxed) + 1ULL, std::memory_order_relaxed);
    }
};

struct alignas(64) TelemetryHeader {
    static constexpr char kMagic[8] = {'N', 'F', 'X', 'T', 'E', 'L', '0', '1'};
    static constexpr std::uint32_t kFormatVersion = 1;

    char magic[8]{};
    std::uint32_t format_version{};
    std::uint32_t max_sessions{};
    std::uint64_t pid{};
    std::uint64_t created_ns{};
    std::atomic<std::uint32_t> active_sessions{0};
    std::uint32_t slot_size_bytes{};
    std::uint8_t reserved[24]{};
};

struct TelemetrySessionSnapshot {
    std::string name;
    std::string local_comp_id;
    std::string remote_comp_id;
    SessionState state{SessionState::disconnected};
    std::uint64_t generation{};
    std::uint64_t next_inbound_seq{1};
    std::uint64_t next_outbound_seq{1};
    std::uint64_t inbound_frames{};
    std::uint64_t outbound_frames{};
    std::uint64_t business_deliveries{};
    std::uint64_t inbound_bytes{};
    std::uint64_t outbound_bytes{};
    std::uint64_t error_count{};
    std::uint64_t active_endpoint_index{};
    std::uint64_t last_update_ns{};
    std::array<StageLatencySummary, static_cast<std::size_t>(TelemetryStage::count)> stages{};
};

// Writer-side mmap telemetry region owned by DirectInitiator, Engine, or shared across ShardedEngine shards.
class TelemetryWriter {
public:
    TelemetryWriter(const TelemetryWriter&) = delete;
    TelemetryWriter& operator=(const TelemetryWriter&) = delete;
    TelemetryWriter(TelemetryWriter&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)),
          mapping_(std::exchange(other.mapping_, nullptr)),
          mapped_bytes_(std::exchange(other.mapped_bytes_, 0)),
          max_sessions_(std::exchange(other.max_sessions_, 0)),
          path_(std::move(other.path_)) {}
    TelemetryWriter& operator=(TelemetryWriter&& other) noexcept {
        if (this == &other) return *this;
        close();
        fd_ = std::exchange(other.fd_, -1);
        mapping_ = std::exchange(other.mapping_, nullptr);
        mapped_bytes_ = std::exchange(other.mapped_bytes_, 0);
        max_sessions_ = std::exchange(other.max_sessions_, 0);
        path_ = std::move(other.path_);
        return *this;
    }
    ~TelemetryWriter() { close(); }

    [[nodiscard]] static Result<std::shared_ptr<TelemetryWriter>> create(
        const std::filesystem::path& path, std::size_t max_sessions = 64) noexcept {
        if (path.empty() || max_sessions == 0 || max_sessions > 1024)
            return Status{ErrorCode::invalid_config};
        try {
            if (path.has_parent_path()) {
                std::error_code ec;
                std::filesystem::create_directories(path.parent_path(), ec);
                if (ec) return Status{ErrorCode::io_error, 0, static_cast<std::size_t>(ec.value())};
            }
            const std::size_t total_bytes =
                sizeof(TelemetryHeader) + max_sessions * sizeof(TelemetrySessionSlot);
            const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            if (fd < 0) return Status{ErrorCode::io_error, 0, static_cast<std::size_t>(errno)};
            if (::ftruncate(fd, static_cast<off_t>(total_bytes)) != 0) {
                const int err = errno;
                ::close(fd);
                return Status{ErrorCode::io_error, 0, static_cast<std::size_t>(err)};
            }
            void* mem = ::mmap(nullptr, total_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            if (mem == MAP_FAILED) {
                const int err = errno;
                ::close(fd);
                return Status{ErrorCode::io_error, 0, static_cast<std::size_t>(err)};
            }
            std::memset(mem, 0, total_bytes);
            auto* hdr = static_cast<TelemetryHeader*>(mem);
            std::memcpy(hdr->magic, TelemetryHeader::kMagic, sizeof(hdr->magic));
            hdr->format_version = TelemetryHeader::kFormatVersion;
            hdr->max_sessions = static_cast<std::uint32_t>(max_sessions);
            hdr->pid = static_cast<std::uint64_t>(::getpid());
            hdr->created_ns = telemetry_now_ns();
            hdr->slot_size_bytes = static_cast<std::uint32_t>(sizeof(TelemetrySessionSlot));
            hdr->active_sessions.store(0, std::memory_order_release);

            auto writer = std::shared_ptr<TelemetryWriter>(new TelemetryWriter{});
            writer->fd_ = fd;
            writer->mapping_ = mem;
            writer->mapped_bytes_ = total_bytes;
            writer->max_sessions_ = max_sessions;
            writer->path_ = path;
            return writer;
        } catch (...) {
            return Status{ErrorCode::capacity_exceeded};
        }
    }

    // Registers a session slot once during startup (thread-safe across ShardedEngine shards).
    // Returns nullptr if capacity is exhausted.
    [[nodiscard]] TelemetrySessionSlot* register_session(
        std::string_view name, std::string_view local_comp_id, std::string_view remote_comp_id) noexcept {
        if (mapping_ == nullptr) return nullptr;
        std::lock_guard lock(alloc_mutex_);
        auto* hdr = header();
        const std::size_t idx = hdr->active_sessions.load(std::memory_order_relaxed);
        if (idx >= max_sessions_) return nullptr;
        auto* slot = slot_at(idx);
        new (slot) TelemetrySessionSlot{};
        copy_cstr(slot->name, sizeof(slot->name), name);
        copy_cstr(slot->local_comp_id, sizeof(slot->local_comp_id), local_comp_id);
        copy_cstr(slot->remote_comp_id, sizeof(slot->remote_comp_id), remote_comp_id);
        slot->occupied.store(1, std::memory_order_release);
        hdr->active_sessions.store(static_cast<std::uint32_t>(idx + 1), std::memory_order_release);
        return slot;
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    TelemetryWriter() noexcept = default;

    static void copy_cstr(char* dst, std::size_t cap, std::string_view src) noexcept {
        if (cap == 0) return;
        const std::size_t n = std::min(src.size(), cap - 1);
        if (n != 0) std::memcpy(dst, src.data(), n);
        dst[n] = '\0';
    }

    [[nodiscard]] TelemetryHeader* header() noexcept {
        return static_cast<TelemetryHeader*>(mapping_);
    }

    [[nodiscard]] TelemetrySessionSlot* slot_at(std::size_t index) noexcept {
        auto* base = static_cast<char*>(mapping_) + sizeof(TelemetryHeader);
        return reinterpret_cast<TelemetrySessionSlot*>(base) + index;
    }

    void close() noexcept {
        if (mapping_ != nullptr && mapping_ != MAP_FAILED) {
            (void)::munmap(const_cast<void*>(mapping_), mapped_bytes_);
        }
        mapping_ = nullptr;
        mapped_bytes_ = 0;
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    int fd_{-1};
    void* mapping_{nullptr};
    std::size_t mapped_bytes_{0};
    std::size_t max_sessions_{0};
    std::filesystem::path path_{};
    std::mutex alloc_mutex_{};
};

// Out-of-band, read-only sidecar reader (zero locks, zero writes, zero jitter impact on trading process).
class TelemetryReader {
public:
    TelemetryReader(const TelemetryReader&) = delete;
    TelemetryReader& operator=(const TelemetryReader&) = delete;
    TelemetryReader(TelemetryReader&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)),
          mapping_(std::exchange(other.mapping_, nullptr)),
          mapped_bytes_(std::exchange(other.mapped_bytes_, 0)),
          max_sessions_(std::exchange(other.max_sessions_, 0)) {}
    TelemetryReader& operator=(TelemetryReader&& other) noexcept {
        if (this == &other) return *this;
        close();
        fd_ = std::exchange(other.fd_, -1);
        mapping_ = std::exchange(other.mapping_, nullptr);
        mapped_bytes_ = std::exchange(other.mapped_bytes_, 0);
        max_sessions_ = std::exchange(other.max_sessions_, 0);
        return *this;
    }
    ~TelemetryReader() { close(); }

    [[nodiscard]] static Result<TelemetryReader> open(const std::filesystem::path& path) noexcept {
        if (path.empty()) return Status{ErrorCode::invalid_config};
        const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return Status{ErrorCode::io_error, 0, static_cast<std::size_t>(errno)};
        struct stat st{};
        if (::fstat(fd, &st) != 0 || st.st_size < static_cast<off_t>(sizeof(TelemetryHeader))) {
            ::close(fd);
            return Status{ErrorCode::corrupt_store};
        }
        const auto size = static_cast<std::size_t>(st.st_size);
        void* mem = ::mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
        if (mem == MAP_FAILED) {
            const int err = errno;
            ::close(fd);
            return Status{ErrorCode::io_error, 0, static_cast<std::size_t>(err)};
        }
        const auto* hdr = static_cast<const TelemetryHeader*>(mem);
        if (std::memcmp(hdr->magic, TelemetryHeader::kMagic, sizeof(hdr->magic)) != 0 ||
            hdr->format_version != TelemetryHeader::kFormatVersion ||
            hdr->slot_size_bytes != sizeof(TelemetrySessionSlot) ||
            hdr->max_sessions == 0 ||
            size < sizeof(TelemetryHeader) + static_cast<std::size_t>(hdr->max_sessions) * sizeof(TelemetrySessionSlot)) {
            (void)::munmap(mem, size);
            ::close(fd);
            return Status{ErrorCode::corrupt_store};
        }
        TelemetryReader reader;
        reader.fd_ = fd;
        reader.mapping_ = mem;
        reader.mapped_bytes_ = size;
        reader.max_sessions_ = hdr->max_sessions;
        return reader;
    }

    [[nodiscard]] std::uint64_t writer_pid() const noexcept {
        return mapping_ ? static_cast<const TelemetryHeader*>(mapping_)->pid : 0ULL;
    }

    [[nodiscard]] std::vector<TelemetrySessionSnapshot> read_all() const {
        std::vector<TelemetrySessionSnapshot> out;
        if (mapping_ == nullptr) return out;
        const auto* hdr = static_cast<const TelemetryHeader*>(mapping_);
        const std::size_t active = std::min<std::size_t>(
            hdr->active_sessions.load(std::memory_order_acquire), max_sessions_);
        const auto* base = reinterpret_cast<const TelemetrySessionSlot*>(
            static_cast<const char*>(mapping_) + sizeof(TelemetryHeader));
        out.reserve(active);
        for (std::size_t i = 0; i < active; ++i) {
            const auto& slot = base[i];
            if (slot.occupied.load(std::memory_order_acquire) == 0) continue;
            TelemetrySessionSnapshot snap;
            snap.name = std::string{slot.name, ::strnlen(slot.name, sizeof(slot.name))};
            snap.local_comp_id = std::string{slot.local_comp_id, ::strnlen(slot.local_comp_id, sizeof(slot.local_comp_id))};
            snap.remote_comp_id = std::string{slot.remote_comp_id, ::strnlen(slot.remote_comp_id, sizeof(slot.remote_comp_id))};
            snap.state = static_cast<SessionState>(slot.state.load(std::memory_order_acquire));
            snap.generation = slot.generation.load(std::memory_order_relaxed);
            snap.next_inbound_seq = slot.next_inbound_seq.load(std::memory_order_relaxed);
            snap.next_outbound_seq = slot.next_outbound_seq.load(std::memory_order_relaxed);
            snap.inbound_frames = slot.inbound_frames.load(std::memory_order_relaxed);
            snap.outbound_frames = slot.outbound_frames.load(std::memory_order_relaxed);
            snap.business_deliveries = slot.business_deliveries.load(std::memory_order_relaxed);
            snap.inbound_bytes = slot.inbound_bytes.load(std::memory_order_relaxed);
            snap.outbound_bytes = slot.outbound_bytes.load(std::memory_order_relaxed);
            snap.error_count = slot.error_count.load(std::memory_order_relaxed);
            snap.active_endpoint_index = slot.active_endpoint_index.load(std::memory_order_relaxed);
            snap.last_update_ns = slot.last_update_ns.load(std::memory_order_relaxed);
            for (std::size_t s = 0; s < static_cast<std::size_t>(TelemetryStage::count); ++s) {
                snap.stages[s] = slot.stages[s].summarize();
            }
            out.push_back(std::move(snap));
        }
        return out;
    }

    // Renders all live sessions and stage histograms in Prometheus text exposition format.
    [[nodiscard]] std::string format_prometheus() const {
        const auto snaps = read_all();
        std::ostringstream os;
        os << "# HELP nimblefix_session_state Current FIX session state (2=active).\n"
           << "# TYPE nimblefix_session_state gauge\n"
           << "# HELP nimblefix_session_inbound_frames_total Total inbound FIX frames processed.\n"
           << "# TYPE nimblefix_session_inbound_frames_total counter\n"
           << "# HELP nimblefix_session_outbound_frames_total Total outbound FIX frames sent.\n"
           << "# TYPE nimblefix_session_outbound_frames_total counter\n"
           << "# HELP nimblefix_session_business_deliveries_total Total business messages delivered.\n"
           << "# TYPE nimblefix_session_business_deliveries_total counter\n"
           << "# HELP nimblefix_session_errors_total Total session errors recorded.\n"
           << "# TYPE nimblefix_session_errors_total counter\n"
           << "# HELP nimblefix_stage_latency_ns Stage latency quantiles in nanoseconds.\n"
           << "# TYPE nimblefix_stage_latency_ns summary\n";
        for (const auto& s : snaps) {
            const std::string labels = "session=\"" + s.name + "\",local=\"" + s.local_comp_id +
                                       "\",remote=\"" + s.remote_comp_id + "\"";
            os << "nimblefix_session_state{" << labels << "} " << static_cast<unsigned>(s.state) << '\n';
            os << "nimblefix_session_inbound_frames_total{" << labels << "} " << s.inbound_frames << '\n';
            os << "nimblefix_session_outbound_frames_total{" << labels << "} " << s.outbound_frames << '\n';
            os << "nimblefix_session_business_deliveries_total{" << labels << "} " << s.business_deliveries << '\n';
            os << "nimblefix_session_errors_total{" << labels << "} " << s.error_count << '\n';
            for (std::size_t st = 0; st < static_cast<std::size_t>(TelemetryStage::count); ++st) {
                const auto& h = s.stages[st];
                if (h.count == 0) continue;
                const auto st_name = telemetry_stage_name(static_cast<TelemetryStage>(st));
                const std::string sl = labels + ",stage=\"" + std::string{st_name} + "\"";
                os << "nimblefix_stage_latency_ns{" << sl << ",quantile=\"0.5\"} " << h.p50_ns << '\n';
                os << "nimblefix_stage_latency_ns{" << sl << ",quantile=\"0.9\"} " << h.p90_ns << '\n';
                os << "nimblefix_stage_latency_ns{" << sl << ",quantile=\"0.99\"} " << h.p99_ns << '\n';
                os << "nimblefix_stage_latency_ns{" << sl << ",quantile=\"0.999\"} " << h.p999_ns << '\n';
                os << "nimblefix_stage_latency_ns{" << sl << ",quantile=\"0.9999\"} " << h.p9999_ns << '\n';
                os << "nimblefix_stage_latency_ns_count{" << sl << "} " << h.count << '\n';
            }
        }
        return os.str();
    }

private:
    TelemetryReader() noexcept = default;

    void close() noexcept {
        if (mapping_ != nullptr && mapping_ != MAP_FAILED) {
            (void)::munmap(const_cast<void*>(mapping_), mapped_bytes_);
        }
        mapping_ = nullptr;
        mapped_bytes_ = 0;
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    int fd_{-1};
    const void* mapping_{nullptr};
    std::size_t mapped_bytes_{0};
    std::size_t max_sessions_{0};
};

} // namespace nimblefix
