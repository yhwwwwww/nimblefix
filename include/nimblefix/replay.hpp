#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <nimblefix/codec.hpp>
#include <nimblefix/connection_strategy.hpp>
#include <nimblefix/journal.hpp>
#include <nimblefix/status.hpp>
#include <nimblefix/tcp_transport.hpp>
#include <nimblefix/telemetry.hpp>

namespace nimblefix {

enum class ReplayDirection : std::uint8_t {
    inbound = 0,
    outbound = 1,
    both = 2,
};

enum class ReplayTiming : std::uint8_t {
    full_speed = 0,
    original = 1,
    scaled = 2,
};

struct ReplayOptions {
    std::filesystem::path journal_path{};
    // Optional when replaying from disk: if empty, auto-detected from the v2 manifest header.
    // When non-empty, strictly validated against the manifest's session identity.
    std::string session_key{};
    ReplayDirection direction{ReplayDirection::outbound};
    ReplayTiming timing{ReplayTiming::full_speed};
    // Used when timing == ReplayTiming::scaled (e.g. 10.0 = 10x faster than original).
    double speed_multiplier{1.0};
    std::uint64_t seq_from{1};
    // 0 = unbounded upper sequence.
    std::uint64_t seq_to{0};
    // 0 = include all epochs.
    std::uint32_t epoch_filter{0};
    bool skip_admin{false};
    bool verify_frames{true};
    bool verify_checksum{true};
    std::size_t max_frame_bytes{default_max_frame_bytes};
    // Optional schema WireHints for custom LENGTH/DATA pairs containing SOH bytes.
    WireHints wire_hints{};
    // Caps any single inter-frame sleep in original/scaled mode (0 = uncapped).
    std::uint64_t max_sleep_us{1'000'000};
    std::size_t journal_max_records{65'536};
};

struct ReplayStats {
    std::uint64_t scanned_records{};
    std::uint64_t replayed_frames{};
    std::uint64_t skipped_records{};
    std::uint64_t invalid_frames{};
    std::uint64_t total_bytes{};
    std::uint64_t elapsed_ns{};
    std::uint64_t total_sleep_ns{};
    std::uint64_t first_seq{};
    std::uint64_t last_seq{};
};

namespace detail {

[[nodiscard]] inline bool replay_is_admin(std::string_view msg_type) noexcept {
    return msg_type == "A" || msg_type == "0" || msg_type == "1" ||
           msg_type == "2" || msg_type == "3" || msg_type == "4" || msg_type == "5";
}

// Parses FIX UTC timestamp ("YYYYMMDD-HH:MM:SS[.mmm[uuu[nnn]]]") into nanoseconds since Unix epoch.
[[nodiscard]] inline bool parse_utc_timestamp_ns(std::string_view value, std::uint64_t& out_ns) noexcept {
    if (!is_valid_utc_timestamp(value)) return false;
    auto num = [&value](std::size_t at, std::size_t count) noexcept -> std::uint64_t {
        std::uint64_t r = 0;
        for (std::size_t i = 0; i < count; ++i) {
            r = r * 10ULL + static_cast<std::uint64_t>(value[at + i] - '0');
        }
        return r;
    };
    using namespace std::chrono;
    const year_month_day d{
        year{static_cast<int>(num(0, 4))},
        month{static_cast<unsigned>(num(4, 2))},
        day{static_cast<unsigned>(num(6, 2))}};
    if (!d.ok()) return false;
    const auto sec = std::min<std::uint64_t>(num(15, 2), 59ULL);
    const auto whole_sec = duration_cast<seconds>(
        sys_days{d}.time_since_epoch() + hours{num(9, 2)} + minutes{num(12, 2)} + seconds{sec}).count();
    if (whole_sec < 0) return false;

    std::uint64_t frac_ns = 0;
    if (value.size() > 18) {
        const std::size_t frac_digits = std::min<std::size_t>(value.size() - 18, 9);
        frac_ns = num(18, frac_digits);
        for (std::size_t i = frac_digits; i < 9; ++i) frac_ns *= 10ULL;
    }
    out_ns = static_cast<std::uint64_t>(whole_sec) * 1'000'000'000ULL + frac_ns;
    return true;
}

struct ProbedManifest {
    std::string session_key;
    std::size_t capacity_bytes{};
    std::size_t segment_bytes{};
    std::size_t max_segments{};
};

[[nodiscard]] inline Result<ProbedManifest> probe_v2_manifest(const std::filesystem::path& path) noexcept {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return Status{ErrorCode::io_error, 0, static_cast<std::size_t>(errno)};
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        const int err = errno;
        ::close(fd);
        return Status{ErrorCode::io_error, 0, static_cast<std::size_t>(err)};
    }
    if (!S_ISREG(st.st_mode) || st.st_size < 4096) {
        ::close(fd);
        return Status{ErrorCode::corrupt_store};
    }
    std::array<unsigned char, 512> hdr{};
    std::size_t got = 0;
    while (got < hdr.size()) {
        const ssize_t n = ::pread(fd, hdr.data() + got, hdr.size() - got, static_cast<off_t>(got));
        if (n > 0) { got += static_cast<std::size_t>(n); continue; }
        if (n < 0 && errno == EINTR) continue;
        ::close(fd);
        return Status{ErrorCode::corrupt_store};
    }
    ::close(fd);

