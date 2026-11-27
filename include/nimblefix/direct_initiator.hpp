#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#include <nimblefix/codec.hpp>
#include <nimblefix/connection_strategy.hpp>
#include <nimblefix/detail/session_core.hpp>
#include <nimblefix/direct_queue.hpp>
#include <nimblefix/execution.hpp>
#include <nimblefix/journal.hpp>
#include <nimblefix/schema.hpp>
#include <nimblefix/session.hpp>
#include <nimblefix/settings.hpp>
#include <nimblefix/status.hpp>
#include <nimblefix/tcp_transport.hpp>
#include <nimblefix/telemetry.hpp>

namespace nimblefix {

struct DirectInitiatorConfig {
    std::string host{};
    std::uint16_t port{};
    std::string storage_directory{};
    std::string session_key{};
    std::string local_comp_id{};
    std::string remote_comp_id{};

    std::uint32_t heartbeat_ms{30'000};
    std::uint32_t logon_timeout_ms{10'000};
    std::uint32_t logout_timeout_ms{5'000};

    std::size_t max_frame_bytes{default_max_frame_bytes};
    std::size_t max_pending_messages{256};
    std::size_t rx_buffer_bytes{64U * 1024U};

    std::size_t journal_capacity_bytes{16U * 1024U * 1024U};
    std::size_t journal_segment_bytes{};
    std::size_t journal_max_segments{};
    std::size_t journal_max_records{65'536};
    std::size_t retain_outbound_messages{};
    JournalTestHook journal_test_hook{};

    FixVersion version{FixVersion::fix44};
    std::string default_appl_ver_id{appl_ver_id::fix50sp2};
    std::string username{};
    std::string password{};
    bool request_sequence_reset{};
    bool allow_sequence_reset{};
    bool emit_last_processed_seq{true};
    bool check_sending_time{};
    std::uint64_t max_sending_time_skew_ms{120'000};
    TimestampPrecision timestamp_precision{TimestampPrecision::milliseconds};
    bool verify_checksum{true};
    // Optional ordered failover endpoint list (up to 16 entries). When non-empty,
    // overrides host/port and follows connection_strategy.
    std::vector<TcpEndpoint> endpoints{};
    ConnectionStrategy connection_strategy{};
    // Optional mmap telemetry region (.nfxt) for zero-overhead out-of-band observability.
    std::filesystem::path telemetry_path{};
    bool telemetry_stage_probes{false};
    // Bounded TCP/TLS connect+handshake timeout and outbound write-stall deadline (0 disables).
    std::uint32_t connect_timeout_ms{10'000};
    std::uint32_t write_stall_ms{30'000};
    // Policy for schema-unknown inbound fields (reject = zero hot-path overhead default).
    UnexpectedFieldPolicy unexpected_field_policy{UnexpectedFieldPolicy::reject};
    // Optional local source IP/port binding & low-latency socket tuning.
    SocketOptions socket_options{};
};

// Default no-op handler for tests or send-only initiators.
struct NullInitiatorHandler {
    [[nodiscard]] Status on_message(const FlatMessageView&, const DeliveryInfo&) noexcept {
        return Status::success();
    }
};

namespace detail {

// Fast cached UTC wall clock ("YYYYMMDD-HH:MM:SS.mmm[uuu[nnn]]") + monotonic milliseconds.
// Calls gmtime_r at most once per whole second; hot-path calls update only fractional digits.
class FastUtcClock {
public:
    [[nodiscard]] std::uint64_t now_ms() const noexcept {
        timespec ts{};
        ::clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<std::uint64_t>(ts.tv_sec) * 1000ULL +
               static_cast<std::uint64_t>(ts.tv_nsec / 1'000'000L);
    }

    [[nodiscard]] std::string_view wall_time(
        TimestampPrecision precision = TimestampPrecision::milliseconds) noexcept {
        timespec ts{};
        ::clock_gettime(CLOCK_REALTIME, &ts);
        if (ts.tv_sec != cached_sec_) {
            cached_sec_ = ts.tv_sec;
            std::tm utc{};
            ::gmtime_r(&ts.tv_sec, &utc);
            write_4(buf_.data() + 0, static_cast<unsigned>(utc.tm_year + 1900));
            write_2(buf_.data() + 4, static_cast<unsigned>(utc.tm_mon + 1));
            write_2(buf_.data() + 6, static_cast<unsigned>(utc.tm_mday));
            buf_[8] = '-';
            write_2(buf_.data() + 9, static_cast<unsigned>(utc.tm_hour));
            buf_[11] = ':';
            write_2(buf_.data() + 12, static_cast<unsigned>(utc.tm_min));
            buf_[14] = ':';
            write_2(buf_.data() + 15, static_cast<unsigned>(utc.tm_sec));
            buf_[17] = '.';
        }
        const auto nsec = static_cast<unsigned>(ts.tv_nsec);
        const unsigned ms = nsec / 1'000'000U;
        write_3(buf_.data() + 18, ms);
        if (precision == TimestampPrecision::milliseconds) {
            return {buf_.data(), timestamp_millis_len};
        }
        const unsigned rem_us = nsec - ms * 1'000'000U;
        const unsigned us = rem_us / 1'000U;
        write_3(buf_.data() + 21, us);
        if (precision == TimestampPrecision::microseconds) {
            return {buf_.data(), timestamp_micros_len};
        }
        write_3(buf_.data() + 24, rem_us - us * 1'000U);
        return {buf_.data(), timestamp_nanos_len};
    }

private:
    static void write_2(char* out, unsigned v) noexcept {
        out[0] = static_cast<char>('0' + (v / 10U) % 10U);
        out[1] = static_cast<char>('0' + v % 10U);
    }
    static void write_3(char* out, unsigned v) noexcept {
        out[0] = static_cast<char>('0' + (v / 100U) % 10U);
        out[1] = static_cast<char>('0' + (v / 10U) % 10U);
        out[2] = static_cast<char>('0' + v % 10U);
    }
    static void write_4(char* out, unsigned v) noexcept {
        out[0] = static_cast<char>('0' + (v / 1000U) % 10U);
        out[1] = static_cast<char>('0' + (v / 100U) % 10U);
        out[2] = static_cast<char>('0' + (v / 10U) % 10U);
        out[3] = static_cast<char>('0' + v % 10U);
    }

    std::time_t cached_sec_{-1};
    std::array<char, 28> buf_{};
};

inline void direct_secure_clear(std::span<char> bytes) noexcept {
    volatile char* p = bytes.data();
    for (std::size_t i = 0; i < bytes.size(); ++i) p[i] = 0;
}

inline void direct_secure_clear(std::string& text) noexcept {
    direct_secure_clear(std::span<char>{text.data(), text.size()});
    text.clear();
}

[[nodiscard]] inline bool direct_parse_u64(std::string_view text, std::uint64_t& value) noexcept {
    if (text.empty()) return false;
    const auto r = std::from_chars(text.data(), text.data() + text.size(), value);
    return r.ec == std::errc{} && r.ptr == text.data() + text.size();
}

[[nodiscard]] inline bool direct_is_admin(std::string_view t) noexcept {
    return msg_type::is_session_admin(t);
}

[[nodiscard]] inline bool direct_parse_utc_ms(std::string_view value, std::int64_t& millis) noexcept {
    if (!is_valid_utc_timestamp(value)) return false;
    auto num = [&value](std::size_t at, std::size_t count) noexcept {
        unsigned r = 0;
        for (std::size_t i = 0; i < count; ++i) r = r * 10U + static_cast<unsigned>(value[at + i] - '0');
        return r;
    };
    using namespace std::chrono;
    const year_month_day d{year{static_cast<int>(num(0, 4))}, month{num(4, 2)}, day{num(6, 2)}};
    if (!d.ok()) return false;
    const auto sec = std::min(num(15, 2), 59U);
    const auto whole = duration_cast<milliseconds>(
        sys_days{d}.time_since_epoch() + hours{num(9, 2)} + minutes{num(12, 2)} + seconds{sec}).count();
    unsigned frac = 0;
    const auto frac_digits = value.size() > 17 ? value.size() - 18 : 0;
    if (frac_digits != 0) {
        const auto digits = std::min<std::size_t>(frac_digits, 3);
        frac = num(18, digits);
        for (std::size_t i = digits; i < 3; ++i) frac *= 10U;
    }
    millis = whole + static_cast<std::int64_t>(frac);
    return true;
}

template <class Schema>
[[nodiscard]] constexpr WireHints direct_schema_wire_hints() noexcept {
    if constexpr (requires { Schema::wire_hints(); }) return Schema::wire_hints();
    else if constexpr (requires { Schema::descriptor().wire_hints; }) return Schema::descriptor().wire_hints;
    else return WireHints{};
}

template <class Schema>
[[nodiscard]] inline Status direct_schema_validate(
    const FlatMessageView& message,
    std::string_view known_msg_type = {},
    std::size_t msg_type_offset = 0,
    UnexpectedFieldPolicy policy = UnexpectedFieldPolicy::reject,
    std::uint64_t* unexpected_count = nullptr) noexcept {
    if constexpr (requires { Schema::descriptor(); }) {
        if (!known_msg_type.empty()) {
            return validate_schema_message_for_type(Schema::descriptor(), known_msg_type,
                msg_type_offset, message.fields(), ValidationLevel::strict,
                message.parsed_index(), policy, unexpected_count);
        }
        return validate_schema_message(Schema::descriptor(), message,
            ValidationLevel::strict, policy, unexpected_count);
    } else if constexpr (requires { Schema::validate(message); }) {
        return Schema::validate(message);
    } else {
        return Status::success();
    }
}

struct DirectAdminField {
    std::uint32_t tag{};
    FieldKind kind{};
    std::uint64_t scalar{};
    std::string_view text{};
};

struct DirectAdminStorage {
    std::array<SlotValue, 24> slots{};
    std::array<PlanField, 24> plan{};
    MessageView view{};
};

inline void direct_format_tag(std::uint32_t tag, PlanField& field) noexcept {
    std::array<char, 10> rev{};
    std::size_t n = 0;
    do { rev[n++] = static_cast<char>('0' + tag % 10); tag /= 10; } while (tag != 0 && n < 9);
    field.prefix_size = static_cast<std::uint8_t>(n + 1);
    for (std::size_t i = 0; i < n; ++i) field.tag_prefix[i] = rev[n - i - 1];
    field.tag_prefix[n] = '=';
}

[[nodiscard]] inline Status direct_build_admin(std::string_view msg_type,
    std::span<const DirectAdminField> fields, DirectAdminStorage& storage,
    std::span<char> arena) noexcept {
    if (msg_type.empty() || msg_type.size() > 32 || fields.size() > storage.plan.size())
        return Status{ErrorCode::invalid_value, tag::msg_type};
    std::size_t used = 0;
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto& f = fields[i];
        auto& pf = storage.plan[i];
        pf.tag = f.tag;
        pf.slot_index = static_cast<std::uint32_t>(i);
        pf.kind = f.kind;
        direct_format_tag(f.tag, pf);
        auto& sl = storage.slots[i];
        if (f.kind == FieldKind::string) {
            if (!is_valid_text(f.text)) return Status{ErrorCode::invalid_value, static_cast<int>(f.tag)};
            if (f.text.size() > arena.size() - used) return Status{ErrorCode::too_large, static_cast<int>(f.tag)};
            if (!f.text.empty()) std::memcpy(arena.data() + used, f.text.data(), f.text.size());
            sl.offset = static_cast<std::uint32_t>(used);
            sl.size = static_cast<std::uint32_t>(f.text.size());
            used += f.text.size();
        } else {
            sl.scalar = f.scalar;
        }
    }
    storage.view = MessageView{storage.slots.data(), arena.data(), used, storage.plan.data(),
        nullptr, fields.size(), nullptr, msg_type, Status::success()};
    return Status::success();
}

struct DirectInboundTags {
    const FieldView* begin{};
    const FieldView* msg_type{};
    const FieldView* seq{};
    const FieldView* sender{};
    const FieldView* target{};
    const FieldView* sending_time{};
    const FieldView* poss_dup{};
    const FieldView* orig_time{};
    const FieldView* test_id{};
    const FieldView* peer_ack{};
    const FieldView* begin_seq{};
    const FieldView* end_seq{};
    const FieldView* new_seq{};
    const FieldView* gap_fill{};
    const FieldView* encrypt{};
    const FieldView* hb_int{};
    const FieldView* reset_flag{};
    const FieldView* appl_ver{};
    const FieldView* text{};
    std::uint32_t dup_tag{};
};

[[nodiscard]] inline DirectInboundTags direct_extract_tags(const FlatMessageView& msg) noexcept {
    DirectInboundTags t{};
    auto put = [&](const FieldView*& slot, const FieldView& f) noexcept {
        if (slot != nullptr && t.dup_tag == 0) t.dup_tag = f.tag;
        else if (slot == nullptr) slot = &f;
    };
    for (const auto& f : msg.fields()) {
        switch (f.tag) {
        case tag::begin_string: put(t.begin, f); break;
        case tag::msg_type: put(t.msg_type, f); break;
        case tag::msg_seq_num: put(t.seq, f); break;
        case tag::sender_comp_id: put(t.sender, f); break;
        case tag::target_comp_id: put(t.target, f); break;
        case tag::sending_time: put(t.sending_time, f); break;
        case tag::poss_dup_flag: put(t.poss_dup, f); break;
        case tag::orig_sending_time: put(t.orig_time, f); break;
        case tag::test_req_id: put(t.test_id, f); break;
        case tag::last_msg_seq_num_processed: put(t.peer_ack, f); break;
        case tag::begin_seq_no: put(t.begin_seq, f); break;
        case tag::end_seq_no: put(t.end_seq, f); break;
        case tag::new_seq_no: put(t.new_seq, f); break;
        case tag::gap_fill_flag: put(t.gap_fill, f); break;
        case tag::encrypt_method: put(t.encrypt, f); break;
        case tag::heart_bt_int: put(t.hb_int, f); break;
        case tag::reset_seq_num_flag: put(t.reset_flag, f); break;
        case tag::default_appl_ver_id: put(t.appl_ver, f); break;
        case tag::text: put(t.text, f); break;
        default: break;
        }
    }
    return t;
}

} // namespace detail

// Layer 0 flagship zero-overhead FIX Initiator.
// Zero std::function, zero pimpl, zero mutexes, zero poll(), zero steady-state heap
// allocations, zero hot-path fsync, direct-to-mmap Journal::reserve/commit encoding,
// and complete restart recovery, sequence persistence, ResendRequest, GapFill & Replay.
template <class Schema = FlatSchema, class Handler = NullInitiatorHandler,
          DirectTransport Transport = TcpTransport>
class DirectInitiator {
public:
    using schema_type = Schema;
    using handler_type = Handler;
    using transport_type = Transport;

    DirectInitiator(const DirectInitiator&) = delete;
    DirectInitiator& operator=(const DirectInitiator&) = delete;
    DirectInitiator(DirectInitiator&&) noexcept = default;
    DirectInitiator& operator=(DirectInitiator&&) noexcept = default;

    ~DirectInitiator() {
        detail::direct_secure_clear(logon_tx_buf_);
        detail::direct_secure_clear(deferred_logon_buf_);
        detail::direct_secure_clear(config_.username);
        detail::direct_secure_clear(config_.password);
    }

    [[nodiscard]] static Result<DirectInitiator> open(
        DirectInitiatorConfig config,
        Handler handler = {},
        Transport transport = {}) noexcept try {
        if (config.storage_directory.empty() ||
            config.local_comp_id.empty() || config.remote_comp_id.empty() ||
            !detail::is_valid_text(config.local_comp_id) ||
            !detail::is_valid_text(config.remote_comp_id) ||
            config.heartbeat_ms == 0 || config.logon_timeout_ms == 0 ||
            config.logout_timeout_ms == 0 || config.max_frame_bytes < 128 ||
            config.max_pending_messages < 4 || config.journal_capacity_bytes == 0 ||
            config.journal_max_records == 0 || begin_string(config.version).empty() ||
            (config.version == FixVersion::fixt11 && !valid_appl_ver_id(config.default_appl_ver_id)) ||
            (config.request_sequence_reset && !config.allow_sequence_reset) ||
            !config.connection_strategy.validate().ok() ||
            config.endpoints.size() > 16 ||
            (config.unexpected_field_policy != UnexpectedFieldPolicy::reject &&
             config.unexpected_field_policy != UnexpectedFieldPolicy::ignore_with_metric &&
             config.unexpected_field_policy != UnexpectedFieldPolicy::passthrough)) {
            return Status{ErrorCode::invalid_config};
        }
        if (config.session_key.empty()) {
            config.session_key = std::string{begin_string(config.version)} + "/" +
                config.local_comp_id + "/" + config.remote_comp_id;
        }
        if (config.session_key.size() > journal_max_session_key_bytes) {
            return Status{ErrorCode::invalid_config};
        }

        // Same deterministic hex-key session journal path as SessionDriver.
        std::string hex;
        hex.reserve(config.session_key.size() * 2);
        constexpr char digits[] = "0123456789abcdef";
        for (const unsigned char ch : config.session_key) {
            hex.push_back(digits[ch >> 4]);
            hex.push_back(digits[ch & 0x0f]);
        }
        std::filesystem::path dir{config.storage_directory};
        dir /= "sessions";
        for (std::size_t off = 0; off < hex.size(); off += 64) {
            dir /= hex.substr(off, std::min<std::size_t>(64, hex.size() - off));
        }
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        if (ec) return Status{ErrorCode::io_error};
        const std::string jpath = (dir / "session.nfxj").string();

        JournalConfig jcfg{};
        jcfg.path = jpath;
        jcfg.session_key = config.session_key;
        jcfg.capacity_bytes = config.journal_capacity_bytes;
        jcfg.max_records = config.journal_max_records;
        jcfg.test_hook = config.journal_test_hook;
        jcfg.segment_bytes = config.journal_segment_bytes;
        jcfg.max_segments = config.journal_max_segments;
        jcfg.max_readers = 0; // Zero hot-path reader registry overhead.
        auto opened = Journal::open(jcfg);
        if (!opened.ok()) return opened.status();

        std::shared_ptr<TelemetryWriter> telemetry_writer{};
        if (!config.telemetry_path.empty()) {
            auto created = TelemetryWriter::create(config.telemetry_path, 4);
            if (!created.ok()) return created.status();
            telemetry_writer = std::move(created).value();
        }

        DirectInitiator init(std::move(config), std::move(handler),
                             std::move(transport), std::move(opened).value(),
                             std::move(telemetry_writer));
        if (!init.recovery_status_.ok()) return init.recovery_status_;
        return init;
    } catch (const std::bad_alloc&) {
        return Status{ErrorCode::capacity_exceeded};
    } catch (...) {
        return Status{ErrorCode::io_error};
    }

    // Initiates nonblocking TCP connection to the active endpoint in the failover list.
    // On immediate transport refusal with immediate_failover enabled, advances to standby
    // endpoints within the same pass before applying backoff.
    [[nodiscard]] Status connect() noexcept {
        return connect(clock_.now_ms(), now_wall_time());
    }

    [[nodiscard]] Status connect(std::uint64_t now_ms, std::string_view wall_time) noexcept {
        if (fatal_) return last_status_.ok() ? Status{ErrorCode::engine_stopped} : last_status_;
        if (transport_.valid()) return Status{ErrorCode::not_ready};
        if (failover_.exhausted()) return Status{ErrorCode::connection_closed};
        const std::size_t max_immediate = std::max<std::size_t>(1, failover_.size());
        Status last_err{ErrorCode::io_error};
        for (std::size_t attempt = 0; attempt < max_immediate; ++attempt) {
            const auto& ep = failover_.current_endpoint();
            const std::size_t idx = failover_.current_index();
            if (telemetry_slot_ != nullptr) {
                telemetry_slot_->active_endpoint_index.store(idx, std::memory_order_relaxed);
            }
            if constexpr (requires { handler_.on_endpoint_selected(ep, idx); }) {
                try { handler_.on_endpoint_selected(ep, idx); } catch (...) {}
            }
            if constexpr (requires { transport_.set_socket_options(config_.socket_options); }) {
                transport_.set_socket_options(config_.socket_options);
            }
            const auto st = transport_.connect(ep.host, ep.port);
            if (st.ok()) {
                if (!transport_.is_connecting()) {
                    connect_deadline_ = 0;
                    return on_transport_connected(now_ms, wall_time);
                }
                connect_deadline_ = config_.connect_timeout_ms != 0
                    ? saturating_add(now_ms, config_.connect_timeout_ms) : 0;
                return Status::success();
            }
            last_err = st;
            (void)failover_.on_failure_or_disconnect(now_ms, false);
            if (failover_.exhausted() || failover_.next_connect_at_ms() > now_ms) {
                return last_err;
            }
        }
        return last_err;
    }

    // Attempts reconnection when disconnected, not fatal, not exhausted, and backoff elapsed.
    // Returns Status::success() when already connected or when a connection was initiated,
    // ErrorCode::need_more while waiting for backoff, or the transport/exhaustion error.
    [[nodiscard]] Status reconnect_if_due() noexcept {
        return reconnect_if_due(clock_.now_ms(), now_wall_time());
    }

    [[nodiscard]] Status reconnect_if_due(std::uint64_t now_ms, std::string_view wall_time) noexcept {
        if (fatal_) return last_status_.ok() ? Status{ErrorCode::engine_stopped} : last_status_;
        if (transport_.valid()) return Status::success();
        if (failover_.exhausted()) return Status{ErrorCode::connection_closed};
        if (now_ms < failover_.next_connect_at_ms()) return Status{ErrorCode::need_more};
        return connect(now_ms, wall_time);
    }

    // Single nonblocking iteration: finish connect -> read & dispatch frames ->
    // flush pending outbound/replay -> process heartbeat/timeout timers.
    // Performs zero heap allocations, zero mutex locks, and zero fsync on steady state.
    [[nodiscard]] Status poll_once() noexcept {
        return poll_once(clock_.now_ms(), now_wall_time());
    }

    [[nodiscard]] Status poll_once(std::uint64_t now_ms, std::string_view wall_time) noexcept {
        if (fatal_) return last_status_.ok() ? Status{ErrorCode::engine_stopped} : last_status_;
        if (const auto ts = set_wall_time(wall_time); !ts.ok()) return ts;
        now_ms_ = now_ms;

        if (!transport_.valid()) return Status{ErrorCode::not_ready};

        if (transport_.is_connecting()) {
            const auto fc = transport_.finish_connect();
            if (fc.code == ErrorCode::need_more) {
                if (connect_deadline_ != 0 && now_ms >= connect_deadline_) {
                    Status timeout_err{ErrorCode::io_error};
                    if constexpr (requires { transport_.is_handshaking(); }) {
                        if (transport_.is_handshaking()) {
                            timeout_err = Status{ErrorCode::tls_handshake_timeout};
                        }
                    }
                    last_status_ = timeout_err;
                    if constexpr (requires { handler_.on_error(timeout_err); }) {
                        try { handler_.on_error(timeout_err); } catch (...) {}
                    }
                    handle_disconnect(timeout_err, now_ms);
                    return timeout_err;
                }
                return Status::success();
            }
            if (!fc.ok()) {
                last_status_ = fc;
                if constexpr (requires { handler_.on_error(fc); }) {
                    try { handler_.on_error(fc); } catch (...) {}
                }
                handle_disconnect(fc, now_ms);
                return fc;
            }
            connect_deadline_ = 0;
            if (const auto st = on_transport_connected(now_ms, wall_time_); !st.ok()) return st;
        }

        // 1. Flush pending TX / replay first so Logon / replies go out immediately.
        if (wants_write()) {
            if (const auto fl = flush_tx(now_ms); !fl.ok()) return fl;
        }
        if (should_close_) {
            handle_disconnect(Status{ErrorCode::connection_closed}, now_ms);
            return Status::success();
        }

        // 2. Nonblocking read + frame dispatch loop.
        for (int reads = 0; reads < 16 && !fatal_ && !should_close_ && transport_.valid(); ++reads) {
            if (rx_tail_ == rx_buf_.size()) {
                if (rx_head_ > 0) {
                    const std::size_t rem = rx_tail_ - rx_head_;
                    std::memmove(rx_buf_.data(), rx_buf_.data() + rx_head_, rem);
                    rx_head_ = 0;
                    rx_tail_ = rem;
                } else {
                    return fail(Status{ErrorCode::too_large});
                }
            }
            const std::uint64_t read_t0 = (telemetry_slot_ != nullptr && config_.telemetry_stage_probes)
                ? telemetry_now_ns() : 0ULL;
            const auto io = transport_.read(std::span<char>{rx_buf_.data() + rx_tail_, rx_buf_.size() - rx_tail_});
            if (io.bytes != 0) {
                if (read_t0 != 0ULL) {
                    telemetry_slot_->record_stage(TelemetryStage::read, telemetry_now_ns() - read_t0);
                }
                rx_tail_ += io.bytes;
                if (const auto st = drain_rx_frames(now_ms); !st.ok()) return st;
            }
            if (io.peer_closed) {
                handle_disconnect(Status{ErrorCode::connection_closed}, now_ms);
                return Status::success();
            }
            if (!io.status.ok()) {
                handle_disconnect(io.status, now_ms);
                return io.status;
            }
            if (io.would_block || io.bytes == 0) break;
        }

        if (should_close_) {
            if (wants_write()) (void)flush_tx(now_ms);
            handle_disconnect(Status{ErrorCode::connection_closed}, now_ms);
            return Status::success();
        }

        // 3. Flush any newly generated replies / ResendRequests / Heartbeats.
        if (wants_write()) {
            if (const auto fl = flush_tx(now_ms); !fl.ok()) return fl;
        }

        // 4. Enforce bounded write-stall deadline (matches SessionDriver::on_tick).
        if (write_timer_active_ && wants_write() && config_.write_stall_ms != 0 &&
            now_ms >= last_byte_progress_ms_ &&
            now_ms - last_byte_progress_ms_ >= config_.write_stall_ms) {
            if (!stall_logout_attempted_ && core_.state() == SessionState::active) {
                stall_logout_attempted_ = true;
                stall_logout_deadline_ = saturating_add(now_ms, config_.logout_timeout_ms);
                detail::CoreEvent lo{};
                lo.kind = detail::CoreEventKind::local_logout;
                lo.now_ms = now_ms;
                lo.text = "Write stalled";
                const auto lout = core_.process(lo);
                if (!lout.status.ok()) return fail(lout.status);
                bool logout_ok = true;
                for (std::size_t i = 0; i < lout.action_count; ++i) {
                    if (lout.actions[i].kind == detail::CoreActionKind::send_logout) {
                        std::array<detail::DirectAdminField, 1> f{};
                        std::size_t n = 0;
                        if (!lout.actions[i].text.empty()) {
                            f[n++] = {tag::text, FieldKind::string, 0, lout.actions[i].text};
                        }
                        if (!send_admin(msg_type::logout, {f.data(), n}, now_ms).ok()) {
                            logout_ok = false;
                            break;
                        }
                    }
                }
                if (!logout_ok) {
                    const Status err{ErrorCode::write_stalled};
                    last_status_ = err;
                    if constexpr (requires { handler_.on_error(err); }) {
                        try { handler_.on_error(err); } catch (...) {}
                    }
                    handle_disconnect(err, now_ms);
                    return err;
                }
            } else if (!stall_logout_attempted_ || now_ms >= stall_logout_deadline_) {
                const Status err{ErrorCode::write_stalled};
                last_status_ = err;
                if constexpr (requires { handler_.on_error(err); }) {
                    try { handler_.on_error(err); } catch (...) {}
                }
                handle_disconnect(err, now_ms);
                return err;
            }
        }

        // 5. Process SessionCore heartbeat / TestRequest / Logon / Logout timers.
        detail::CoreEvent tick{};
        tick.kind = detail::CoreEventKind::tick;
        tick.now_ms = now_ms;
        tick.has_pending_tx = wants_write();
        const auto out = core_.process(tick);
        if (out.status.code == ErrorCode::logon_timeout ||
            out.status.code == ErrorCode::heartbeat_timeout ||
            out.status.code == ErrorCode::logout_timeout) {
            last_status_ = out.status;
            if (telemetry_slot_ != nullptr) {
                telemetry_slot_->inc_error();
            }
            if constexpr (requires { handler_.on_error(out.status); }) {
                try { handler_.on_error(out.status); } catch (...) {}
            }
            handle_disconnect(out.status, now_ms);
            return out.status;
        }
        if (const auto st = apply_actions(out, nullptr, 0, 0, now_ms); !st.ok()) return st;

        if (wants_write()) {
            if (const auto fl = flush_tx(now_ms); !fl.ok()) return fl;
        }
        if (should_close_) {
            handle_disconnect(Status{ErrorCode::connection_closed}, now_ms);
        }
        return Status::success();
    }

    // Zero-allocation, zero-copy outbound send: encodes directly into the pre-faulted
    // v2 mmap Journal segment via reserve()/commit(), then writes directly to TCP.
    // Returns the assigned FIX MsgSeqNum (tag 34).
    template <class Message>
        requires requires(const Message& m) { m.view(); }
    [[nodiscard]] Result<std::uint64_t> send(const Message& message) noexcept {
        return send(message, clock_.now_ms(), now_wall_time());
    }

    template <class Message>
        requires requires(const Message& m) { m.view(); }
    [[nodiscard]] Result<std::uint64_t> send(const Message& message,
                                             std::uint64_t now_ms,
                                             std::string_view wall_time) noexcept {
        static_assert(!requires { requires std::remove_cvref_t<Message>::is_group_entry; },
            "repeating group entries cannot be sent as standalone FIX messages");
        if constexpr (requires { Schema::template allows_outbound_message<Message>(); }) {
            static_assert(Schema::template allows_outbound_message<Message>(),
                "message type does not belong to this DirectInitiator's Schema");
        }
        return send_view(message.view(), now_ms, wall_time, true);
    }

    // Same-thread batched enqueue: commits directly to mmap Journal and pushes into tx_ring_
    // WITHOUT issuing a socket syscall yet. Call flush() to send all queued frames in 1 writev/sendmsg.
    template <class Message>
        requires requires(const Message& m) { m.view(); }
    [[nodiscard]] Result<std::uint64_t> enqueue(const Message& message) noexcept {
        return enqueue(message, clock_.now_ms(), now_wall_time());
    }

    template <class Message>
        requires requires(const Message& m) { m.view(); }
    [[nodiscard]] Result<std::uint64_t> enqueue(const Message& message,
                                                std::uint64_t now_ms,
                                                std::string_view wall_time) noexcept {
        static_assert(!requires { requires std::remove_cvref_t<Message>::is_group_entry; },
            "repeating group entries cannot be sent as standalone FIX messages");
        if constexpr (requires { Schema::template allows_outbound_message<Message>(); }) {
            static_assert(Schema::template allows_outbound_message<Message>(),
                "message type does not belong to this DirectInitiator's Schema");
        }
        return send_view(message.view(), now_ms, wall_time, false);
    }

    // Zero-intermediate-copy streaming send: writes FIX wire bytes directly into the
    // pre-faulted v2 mmap Journal segment via reserve()/commit(), then writes to TCP.
    // Only session-immutable 8/9 and 49/56 are cached statically; dynamic standard header
    // fields (50/57/142/115/128/...) can be written per message via w.header().
    template <class Message, class Fn>
        requires detail::streamable_message<Message>
    [[nodiscard]] Result<std::uint64_t> send_stream(Fn&& fn) noexcept {
        return send_stream<Message>(std::forward<Fn>(fn), clock_.now_ms(), now_wall_time());
    }

    template <class Message, class Fn>
        requires detail::streamable_message<Message>
    [[nodiscard]] Result<std::uint64_t> send_stream(Fn&& fn,
                                                    std::uint64_t now_ms,
                                                    std::string_view wall_time) noexcept {
        static_assert(!requires { requires Message::is_group_entry; },
            "repeating group entries cannot be sent as standalone FIX messages");
        if constexpr (!std::is_same_v<Schema, FlatSchema>) {
            static_assert(std::is_same_v<Schema, typename Message::schema_type>,
                "message type does not belong to this DirectInitiator's Schema");
        }
        return send_stream_impl<Message>(std::forward<Fn>(fn), now_ms, wall_time, true);
    }

    // Same-thread batched streaming enqueue: streams directly into mmap Journal and pushes
    // into tx_ring_ without issuing a socket syscall yet. Call flush() to writev/sendmsg.
    template <class Message, class Fn>
        requires detail::streamable_message<Message>
    [[nodiscard]] Result<std::uint64_t> enqueue_stream(Fn&& fn) noexcept {
        return enqueue_stream<Message>(std::forward<Fn>(fn), clock_.now_ms(), now_wall_time());
    }

    template <class Message, class Fn>
        requires detail::streamable_message<Message>
    [[nodiscard]] Result<std::uint64_t> enqueue_stream(Fn&& fn,
                                                       std::uint64_t now_ms,
                                                       std::string_view wall_time) noexcept {
        static_assert(!requires { requires Message::is_group_entry; },
            "repeating group entries cannot be sent as standalone FIX messages");
        if constexpr (!std::is_same_v<Schema, FlatSchema>) {
            static_assert(std::is_same_v<Schema, typename Message::schema_type>,
                "message type does not belong to this DirectInitiator's Schema");
        }
        return send_stream_impl<Message>(std::forward<Fn>(fn), now_ms, wall_time, false);
    }

    // Flush all queued outbound / replay frames to transport (coalesces via writev/sendmsg when available).
    [[nodiscard]] Status flush() noexcept {
        return flush(clock_.now_ms());
    }

    [[nodiscard]] Status flush(std::uint64_t now_ms) noexcept {
        if (fatal_) return last_status_.ok() ? Status{ErrorCode::engine_stopped} : last_status_;
        if (!wants_write()) return Status::success();
        return flush_tx(now_ms);
    }

    // Drain up to max_batch pre-encoded messages from a lock-free SPSC DirectSendQueue,
    // stamp FIX MsgSeqNum/SendingTime, commit to mmap Journal, and flush in a single writev/sendmsg.
    [[nodiscard]] Result<std::size_t> drain(DirectSendQueue& queue,
                                            std::size_t max_batch = 64) noexcept {
        return drain(queue, max_batch, clock_.now_ms(), now_wall_time());
    }

    [[nodiscard]] Result<std::size_t> drain(DirectSendQueue& queue,
                                            std::size_t max_batch,
                                            std::uint64_t now_ms,
                                            std::string_view wall_time) noexcept {
        if (fatal_) return last_status_.ok() ? Status{ErrorCode::engine_stopped} : last_status_;
        if (!is_active()) return Status{ErrorCode::not_ready};
        if (max_batch == 0) return std::size_t{0};

        std::size_t drained = 0;
        bool wall_set = false;
        DirectSendQueue::SlotView slot{};
        const bool in_recovery_delivery = (active_delivery_id_ != 0 && active_delivery_recovery_);
        while (drained < max_batch && tx_count_ < tx_ring_.size() - 2 && queue.try_peek(slot)) {
            if (!wall_set) {
                if (const auto ts = set_wall_time(wall_time); !ts.ok()) return ts;
                now_ms_ = now_ms;
                wall_set = true;
            }
            const auto st = enqueue_prebuilt_body(slot.msg_type, slot.body, now_ms);
            if (!st.ok()) {
                if (drained == 0 || in_recovery_delivery || st.status().code != ErrorCode::queue_full) {
                    return st.status();
                }
                break;
            }
            queue.consume_one();
            ++drained;
        }
        if (drained != 0 && wants_write() && transport_.valid() && !transport_.is_connecting()) {
            if (const auto fl = flush_tx(now_ms); !fl.ok()) return fl;
        }
        return drained;
    }

    // Off-hot-path journal maintenance (checkpoint flush & spare segment refill).
    // Call periodically when idle (never required inside send()).
    [[nodiscard]] Status poll_maintenance() noexcept {
        if (!journal_.needs_maintenance()) return Status::success();
        return journal_.poll_maintenance();
    }

    [[nodiscard]] Status logout(std::string_view reason = {}) noexcept {
        return logout(reason, clock_.now_ms(), now_wall_time());
    }

    [[nodiscard]] Status logout(std::string_view reason, std::uint64_t now_ms,
                                std::string_view wall_time) noexcept {
        if (fatal_) return last_status_.ok() ? Status{ErrorCode::engine_stopped} : last_status_;
        if (const auto ts = set_wall_time(wall_time); !ts.ok()) return ts;
        now_ms_ = now_ms;
        detail::CoreEvent ev{};
        ev.kind = detail::CoreEventKind::local_logout;
        ev.now_ms = now_ms;
        ev.text = reason;
        const auto out = core_.process(ev);
        if (const auto st = apply_actions(out, nullptr, 0, 0, now_ms); !st.ok()) return st;
        if (wants_write() && transport_.valid() && !transport_.is_connecting()) {
            return flush_tx(now_ms);
        }
        return Status::success();
    }

    void disconnect(Status reason = Status{ErrorCode::connection_closed}) noexcept {
        handle_disconnect(reason, clock_.now_ms());
    }

    // Arms a one-shot ResetSeqNumFlag=Y (141=Y) epoch reset for the next connect().
    // Requires DirectInitiatorConfig::allow_sequence_reset == true.
    [[nodiscard]] Status request_sequence_reset(
        std::string_view reason = "admin sequence reset") noexcept {
        if (fatal_) return last_status_.ok() ? Status{ErrorCode::engine_stopped} : last_status_;
        if (!config_.allow_sequence_reset) return Status{ErrorCode::sequence_reset_denied, tag::reset_seq_num_flag};
        if (reason.empty() || reason.size() > max_admin_reason_bytes || !detail::is_valid_text(reason))
            return Status{ErrorCode::invalid_value, tag::text};
        try {
            one_shot_reset_reason_.assign(reason.data(), reason.size());
            one_shot_sequence_reset_ = true;
            return Status::success();
        } catch (...) {
            return Status{ErrorCode::capacity_exceeded};
        }
    }

    // Durably adjusts next_inbound / next_outbound sequences while disconnected.
    // Lowering below current sequences requires allow_sequence_reset == true and
    // records an audited epoch reset before advancing to the target pair.
    [[nodiscard]] Status adjust_sequences(std::uint64_t next_inbound,
                                          std::uint64_t next_outbound,
                                          std::string_view reason = "admin sequence adjust") noexcept {
        if (fatal_) return last_status_.ok() ? Status{ErrorCode::engine_stopped} : last_status_;
        if (transport_.valid() || core_.state() != SessionState::disconnected)
            return Status{ErrorCode::not_ready};
        if (next_inbound == 0 || next_outbound == 0) return Status{ErrorCode::invalid_value, tag::msg_seq_num};
        if (reason.empty() || reason.size() > max_admin_reason_bytes || !detail::is_valid_text(reason))
            return Status{ErrorCode::invalid_value, tag::text};
        if (next_inbound == core_.next_inbound() && next_outbound == core_.next_outbound())
            return Status::success();
        if (next_inbound < core_.next_inbound() || next_outbound < core_.next_outbound()) {
            if (!config_.allow_sequence_reset)
                return Status{ErrorCode::sequence_reset_denied, tag::reset_seq_num_flag};
            const auto rs = journal_.begin_epoch(reason);
            if (!rs.ok()) return rs;
            detail::CoreEvent rc{};
            rc.kind = detail::CoreEventKind::reset_epoch;
            const auto rout = core_.process(rc);
            if (!rout.status.ok()) return rout.status;
            peer_ack_seq_ = 0;
        }
        const std::string_view wt = now_wall_time();
        if (next_inbound > core_.next_inbound()) {
            const auto cur_in = core_.next_inbound();
            std::array<detail::DirectAdminField, 3> fields{
                detail::DirectAdminField{tag::gap_fill_flag, FieldKind::character, static_cast<unsigned char>('N'), {}},
                detail::DirectAdminField{tag::new_seq_no, FieldKind::unsigned_integer, next_inbound, {}},
                detail::DirectAdminField{tag::text, FieldKind::string, 0, reason},
            };
            detail::DirectAdminStorage stg{};
            if (const auto st = detail::direct_build_admin(msg_type::sequence_reset, fields, stg, admin_arena_); !st.ok())
                return st;
            SessionHeader hdr{config_.remote_comp_id, config_.local_comp_id, cur_in, wt};
            hdr.version = config_.version;
            const auto enc = encode_fix(stg.view, hdr, encode_buf_);
            if (!enc.ok()) return enc.status();
            const auto app = journal_.append(RecordKind::inbound, cur_in, 0, 0,
                std::string_view{encode_buf_.data(), enc.value()}, next_inbound);
            if (!app.ok()) return app.status();
            if (!journal_.has_delivery(app.value().id)) {
                if (const auto del = append_delivery(app.value().id, cur_in); !del.ok()) return del;
            }
        }
        if (next_outbound > core_.next_outbound()) {
            const auto cur_out = core_.next_outbound();
            std::array<detail::DirectAdminField, 3> fields{
                detail::DirectAdminField{tag::gap_fill_flag, FieldKind::character, static_cast<unsigned char>('Y'), {}},
                detail::DirectAdminField{tag::new_seq_no, FieldKind::unsigned_integer, next_outbound, {}},
                detail::DirectAdminField{tag::text, FieldKind::string, 0, reason},
            };
            detail::DirectAdminStorage stg{};
            if (const auto st = detail::direct_build_admin(msg_type::sequence_reset, fields, stg, admin_arena_); !st.ok())
                return st;
            SessionHeader hdr{config_.local_comp_id, config_.remote_comp_id, cur_out, wt, true, wt};
            hdr.version = config_.version;
            const auto enc = encode_fix(stg.view, hdr, encode_buf_);
            if (!enc.ok()) return enc.status();
            const std::uint64_t persisted_next = next_outbound > cur_out + 1 ? next_outbound : 0;
            const auto app = journal_.append(RecordKind::outbound, cur_out, 0, 0,
                std::string_view{encode_buf_.data(), enc.value()}, persisted_next);
            if (!app.ok()) return app.status();
        }
        detail::CoreEvent adj{};
        adj.kind = detail::CoreEventKind::adjust_sequences;
        adj.first = next_inbound;
        adj.last = next_outbound;
        const auto aout = core_.process(adj);
        if (!aout.status.ok()) return aout.status;
        update_retention_floor();
        sync_telemetry_state();
        return Status::success();
    }

    [[nodiscard]] std::uint64_t unexpected_fields_count() const noexcept { return unexpected_fields_; }

    [[nodiscard]] std::string_view now_wall_time() noexcept {
        return clock_.wall_time(config_.timestamp_precision);
    }
    [[nodiscard]] const TcpEndpoint& active_endpoint() const noexcept { return failover_.current_endpoint(); }
    [[nodiscard]] std::size_t active_endpoint_index() const noexcept { return failover_.current_index(); }
    [[nodiscard]] bool failover_exhausted() const noexcept { return failover_.exhausted(); }
    [[nodiscard]] std::uint64_t next_reconnect_at_ms() const noexcept { return failover_.next_connect_at_ms(); }
    [[nodiscard]] std::uint32_t consecutive_connect_failures() const noexcept { return failover_.consecutive_failures(); }
    [[nodiscard]] int native_fd() const noexcept { return transport_.native_fd(); }
    [[nodiscard]] bool valid_transport() const noexcept { return transport_.valid(); }
    [[nodiscard]] bool is_connecting() const noexcept { return transport_.is_connecting(); }
    [[nodiscard]] bool wants_write() const noexcept {
        return tx_count_ != 0 || replay_active_ || replay_count_ != 0;
    }
    [[nodiscard]] bool wants_transport_write() const noexcept {
        if (wants_write() || transport_.is_connecting()) {
            if constexpr (requires { transport_.wants_write(); }) {
                return wants_write() || transport_.wants_write();
            }
            return true;
        }
        return false;
    }
    [[nodiscard]] bool has_pending_read() const noexcept {
        if (rx_head_ < rx_tail_) return true;
        if constexpr (requires { transport_.pending_plaintext(); }) {
            return transport_.pending_plaintext() != 0;
        }
        return false;
    }
    [[nodiscard]] bool is_active() const noexcept {
        return core_.state() == SessionState::active && !fatal_;
    }
    [[nodiscard]] SessionState state() const noexcept {
        return fatal_ ? SessionState::failed : core_.state();
    }
    [[nodiscard]] Status status() const noexcept { return last_status_; }
    [[nodiscard]] std::uint64_t next_deadline_ms() const noexcept {
        if (transport_.valid() && transport_.is_connecting()) {
            return connect_deadline_ != 0 ? connect_deadline_ : std::numeric_limits<std::uint64_t>::max();
        }
        auto deadline = core_.next_deadline();
        if (stall_logout_attempted_) {
            deadline = std::min(deadline, stall_logout_deadline_);
        } else if (write_timer_active_ && wants_write() && config_.write_stall_ms != 0) {
            deadline = std::min(deadline, saturating_add(last_byte_progress_ms_, config_.write_stall_ms));
        }
        return deadline;
    }
    [[nodiscard]] std::uint64_t next_inbound_seq() const noexcept { return core_.next_inbound(); }
    [[nodiscard]] std::uint64_t next_outbound_seq() const noexcept { return core_.next_outbound(); }
    [[nodiscard]] std::size_t pending_outbound() const noexcept {
        return tx_count_ + replay_count_ + (replay_active_ ? 1U : 0U);
    }
    [[nodiscard]] bool needs_maintenance() const noexcept { return journal_.needs_maintenance(); }
    [[nodiscard]] const Journal& journal() const noexcept { return journal_; }
    [[nodiscard]] const TelemetrySessionSlot* telemetry_slot() const noexcept { return telemetry_slot_; }
    [[nodiscard]] Handler& handler() noexcept { return handler_; }
    [[nodiscard]] const Handler& handler() const noexcept { return handler_; }
    [[nodiscard]] Transport& transport() noexcept { return transport_; }
    [[nodiscard]] const Transport& transport() const noexcept { return transport_; }

private:
    struct TxEntry {
        RecordId record_id{};
        std::size_t offset{};
        std::size_t size{};
        std::uint64_t seq{};
        std::array<char, 32> msg_type{};
        std::uint8_t msg_type_size{};
        bool ephemeral{};
    };

    struct ReplayRange {
        std::uint64_t begin{};
        std::uint64_t end{};
        RecordId dependency{};
        std::vector<RecordId> merged_deps{};
    };

    DirectInitiator(DirectInitiatorConfig cfg, Handler h, Transport tr, Journal j,
                    std::shared_ptr<TelemetryWriter> tw = {})
        : config_(std::move(cfg)),
          handler_(std::move(h)),
          transport_(std::move(tr)),
          journal_(std::move(j)),
          telemetry_writer_(std::move(tw)),
          core_(SessionRole::initiator, config_.heartbeat_ms,
                config_.logon_timeout_ms, config_.logout_timeout_ms,
                journal_.next_inbound_seq(), journal_.next_outbound_seq()),
          rx_buf_(std::max(config_.rx_buffer_bytes, config_.max_frame_bytes * 2)),
          encode_buf_(config_.max_frame_bytes),
          replay_buf_(config_.max_frame_bytes + 128),
          deferred_logon_buf_(config_.max_frame_bytes),
          logon_tx_buf_(config_.max_frame_bytes),
          admin_arena_(config_.max_frame_bytes),
          parse_fields_(config_.max_frame_bytes / 4 + 2),
          aux_parse_fields_(config_.max_frame_bytes / 4 + 2),
          tx_ring_(config_.max_pending_messages),
          replay_ring_(config_.max_pending_messages),
          recovery_ids_() {
        (void)session_prefix_.init(config_.version, config_.local_comp_id, config_.remote_comp_id);
        std::vector<TcpEndpoint> eps = config_.endpoints;
        if (eps.empty()) {
            eps.push_back(TcpEndpoint{config_.host, config_.port});
        }
        failover_.configure(std::move(eps), config_.connection_strategy, 0);
        const auto retained_floor = journal_.lowest_retained_outbound_seq();
        const auto current_epoch = journal_.checkpoint().epoch;
        const auto max_out = journal_.next_outbound_seq() == 0 ? 0 : journal_.next_outbound_seq() - 1;
        std::uint32_t ack_epoch = journal_.records().empty()
            ? current_epoch : journal_.records().front().epoch;
        std::uint64_t scanned_ack = 0;
        std::uint64_t current_epoch_ack = 0;
        recovery_ids_.reserve(64);
        for (const auto& rec : journal_.records()) {
            if (rec.epoch != ack_epoch) { ack_epoch = rec.epoch; scanned_ack = 0; }
            if (rec.kind == RecordKind::epoch_reset) { ack_epoch = rec.epoch + 1U; scanned_ack = 0; }
            if (rec.kind == RecordKind::inbound) {
                FlatMessageView msg{};
                if (!parse_frame(rec.payload, msg).ok()) {
                    recovery_status_ = Status{ErrorCode::corrupt_store};
                    break;
                }
                const auto tags = detail::direct_extract_tags(msg);
                if (tags.dup_tag == tag::last_msg_seq_num_processed) {
                    recovery_status_ = Status{ErrorCode::corrupt_store, tag::last_msg_seq_num_processed};
                    break;
                }
                if (tags.peer_ack != nullptr) {
                    std::uint64_t val{};
                    if (!detail::direct_parse_u64(tags.peer_ack->value, val) || val < scanned_ack ||
                        (rec.epoch == current_epoch && val > max_out)) {
                        recovery_status_ = Status{ErrorCode::corrupt_store, tag::last_msg_seq_num_processed};
                        break;
                    }
                    scanned_ack = val;
                    if (rec.epoch == current_epoch) current_epoch_ack = val;
                }
                if (!journal_.has_delivery(rec.id)) {
                    recovery_ids_.push_back(rec.id);
                }
            }
        }
        peer_ack_seq_ = std::max(current_epoch_ack, retained_floor > 1 ? retained_floor - 1 : 0);
        if (telemetry_writer_) {
            telemetry_slot_ = telemetry_writer_->register_session(
                config_.session_key, config_.local_comp_id, config_.remote_comp_id);
            sync_telemetry_state();
        }
    }

    void sync_telemetry_state() noexcept {
        if (telemetry_slot_ == nullptr) return;
        telemetry_slot_->state.store(static_cast<std::uint8_t>(state()), std::memory_order_release);
        telemetry_slot_->next_inbound_seq.store(core_.next_inbound(), std::memory_order_relaxed);
        telemetry_slot_->next_outbound_seq.store(core_.next_outbound(), std::memory_order_relaxed);
        telemetry_slot_->active_endpoint_index.store(failover_.current_index(), std::memory_order_relaxed);
        telemetry_slot_->last_update_ns.store(telemetry_now_ns(), std::memory_order_relaxed);
    }

    [[nodiscard]] Status set_wall_time(std::string_view v) noexcept {
        if (wall_time_size_ != 0 && v == std::string_view{wall_time_buf_.data(), wall_time_size_}) {
            wall_time_ = {wall_time_buf_.data(), wall_time_size_};
            return Status::success();
        }
        if (!detail::is_valid_utc_timestamp(v) || v.size() > wall_time_buf_.size())
            return Status{ErrorCode::invalid_timestamp, tag::sending_time};
        std::memcpy(wall_time_buf_.data(), v.data(), v.size());
        wall_time_size_ = v.size();
        wall_time_ = {wall_time_buf_.data(), wall_time_size_};
        wall_time_ms_valid_ = false;
        return Status::success();
    }

    [[nodiscard]] SessionHeader make_header(std::uint64_t seq, std::string_view wt,
                                            bool poss_dup = false,
                                            std::string_view orig_time = {}) const noexcept {
        SessionHeader h{config_.local_comp_id, config_.remote_comp_id, seq, wt, poss_dup, orig_time};
        h.version = config_.version;
        if (config_.emit_last_processed_seq) {
            const auto completed = journal_.completed_inbound_seq();
            if (completed != 0) h.last_msg_seq_num_processed = completed;
        }
        return h;
    }

    [[nodiscard]] Status parse_frame(std::string_view bytes, FlatMessageView& out) noexcept {
        if (bytes.size() > config_.max_frame_bytes) return Status{ErrorCode::too_large};
        FrameView fv{};
        constexpr auto hints = detail::direct_schema_wire_hints<Schema>();
        if (const auto st = inspect_frame(bytes, fv, hints,
                CodecLimits{config_.max_frame_bytes, config_.verify_checksum}); !st.ok())
            return st;
        if (fv.consumed != bytes.size()) return Status{ErrorCode::invalid_frame};
        if (const auto st = parse_flat_verified(fv, parse_fields_, out, hints); !st.ok()) return st;
        out.bind_index(&parse_index_);
        return Status::success();
    }

    // Auxiliary parser for historical outbound records (replay / recovery reuse) that
    // must NEVER overwrite parse_fields_ or parse_index_ while an inbound callback is active.
    [[nodiscard]] Status parse_frame_aux(std::string_view bytes, FlatMessageView& out) noexcept {
        if (bytes.size() > config_.max_frame_bytes) return Status{ErrorCode::too_large};
        FrameView fv{};
        constexpr auto hints = detail::direct_schema_wire_hints<Schema>();
        if (const auto st = inspect_frame(bytes, fv, hints,
                CodecLimits{config_.max_frame_bytes, config_.verify_checksum}); !st.ok())
            return st;
        if (fv.consumed != bytes.size()) return Status{ErrorCode::invalid_frame};
        return parse_flat_verified(fv, aux_parse_fields_, out, hints);
    }

    [[nodiscard]] Status fail(Status err) noexcept {
        if (fatal_) return last_status_.ok() ? err : last_status_;
        if (err.ok()) err = Status{ErrorCode::invalid_value};
        last_status_ = err;
        fatal_ = true;
        should_close_ = true;
        if (transport_.valid()) transport_.close();
        detail::direct_secure_clear(logon_tx_buf_);
        logon_tx_size_ = 0;
        detail::direct_secure_clear(deferred_logon_buf_);
        deferred_logon_size_ = 0;
        if (telemetry_slot_ != nullptr) {
            telemetry_slot_->inc_error();
            sync_telemetry_state();
        }
        if constexpr (requires { handler_.on_error(err); }) {
            try { handler_.on_error(err); } catch (...) {}
        }
        return last_status_;
    }

    void handle_disconnect(Status reason, std::uint64_t now_ms) noexcept {
        const bool was_active = (core_.state() == SessionState::active);
        transport_.close();
        rx_head_ = rx_tail_ = 0;
        tx_head_ = tx_count_ = 0;
        replay_head_ = replay_count_ = 0;
        replay_origin_begin_ = 0;
        replay_active_ = replay_prepared_ = false;
        replay_offset_ = replay_bytes_ = 0;
        replay_dep_ = 0;
        replay_merged_deps_.clear();
        for (auto& slot : replay_ring_) slot.merged_deps.clear();
        retention_deferred_ = false;
        logon_tx_size_ = 0;
        deferred_logon_size_ = 0;
        deferred_logon_seq_ = 0;
        deferred_logon_pending_ = false;
        local_epoch_reset_started_ = false;
        one_shot_reset_active_ = false;
        connect_deadline_ = 0;
        write_timer_active_ = false;
        stall_logout_attempted_ = false;
        stall_logout_deadline_ = 0;
        should_close_ = false;
        detail::direct_secure_clear(logon_tx_buf_);
        detail::direct_secure_clear(deferred_logon_buf_);

        detail::CoreEvent ev{};
        ev.kind = detail::CoreEventKind::disconnected;
        ev.now_ms = now_ms;
        (void)core_.process(ev);
        (void)failover_.on_failure_or_disconnect(now_ms, was_active);
        sync_telemetry_state();
        if constexpr (requires { handler_.on_disconnected(reason); }) {
            try { handler_.on_disconnected(reason); } catch (...) {}
        }
    }

    [[nodiscard]] Status on_transport_connected(std::uint64_t now_ms, std::string_view wt) noexcept {
        if (const auto ts = set_wall_time(wt); !ts.ok()) return ts;
        now_ms_ = now_ms;
        last_status_ = Status::success();
        const bool want_reset = config_.request_sequence_reset || one_shot_sequence_reset_;
        if (want_reset && !local_epoch_reset_started_) {
            if (!config_.allow_sequence_reset)
                return fail(Status{ErrorCode::sequence_reset_denied, tag::reset_seq_num_flag});
            const std::string_view rs_reason = (one_shot_sequence_reset_ && !one_shot_reset_reason_.empty())
                ? std::string_view{one_shot_reset_reason_}
                : std::string_view{"local Logon ResetSeqNumFlag=Y"};
            const auto rs = journal_.begin_epoch(rs_reason);
            if (!rs.ok()) return fail(rs);
            detail::CoreEvent rc{};
            rc.kind = detail::CoreEventKind::reset_epoch;
            const auto rout = core_.process(rc);
            if (!rout.status.ok()) return fail(rout.status);
            peer_ack_seq_ = 0;
            local_epoch_reset_started_ = true;
            if (one_shot_sequence_reset_) {
                one_shot_sequence_reset_ = false;
                one_shot_reset_active_ = true;
            }
        }
        detail::CoreEvent ev{};
        ev.kind = detail::CoreEventKind::connected;
        ev.now_ms = now_ms;
        const auto out = core_.process(ev);
        if (!out.status.ok()) return fail(out.status);
        if (const auto st = apply_actions(out, nullptr, 0, 0, now_ms); !st.ok()) return st;
        if (wants_write()) return flush_tx(now_ms);
        return Status::success();
    }

    [[nodiscard]] static constexpr std::uint64_t saturating_add(
        std::uint64_t v, std::uint64_t amount) noexcept {
        return amount > std::numeric_limits<std::uint64_t>::max() - v
            ? std::numeric_limits<std::uint64_t>::max() : v + amount;
    }

    [[nodiscard]] bool has_dependent_outbound(RecordId inbound_id) const noexcept {
        for (const auto& rec : journal_.records()) {
            if (rec.kind == RecordKind::outbound && rec.dependency == inbound_id) return true;
        }
        return false;
    }

    [[nodiscard]] const JournalRecord* find_dependent_business_reply(
        RecordId inbound_id, std::size_t target_ordinal) const noexcept {
        std::size_t ordinal = 0;
        for (const auto& rec : journal_.records()) {
            if (rec.kind != RecordKind::outbound || rec.dependency != inbound_id || rec.receipt == 0) continue;
            if (ordinal++ == target_ordinal) return &rec;
        }
        return nullptr;
    }

    [[nodiscard]] std::size_t count_dependent_business_replies(RecordId inbound_id) const noexcept {
        std::size_t count = 0;
        for (const auto& rec : journal_.records()) {
            if (rec.kind == RecordKind::outbound && rec.dependency == inbound_id && rec.receipt != 0) ++count;
        }
        return count;
    }

    [[nodiscard]] Status extract_original_reply_header(
        const JournalRecord& rec, SessionHeader& out) noexcept {
        FlatMessageView original{};
        if (const auto st = parse_frame_aux(rec.payload, original); !st.ok()) {
            return Status{ErrorCode::corrupt_store};
        }
        const auto tags = detail::direct_extract_tags(original);
        if (tags.dup_tag == tag::sending_time || tags.sending_time == nullptr)
            return Status{ErrorCode::corrupt_store, tag::sending_time};
        if (tags.dup_tag == tag::last_msg_seq_num_processed)
            return Status{ErrorCode::corrupt_store, tag::last_msg_seq_num_processed};
        out = make_header(rec.seq, tags.sending_time->value);
        out.last_msg_seq_num_processed.reset();
        if (tags.peer_ack != nullptr) {
            std::uint64_t ack{};
            if (!detail::direct_parse_u64(tags.peer_ack->value, ack))
                return Status{ErrorCode::corrupt_store, tag::last_msg_seq_num_processed};
            out.last_msg_seq_num_processed = ack;
        }
        return Status::success();
    }

    [[nodiscard]] Status enqueue_tx(RecordId id, std::size_t size, std::uint64_t seq,
                                    std::string_view msg_type, bool ephemeral = false) noexcept {
        if (tx_count_ >= tx_ring_.size() || msg_type.size() > 32) return Status{ErrorCode::queue_full};
        if (!write_timer_active_) {
            last_byte_progress_ms_ = now_ms_;
            write_timer_active_ = true;
        }
        auto& slot = tx_ring_[(tx_head_ + tx_count_) % tx_ring_.size()];
        slot.record_id = id;
        slot.offset = 0;
        slot.size = size;
        slot.seq = seq;
        slot.msg_type_size = static_cast<std::uint8_t>(msg_type.size());
        std::memcpy(slot.msg_type.data(), msg_type.data(), msg_type.size());
        slot.ephemeral = ephemeral;
        ++tx_count_;
        return Status::success();
    }

    [[nodiscard]] Status pop_tx(bool written, std::uint64_t now_ms) noexcept {
        if (tx_count_ == 0) return Status{ErrorCode::invalid_value};
        auto& front = tx_ring_[tx_head_];
        const auto mtype = std::string_view{front.msg_type.data(), front.msg_type_size};
        const auto seq = front.seq;
        if (front.ephemeral) {
            detail::direct_secure_clear(logon_tx_buf_);
            logon_tx_size_ = 0;
        }
        tx_head_ = (tx_head_ + 1) % tx_ring_.size();
        --tx_count_;
        if (!wants_write()) {
            write_timer_active_ = false;
            stall_logout_attempted_ = false;
            stall_logout_deadline_ = 0;
        }
        if (written) {
            detail::CoreEvent ev{};
            ev.kind = detail::CoreEventKind::outbound_written;
            ev.now_ms = now_ms;
            ev.sequence = seq;
            ev.msg_type = mtype;
            const auto out = core_.process(ev);
            if (!out.status.ok()) return fail(out.status);
            for (std::size_t i = 0; i < out.action_count; ++i)
                if (out.actions[i].kind == detail::CoreActionKind::close_transport) should_close_ = true;
        }
        if (retention_deferred_) [[unlikely]] update_retention_floor();
        return Status::success();
    }

    [[gnu::noinline, gnu::cold]] [[nodiscard]] Result<std::optional<std::uint64_t>>
    try_reuse_recovery_view(RecordId dep, detail::MessageView msg) noexcept {
        const auto* existing = find_dependent_business_reply(dep, active_delivery_send_ordinal_);
        if (existing == nullptr) return std::optional<std::uint64_t>{};
        SessionHeader orig_header{};
        if (const auto st = extract_original_reply_header(*existing, orig_header); !st.ok()) return st;
        const auto candidate = encode_fix(msg, orig_header, encode_buf_);
        if (!candidate.ok()) return candidate.status();
        if (candidate.value() != existing->payload.size() ||
            std::memcmp(encode_buf_.data(), existing->payload.data(), candidate.value()) != 0) {
            return Status{ErrorCode::invalid_value};
        }
        ++active_delivery_send_ordinal_;
        return std::optional<std::uint64_t>{existing->seq};
    }

    template <class Message, class Fn>
    [[gnu::noinline, gnu::cold]] [[nodiscard]] Result<std::optional<std::uint64_t>>
    try_reuse_recovery_stream(RecordId dep, Fn& fn) noexcept {
        const auto* existing = find_dependent_business_reply(dep, active_delivery_send_ordinal_);
        if (existing == nullptr) return std::optional<std::uint64_t>{};
        SessionHeader orig_header{};
        if (const auto st = extract_original_reply_header(*existing, orig_header); !st.ok()) return st;
        auto buf = detail::StreamBuffer::begin_frame(encode_buf_, Message::msg_type, orig_header);
        if (!buf.ok()) return buf.status();
        typename Message::stream_writer_type writer{buf};
        fn(writer);
        if (const auto req = writer.validate_required(); !req.ok()) return req;
        const auto candidate = buf.finish_frame();
        if (!candidate.ok()) return candidate.status();
        if (candidate.value() != existing->payload.size() ||
            std::memcmp(encode_buf_.data(), existing->payload.data(), candidate.value()) != 0) {
            return Status{ErrorCode::invalid_value};
        }
        ++active_delivery_send_ordinal_;
        return std::optional<std::uint64_t>{existing->seq};
    }

    [[nodiscard]] Result<std::uint64_t> send_view(detail::MessageView msg,
                                                  std::uint64_t now_ms,
                                                  std::string_view wt,
                                                  bool immediate_write) noexcept {
        if (fatal_) return last_status_.ok() ? Status{ErrorCode::engine_stopped} : last_status_;
        if (!is_active()) return Status{ErrorCode::not_ready};
        if (msg.msg_type.empty() || detail::direct_is_admin(msg.msg_type))
            return Status{ErrorCode::managed_field, tag::msg_type};
        const RecordId dep = active_delivery_id_;
        if (dep != 0 && active_delivery_recovery_) [[unlikely]] {
            const auto reused = try_reuse_recovery_view(dep, msg);
            if (!reused.ok()) return reused.status();
            if (reused.value().has_value()) return *reused.value();
        }
        if (tx_count_ >= tx_ring_.size() - 2) return Status{ErrorCode::queue_full};
        if (const auto ts = set_wall_time(wt); !ts.ok()) return ts;
        now_ms_ = now_ms;

        const auto seq = core_.next_outbound();
        const auto header = make_header(seq, wall_time_);

        // Zero-copy path with tight upper-bound reserve so small segments pack densely.
        std::size_t reserve_bound = 192U + msg.value_byte_count + msg.field_count * 40U;
        if (msg.group_count != 0 && msg.groups != nullptr) {
            for (std::size_t g = 0; g < msg.group_count; ++g) {
                const auto& grp = msg.groups[g];
                if (grp.entries != nullptr) {
                    for (std::size_t e = 0; e < grp.entry_count; ++e) {
                        reserve_bound += grp.entries[e].value_byte_count + grp.entries[e].field_count * 40U;
                    }
                }
            }
        }
        reserve_bound = std::min(config_.max_frame_bytes, reserve_bound);

        JournalRecord committed_rec{};
        bool committed_ok = false;
        const std::uint64_t j_t0 = (telemetry_slot_ != nullptr && config_.telemetry_stage_probes)
            ? telemetry_now_ns() : 0ULL;
        auto reserved = journal_.reserve(RecordKind::outbound, seq, seq, dep, reserve_bound);
        if (reserved.ok()) {
            const auto encoded = encode_fix(msg, header, reserved.value().payload);
            if (encoded.ok()) {
                const auto committed = journal_.commit(reserved.value(), encoded.value());
                if (!committed.ok()) return fail(committed.status());
                committed_rec = committed.value();
                committed_ok = true;
            } else if (encoded.status().code != ErrorCode::capacity_exceeded) {
                return encoded.status();
            }
        }
        if (!committed_ok) {
            const auto encoded = encode_fix(msg, header, encode_buf_);
            if (!encoded.ok()) return encoded.status();
            const auto appended = journal_.append(RecordKind::outbound, seq, seq, dep,
                std::string_view{encode_buf_.data(), encoded.value()});
            if (!appended.ok()) return fail(appended.status());
            committed_rec = appended.value();
        }
        if (j_t0 != 0ULL) {
            telemetry_slot_->record_stage(TelemetryStage::journal, telemetry_now_ns() - j_t0);
        }
        if (dep != 0) ++active_delivery_send_ordinal_;

        detail::CoreEvent app{};
        app.kind = detail::CoreEventKind::outbound_appended;
        app.now_ms = now_ms;
        app.sequence = seq;
        app.msg_type = msg.msg_type;
        const auto aout = core_.process(app);
        if (!aout.status.ok()) return fail(aout.status);

        // Fast-path direct socket write when immediate_write is requested and TX queue & replay are idle!
        const auto payload = committed_rec.payload;
        if (telemetry_slot_ != nullptr) {
            telemetry_slot_->inc_outbound(payload.size());
            sync_telemetry_state();
        }
        if (immediate_write && tx_count_ == 0 && !replay_active_ && replay_count_ == 0 &&
            transport_.valid() && !transport_.is_connecting()) {
            const std::uint64_t w_t0 = (telemetry_slot_ != nullptr && config_.telemetry_stage_probes)
                ? telemetry_now_ns() : 0ULL;
            const auto io = transport_.write(std::span<const char>{payload.data(), payload.size()});
            if (w_t0 != 0ULL && io.bytes != 0) {
                telemetry_slot_->record_stage(TelemetryStage::write, telemetry_now_ns() - w_t0);
            }
            if (io.bytes != 0) {
                last_byte_progress_ms_ = now_ms;
                stall_logout_attempted_ = false;
                stall_logout_deadline_ = 0;
            }
            if (io.bytes == payload.size() && io.status.ok()) {
                detail::CoreEvent wr{};
                wr.kind = detail::CoreEventKind::outbound_written;
                wr.now_ms = now_ms;
                wr.sequence = seq;
                wr.msg_type = msg.msg_type;
                const auto wout = core_.process(wr);
                if (!wout.status.ok()) return fail(wout.status);
                return seq;
            }
            if (!io.status.ok()) {
                handle_disconnect(io.status, now_ms);
                return seq; // Already durably committed in journal; will replay on reconnect!
            }
            // Partial write or EAGAIN: enqueue remaining tail in tx_ring_.
            if (const auto st = enqueue_tx(committed_rec.id, payload.size(), seq, msg.msg_type); !st.ok())
                return fail(st);
            tx_ring_[(tx_head_ + tx_count_ - 1) % tx_ring_.size()].offset = io.bytes;
            return seq;
        }

        if (const auto st = enqueue_tx(committed_rec.id, payload.size(), seq, msg.msg_type); !st.ok())
            return fail(st);
        return seq;
    }

    template <class Message, class Fn>
    [[nodiscard]] Result<std::uint64_t> send_stream_impl(Fn&& fn,
                                                         std::uint64_t now_ms,
                                                         std::string_view wt,
                                                         bool immediate_write) noexcept {
        constexpr std::string_view msg_type = Message::msg_type;
        if (fatal_) return last_status_.ok() ? Status{ErrorCode::engine_stopped} : last_status_;
        if (!is_active()) return Status{ErrorCode::not_ready};
        if (msg_type.empty() || detail::direct_is_admin(msg_type))
            return Status{ErrorCode::managed_field, tag::msg_type};
        const RecordId dep = active_delivery_id_;
        if (dep != 0 && active_delivery_recovery_) [[unlikely]] {
            const auto reused = try_reuse_recovery_stream<Message>(dep, fn);
            if (!reused.ok()) return reused.status();
            if (reused.value().has_value()) return *reused.value();
        }
        if (tx_count_ >= tx_ring_.size() - 2) return Status{ErrorCode::queue_full};
        if (const auto ts = set_wall_time(wt); !ts.ok()) return ts;
        now_ms_ = now_ms;

        const auto seq = core_.next_outbound();
        std::optional<std::uint64_t> last_processed{};
        if (config_.emit_last_processed_seq) {
            const auto completed = journal_.completed_inbound_seq();
            if (completed != 0) last_processed = completed;
        }

        const std::size_t seg_hint = config_.journal_segment_bytes != 0
            ? std::max<std::size_t>(512U, std::min<std::size_t>(config_.journal_segment_bytes / 2U, 4096U))
            : std::size_t{4096U};
        const std::size_t reserve_bound = std::min(config_.max_frame_bytes, seg_hint);

        JournalRecord committed_rec{};
        const std::uint64_t j_t0 = (telemetry_slot_ != nullptr && config_.telemetry_stage_probes)
            ? telemetry_now_ns() : 0ULL;
        struct SpillCtx {
            std::span<char> fallback;
            bool spilled{false};
        } spill{encode_buf_, false};
        auto reserved = journal_.reserve(RecordKind::outbound, seq, seq, dep, reserve_bound);
        std::span<char> target;
        detail::StreamBuffer::GrowFn grow = nullptr;
        if (reserved.ok()) [[likely]] {
            target = reserved.value().payload;
            grow = +[](void* raw, const char* old_data, std::size_t written,
                       std::size_t min_cap) noexcept -> std::span<char> {
                auto* st = static_cast<SpillCtx*>(raw);
                if (st->spilled || min_cap > st->fallback.size()) return {};
                if (written != 0) std::memcpy(st->fallback.data(), old_data, written);
                st->spilled = true;
                return st->fallback;
            };
        } else {
            target = encode_buf_;
            spill.spilled = true;
        }
        detail::StreamBuffer buf = session_prefix_.valid
            ? detail::StreamBuffer::begin_session_frame(
                target, msg_type, session_prefix_, seq, wall_time_, last_processed, grow, &spill)
            : detail::StreamBuffer::begin_frame(
                target, msg_type, make_header(seq, wall_time_), grow, &spill);
        if (!buf.ok()) return buf.status();
        typename Message::stream_writer_type writer{buf};
        fn(writer);
        if (const auto req = writer.validate_required(); !req.ok()) return req;
        const auto encoded = buf.finish_frame();
        if (!encoded.ok()) return encoded.status();
        if (!spill.spilled) [[likely]] {
            const auto committed = journal_.commit(reserved.value(), encoded.value());
            if (!committed.ok()) return fail(committed.status());
            committed_rec = committed.value();
        } else {
            const auto appended = journal_.append(RecordKind::outbound, seq, seq, dep,
                std::string_view{encode_buf_.data(), encoded.value()});
            if (!appended.ok()) return fail(appended.status());
            committed_rec = appended.value();
        }
        if (j_t0 != 0ULL) {
            telemetry_slot_->record_stage(TelemetryStage::journal, telemetry_now_ns() - j_t0);
        }
        if (dep != 0) ++active_delivery_send_ordinal_;

        detail::CoreEvent app{};
        app.kind = detail::CoreEventKind::outbound_appended;
        app.now_ms = now_ms;
        app.sequence = seq;
        app.msg_type = msg_type;
        const auto aout = core_.process(app);
        if (!aout.status.ok()) return fail(aout.status);

        const auto payload = committed_rec.payload;
        if (telemetry_slot_ != nullptr) {
            telemetry_slot_->inc_outbound(payload.size());
            sync_telemetry_state();
        }
        if (immediate_write && tx_count_ == 0 && !replay_active_ && replay_count_ == 0 &&
            transport_.valid() && !transport_.is_connecting()) {
            const std::uint64_t w_t0 = (telemetry_slot_ != nullptr && config_.telemetry_stage_probes)
                ? telemetry_now_ns() : 0ULL;
            const auto io = transport_.write(std::span<const char>{payload.data(), payload.size()});
            if (w_t0 != 0ULL && io.bytes != 0) {
                telemetry_slot_->record_stage(TelemetryStage::write, telemetry_now_ns() - w_t0);
            }
            if (io.bytes != 0) {
                last_byte_progress_ms_ = now_ms;
                stall_logout_attempted_ = false;
                stall_logout_deadline_ = 0;
            }
            if (io.bytes == payload.size() && io.status.ok()) {
                detail::CoreEvent wr{};
                wr.kind = detail::CoreEventKind::outbound_written;
                wr.now_ms = now_ms;
                wr.sequence = seq;
                wr.msg_type = msg_type;
                const auto wout = core_.process(wr);
                if (!wout.status.ok()) return fail(wout.status);
                return seq;
            }
            if (!io.status.ok()) {
                handle_disconnect(io.status, now_ms);
                return seq;
            }
            if (const auto st = enqueue_tx(committed_rec.id, payload.size(), seq, msg_type); !st.ok())
                return fail(st);
            tx_ring_[(tx_head_ + tx_count_ - 1) % tx_ring_.size()].offset = io.bytes;
            return seq;
        }

        if (const auto st = enqueue_tx(committed_rec.id, payload.size(), seq, msg_type); !st.ok())
            return fail(st);
        return seq;
    }

    [[gnu::noinline, gnu::cold]] [[nodiscard]] Result<std::optional<std::uint64_t>>
    try_reuse_recovery_prebuilt(RecordId dep, std::string_view msg_type,
                                std::string_view body) noexcept {
        const auto* existing = find_dependent_business_reply(dep, active_delivery_send_ordinal_);
        if (existing == nullptr) return std::optional<std::uint64_t>{};
        SessionHeader orig_header{};
        if (const auto st = extract_original_reply_header(*existing, orig_header); !st.ok()) return st;
        const auto candidate = encode_fix_from_body(msg_type, body, orig_header, encode_buf_);
        if (!candidate.ok()) return candidate.status();
        if (candidate.value() != existing->payload.size() ||
            std::memcmp(encode_buf_.data(), existing->payload.data(), candidate.value()) != 0) {
            return Status{ErrorCode::invalid_value};
        }
        ++active_delivery_send_ordinal_;
        return std::optional<std::uint64_t>{existing->seq};
    }

    [[nodiscard]] Result<std::uint64_t> enqueue_prebuilt_body(std::string_view msg_type,
                                                              std::string_view body,
                                                              std::uint64_t now_ms) noexcept {
        if (msg_type.empty() || detail::direct_is_admin(msg_type))
            return Status{ErrorCode::managed_field, tag::msg_type};
        const RecordId dep = active_delivery_id_;
        if (dep != 0 && active_delivery_recovery_) [[unlikely]] {
            const auto reused = try_reuse_recovery_prebuilt(dep, msg_type, body);
            if (!reused.ok()) return reused.status();
            if (reused.value().has_value()) return *reused.value();
        }
        const auto seq = core_.next_outbound();
        const auto header = make_header(seq, wall_time_);
        const std::size_t reserve_bound = std::min(config_.max_frame_bytes, std::size_t{192} + body.size());

        JournalRecord committed_rec{};
        bool committed_ok = false;
        auto reserved = journal_.reserve(RecordKind::outbound, seq, seq, dep, reserve_bound);
        if (reserved.ok()) {
            const auto encoded = encode_fix_from_body(msg_type, body, header, reserved.value().payload);
            if (encoded.ok()) {
                const auto committed = journal_.commit(reserved.value(), encoded.value());
                if (!committed.ok()) return fail(committed.status());
                committed_rec = committed.value();
                committed_ok = true;
            } else if (encoded.status().code != ErrorCode::capacity_exceeded) {
                return encoded.status();
            }
        }
        if (!committed_ok) {
            const auto encoded = encode_fix_from_body(msg_type, body, header, encode_buf_);
            if (!encoded.ok()) return encoded.status();
            const auto appended = journal_.append(RecordKind::outbound, seq, seq, dep,
                std::string_view{encode_buf_.data(), encoded.value()});
            if (!appended.ok()) return fail(appended.status());
            committed_rec = appended.value();
        }
        if (dep != 0) ++active_delivery_send_ordinal_;

        detail::CoreEvent app{};
        app.kind = detail::CoreEventKind::outbound_appended;
        app.now_ms = now_ms;
        app.sequence = seq;
        app.msg_type = msg_type;
        const auto aout = core_.process(app);
        if (!aout.status.ok()) return fail(aout.status);

        if (const auto st = enqueue_tx(committed_rec.id, committed_rec.payload.size(), seq, msg_type); !st.ok())
            return fail(st);
        if (telemetry_slot_ != nullptr) {
            telemetry_slot_->inc_outbound(committed_rec.payload.size());
            sync_telemetry_state();
        }
        return seq;
    }

    [[nodiscard]] Status send_admin(std::string_view msg_type,
                                    std::span<const detail::DirectAdminField> fields,
                                    std::uint64_t now_ms, RecordId dependency = 0) noexcept {
        const auto seq = core_.next_outbound();
        detail::DirectAdminStorage stg{};
        if (const auto st = detail::direct_build_admin(msg_type, fields, stg, admin_arena_); !st.ok())
            return st;
        const auto header = make_header(seq, wall_time_);
        const std::size_t admin_bound = std::min(config_.max_frame_bytes,
            std::size_t{256} + stg.view.value_byte_count + stg.view.field_count * 40U);
        RecordId committed_id = 0;
        std::size_t encoded_size = 0;
        auto reserved = journal_.reserve(RecordKind::outbound, seq, 0, dependency, admin_bound);
        if (reserved.ok()) {
            const auto encoded = encode_fix(stg.view, header, reserved.value().payload);
            if (encoded.ok()) {
                const auto committed = journal_.commit(reserved.value(), encoded.value());
                if (!committed.ok()) return committed.status();
                committed_id = committed.value().id;
                encoded_size = encoded.value();
            } else if (encoded.status().code != ErrorCode::capacity_exceeded) {
                return encoded.status();
            }
        }
        if (committed_id == 0) {
            const auto encoded = encode_fix(stg.view, header, encode_buf_);
            if (!encoded.ok()) return encoded.status();
            const auto appended = journal_.append(RecordKind::outbound, seq, 0, dependency,
                std::string_view{encode_buf_.data(), encoded.value()});
            if (!appended.ok()) return appended.status();
            committed_id = appended.value().id;
            encoded_size = encoded.value();
        }
        if (const auto st = enqueue_tx(committed_id, encoded_size, seq, msg_type); !st.ok())
            return st;
        if (telemetry_slot_ != nullptr) {
            telemetry_slot_->inc_outbound(encoded_size);
        }
        detail::CoreEvent ev{};
        ev.kind = detail::CoreEventKind::outbound_appended;
        ev.now_ms = now_ms;
        ev.sequence = seq;
        ev.msg_type = msg_type;
        return core_.process(ev).status;
    }

    [[nodiscard]] Status send_logon(std::uint64_t now_ms) noexcept {
        const auto seq = core_.next_outbound();
        std::array<detail::DirectAdminField, 8> safe{};
        std::array<detail::DirectAdminField, 8> wire{};
        std::size_t sc = 0;
        auto add = [&](const detail::DirectAdminField& f) { safe[sc] = f; wire[sc] = f; ++sc; };
        add({tag::encrypt_method, FieldKind::unsigned_integer, encrypt_method_none, {}});
        const auto hb_sec = std::max<std::uint64_t>(1, (static_cast<std::uint64_t>(config_.heartbeat_ms) + 999) / 1000);
        add({tag::heart_bt_int, FieldKind::unsigned_integer, hb_sec, {}});
        if (config_.request_sequence_reset || one_shot_reset_active_)
            add({tag::reset_seq_num_flag, FieldKind::character, static_cast<unsigned char>('Y'), {}});
        if (config_.version == FixVersion::fixt11)
            add({tag::default_appl_ver_id, FieldKind::string, 0, config_.default_appl_ver_id});

        detail::DirectAdminStorage stg{};
        const auto header = make_header(seq, wall_time_);
        if (const auto st = detail::direct_build_admin(msg_type::logon, {safe.data(), sc}, stg, admin_arena_); !st.ok())
            return st;
        const auto persisted = encode_fix(stg.view, header, encode_buf_);
        if (!persisted.ok()) return persisted.status();

        std::size_t wc = sc;
        if (!config_.username.empty()) wire[wc++] = {tag::username, FieldKind::string, 0, config_.username};
        if (!config_.password.empty()) wire[wc++] = {tag::password, FieldKind::string, 0, config_.password};
        if (const auto st = detail::direct_build_admin(msg_type::logon, {wire.data(), wc}, stg, admin_arena_); !st.ok())
            return st;
        const auto actual = encode_fix(stg.view, header, logon_tx_buf_);
        detail::direct_secure_clear(admin_arena_);
        if (!actual.ok()) { detail::direct_secure_clear(logon_tx_buf_); return actual.status(); }

        const auto appended = journal_.append(RecordKind::outbound, seq, 0, 0,
            std::string_view{encode_buf_.data(), persisted.value()});
        if (!appended.ok()) { detail::direct_secure_clear(logon_tx_buf_); return appended.status(); }
        logon_tx_size_ = actual.value();
        if (const auto st = enqueue_tx(appended.value().id, actual.value(), seq, msg_type::logon, true); !st.ok()) {
            detail::direct_secure_clear(logon_tx_buf_);
            logon_tx_size_ = 0;
            return st;
        }
        if (telemetry_slot_ != nullptr) {
            telemetry_slot_->inc_outbound(actual.value());
        }
        detail::CoreEvent ev{};
        ev.kind = detail::CoreEventKind::outbound_appended;
        ev.now_ms = now_ms;
        ev.sequence = seq;
        ev.msg_type = msg_type::logon;
        return core_.process(ev).status;
    }

    [[nodiscard]] bool attach_merged_replay_dep(std::vector<RecordId>& merged,
                                                RecordId primary, RecordId dep) const noexcept {
        if (dep == 0 || dep == primary) return true;
        if (std::find(merged.begin(), merged.end(), dep) != merged.end()) return true;
        const std::size_t cap = std::max<std::size_t>(replay_ring_.size() * 4U, 256U);
        if (merged.size() >= cap) return false;
        try {
            merged.push_back(dep);
            return true;
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] Status queue_replay(std::uint64_t begin, std::uint64_t end, RecordId dep) noexcept {
        const auto next_out = journal_.next_outbound_seq();
        const auto max_out = next_out == 0 ? 0 : next_out - 1;
        if (max_out != 0 && begin < journal_.lowest_retained_outbound_seq())
            return fail(Status{ErrorCode::history_unavailable, tag::begin_seq_no, static_cast<std::size_t>(begin)});
        if (end == 0 || end > max_out) end = max_out;
        if (max_out == 0 || begin > end) {
            if (dep != 0) {
                const auto* in = journal_.find_record(dep);
                if (in == nullptr || in->kind != RecordKind::inbound)
                    return Status{ErrorCode::corrupt_store};
                return append_delivery(dep, in->seq);
            }
            return Status::success();
        }
        // Merge/deduplicate overlapping or adjacent ResendRequests against active replay.
        if (replay_active_) {
            const std::uint64_t active_cover_begin =
                (dep == replay_dep_) ? replay_next_ : replay_origin_begin_;
            const std::uint64_t active_adj_end =
                replay_end_ + (replay_end_ != std::numeric_limits<std::uint64_t>::max());
            if (begin >= active_cover_begin && begin <= active_adj_end &&
                end >= active_cover_begin) {
                if (!attach_merged_replay_dep(replay_merged_deps_, replay_dep_, dep))
                    return Status{ErrorCode::queue_full};
                if (end > replay_end_) replay_end_ = end;
                return Status::success();
            }
        }
        // Merge overlapping or adjacent queued replay ranges.
        for (std::size_t i = replay_count_; i > 0; --i) {
            auto& slot = replay_ring_[(replay_head_ + i - 1) % replay_ring_.size()];
            const std::uint64_t slot_adj_end =
                slot.end + (slot.end != std::numeric_limits<std::uint64_t>::max());
            const std::uint64_t req_adj_end =
                end + (end != std::numeric_limits<std::uint64_t>::max());
            if (begin <= slot_adj_end && req_adj_end >= slot.begin) {
                if (!attach_merged_replay_dep(slot.merged_deps, slot.dependency, dep))
                    return Status{ErrorCode::queue_full};
                slot.begin = std::min(slot.begin, begin);
                slot.end = std::max(slot.end, end);
                return Status::success();
            }
        }
        if (replay_count_ >= replay_ring_.size()) return Status{ErrorCode::queue_full};
        if (!write_timer_active_) {
            last_byte_progress_ms_ = now_ms_;
            write_timer_active_ = true;
        }
        auto& target = replay_ring_[(replay_head_ + replay_count_) % replay_ring_.size()];
        target.begin = begin;
        target.end = end;
        target.dependency = dep;
        target.merged_deps.clear();
        ++replay_count_;
        start_next_replay();
        if (!wants_write()) {
            write_timer_active_ = false;
            stall_logout_attempted_ = false;
            stall_logout_deadline_ = 0;
        }
        return Status::success();
    }

    void start_next_replay() noexcept {
        if (replay_active_ || replay_count_ == 0) return;
        auto& r = replay_ring_[replay_head_];
        replay_origin_begin_ = r.begin;
        replay_next_ = r.begin;
        replay_end_ = r.end;
        replay_dep_ = r.dependency;
        replay_merged_deps_ = std::move(r.merged_deps);
        r.merged_deps.clear();
        replay_head_ = (replay_head_ + 1) % replay_ring_.size();
        --replay_count_;
        replay_active_ = true;
        replay_prepared_ = false;
    }

    [[nodiscard]] Status build_replay_frame() noexcept {
        if (!replay_active_ || replay_prepared_) return Status::success();
        while (replay_next_ <= replay_end_) {
            const auto* rec = journal_.find_outbound(replay_next_);
            if (rec == nullptr || rec->payload.empty()) return Status{ErrorCode::corrupt_store, tag::msg_seq_num};
            FlatMessageView parsed{};
            if (const auto st = parse_frame_aux(rec->payload, parsed); !st.ok()) return st;
            const auto tags = detail::direct_extract_tags(parsed);
            if (tags.msg_type == nullptr || tags.sending_time == nullptr)
                return Status{ErrorCode::corrupt_store, tag::msg_type};
            const auto orig_time = tags.orig_time ? tags.orig_time->value : tags.sending_time->value;
            if (!detail::direct_is_admin(tags.msg_type->value)) {
                auto header = make_header(rec->seq, wall_time_, true, orig_time);
                const auto enc = encode_fix_replay(parsed, header, replay_buf_);
                if (!enc.ok()) return enc.status();
                replay_bytes_ = enc.value();
                replay_offset_ = 0;
                replay_seq_ = rec->seq;
                replay_advance_ = rec->seq + 1;
                replay_msg_type_size_ = static_cast<std::uint8_t>(tags.msg_type->value.size());
                std::memcpy(replay_msg_type_.data(), tags.msg_type->value.data(), replay_msg_type_size_);
                replay_prepared_ = true;
                return Status::success();
            }
            // Coalesce consecutive admin messages into SequenceReset-GapFill (35=4, 123=Y).
            const auto gap_begin = replay_next_;
            auto gap_end = std::max(replay_next_ + 1,
                rec->next_seq > rec->seq ? rec->next_seq : rec->seq + 1);
            while (gap_end <= replay_end_) {
                const auto* nr = journal_.find_outbound(gap_end);
                if (nr == nullptr || nr->payload.empty()) return Status{ErrorCode::corrupt_store, tag::msg_seq_num};
                FlatMessageView np{};
                if (const auto st = parse_frame_aux(nr->payload, np); !st.ok()) return st;
                const auto nt = detail::direct_extract_tags(np);
                if (nt.msg_type == nullptr) return Status{ErrorCode::corrupt_store, tag::msg_type};
                if (!detail::direct_is_admin(nt.msg_type->value)) break;
                const auto adv = nr->next_seq > nr->seq ? nr->next_seq : nr->seq + 1;
                gap_end = std::max(gap_end + 1, adv);
            }
            std::array<detail::DirectAdminField, 2> gf{
                detail::DirectAdminField{tag::gap_fill_flag, FieldKind::character, static_cast<unsigned char>('Y'), {}},
                detail::DirectAdminField{tag::new_seq_no, FieldKind::unsigned_integer, gap_end, {}},
            };
            detail::DirectAdminStorage stg{};
            if (const auto st = detail::direct_build_admin(msg_type::sequence_reset, gf, stg, admin_arena_); !st.ok())
                return st;
            auto header = make_header(gap_begin, wall_time_, true, orig_time);
            const auto enc = encode_fix(stg.view, header, replay_buf_);
            if (!enc.ok()) return enc.status();
            replay_bytes_ = enc.value();
            replay_offset_ = 0;
            replay_seq_ = gap_begin;
            replay_advance_ = gap_end;
            replay_msg_type_size_ = static_cast<std::uint8_t>(msg_type::sequence_reset.size());
            std::memcpy(replay_msg_type_.data(), msg_type::sequence_reset.data(), msg_type::sequence_reset.size());
            replay_prepared_ = true;
            return Status::success();
        }
        return finish_replay_range();
    }

    [[nodiscard]] Status finish_replay_range() noexcept {
        const auto dep = replay_dep_;
        auto merged = std::move(replay_merged_deps_);
        replay_merged_deps_.clear();
        replay_origin_begin_ = 0;
        replay_active_ = replay_prepared_ = false;
        replay_offset_ = replay_bytes_ = 0;
        replay_dep_ = 0;
        auto complete_dep = [&](RecordId dep_id) noexcept -> Status {
            if (dep_id == 0) return Status::success();
            const auto* in = journal_.find_record(dep_id);
            if (in == nullptr || in->kind != RecordKind::inbound) return Status{ErrorCode::corrupt_store};
            return append_delivery(dep_id, in->seq);
        };
        if (const auto st = complete_dep(dep); !st.ok()) return st;
        for (const auto extra : merged) {
            if (const auto st = complete_dep(extra); !st.ok()) return st;
        }
        start_next_replay();
        if (retention_deferred_) [[unlikely]] update_retention_floor();
        if (!wants_write()) {
            write_timer_active_ = false;
            stall_logout_attempted_ = false;
            stall_logout_deadline_ = 0;
        }
        return Status::success();
    }

    [[nodiscard]] Status flush_tx(std::uint64_t now_ms) noexcept {
        while (!fatal_ && !should_close_ && transport_.valid() && !transport_.is_connecting()) {
            start_next_replay();
            if (replay_active_ && !replay_prepared_) {
                if (const auto st = build_replay_frame(); !st.ok()) return fail(st);
                if (!replay_active_) continue;
            }
            TxEntry* q = tx_count_ == 0 ? nullptr : &tx_ring_[tx_head_];
            const bool replay_due = replay_active_ && replay_prepared_ &&
                (q == nullptr || (q->offset == 0 && replay_seq_ <= q->seq));
            if (!replay_due && q == nullptr) break;

            // Coalesce multiple queued frames into 1 writev/sendmsg syscall when transport supports write_batch.
            if constexpr (requires(Transport& tr, std::span<const std::span<const char>> ch) {
                              tr.write_batch(ch);
                          }) {
                if (!replay_due && !replay_active_ && tx_count_ >= 2) {
                    std::array<std::span<const char>, 64> chunks{};
                    std::size_t n_chunks = 0;
                    const std::size_t limit = std::min<std::size_t>(tx_count_, chunks.size());
                    for (std::size_t i = 0; i < limit; ++i) {
                        const auto& entry = tx_ring_[(tx_head_ + i) % tx_ring_.size()];
                        if (entry.ephemeral) {
                            chunks[n_chunks++] = {logon_tx_buf_.data() + entry.offset, logon_tx_size_ - entry.offset};
                        } else {
                            const auto* rec = journal_.find_record(entry.record_id);
                            if (rec == nullptr && entry.record_id < journal_.lowest_retained_record_id())
                                return fail(Status{ErrorCode::history_unavailable});
                            if (rec == nullptr || rec->payload.size() != entry.size)
                                return fail(Status{ErrorCode::corrupt_store});
                            chunks[n_chunks++] = {rec->payload.data() + entry.offset, entry.size - entry.offset};
                        }
                    }
                    const std::uint64_t wb_t0 = (telemetry_slot_ != nullptr && config_.telemetry_stage_probes)
                        ? telemetry_now_ns() : 0ULL;
                    const auto io = transport_.write_batch(std::span<const std::span<const char>>{chunks.data(), n_chunks});
                    if (wb_t0 != 0ULL && io.bytes != 0) {
                        telemetry_slot_->record_stage(TelemetryStage::write, telemetry_now_ns() - wb_t0);
                    }
                    if (io.bytes != 0) {
                        last_byte_progress_ms_ = now_ms;
                        stall_logout_attempted_ = false;
                        stall_logout_deadline_ = 0;
                    }
                    std::size_t remaining = io.bytes;
                    while (remaining > 0 && tx_count_ != 0) {
                        auto& front = tx_ring_[tx_head_];
                        const std::size_t left = front.size - front.offset;
                        if (remaining >= left) {
                            remaining -= left;
                            front.offset = front.size;
                            if (const auto st = pop_tx(true, now_ms); !st.ok()) return st;
                        } else {
                            front.offset += remaining;
                            remaining = 0;
                        }
                    }
                    if (!io.status.ok()) {
                        handle_disconnect(io.status, now_ms);
                        return io.status;
                    }
                    if (io.would_block || io.bytes == 0) break;
                    continue;
                }
            }

            std::span<const char> slice;
            if (replay_due) {
                slice = {replay_buf_.data() + replay_offset_, replay_bytes_ - replay_offset_};
            } else if (q->ephemeral) {
                slice = {logon_tx_buf_.data() + q->offset, logon_tx_size_ - q->offset};
            } else {
                const auto* rec = journal_.find_record(q->record_id);
                if (rec == nullptr && q->record_id < journal_.lowest_retained_record_id())
                    return fail(Status{ErrorCode::history_unavailable});
                if (rec == nullptr || rec->payload.size() != q->size)
                    return fail(Status{ErrorCode::corrupt_store});
                slice = {rec->payload.data() + q->offset, q->size - q->offset};
            }

            const std::uint64_t w_t0 = (telemetry_slot_ != nullptr && config_.telemetry_stage_probes)
                ? telemetry_now_ns() : 0ULL;
            const auto io = transport_.write(slice);
            if (w_t0 != 0ULL && io.bytes != 0) {
                telemetry_slot_->record_stage(TelemetryStage::write, telemetry_now_ns() - w_t0);
            }
            if (io.bytes != 0) {
                last_byte_progress_ms_ = now_ms;
                stall_logout_attempted_ = false;
                stall_logout_deadline_ = 0;
                if (replay_due) replay_offset_ += io.bytes;
                else q->offset += io.bytes;
            }
            if (replay_due && replay_offset_ == replay_bytes_) {
                const auto wseq = replay_seq_;
                const auto wadv = replay_advance_;
                const auto wt = std::string_view{replay_msg_type_.data(), replay_msg_type_size_};
                detail::CoreEvent wev{};
                wev.kind = detail::CoreEventKind::outbound_written;
                wev.now_ms = now_ms;
                wev.sequence = wseq;
                wev.msg_type = wt;
                const auto wout = core_.process(wev);
                if (!wout.status.ok()) return fail(wout.status);
                if (wt == msg_type::sequence_reset) {
                    while (tx_count_ != 0) {
                        const auto& f = tx_ring_[tx_head_];
                        if (f.seq < wseq || f.seq >= wadv) break;
                        const auto ft = std::string_view{f.msg_type.data(), f.msg_type_size};
                        if (!detail::direct_is_admin(ft)) break;
                        detail::CoreEvent sk{};
                        sk.kind = detail::CoreEventKind::outbound_skipped;
                        sk.now_ms = now_ms;
                        sk.sequence = f.seq;
                        sk.msg_type = ft;
                        const auto sout = core_.process(sk);
                        if (const auto st = pop_tx(false, now_ms); !st.ok()) return st;
                        if (sout.action_count != 0 || !sout.status.ok()) {
                            if (const auto st = apply_actions(sout, nullptr, 0, 0, now_ms); !st.ok()) return st;
                        }
                    }
                } else if (tx_count_ != 0 && tx_ring_[tx_head_].seq == wseq) {
                    if (const auto st = pop_tx(false, now_ms); !st.ok()) return st;
                }
                replay_next_ = wadv;
                replay_prepared_ = false;
                replay_offset_ = replay_bytes_ = 0;
                if (replay_next_ > replay_end_) {
                    if (const auto st = finish_replay_range(); !st.ok()) return fail(st);
                }
            } else if (!replay_due && q->offset == q->size) {
                if (const auto st = pop_tx(true, now_ms); !st.ok()) return st;
            }
            if (!io.status.ok()) {
                handle_disconnect(io.status, now_ms);
                return io.status;
            }
            if (io.would_block || io.bytes == 0) break;
        }
        return Status::success();
    }

    [[nodiscard]] Status drain_rx_frames(std::uint64_t now_ms) noexcept {
        constexpr auto hints = detail::direct_schema_wire_hints<Schema>();
        while (rx_head_ < rx_tail_ && !fatal_ && !should_close_) {
            const std::string_view avail{rx_buf_.data() + rx_head_, rx_tail_ - rx_head_};
            FrameView fv{};
            const auto st = inspect_frame(avail, fv, hints,
                CodecLimits{config_.max_frame_bytes, config_.verify_checksum});
            if (st.code == ErrorCode::need_more) break;
            if (!st.ok()) return fail(st);
            rx_head_ += fv.consumed;
            if (const auto pst = process_inbound_frame(fv, now_ms, false); !pst.ok()) return pst;
        }
        if (rx_head_ == rx_tail_) { rx_head_ = 0; rx_tail_ = 0; }
        return Status::success();
    }

    [[nodiscard]] Status append_delivery(RecordId id, std::uint64_t seq) noexcept {
        if (journal_.has_delivery(id)) return Status::success();
        const auto app = journal_.append(RecordKind::delivery, seq, 0, id, {});
        if (!app.ok()) return app.status();
        update_retention_floor();
        return Status::success();
    }

    [[nodiscard]] std::uint64_t min_in_flight_outbound_seq() const noexcept {
        std::uint64_t bound = journal_.next_outbound_seq();
        if (tx_count_ != 0) {
            bound = std::min(bound, tx_ring_[tx_head_].seq);
        }
        if (replay_active_) {
            const std::uint64_t active_min = replay_prepared_
                ? std::min(replay_seq_, replay_next_) : replay_next_;
            if (active_min != 0) bound = std::min(bound, active_min);
        }
        for (std::size_t i = 0; i < replay_count_; ++i) {
            const auto& r = replay_ring_[(replay_head_ + i) % replay_ring_.size()];
            if (r.begin != 0) bound = std::min(bound, r.begin);
        }
        return bound;
    }

    void update_retention_floor() noexcept {
        if (config_.retain_outbound_messages == 0) {
            retention_deferred_ = false;
            return;
        }
        const auto latest = journal_.next_outbound_seq() == 0 ? 0 : journal_.next_outbound_seq() - 1;
        if (latest < config_.retain_outbound_messages) {
            retention_deferred_ = false;
            return;
        }
        const auto window_floor = latest - config_.retain_outbound_messages + 1;
        const auto ack_floor = peer_ack_seq_ == std::numeric_limits<std::uint64_t>::max()
            ? peer_ack_seq_ : peer_ack_seq_ + 1;
        const auto target_floor = std::min(window_floor, ack_floor);
        // Never allow retention to prune outbound records still queued in TX or replay!
        const auto in_flight_floor = min_in_flight_outbound_seq();
        retention_deferred_ = (in_flight_floor < target_floor);
        const auto floor = std::max(std::min(target_floor, in_flight_floor),
                                    journal_.lowest_retained_outbound_seq());
        if (floor > journal_.lowest_retained_outbound_seq()) {
            (void)journal_.set_retention_floor(floor, journal_.completed_inbound_record_id());
        }
    }

    [[nodiscard]] Status deliver_to_handler(const FlatMessageView& msg, DeliveryInfo info,
                                            std::uint64_t now_ms,
                                            std::string_view known_msg_type = {},
                                            std::size_t msg_type_offset = 0) noexcept {
        const std::uint64_t v_t0 = (telemetry_slot_ != nullptr && config_.telemetry_stage_probes)
            ? telemetry_now_ns() : 0ULL;
        const auto policy = config_.unexpected_field_policy;
        Status valid{};
        if (policy == UnexpectedFieldPolicy::reject) [[likely]] {
            valid = detail::direct_schema_validate<Schema>(msg, known_msg_type, msg_type_offset);
        } else {
            std::uint64_t unexpected_count = 0;
            valid = detail::direct_schema_validate<Schema>(
                msg, known_msg_type, msg_type_offset, policy,
                policy == UnexpectedFieldPolicy::ignore_with_metric ? &unexpected_count : nullptr);
            if (unexpected_count != 0) {
                unexpected_fields_ += unexpected_count;
                if (telemetry_slot_ != nullptr) {
                    telemetry_slot_->inc_unexpected_fields(unexpected_count);
                }
            }
        }
        if (v_t0 != 0ULL) {
            telemetry_slot_->record_stage(TelemetryStage::validate, telemetry_now_ns() - v_t0);
        }
        if (!valid.ok()) {
            // Never silently mark an unfinished recovered inbound record as delivered if
            // validation fails during recovery; fail-closed so the incomplete state remains diagnosable.
            if (info.is_recovery) return fail(valid);
            const auto tags = detail::direct_extract_tags(msg);
            const auto ref_type = tags.msg_type ? tags.msg_type->value : std::string_view{};
            std::array<detail::DirectAdminField, 5> rfields{};
            std::size_t rc = 0;
            rfields[rc++] = {tag::ref_seq_num, FieldKind::unsigned_integer, info.msg_seq_num, {}};
            if (valid.tag > 0)
                rfields[rc++] = {tag::ref_tag_id, FieldKind::unsigned_integer, static_cast<std::uint64_t>(valid.tag), {}};
            if (!ref_type.empty()) rfields[rc++] = {tag::ref_msg_type, FieldKind::string, 0, ref_type};
            rfields[rc++] = {tag::session_reject_reason, FieldKind::unsigned_integer,
                valid.code == ErrorCode::required_field_missing ? reject_reason::required_tag_missing :
                valid.code == ErrorCode::message_unhandled ? reject_reason::invalid_msg_type :
                reject_reason::value_is_incorrect, {}};
            rfields[rc++] = {tag::text, FieldKind::string, 0, valid.message()};
            if (const auto st = send_admin(msg_type::reject, {rfields.data(), rc}, now_ms); !st.ok())
                return fail(st);
            if constexpr (requires { handler_.on_error(valid); }) {
                try { handler_.on_error(valid); } catch (...) {}
            }
            const auto d = append_delivery(info.record_id, info.msg_seq_num);
            return d.ok() ? d : fail(d);
        }
        Status cb{};
        struct DeliveryScopeGuard {
            RecordId& id_slot;
            bool& rec_slot;
            std::size_t& ord_slot;
            RecordId prev_id;
            bool prev_rec;
            std::size_t prev_ord;
            ~DeliveryScopeGuard() {
                id_slot = prev_id;
                rec_slot = prev_rec;
                ord_slot = prev_ord;
            }
        } scope{active_delivery_id_, active_delivery_recovery_, active_delivery_send_ordinal_,
                active_delivery_id_, active_delivery_recovery_, active_delivery_send_ordinal_};
        active_delivery_id_ = info.record_id;
        active_delivery_recovery_ = info.is_recovery;
        active_delivery_send_ordinal_ = 0;
        try {
            if constexpr (requires { handler_.on_message(*this, msg, info); }) {
                cb = handler_.on_message(*this, msg, info);
            } else if constexpr (requires { handler_.on_message(msg, info); }) {
                cb = handler_.on_message(msg, info);
            }
        } catch (...) {
            cb = Status{ErrorCode::callback_failed};
        }
        const std::size_t accepted_replies = active_delivery_send_ordinal_;
        if (!cb.ok()) return fail(Status{ErrorCode::callback_failed, cb.tag, cb.offset});
        if (info.is_recovery && count_dependent_business_replies(info.record_id) > accepted_replies) {
            return fail(Status{ErrorCode::callback_failed});
        }
        if (telemetry_slot_ != nullptr) telemetry_slot_->inc_delivery();
        const auto d = append_delivery(info.record_id, info.msg_seq_num);
        return d.ok() ? d : fail(d);
    }

    [[nodiscard]] Status deliver_pending_recovery(std::uint64_t now_ms) noexcept {
        std::size_t idx = 0;
        while (idx < recovery_ids_.size()) {
            const RecordId id = recovery_ids_[idx++];
            if (journal_.has_delivery(id)) continue;
            const auto* rec = journal_.find_record(id);
            if (rec == nullptr || rec->kind != RecordKind::inbound) return fail(Status{ErrorCode::corrupt_store});
            FlatMessageView msg{};
            if (const auto st = parse_frame(rec->payload, msg); !st.ok()) return fail(st);
            const auto tags = detail::direct_extract_tags(msg);
            if (tags.msg_type == nullptr) return fail(Status{ErrorCode::corrupt_store, tag::msg_type});
            const auto mt = tags.msg_type->value;
            if (detail::direct_is_admin(mt)) {
                if (mt == msg_type::resend_request && tags.begin_seq && tags.end_seq) {
                    std::uint64_t b{}, e{};
                    if (!detail::direct_parse_u64(tags.begin_seq->value, b) ||
                        !detail::direct_parse_u64(tags.end_seq->value, e))
                        return fail(Status{ErrorCode::corrupt_store, tag::begin_seq_no});
                    if (const auto st = queue_replay(b, e, id); !st.ok()) return fail(st);
                } else if (mt == msg_type::test_request && tags.test_id) {
                    if (!has_dependent_outbound(id)) {
                        std::array<detail::DirectAdminField, 1> f{
                            detail::DirectAdminField{tag::test_req_id, FieldKind::string, 0, tags.test_id->value}};
                        if (const auto st = send_admin(msg_type::heartbeat, f, now_ms, id); !st.ok())
                            return fail(st);
                    }
                    if (const auto st = append_delivery(id, rec->seq); !st.ok()) return fail(st);
                } else {
                    if (const auto st = append_delivery(id, rec->seq); !st.ok()) return fail(st);
                }
            } else {
                if (const auto st = deliver_to_handler(msg, DeliveryInfo{id, rec->seq, true}, now_ms,
                        mt, tags.msg_type->offset); !st.ok())
                    return st;
            }
        }
        recovery_ids_.clear();
        return Status::success();
    }

    [[nodiscard]] Status apply_actions(const detail::CoreOutput& out,
                                       const FlatMessageView* delivery_msg,
                                       RecordId inbound_id, std::uint64_t inbound_seq,
                                       std::uint64_t now_ms,
                                       std::string_view known_msg_type = {},
                                       std::size_t msg_type_offset = 0) noexcept {
        if (!out.status.ok()) return fail(out.status);
        for (std::size_t i = 0; i < out.action_count; ++i) {
            const auto& a = out.actions[i];
            switch (a.kind) {
            case detail::CoreActionKind::send_logon:
                if (const auto st = send_logon(now_ms); !st.ok()) return fail(st);
                break;
            case detail::CoreActionKind::send_logout: {
                std::array<detail::DirectAdminField, 1> f{};
                std::size_t n = 0;
                if (!a.text.empty()) f[n++] = {tag::text, FieldKind::string, 0, a.text};
                if (const auto st = send_admin(msg_type::logout, {f.data(), n}, now_ms, inbound_id); !st.ok())
                    return fail(st);
                break;
            }
            case detail::CoreActionKind::send_heartbeat: {
                std::array<detail::DirectAdminField, 1> f{};
                std::size_t n = 0;
                if (!a.text.empty()) f[n++] = {tag::test_req_id, FieldKind::string, 0, a.text};
                if (const auto st = send_admin(msg_type::heartbeat, {f.data(), n}, now_ms, inbound_id); !st.ok())
                    return fail(st);
                break;
            }
            case detail::CoreActionKind::send_test_request: {
                std::array<detail::DirectAdminField, 1> f{{{tag::test_req_id, FieldKind::string, 0, a.text}}};
                if (const auto st = send_admin(msg_type::test_request, f, now_ms, inbound_id); !st.ok())
                    return fail(st);
                break;
            }
            case detail::CoreActionKind::send_resend_request: {
                std::array<detail::DirectAdminField, 2> f{
                    detail::DirectAdminField{tag::begin_seq_no, FieldKind::unsigned_integer, a.first, {}},
                    detail::DirectAdminField{tag::end_seq_no, FieldKind::unsigned_integer, a.last, {}},
                };
                if (const auto st = send_admin(msg_type::resend_request, f, now_ms, inbound_id); !st.ok())
                    return fail(st);
                break;
            }
            case detail::CoreActionKind::start_replay:
                if (const auto st = queue_replay(a.first, a.last, inbound_id); !st.ok()) return fail(st);
                break;
            case detail::CoreActionKind::become_active: {
                failover_.on_session_active();
                sync_telemetry_state();
                Status act{};
                try {
                    if constexpr (requires { handler_.on_active(*this); }) act = handler_.on_active(*this);
                    else if constexpr (requires { handler_.on_active(); }) act = handler_.on_active();
                } catch (...) { act = Status{ErrorCode::callback_failed}; }
                if (!act.ok()) return fail(Status{ErrorCode::callback_failed, act.tag, act.offset});
                if (const auto st = deliver_pending_recovery(now_ms); !st.ok()) return st;
                break;
            }
            case detail::CoreActionKind::deliver_message:
                if (delivery_msg == nullptr || inbound_id == 0) return fail(Status{ErrorCode::protocol_error});
                if (const auto st = deliver_to_handler(*delivery_msg,
                        DeliveryInfo{inbound_id, inbound_seq, false}, now_ms,
                        known_msg_type, msg_type_offset); !st.ok()) return st;
                break;
            case detail::CoreActionKind::close_transport:
                should_close_ = true;
                break;
            }
        }
        return Status::success();
    }

    [[nodiscard]] Result<std::size_t> encode_safe_inbound_logon(
        const detail::DirectInboundTags& tags, std::uint64_t wire_seq,
        bool poss_dup, bool orig_valid, std::optional<std::uint64_t> peer_ack) noexcept {
        std::array<detail::DirectAdminField, 4> fields{};
        std::size_t count = 0;
        std::uint64_t enc_val{}, hb_val{};
        if (tags.encrypt == nullptr || !detail::direct_parse_u64(tags.encrypt->value, enc_val))
            return Status{ErrorCode::corrupt_store, tag::encrypt_method};
        if (tags.hb_int == nullptr || !detail::direct_parse_u64(tags.hb_int->value, hb_val))
            return Status{ErrorCode::corrupt_store, tag::heart_bt_int};
        fields[count++] = {tag::encrypt_method, FieldKind::unsigned_integer, enc_val, {}};
        fields[count++] = {tag::heart_bt_int, FieldKind::unsigned_integer, hb_val, {}};
        if (tags.reset_flag != nullptr) {
            fields[count++] = {tag::reset_seq_num_flag, FieldKind::character,
                static_cast<unsigned char>(tags.reset_flag->value.empty() ? 0 : tags.reset_flag->value[0]), {}};
        }
        if (config_.version == FixVersion::fixt11) {
            if (tags.appl_ver == nullptr) return Status{ErrorCode::corrupt_store, tag::default_appl_ver_id};
            fields[count++] = {tag::default_appl_ver_id, FieldKind::string, 0, tags.appl_ver->value};
        }
        detail::DirectAdminStorage stg{};
        if (const auto st = detail::direct_build_admin(msg_type::logon, {fields.data(), count}, stg, admin_arena_); !st.ok()) {
            detail::direct_secure_clear(admin_arena_);
            return st;
        }
        SessionHeader hdr{config_.remote_comp_id, config_.local_comp_id, wire_seq,
            tags.sending_time->value, poss_dup,
            (poss_dup && orig_valid && tags.orig_time != nullptr) ? tags.orig_time->value : std::string_view{}};
        hdr.version = config_.version;
        hdr.last_msg_seq_num_processed = peer_ack;
        const auto encoded = encode_fix(stg.view, hdr, encode_buf_);
        detail::direct_secure_clear(admin_arena_);
        if (!encoded.ok()) return encoded.status();
        return encoded.value();
    }

    [[nodiscard]] Status process_inbound_frame(const FrameView& fv, std::uint64_t now_ms,
                                               bool is_deferred) noexcept {
        constexpr auto hints = detail::direct_schema_wire_hints<Schema>();
        FlatMessageView message{};
        const std::uint64_t p_t0 = (telemetry_slot_ != nullptr && config_.telemetry_stage_probes)
            ? telemetry_now_ns() : 0ULL;
        if (const auto st = parse_flat_verified(fv, parse_fields_, message, hints); !st.ok()) return fail(st);
        if (p_t0 != 0ULL) {
            telemetry_slot_->record_stage(TelemetryStage::parse, telemetry_now_ns() - p_t0);
        }
        message.bind_index(&parse_index_);
        const auto tags = detail::direct_extract_tags(message);
        if (tags.dup_tag != 0) return fail(Status{ErrorCode::duplicate_field, static_cast<int>(tags.dup_tag)});
        if (tags.begin == nullptr || tags.begin->value != begin_string(config_.version))
            return fail(Status{ErrorCode::unsupported_protocol, tag::begin_string});
        if (tags.msg_type == nullptr || tags.msg_type->value.empty())
            return fail(Status{ErrorCode::invalid_header, tag::msg_type});
        std::uint64_t sequence{};
        if (tags.seq == nullptr || !detail::direct_parse_u64(tags.seq->value, sequence) || sequence == 0)
            return fail(Status{ErrorCode::invalid_header, tag::msg_seq_num});
        if (tags.sender == nullptr || tags.sender->value != config_.remote_comp_id)
            return fail(Status{ErrorCode::identity_mismatch, tag::sender_comp_id});
        if (tags.target == nullptr || tags.target->value != config_.local_comp_id)
            return fail(Status{ErrorCode::identity_mismatch, tag::target_comp_id});
        if (tags.sending_time == nullptr || !detail::is_valid_utc_timestamp(tags.sending_time->value))
            return fail(Status{ErrorCode::invalid_timestamp, tag::sending_time});

        const bool poss_dup = tags.poss_dup != nullptr && tags.poss_dup->value == "Y";
        if (tags.poss_dup != nullptr && tags.poss_dup->value != "Y" && tags.poss_dup->value != "N")
            return fail(Status{ErrorCode::invalid_value, tag::poss_dup_flag});
        const bool orig_valid = tags.orig_time != nullptr && detail::is_valid_utc_timestamp(tags.orig_time->value);
        if (poss_dup && (!orig_valid || detail::compare_utc_timestamps(tags.orig_time->value, tags.sending_time->value) > 0))
            return fail(Status{ErrorCode::invalid_timestamp, tag::orig_sending_time});
        if (!poss_dup && tags.orig_time != nullptr)
            return fail(Status{ErrorCode::invalid_header, tag::orig_sending_time});

        const auto msg_type = tags.msg_type->value;
        std::string_view test_id{};
        if (msg_type == msg_type::test_request || msg_type == msg_type::heartbeat) {
            if (tags.test_id != nullptr) test_id = tags.test_id->value;
            if (msg_type == msg_type::test_request && tags.test_id == nullptr)
                return fail(Status{ErrorCode::missing_field, tag::test_req_id});
        }

        if (config_.check_sending_time) {
            std::int64_t sent_ms{}, cur_ms{};
            if (!detail::direct_parse_utc_ms(tags.sending_time->value, sent_ms))
                return fail(Status{ErrorCode::invalid_timestamp, tag::sending_time});
            if (!wall_time_ms_valid_) {
                if (!detail::direct_parse_utc_ms(wall_time_, cached_wall_ms_))
                    return fail(Status{ErrorCode::invalid_timestamp, tag::sending_time});
                wall_time_ms_valid_ = true;
            }
            cur_ms = cached_wall_ms_;
            const auto diff = sent_ms >= cur_ms
                ? static_cast<std::uint64_t>(sent_ms - cur_ms)
                : static_cast<std::uint64_t>(cur_ms - sent_ms);
            if (diff > config_.max_sending_time_skew_ms) {
                return logout("SendingTime accuracy problem", now_ms, wall_time_);
            }
        }

        std::uint64_t peer_hb_ms = 0;
        std::uint64_t resend_begin = 0, resend_end = 0;
        std::uint64_t next_inbound = 0;
        std::uint64_t effective_seq = sequence;
        bool seq_reset_ev = false, gapfill_ev = false, dup_gapfill = false;
        bool noop_seq_reset = false, reject_seq_reset = false;
        std::string_view logout_text{};

        if (msg_type == msg_type::logon) {
            if (tags.encrypt == nullptr || tags.encrypt->value != encrypt_method_none_str)
                return fail(Status{ErrorCode::protocol_error, tag::encrypt_method});
            std::uint64_t secs{};
            if (tags.hb_int == nullptr || !detail::direct_parse_u64(tags.hb_int->value, secs) ||
                secs == 0 || secs > std::numeric_limits<std::uint32_t>::max() / 1000U)
                return fail(Status{ErrorCode::protocol_error, tag::heart_bt_int});
            peer_hb_ms = secs * 1000;
            if (config_.version == FixVersion::fixt11 &&
                (tags.appl_ver == nullptr || tags.appl_ver->value != config_.default_appl_ver_id))
                return fail(Status{ErrorCode::unsupported_protocol, tag::default_appl_ver_id});
            const bool peer_reset = tags.reset_flag != nullptr && tags.reset_flag->value == "Y";
            if (tags.reset_flag != nullptr && tags.reset_flag->value != "Y" && tags.reset_flag->value != "N")
                return fail(Status{ErrorCode::invalid_value, tag::reset_seq_num_flag});
            if (peer_reset && (!config_.allow_sequence_reset || !local_epoch_reset_started_ || sequence != 1))
                return fail(Status{ErrorCode::sequence_reset_denied, tag::reset_seq_num_flag});
            const bool expected_reset = config_.request_sequence_reset || one_shot_reset_active_;
            if (!peer_reset && expected_reset && core_.state() == SessionState::awaiting_logon)
                return fail(Status{ErrorCode::sequence_reset_denied, tag::reset_seq_num_flag});
        } else if (msg_type == msg_type::resend_request) {
            if (tags.begin_seq == nullptr || !detail::direct_parse_u64(tags.begin_seq->value, resend_begin) ||
                tags.end_seq == nullptr || !detail::direct_parse_u64(tags.end_seq->value, resend_end) ||
                resend_begin == 0 || (resend_end != 0 && resend_end < resend_begin))
                return fail(Status{ErrorCode::protocol_error, tag::begin_seq_no});
        } else if (msg_type == msg_type::sequence_reset) {
            if (tags.gap_fill != nullptr && tags.gap_fill->value != "Y" && tags.gap_fill->value != "N")
                return fail(Status{ErrorCode::invalid_value, tag::gap_fill_flag});
            const bool is_gf = tags.gap_fill != nullptr && tags.gap_fill->value == "Y";
            if (tags.new_seq == nullptr || !detail::direct_parse_u64(tags.new_seq->value, next_inbound))
                return fail(Status{ErrorCode::protocol_error, tag::new_seq_no});
            if (!is_gf) {
                if (next_inbound > core_.next_inbound()) {
                    seq_reset_ev = true;
                    effective_seq = core_.next_inbound();
                } else if (next_inbound == core_.next_inbound() || poss_dup) {
                    noop_seq_reset = true;
                } else {
                    reject_seq_reset = true;
                }
            } else {
                if (!poss_dup || !orig_valid || next_inbound <= sequence)
                    return fail(Status{ErrorCode::protocol_error, tag::gap_fill_flag});
                if (next_inbound <= core_.next_inbound()) dup_gapfill = true;
                else {
                    gapfill_ev = true;
                    if (sequence < core_.next_inbound()) effective_seq = core_.next_inbound();
                }
            }
        } else if (msg_type == msg_type::logout) {
            if (tags.text != nullptr) logout_text = tags.text->value;
        }

        std::optional<std::uint64_t> ack_cand{};
        if (tags.peer_ack != nullptr) {
            std::uint64_t av{};
            if (!detail::direct_parse_u64(tags.peer_ack->value, av))
                return fail(Status{ErrorCode::invalid_value, tag::last_msg_seq_num_processed});
            ack_cand = av;
        }

        if (dup_gapfill) {
            detail::CoreEvent dc{};
            dc.kind = detail::CoreEventKind::inbound_candidate;
            dc.now_ms = now_ms;
            dc.sequence = sequence;
            dc.msg_type = msg_type;
            dc.poss_dup = poss_dup;
            dc.orig_sending_time_valid = orig_valid;
            const auto dd = core_.process(dc);
            if (!dd.status.ok()) return fail(dd.status);
            return Status::success();
        }

        if (noop_seq_reset || reject_seq_reset) {
            detail::CoreEvent rc{};
            rc.kind = detail::CoreEventKind::inbound_candidate;
            rc.now_ms = now_ms;
            rc.sequence = core_.next_inbound();
            rc.msg_type = msg_type;
            rc.poss_dup = poss_dup;
            rc.orig_sending_time_valid = orig_valid;
            const auto rd = core_.process(rc);
            if (!rd.status.ok()) return fail(rd.status);
            if (!poss_dup && ack_cand) {
                const auto max_out = journal_.next_outbound_seq() == 0 ? 0 : journal_.next_outbound_seq() - 1;
                if (*ack_cand < peer_ack_seq_ || *ack_cand > max_out)
                    return fail(Status{ErrorCode::protocol_error, tag::last_msg_seq_num_processed});
            }
            if (reject_seq_reset) {
                const Status reject_err{ErrorCode::invalid_value, tag::new_seq_no};
                std::array<detail::DirectAdminField, 5> rfields{
                    detail::DirectAdminField{tag::ref_seq_num, FieldKind::unsigned_integer, sequence, {}},
                    detail::DirectAdminField{tag::ref_tag_id, FieldKind::unsigned_integer, tag::new_seq_no, {}},
                    detail::DirectAdminField{tag::ref_msg_type, FieldKind::string, 0, msg_type::sequence_reset},
                    detail::DirectAdminField{tag::session_reject_reason, FieldKind::unsigned_integer,
                        reject_reason::value_is_incorrect, {}},
                    detail::DirectAdminField{tag::text, FieldKind::string, 0, reject_err.message()},
                };
                if (const auto st = send_admin(msg_type::reject, rfields, now_ms); !st.ok()) return fail(st);
                if constexpr (requires { handler_.on_error(reject_err); }) {
                    try { handler_.on_error(reject_err); } catch (...) {}
                }
            }
            return Status::success();
        }

        bool deferred_logon = is_deferred;
        if (msg_type == msg_type::logon && !deferred_logon && core_.state() == SessionState::awaiting_logon &&
            sequence > core_.next_inbound()) {
            if (deferred_logon_pending_) {
                if (sequence == deferred_logon_seq_ &&
                    fv.bytes == std::string_view{deferred_logon_buf_.data(), deferred_logon_size_})
                    return Status::success();
                return fail(Status{ErrorCode::protocol_error, tag::msg_type});
            }
            if (fv.bytes.size() > deferred_logon_buf_.size()) return fail(Status{ErrorCode::too_large});
            std::memcpy(deferred_logon_buf_.data(), fv.bytes.data(), fv.bytes.size());
            deferred_logon_size_ = fv.bytes.size();
            deferred_logon_seq_ = sequence;
            deferred_logon_pending_ = true;

            detail::CoreEvent cand{};
            cand.kind = detail::CoreEventKind::inbound_candidate;
            cand.now_ms = now_ms;
            cand.sequence = effective_seq;
            cand.last = sequence - 1;
            cand.msg_type = msg_type;
            cand.poss_dup = poss_dup;
            cand.orig_sending_time_valid = orig_valid;
            const auto gap_out = core_.process(cand);
            if (!gap_out.status.ok() || gap_out.inbound != detail::InboundDisposition::gap)
                return fail(gap_out.status.ok() ? Status{ErrorCode::protocol_error, tag::msg_seq_num} : gap_out.status);

            detail::CoreEvent ooo{};
            ooo.kind = detail::CoreEventKind::out_of_order_logon;
            ooo.now_ms = now_ms;
            ooo.first = peer_hb_ms;
            const auto lout = core_.process(ooo);
            if (!lout.status.ok()) return fail(lout.status);
            if (const auto st = apply_actions(lout, nullptr, 0, 0, now_ms); !st.ok()) return st;
            return apply_actions(gap_out, nullptr, 0, 0, now_ms);
        }
        if (msg_type == msg_type::logon && deferred_logon_pending_ && sequence == deferred_logon_seq_) {
            deferred_logon = true;
        }

        detail::CoreEvent cand{};
        cand.kind = detail::CoreEventKind::inbound_candidate;
        cand.now_ms = now_ms;
        cand.sequence = effective_seq;
        cand.msg_type = msg_type;
        cand.poss_dup = poss_dup;
        cand.orig_sending_time_valid = orig_valid;
        cand.is_deferred_logon = deferred_logon;
        if (msg_type == msg_type::resend_request) { cand.first = resend_begin; cand.last = resend_end; }
        const auto disp = core_.process(cand);
        if (!disp.status.ok()) return fail(disp.status);
        if (disp.inbound == detail::InboundDisposition::duplicate) return Status::success();

        if (ack_cand) {
            const auto max_out = journal_.next_outbound_seq() == 0 ? 0 : journal_.next_outbound_seq() - 1;
            if (*ack_cand < peer_ack_seq_ || *ack_cand > max_out)
                return fail(Status{ErrorCode::protocol_error, tag::last_msg_seq_num_processed});
        }

        if (disp.inbound == detail::InboundDisposition::gap) {
            return apply_actions(disp, nullptr, 0, 0, now_ms);
        }
        if (disp.inbound != detail::InboundDisposition::accept)
            return fail(Status{ErrorCode::protocol_error});

        std::string_view stored_payload = fv.bytes;
        if (msg_type == msg_type::logon) {
            const auto safe = encode_safe_inbound_logon(tags, sequence, poss_dup, orig_valid, ack_cand);
            if (!safe.ok()) return fail(safe.status());
            stored_payload = {encode_buf_.data(), safe.value()};
        }
        const std::uint64_t persisted_next = (seq_reset_ev || gapfill_ev) ? next_inbound : 0;
        const std::uint64_t ij_t0 = (telemetry_slot_ != nullptr && config_.telemetry_stage_probes)
            ? telemetry_now_ns() : 0ULL;
        const auto appended = journal_.append(RecordKind::inbound, effective_seq, 0, 0,
                                              stored_payload, persisted_next);
        if (ij_t0 != 0ULL) {
            telemetry_slot_->record_stage(TelemetryStage::journal, telemetry_now_ns() - ij_t0);
        }
        if (!appended.ok()) return fail(appended.status());
        if (telemetry_slot_ != nullptr) {
            telemetry_slot_->inc_inbound(fv.bytes.size());
        }
        const RecordId inbound_id = appended.value().id;
        if (ack_cand) peer_ack_seq_ = *ack_cand;

        // If a business message arrives during awaiting_logon (gap recovery before deferred Logon),
        // defer delivery to recovery_ids_ so handler sees it in order upon becoming active.
        if (!detail::direct_is_admin(msg_type) && core_.state() == SessionState::awaiting_logon) {
            recovery_ids_.push_back(inbound_id);
        } else if (msg_type == msg_type::resend_request) {
            recovery_ids_.push_back(inbound_id);
        }

        const bool merged_gf_covered_logon =
            (seq_reset_ev || gapfill_ev) && deferred_logon_pending_ &&
            appended.value().next_seq > deferred_logon_seq_;

        detail::CoreEvent comm{};
        comm.kind = (seq_reset_ev || gapfill_ev)
            ? detail::CoreEventKind::sequence_reset_committed
            : detail::CoreEventKind::inbound_committed;
        comm.now_ms = now_ms;
        comm.sequence = effective_seq;
        comm.next_sequence = appended.value().next_seq;
        comm.msg_type = msg_type;
        comm.first = msg_type == msg_type::resend_request ? resend_begin : peer_hb_ms;
        comm.last = resend_end;
        comm.text = (msg_type == msg_type::test_request || msg_type == msg_type::heartbeat)
            ? test_id : logout_text;
        comm.poss_dup = poss_dup;
        comm.orig_sending_time_valid = orig_valid;
        comm.is_deferred_logon = deferred_logon || merged_gf_covered_logon;
        const auto actions = core_.process(comm);
        if (!actions.status.ok()) return fail(actions.status);

        if (const auto st = apply_actions(actions, detail::direct_is_admin(msg_type) ? nullptr : &message,
                                          inbound_id, effective_seq, now_ms,
                                          msg_type, tags.msg_type->offset); !st.ok()) return st;

        if (detail::direct_is_admin(msg_type) && msg_type != msg_type::resend_request) {
            if (const auto st = append_delivery(inbound_id, effective_seq); !st.ok()) return fail(st);
        }
        if (msg_type == msg_type::reject) {
            if constexpr (requires { handler_.on_error(Status{ErrorCode::peer_rejected}); }) {
                try { handler_.on_error(Status{ErrorCode::peer_rejected}); } catch (...) {}
            }
        }
        update_retention_floor();
        sync_telemetry_state();

        if (deferred_logon || merged_gf_covered_logon) {
            detail::direct_secure_clear(deferred_logon_buf_);
            deferred_logon_pending_ = false;
            deferred_logon_size_ = 0;
            deferred_logon_seq_ = 0;
        } else if (deferred_logon_pending_ && core_.next_inbound() == deferred_logon_seq_) {
            FrameView dfv{std::string_view{deferred_logon_buf_.data(), deferred_logon_size_},
                          deferred_logon_size_};
            const auto dst = process_inbound_frame(dfv, now_ms, true);
            detail::direct_secure_clear(deferred_logon_buf_);
            deferred_logon_pending_ = false;
            deferred_logon_size_ = 0;
            deferred_logon_seq_ = 0;
            if (!dst.ok()) return dst;
        }
        return Status::success();
    }

    DirectInitiatorConfig config_;
    Handler handler_;
    Transport transport_;
    Journal journal_;
    std::shared_ptr<TelemetryWriter> telemetry_writer_{};
    TelemetrySessionSlot* telemetry_slot_{nullptr};
    detail::SessionCore core_;
    detail::FastUtcClock clock_{};
    detail::StaticSessionPrefix session_prefix_{};
    detail::FailoverController failover_{};

    std::vector<char> rx_buf_;
    std::size_t rx_head_{};
    std::size_t rx_tail_{};

    std::vector<char> encode_buf_;
    std::vector<char> replay_buf_;
    std::vector<char> deferred_logon_buf_;
    std::vector<char> logon_tx_buf_;
    std::vector<char> admin_arena_;
    std::vector<FieldView> parse_fields_;
    std::vector<FieldView> aux_parse_fields_;
    detail::ParsedFieldIndex parse_index_{};
    std::vector<TxEntry> tx_ring_;
    std::vector<ReplayRange> replay_ring_;
    std::vector<RecordId> recovery_ids_;

    std::size_t tx_head_{};
    std::size_t tx_count_{};
    std::size_t replay_head_{};
    std::size_t replay_count_{};
    bool replay_active_{};
    bool replay_prepared_{};
    std::uint64_t replay_origin_begin_{};
    std::uint64_t replay_next_{};
    std::uint64_t replay_end_{};
    std::uint64_t replay_seq_{};
    std::uint64_t replay_advance_{};
    RecordId replay_dep_{};
    std::vector<RecordId> replay_merged_deps_{};
    bool retention_deferred_{};
    std::size_t replay_bytes_{};
    std::size_t replay_offset_{};
    std::array<char, 32> replay_msg_type_{};
    std::uint8_t replay_msg_type_size_{};

    std::size_t logon_tx_size_{};
    std::size_t deferred_logon_size_{};
    std::uint64_t deferred_logon_seq_{};
    bool deferred_logon_pending_{};
    bool local_epoch_reset_started_{};
    bool one_shot_sequence_reset_{};
    bool one_shot_reset_active_{};
    std::string one_shot_reset_reason_{};
    std::uint64_t unexpected_fields_{};

    std::array<char, 32> wall_time_buf_{};
    std::size_t wall_time_size_{};
    std::string_view wall_time_{};
    std::int64_t cached_wall_ms_{};
    bool wall_time_ms_valid_{};
    std::uint64_t now_ms_{};
    std::uint64_t peer_ack_seq_{};
    std::uint64_t connect_deadline_{};
    std::uint64_t last_byte_progress_ms_{};
    std::uint64_t stall_logout_deadline_{};
    bool write_timer_active_{};
    bool stall_logout_attempted_{};

    RecordId active_delivery_id_{};
    bool active_delivery_recovery_{};
    std::size_t active_delivery_send_ordinal_{};

    bool should_close_{};
    bool fatal_{};
    Status last_status_{};
    Status recovery_status_{};
};

} // namespace nimblefix