    static constexpr std::array<unsigned char, 8> kMagic{'N', 'I', 'M', 'B', 'L', 'J', '2', 0};
    if (std::memcmp(hdr.data(), kMagic.data(), kMagic.size()) != 0) {
        return Status{ErrorCode::corrupt_store};
    }
    auto get_u32 = [](const unsigned char* p) noexcept -> std::uint32_t {
        std::uint32_t v = 0;
        for (std::size_t i = 0; i < 4; ++i) v |= static_cast<std::uint32_t>(p[i]) << (i * 8U);
        return v;
    };
    auto get_u64 = [](const unsigned char* p) noexcept -> std::uint64_t {
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < 8; ++i) v |= static_cast<std::uint64_t>(p[i]) << (i * 8U);
        return v;
    };
    if (get_u32(hdr.data() + 8) != 2U) return Status{ErrorCode::unsupported_version};
    const std::size_t key_len = get_u32(hdr.data() + 24);
    if (key_len == 0 || key_len > journal_max_session_key_bytes || 64U + key_len > hdr.size()) {
        return Status{ErrorCode::corrupt_store};
    }
    ProbedManifest out{};
    try {
        out.session_key.assign(reinterpret_cast<const char*>(hdr.data() + 64), key_len);
    } catch (...) {
        return Status{ErrorCode::capacity_exceeded};
    }
    out.max_segments = get_u32(hdr.data() + 36);
    out.segment_bytes = static_cast<std::size_t>(get_u64(hdr.data() + 40));
    out.capacity_bytes = static_cast<std::size_t>(get_u64(hdr.data() + 48));
    if (out.max_segments == 0 || out.segment_bytes == 0 || out.capacity_bytes == 0) {
        return Status{ErrorCode::corrupt_store};
    }
    return out;
}

[[nodiscard]] inline Status replay_write_all_tcp(TcpTransport& transport,
                                                 std::string_view payload,
                                                 std::uint32_t timeout_ms) noexcept {
    std::size_t sent = 0;
    const std::uint64_t deadline = telemetry_now_ns() +
        static_cast<std::uint64_t>(timeout_ms) * 1'000'000ULL;
    while (sent < payload.size()) {
        const auto io = transport.write(std::span<const char>{
            payload.data() + sent, payload.size() - sent});
        if (!io.status.ok()) return io.status;
        if (io.peer_closed) return Status{ErrorCode::connection_closed};
        sent += io.bytes;
        if (sent == payload.size()) return Status::success();
        if (io.would_block || io.bytes == 0) {
            const std::uint64_t now = telemetry_now_ns();
            if (now >= deadline) return Status{ErrorCode::write_stalled};
            const int rem_ms = static_cast<int>(
                std::min<std::uint64_t>((deadline - now + 999'999ULL) / 1'000'000ULL, 1000ULL));
            pollfd pfd{transport.native_fd(), POLLOUT, 0};
            const int rc = ::poll(&pfd, 1, rem_ms);
            if (rc < 0 && errno != EINTR) return Status{ErrorCode::io_error, 0, static_cast<std::size_t>(errno)};
            if (rc > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                return Status{ErrorCode::connection_closed};
            }
        }
    }
    return Status::success();
}

} // namespace detail

// Deterministic historical replayer built on the mmap JournalReader (in-process)
// and lock-free read-only JournalTailer (cross-process live or offline .nfxj).
// Replays recorded FIX sessions into a callback sink or over real TCP with
// original (tag 52=SendingTime), scaled, or full-speed timing.
class JournalReplayer {
public:
    // Replays from an already-open Journal using an in-process lease-protected JournalReader.
    // Sink signature: Status on_frame(const JournalRecord& rec, const FlatMessageView* parsed)
    template <class Sink>
    [[nodiscard]] static Result<ReplayStats> replay(Journal& journal,
                                                    const ReplayOptions& opts,
                                                    Sink&& on_frame) noexcept try {
        if (const auto st = validate_options(opts); !st.ok()) return st;
        auto reader_res = journal.create_reader(journal.lowest_retained_record_id());
        if (!reader_res.ok()) return reader_res.status();
        auto reader = std::move(reader_res).value();
        return replay_from_source(reader, opts, std::forward<Sink>(on_frame));
    } catch (const std::bad_alloc&) {
        return Status{ErrorCode::capacity_exceeded};
    } catch (...) {
        return Status{ErrorCode::io_error};
    }

    // Replays from an already-open cross-process read-only JournalTailer.
    template <class Sink>
    [[nodiscard]] static Result<ReplayStats> replay(JournalTailer& tailer,
                                                    const ReplayOptions& opts,
                                                    Sink&& on_frame) noexcept try {
        if (const auto st = validate_options(opts); !st.ok()) return st;
        return replay_from_source(tailer, opts, std::forward<Sink>(on_frame));
    } catch (const std::bad_alloc&) {
        return Status{ErrorCode::capacity_exceeded};
    } catch (...) {
        return Status{ErrorCode::io_error};
    }

    // Opens a segmented v2 mmap Journal in lock-free read-only mode (JournalTailer)
    // and replays matching frames into on_frame. Works on BOTH live active sessions
    // (without blocking or locking the writer process) and offline .nfxj journals.
    template <class Sink>
    [[nodiscard]] static Result<ReplayStats> replay(const ReplayOptions& opts,
                                                    Sink&& on_frame) noexcept try {
        if (opts.journal_path.empty()) return Status{ErrorCode::invalid_config};
        if (const auto st = validate_options(opts); !st.ok()) return st;
        const std::string path_str = opts.journal_path.string();
        JournalTailerConfig tcfg{};
        tcfg.path = path_str;
        tcfg.session_key = opts.session_key;
        tcfg.start = JournalTailStart::beginning;
        tcfg.verify_crc = true;
        auto opened = JournalTailer::open(tcfg);
        if (!opened.ok()) return opened.status();
        return replay_from_source(opened.value(), opts, std::forward<Sink>(on_frame));
    } catch (const std::bad_alloc&) {
        return Status{ErrorCode::capacity_exceeded};
    } catch (...) {
        return Status{ErrorCode::io_error};
    }

    // Replays matching frames from an open Journal directly to a TCP endpoint.
    [[nodiscard]] static Result<ReplayStats> replay_to_tcp(Journal& journal,
                                                           const ReplayOptions& opts,
                                                           const TcpEndpoint& target,
                                                           std::uint32_t connect_timeout_ms = 5'000,
                                                           std::uint32_t write_timeout_ms = 5'000) noexcept {
        TcpTransport transport{};
        if (const auto st = connect_tcp_target(transport, target, connect_timeout_ms); !st.ok()) return st;
        return replay(journal, opts, [&](const JournalRecord& rec, const FlatMessageView*) noexcept -> Status {
            return detail::replay_write_all_tcp(transport, rec.payload, write_timeout_ms);
        });
    }

    // Opens a segmented v2 mmap Journal in lock-free read-only mode (live or offline)
    // and streams matching frames to a TCP endpoint.
    [[nodiscard]] static Result<ReplayStats> replay_to_tcp(const ReplayOptions& opts,
                                                           const TcpEndpoint& target,
                                                           std::uint32_t connect_timeout_ms = 5'000,
                                                           std::uint32_t write_timeout_ms = 5'000) noexcept try {
        if (opts.journal_path.empty()) return Status{ErrorCode::invalid_config};
        if (const auto st = validate_options(opts); !st.ok()) return st;
        const std::string path_str = opts.journal_path.string();
        JournalTailerConfig tcfg{};
        tcfg.path = path_str;
        tcfg.session_key = opts.session_key;
        tcfg.start = JournalTailStart::beginning;
        tcfg.verify_crc = true;
        auto opened = JournalTailer::open(tcfg);
        if (!opened.ok()) return opened.status();
        TcpTransport transport{};
        if (const auto st = connect_tcp_target(transport, target, connect_timeout_ms); !st.ok()) return st;
        return replay_from_source(opened.value(), opts,
            [&](const JournalRecord& rec, const FlatMessageView*) noexcept -> Status {
                return detail::replay_write_all_tcp(transport, rec.payload, write_timeout_ms);
            });
    } catch (const std::bad_alloc&) {
        return Status{ErrorCode::capacity_exceeded};
    } catch (...) {
        return Status{ErrorCode::io_error};
    }

private:
    [[nodiscard]] static Status validate_options(const ReplayOptions& opts) noexcept {
        if (opts.timing == ReplayTiming::scaled &&
            (!std::isfinite(opts.speed_multiplier) || opts.speed_multiplier <= 0.0)) {
            return Status{ErrorCode::invalid_config};
        }
        if (opts.seq_to != 0 && opts.seq_to < opts.seq_from) {
            return Status{ErrorCode::invalid_config};
        }
        if (opts.max_frame_bytes < 64) return Status{ErrorCode::invalid_config};
        return Status::success();
    }

    [[nodiscard]] static Status connect_tcp_target(TcpTransport& transport,
                                                   const TcpEndpoint& target,
                                                   std::uint32_t connect_timeout_ms) noexcept {
        if (target.host.empty() || target.port == 0) return Status{ErrorCode::invalid_config};
        const auto cst = transport.connect(target.host, target.port);
        if (!cst.ok()) return cst;
        if (transport.is_connecting()) {
            const std::uint64_t deadline = telemetry_now_ns() +
                static_cast<std::uint64_t>(connect_timeout_ms) * 1'000'000ULL;
            while (transport.is_connecting()) {
                const auto fc = transport.finish_connect();
                if (fc.ok()) break;
                if (fc.code != ErrorCode::need_more) return fc;
                const std::uint64_t now = telemetry_now_ns();
                if (now >= deadline) return Status{ErrorCode::io_error};
                const int rem_ms = static_cast<int>(
                    std::min<std::uint64_t>((deadline - now + 999'999ULL) / 1'000'000ULL, 1000ULL));
                pollfd pfd{transport.native_fd(), POLLOUT, 0};
                const int rc = ::poll(&pfd, 1, rem_ms);
                if (rc < 0 && errno != EINTR) return Status{ErrorCode::io_error, 0, static_cast<std::size_t>(errno)};
            }
        }
        return Status::success();
    }

    template <class Source, class Sink>
    [[nodiscard]] static Result<ReplayStats> replay_from_source(Source& source,
                                                                const ReplayOptions& opts,
                                                                Sink&& on_frame) noexcept try {
        std::vector<FieldView> fields(opts.max_frame_bytes / 4 + 16);
        detail::ParsedFieldIndex index{};
        ReplayStats stats{};
        bool has_prev_ts = false;
        std::uint64_t prev_ts_ns = 0;
        const std::uint64_t start_ns = telemetry_now_ns();

        JournalRecord rec{};
        for (;;) {
            const auto st = source.next(rec);
            if (st.code == ErrorCode::need_more) break;
            if (!st.ok()) return st;
            ++stats.scanned_records;

            const bool kind_match =
                (rec.kind == RecordKind::inbound &&
                 (opts.direction == ReplayDirection::inbound || opts.direction == ReplayDirection::both)) ||
                (rec.kind == RecordKind::outbound &&
                 (opts.direction == ReplayDirection::outbound || opts.direction == ReplayDirection::both));
            if (!kind_match || rec.payload.empty()) {
                ++stats.skipped_records;
                continue;
            }
            if (opts.epoch_filter != 0 && rec.epoch != opts.epoch_filter) {
                ++stats.skipped_records;
                continue;
            }
            if (rec.seq < opts.seq_from || (opts.seq_to != 0 && rec.seq > opts.seq_to)) {
                ++stats.skipped_records;
                continue;
            }

            const bool need_parse =
                opts.verify_frames || opts.skip_admin || opts.timing != ReplayTiming::full_speed;
            FlatMessageView parsed{};
            const FlatMessageView* parsed_ptr = nullptr;
            if (need_parse) {
                FrameView fv{};
                const auto ist = inspect_frame(rec.payload, fv, opts.wire_hints,
                    CodecLimits{opts.max_frame_bytes, opts.verify_checksum});
                if (!ist.ok() || fv.consumed != rec.payload.size()) {
                    ++stats.invalid_frames;
                    if (opts.verify_frames) return ist.ok() ? Status{ErrorCode::invalid_frame} : ist;
                    ++stats.skipped_records;
                    continue;
                }
                const auto pst = parse_flat_verified(fv, fields, parsed, opts.wire_hints);
                if (!pst.ok()) {
                    ++stats.invalid_frames;
                    if (opts.verify_frames) return pst;
                    ++stats.skipped_records;
                    continue;
                }
                parsed.bind_index(&index);
                parsed_ptr = &parsed;

                if (opts.skip_admin) {
                    const auto mt = parsed.find_unique(35);
                    if (mt.ok() && detail::replay_is_admin(mt.value().value)) {
                        ++stats.skipped_records;
                        continue;
                    }
                }

                if (opts.timing != ReplayTiming::full_speed) {
                    const auto ts_field = parsed.find_unique(52);
                    std::uint64_t ts_ns = 0;
                    if (ts_field.ok() && detail::parse_utc_timestamp_ns(ts_field.value().value, ts_ns)) {
                        if (has_prev_ts && ts_ns > prev_ts_ns) {
                            std::uint64_t delta_ns = ts_ns - prev_ts_ns;
                            if (opts.timing == ReplayTiming::scaled) {
                                delta_ns = static_cast<std::uint64_t>(
                                    static_cast<double>(delta_ns) / opts.speed_multiplier);
                            }
                            if (opts.max_sleep_us != 0) {
                                delta_ns = std::min<std::uint64_t>(delta_ns, opts.max_sleep_us * 1'000ULL);
                            }
                            if (delta_ns > 0) {
                                std::this_thread::sleep_for(std::chrono::nanoseconds{delta_ns});
                                stats.total_sleep_ns += delta_ns;
                            }
                        }
                        prev_ts_ns = ts_ns;
                        has_prev_ts = true;
                    }
                }
            }

            const Status sink_st = on_frame(rec, parsed_ptr);
            if (!sink_st.ok()) return sink_st;

            if (stats.replayed_frames == 0) stats.first_seq = rec.seq;
            stats.last_seq = rec.seq;
            ++stats.replayed_frames;
            stats.total_bytes += rec.payload.size();
        }
        stats.elapsed_ns = telemetry_now_ns() - start_ns;
        return stats;
    } catch (const std::bad_alloc&) {
        return Status{ErrorCode::capacity_exceeded};
    } catch (...) {
        return Status{ErrorCode::io_error};
    }
};

} // namespace nimblefix
