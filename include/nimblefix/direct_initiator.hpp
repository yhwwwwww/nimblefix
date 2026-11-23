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
#include <nimblefix/detail/session_core.hpp>
#include <nimblefix/execution.hpp>
#include <nimblefix/journal.hpp>
#include <nimblefix/schema.hpp>
#include <nimblefix/session.hpp>
#include <nimblefix/settings.hpp>
#include <nimblefix/status.hpp>
#include <nimblefix/tcp_transport.hpp>

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
    std::string default_appl_ver_id{"9"};
    std::string username{};
    std::string password{};
    bool request_sequence_reset{};
    bool allow_sequence_reset{};
    bool emit_last_processed_seq{true};
    bool check_sending_time{};
    std::uint64_t max_sending_time_skew_ms{120'000};
};

// Default no-op handler for tests or send-only initiators.
struct NullInitiatorHandler {
    [[nodiscard]] Status on_message(const FlatMessageView&, const DeliveryInfo&) noexcept {
        return Status::success();
    }
};

namespace detail {

// Fast cached UTC wall clock ("YYYYMMDD-HH:MM:SS.mmm") + monotonic milliseconds.
// Calls gmtime_r at most once per whole second; hot-path calls update only 3 ms digits.
class FastUtcClock {
public:
    [[nodiscard]] std::uint64_t now_ms() const noexcept {
        timespec ts{};
        ::clock_gettime(CLOCK_MONOTONIC, &ts);
        return static_cast<std::uint64_t>(ts.tv_sec) * 1000ULL +
               static_cast<std::uint64_t>(ts.tv_nsec / 1'000'000L);
    }

    [[nodiscard]] std::string_view wall_time() noexcept {
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
        const auto ms = static_cast<unsigned>(ts.tv_nsec / 1'000'000L);
        buf_[18] = static_cast<char>('0' + (ms / 100U) % 10U);
        buf_[19] = static_cast<char>('0' + (ms / 10U) % 10U);
        buf_[20] = static_cast<char>('0' + ms % 10U);
        return {buf_.data(), 21};
    }

private:
    static void write_2(char* out, unsigned v) noexcept {
        out[0] = static_cast<char>('0' + (v / 10U) % 10U);
        out[1] = static_cast<char>('0' + v % 10U);
    }
    static void write_4(char* out, unsigned v) noexcept {
        out[0] = static_cast<char>('0' + (v / 1000U) % 10U);
        out[1] = static_cast<char>('0' + (v / 100U) % 10U);
        out[2] = static_cast<char>('0' + (v / 10U) % 10U);
        out[3] = static_cast<char>('0' + v % 10U);
    }

    std::time_t cached_sec_{-1};
    std::array<char, 24> buf_{};
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
    return t == "A" || t == "0" || t == "1" || t == "2" || t == "3" || t == "4" || t == "5";
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
[[nodiscard]] inline Status direct_schema_validate(const FlatMessageView& message,
                                                   std::string_view known_msg_type = {},
                                                   std::size_t msg_type_offset = 0) noexcept {
    if constexpr (requires { Schema::validate(message); }) return Schema::validate(message);
    else if constexpr (requires { Schema::descriptor(); }) {
        if (!known_msg_type.empty()) {
            return validate_schema_message_for_type(Schema::descriptor(), known_msg_type,
                msg_type_offset, message.fields(), ValidationLevel::strict);
        }
        return validate_schema_message(Schema::descriptor(), message, ValidationLevel::strict);
    } else return Status::success();
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
        return Status{ErrorCode::invalid_value, 35};
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
        case 8: put(t.begin, f); break;
        case 35: put(t.msg_type, f); break;
        case 34: put(t.seq, f); break;
        case 49: put(t.sender, f); break;
        case 56: put(t.target, f); break;
        case 52: put(t.sending_time, f); break;
        case 43: put(t.poss_dup, f); break;
        case 122: put(t.orig_time, f); break;
        case 112: put(t.test_id, f); break;
        case 369: put(t.peer_ack, f); break;
        case 7: put(t.begin_seq, f); break;
        case 16: put(t.end_seq, f); break;
        case 36: put(t.new_seq, f); break;
        case 123: put(t.gap_fill, f); break;
        case 98: put(t.encrypt, f); break;
        case 108: put(t.hb_int, f); break;
        case 141: put(t.reset_flag, f); break;
        case 1137: put(t.appl_ver, f); break;
        case 58: put(t.text, f); break;
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
            (config.request_sequence_reset && !config.allow_sequence_reset)) {
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

        DirectInitiator init(std::move(config), std::move(handler),
                             std::move(transport), std::move(opened).value());
        if (!init.recovery_status_.ok()) return init.recovery_status_;
        return init;
    } catch (const std::bad_alloc&) {
        return Status{ErrorCode::capacity_exceeded};
    } catch (...) {
        return Status{ErrorCode::io_error};
    }

    // Initiates nonblocking TCP connection. Call poll_once() to complete handshake & I/O.
    [[nodiscard]] Status connect() noexcept {
        return connect(clock_.now_ms(), clock_.wall_time());
    }

    [[nodiscard]] Status connect(std::uint64_t now_ms, std::string_view wall_time) noexcept {
        if (fatal_) return last_status_.ok() ? Status{ErrorCode::engine_stopped} : last_status_;
        if (transport_.valid()) return Status{ErrorCode::not_ready};
        const auto st = transport_.connect(config_.host, config_.port);
        if (!st.ok()) return st;
        if (!transport_.is_connecting()) {
            return on_transport_connected(now_ms, wall_time);
        }
        return Status::success();
    }

    // Single nonblocking iteration: finish connect -> read & dispatch frames ->
    // flush pending outbound/replay -> process heartbeat/timeout timers.
    // Performs zero heap allocations, zero mutex locks, and zero fsync on steady state.
    [[nodiscard]] Status poll_once() noexcept {
        return poll_once(clock_.now_ms(), clock_.wall_time());
    }

    [[nodiscard]] Status poll_once(std::uint64_t now_ms, std::string_view wall_time) noexcept {
        if (fatal_) return last_status_.ok() ? Status{ErrorCode::engine_stopped} : last_status_;
        if (const auto ts = set_wall_time(wall_time); !ts.ok()) return ts;
        now_ms_ = now_ms;

        if (!transport_.valid()) return Status{ErrorCode::not_ready};

        if (transport_.is_connecting()) {
            const auto fc = transport_.finish_connect();
            if (fc.code == ErrorCode::need_more) return Status::success();
            if (!fc.ok()) {
                handle_disconnect(fc, now_ms);
                return fc;
            }
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
            const auto io = transport_.read(std::span<char>{rx_buf_.data() + rx_tail_, rx_buf_.size() - rx_tail_});
            if (io.bytes != 0) {
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

        // 4. Process SessionCore heartbeat / TestRequest / Logon / Logout timers.
        detail::CoreEvent tick{};
        tick.kind = detail::CoreEventKind::tick;
        tick.now_ms = now_ms;
        tick.has_pending_tx = wants_write();
        const auto out = core_.process(tick);
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
        return send(message, clock_.now_ms(), clock_.wall_time());
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
        return send_view(message.view(), now_ms, wall_time);
    }

    // Off-hot-path journal maintenance (checkpoint flush & spare segment refill).
    // Call periodically when idle (never required inside send()).
    [[nodiscard]] Status poll_maintenance() noexcept {
        if (!journal_.needs_maintenance()) return Status::success();
        return journal_.poll_maintenance();
    }

    [[nodiscard]] Status logout(std::string_view reason = {}) noexcept {
        return logout(reason, clock_.now_ms(), clock_.wall_time());
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
    [[nodiscard]] std::uint64_t next_deadline_ms() const noexcept { return core_.next_deadline(); }
    [[nodiscard]] std::uint64_t next_inbound_seq() const noexcept { return core_.next_inbound(); }
    [[nodiscard]] std::uint64_t next_outbound_seq() const noexcept { return core_.next_outbound(); }
    [[nodiscard]] std::size_t pending_outbound() const noexcept {
        return tx_count_ + replay_count_ + (replay_active_ ? 1U : 0U);
    }
    [[nodiscard]] bool needs_maintenance() const noexcept { return journal_.needs_maintenance(); }
    [[nodiscard]] const Journal& journal() const noexcept { return journal_; }
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
    };

    DirectInitiator(DirectInitiatorConfig cfg, Handler h, Transport tr, Journal j)
        : config_(std::move(cfg)),
          handler_(std::move(h)),
          transport_(std::move(tr)),
          journal_(std::move(j)),
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
          tx_ring_(config_.max_pending_messages),
          replay_ring_(config_.max_pending_messages),
          recovery_ids_() {
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
                if (tags.dup_tag == 369) { recovery_status_ = Status{ErrorCode::corrupt_store, 369}; break; }
                if (tags.peer_ack != nullptr) {
                    std::uint64_t val{};
                    if (!detail::direct_parse_u64(tags.peer_ack->value, val) || val < scanned_ack ||
                        (rec.epoch == current_epoch && val > max_out)) {
                        recovery_status_ = Status{ErrorCode::corrupt_store, 369};
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
    }

    [[nodiscard]] Status set_wall_time(std::string_view v) noexcept {
        if (wall_time_size_ != 0 && v == std::string_view{wall_time_buf_.data(), wall_time_size_}) {
            wall_time_ = {wall_time_buf_.data(), wall_time_size_};
            return Status::success();
        }
        if (!detail::is_valid_utc_timestamp(v) || v.size() > wall_time_buf_.size())
            return Status{ErrorCode::invalid_timestamp, 52};
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
        if (const auto st = inspect_frame(bytes, fv, hints, CodecLimits{config_.max_frame_bytes}); !st.ok())
            return st;
        if (fv.consumed != bytes.size()) return Status{ErrorCode::invalid_frame};
        return parse_flat_verified(fv, parse_fields_, out, hints);
    }

    [[nodiscard]] Status fail(Status err) noexcept {
        if (fatal_) return last_status_.ok() ? err : last_status_;
        if (err.ok()) err = Status{ErrorCode::invalid_value};
        last_status_ = err;
        fatal_ = true;
        should_close_ = true;
        detail::direct_secure_clear(logon_tx_buf_);
        logon_tx_size_ = 0;
        detail::direct_secure_clear(deferred_logon_buf_);
        deferred_logon_size_ = 0;
        if constexpr (requires { handler_.on_error(err); }) {
            try { handler_.on_error(err); } catch (...) {}
        }
        return last_status_;
    }

    void handle_disconnect(Status reason, std::uint64_t now_ms) noexcept {
        transport_.close();
        rx_head_ = rx_tail_ = 0;
        tx_head_ = tx_count_ = 0;
        replay_head_ = replay_count_ = 0;
        replay_active_ = replay_prepared_ = false;
        replay_offset_ = replay_bytes_ = 0;
        logon_tx_size_ = 0;
        deferred_logon_size_ = 0;
        deferred_logon_seq_ = 0;
        deferred_logon_pending_ = false;
        local_epoch_reset_started_ = false;
        should_close_ = false;
        detail::direct_secure_clear(logon_tx_buf_);
        detail::direct_secure_clear(deferred_logon_buf_);

        detail::CoreEvent ev{};
        ev.kind = detail::CoreEventKind::disconnected;
        ev.now_ms = now_ms;
        (void)core_.process(ev);
        if constexpr (requires { handler_.on_disconnected(reason); }) {
            try { handler_.on_disconnected(reason); } catch (...) {}
        }
    }

    [[nodiscard]] Status on_transport_connected(std::uint64_t now_ms, std::string_view wt) noexcept {
        if (const auto ts = set_wall_time(wt); !ts.ok()) return ts;
        now_ms_ = now_ms;
        last_status_ = Status::success();
        if (config_.request_sequence_reset && !local_epoch_reset_started_) {
            const auto rs = journal_.begin_epoch("local Logon ResetSeqNumFlag=Y");
            if (!rs.ok()) return fail(rs);
            detail::CoreEvent rc{};
            rc.kind = detail::CoreEventKind::reset_epoch;
            const auto rout = core_.process(rc);
            if (!rout.status.ok()) return fail(rout.status);
            peer_ack_seq_ = 0;
            local_epoch_reset_started_ = true;
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

    [[nodiscard]] Status enqueue_tx(RecordId id, std::size_t size, std::uint64_t seq,
                                    std::string_view msg_type, bool ephemeral = false) noexcept {
        if (tx_count_ >= tx_ring_.size() || msg_type.size() > 32) return Status{ErrorCode::queue_full};
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
        return Status::success();
    }

    [[nodiscard]] Result<std::uint64_t> send_view(detail::MessageView msg,
                                                  std::uint64_t now_ms,
                                                  std::string_view wt) noexcept {
        if (fatal_) return last_status_.ok() ? Status{ErrorCode::engine_stopped} : last_status_;
        if (!is_active()) return Status{ErrorCode::not_ready};
        if (msg.msg_type.empty() || detail::direct_is_admin(msg.msg_type))
            return Status{ErrorCode::managed_field, 35};
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
        auto reserved = journal_.reserve(RecordKind::outbound, seq, seq, 0, reserve_bound);
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
            const auto appended = journal_.append(RecordKind::outbound, seq, seq, 0,
                std::string_view{encode_buf_.data(), encoded.value()});
            if (!appended.ok()) return fail(appended.status());
            committed_rec = appended.value();
        }

        detail::CoreEvent app{};
        app.kind = detail::CoreEventKind::outbound_appended;
        app.now_ms = now_ms;
        app.sequence = seq;
        app.msg_type = msg.msg_type;
        const auto aout = core_.process(app);
        if (!aout.status.ok()) return fail(aout.status);

        // Fast-path direct socket write when TX queue & replay are idle!
        const auto payload = committed_rec.payload;
        if (tx_count_ == 0 && !replay_active_ && replay_count_ == 0 &&
            transport_.valid() && !transport_.is_connecting()) {
            const auto io = transport_.write(std::span<const char>{payload.data(), payload.size()});
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
        add({98, FieldKind::unsigned_integer, 0, {}});
        const auto hb_sec = std::max<std::uint64_t>(1, (static_cast<std::uint64_t>(config_.heartbeat_ms) + 999) / 1000);
        add({108, FieldKind::unsigned_integer, hb_sec, {}});
        if (config_.request_sequence_reset)
            add({141, FieldKind::character, static_cast<unsigned char>('Y'), {}});
        if (config_.version == FixVersion::fixt11)
            add({1137, FieldKind::string, 0, config_.default_appl_ver_id});

        detail::DirectAdminStorage stg{};
        const auto header = make_header(seq, wall_time_);
        if (const auto st = detail::direct_build_admin("A", {safe.data(), sc}, stg, admin_arena_); !st.ok())
            return st;
        const auto persisted = encode_fix(stg.view, header, encode_buf_);
        if (!persisted.ok()) return persisted.status();

        std::size_t wc = sc;
        if (!config_.username.empty()) wire[wc++] = {553, FieldKind::string, 0, config_.username};
        if (!config_.password.empty()) wire[wc++] = {554, FieldKind::string, 0, config_.password};
        if (const auto st = detail::direct_build_admin("A", {wire.data(), wc}, stg, admin_arena_); !st.ok())
            return st;
        const auto actual = encode_fix(stg.view, header, logon_tx_buf_);
        detail::direct_secure_clear(admin_arena_);
        if (!actual.ok()) { detail::direct_secure_clear(logon_tx_buf_); return actual.status(); }

        const auto appended = journal_.append(RecordKind::outbound, seq, 0, 0,
            std::string_view{encode_buf_.data(), persisted.value()});
        if (!appended.ok()) { detail::direct_secure_clear(logon_tx_buf_); return appended.status(); }
        logon_tx_size_ = actual.value();
        if (const auto st = enqueue_tx(appended.value().id, actual.value(), seq, "A", true); !st.ok()) {
            detail::direct_secure_clear(logon_tx_buf_);
            logon_tx_size_ = 0;
            return st;
        }
        detail::CoreEvent ev{};
        ev.kind = detail::CoreEventKind::outbound_appended;
        ev.now_ms = now_ms;
        ev.sequence = seq;
        ev.msg_type = "A";
        return core_.process(ev).status;
    }

    [[nodiscard]] Status queue_replay(std::uint64_t begin, std::uint64_t end, RecordId dep) noexcept {
        const auto next_out = journal_.next_outbound_seq();
        const auto max_out = next_out == 0 ? 0 : next_out - 1;
        if (max_out != 0 && begin < journal_.lowest_retained_outbound_seq())
            return fail(Status{ErrorCode::history_unavailable, 7, static_cast<std::size_t>(begin)});
        if (end == 0 || end > max_out) end = max_out;
        if (max_out == 0 || begin > end) {
            if (dep != 0) return append_delivery(dep, journal_.find_record(dep)->seq);
            return Status::success();
        }
        if (replay_count_ >= replay_ring_.size()) return Status{ErrorCode::queue_full};
        replay_ring_[(replay_head_ + replay_count_) % replay_ring_.size()] = ReplayRange{begin, end, dep};
        ++replay_count_;
        start_next_replay();
        return Status::success();
    }

    void start_next_replay() noexcept {
        if (replay_active_ || replay_count_ == 0) return;
        const auto r = replay_ring_[replay_head_];
        replay_head_ = (replay_head_ + 1) % replay_ring_.size();
        --replay_count_;
        replay_next_ = r.begin;
        replay_end_ = r.end;
        replay_dep_ = r.dependency;
        replay_active_ = true;
        replay_prepared_ = false;
    }

    [[nodiscard]] Status build_replay_frame() noexcept {
        if (!replay_active_ || replay_prepared_) return Status::success();
        while (replay_next_ <= replay_end_) {
            const auto* rec = journal_.find_outbound(replay_next_);
            if (rec == nullptr || rec->payload.empty()) return Status{ErrorCode::corrupt_store, 34};
            FlatMessageView parsed{};
            if (const auto st = parse_frame(rec->payload, parsed); !st.ok()) return st;
            const auto tags = detail::direct_extract_tags(parsed);
            if (tags.msg_type == nullptr || tags.sending_time == nullptr)
                return Status{ErrorCode::corrupt_store, 35};
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
            auto gap_end = replay_next_ + 1;
            while (gap_end <= replay_end_) {
                const auto* nr = journal_.find_outbound(gap_end);
                if (nr == nullptr || nr->payload.empty()) return Status{ErrorCode::corrupt_store, 34};
                FlatMessageView np{};
                if (const auto st = parse_frame(nr->payload, np); !st.ok()) return st;
                const auto nt = detail::direct_extract_tags(np);
                if (nt.msg_type == nullptr) return Status{ErrorCode::corrupt_store, 35};
                if (!detail::direct_is_admin(nt.msg_type->value)) break;
                ++gap_end;
            }
            std::array<detail::DirectAdminField, 2> gf{
                detail::DirectAdminField{123, FieldKind::character, static_cast<unsigned char>('Y'), {}},
                detail::DirectAdminField{36, FieldKind::unsigned_integer, gap_end, {}},
            };
            detail::DirectAdminStorage stg{};
            if (const auto st = detail::direct_build_admin("4", gf, stg, admin_arena_); !st.ok()) return st;
            auto header = make_header(gap_begin, wall_time_, true, orig_time);
            const auto enc = encode_fix(stg.view, header, replay_buf_);
            if (!enc.ok()) return enc.status();
            replay_bytes_ = enc.value();
            replay_offset_ = 0;
            replay_seq_ = gap_begin;
            replay_advance_ = gap_end;
            replay_msg_type_[0] = '4';
            replay_msg_type_size_ = 1;
            replay_prepared_ = true;
            return Status::success();
        }
        return finish_replay_range();
    }

    [[nodiscard]] Status finish_replay_range() noexcept {
        const auto dep = replay_dep_;
        replay_active_ = replay_prepared_ = false;
        replay_offset_ = replay_bytes_ = 0;
        replay_dep_ = 0;
        if (dep != 0) {
            const auto* in = journal_.find_record(dep);
            if (in == nullptr || in->kind != RecordKind::inbound) return Status{ErrorCode::corrupt_store};
            if (const auto st = append_delivery(dep, in->seq); !st.ok()) return st;
        }
        start_next_replay();
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

            std::span<const char> slice;
            if (replay_due) {
                slice = {replay_buf_.data() + replay_offset_, replay_bytes_ - replay_offset_};
            } else if (q->ephemeral) {
                slice = {logon_tx_buf_.data() + q->offset, logon_tx_size_ - q->offset};
            } else {
                const auto* rec = journal_.find_record(q->record_id);
                if (rec == nullptr || rec->payload.size() != q->size)
                    return fail(Status{ErrorCode::corrupt_store});
                slice = {rec->payload.data() + q->offset, q->size - q->offset};
            }

            const auto io = transport_.write(slice);
            if (io.bytes != 0) {
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
                if (wt == "4") {
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
            const auto st = inspect_frame(avail, fv, hints, CodecLimits{config_.max_frame_bytes});
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

    void update_retention_floor() noexcept {
        if (config_.retain_outbound_messages == 0) return;
        const auto latest = journal_.next_outbound_seq() == 0 ? 0 : journal_.next_outbound_seq() - 1;
        if (latest < config_.retain_outbound_messages) return;
        const auto window_floor = latest - config_.retain_outbound_messages + 1;
        const auto ack_floor = peer_ack_seq_ == std::numeric_limits<std::uint64_t>::max()
            ? peer_ack_seq_ : peer_ack_seq_ + 1;
        const auto floor = std::max(std::min(window_floor, ack_floor),
                                    journal_.lowest_retained_outbound_seq());
        if (floor > journal_.lowest_retained_outbound_seq()) {
            (void)journal_.set_retention_floor(floor, journal_.completed_inbound_record_id());
        }
    }

    [[nodiscard]] Status deliver_to_handler(const FlatMessageView& msg, DeliveryInfo info,
                                            std::uint64_t now_ms,
                                            std::string_view known_msg_type = {},
                                            std::size_t msg_type_offset = 0) noexcept {
        const auto valid = detail::direct_schema_validate<Schema>(msg, known_msg_type, msg_type_offset);
        if (!valid.ok()) {
            const auto tags = detail::direct_extract_tags(msg);
            const auto ref_type = tags.msg_type ? tags.msg_type->value : std::string_view{};
            std::array<detail::DirectAdminField, 5> rfields{};
            std::size_t rc = 0;
            rfields[rc++] = {45, FieldKind::unsigned_integer, info.msg_seq_num, {}};
            if (valid.tag > 0) rfields[rc++] = {371, FieldKind::unsigned_integer, static_cast<std::uint64_t>(valid.tag), {}};
            if (!ref_type.empty()) rfields[rc++] = {372, FieldKind::string, 0, ref_type};
            rfields[rc++] = {373, FieldKind::unsigned_integer,
                valid.code == ErrorCode::required_field_missing ? 1ULL :
                valid.code == ErrorCode::message_unhandled ? 11ULL : 5ULL, {}};
            rfields[rc++] = {58, FieldKind::string, 0, valid.message()};
            if (const auto st = send_admin("3", {rfields.data(), rc}, now_ms); !st.ok()) return fail(st);
            if constexpr (requires { handler_.on_error(valid); }) {
                try { handler_.on_error(valid); } catch (...) {}
            }
            const auto d = append_delivery(info.record_id, info.msg_seq_num);
            return d.ok() ? d : fail(d);
        }
        Status cb{};
        try {
            if constexpr (requires { handler_.on_message(*this, msg, info); }) {
                cb = handler_.on_message(*this, msg, info);
            } else if constexpr (requires { handler_.on_message(msg, info); }) {
                cb = handler_.on_message(msg, info);
            }
        } catch (...) {
            cb = Status{ErrorCode::callback_failed};
        }
        if (!cb.ok()) return fail(Status{ErrorCode::callback_failed, cb.tag, cb.offset});
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
            if (tags.msg_type == nullptr) return fail(Status{ErrorCode::corrupt_store, 35});
            const auto mt = tags.msg_type->value;
            if (detail::direct_is_admin(mt)) {
                if (mt == "2" && tags.begin_seq && tags.end_seq) {
                    std::uint64_t b{}, e{};
                    if (!detail::direct_parse_u64(tags.begin_seq->value, b) ||
                        !detail::direct_parse_u64(tags.end_seq->value, e))
                        return fail(Status{ErrorCode::corrupt_store, 7});
                    if (const auto st = queue_replay(b, e, id); !st.ok()) return fail(st);
                } else if (mt == "1" && tags.test_id) {
                    std::array<detail::DirectAdminField, 1> f{
                        detail::DirectAdminField{112, FieldKind::string, 0, tags.test_id->value}};
                    if (const auto st = send_admin("0", f, now_ms, id); !st.ok()) return fail(st);
                    if (const auto st = append_delivery(id, rec->seq); !st.ok()) return fail(st);
                } else {
                    if (const auto st = append_delivery(id, rec->seq); !st.ok()) return fail(st);
                }
            } else {
                if (const auto st = deliver_to_handler(msg, DeliveryInfo{id, rec->seq, true}, now_ms); !st.ok())
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
                if (!a.text.empty()) f[n++] = {58, FieldKind::string, 0, a.text};
                if (const auto st = send_admin("5", {f.data(), n}, now_ms, inbound_id); !st.ok()) return fail(st);
                break;
            }
            case detail::CoreActionKind::send_heartbeat: {
                std::array<detail::DirectAdminField, 1> f{};
                std::size_t n = 0;
                if (!a.text.empty()) f[n++] = {112, FieldKind::string, 0, a.text};
                if (const auto st = send_admin("0", {f.data(), n}, now_ms, inbound_id); !st.ok()) return fail(st);
                break;
            }
            case detail::CoreActionKind::send_test_request: {
                std::array<detail::DirectAdminField, 1> f{{{112, FieldKind::string, 0, a.text}}};
                if (const auto st = send_admin("1", f, now_ms, inbound_id); !st.ok()) return fail(st);
                break;
            }
            case detail::CoreActionKind::send_resend_request: {
                std::array<detail::DirectAdminField, 2> f{
                    detail::DirectAdminField{7, FieldKind::unsigned_integer, a.first, {}},
                    detail::DirectAdminField{16, FieldKind::unsigned_integer, a.last, {}},
                };
                if (const auto st = send_admin("2", f, now_ms, inbound_id); !st.ok()) return fail(st);
                break;
            }
            case detail::CoreActionKind::start_replay:
                if (const auto st = queue_replay(a.first, a.last, inbound_id); !st.ok()) return fail(st);
                break;
            case detail::CoreActionKind::become_active: {
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

    [[nodiscard]] Status process_inbound_frame(const FrameView& fv, std::uint64_t now_ms,
                                               bool is_deferred) noexcept {
        constexpr auto hints = detail::direct_schema_wire_hints<Schema>();
        FlatMessageView message{};
        if (const auto st = parse_flat_verified(fv, parse_fields_, message, hints); !st.ok()) return fail(st);
        const auto tags = detail::direct_extract_tags(message);
        if (tags.dup_tag != 0) return fail(Status{ErrorCode::duplicate_field, static_cast<int>(tags.dup_tag)});
        if (tags.begin == nullptr || tags.begin->value != begin_string(config_.version))
            return fail(Status{ErrorCode::unsupported_protocol, 8});
        if (tags.msg_type == nullptr || tags.msg_type->value.empty())
            return fail(Status{ErrorCode::invalid_header, 35});
        std::uint64_t sequence{};
        if (tags.seq == nullptr || !detail::direct_parse_u64(tags.seq->value, sequence) || sequence == 0)
            return fail(Status{ErrorCode::invalid_header, 34});
        if (tags.sender == nullptr || tags.sender->value != config_.remote_comp_id)
            return fail(Status{ErrorCode::identity_mismatch, 49});
        if (tags.target == nullptr || tags.target->value != config_.local_comp_id)
            return fail(Status{ErrorCode::identity_mismatch, 56});
        if (tags.sending_time == nullptr || !detail::is_valid_utc_timestamp(tags.sending_time->value))
            return fail(Status{ErrorCode::invalid_timestamp, 52});

        const bool poss_dup = tags.poss_dup != nullptr && tags.poss_dup->value == "Y";
        if (tags.poss_dup != nullptr && tags.poss_dup->value != "Y" && tags.poss_dup->value != "N")
            return fail(Status{ErrorCode::invalid_value, 43});
        const bool orig_valid = tags.orig_time != nullptr && detail::is_valid_utc_timestamp(tags.orig_time->value);
        if (poss_dup && (!orig_valid || detail::compare_utc_timestamps(tags.orig_time->value, tags.sending_time->value) > 0))
            return fail(Status{ErrorCode::invalid_timestamp, 122});
        if (!poss_dup && tags.orig_time != nullptr) return fail(Status{ErrorCode::invalid_header, 122});

        const auto msg_type = tags.msg_type->value;
        std::string_view test_id{};
        if (msg_type == "1" || msg_type == "0") {
            if (tags.test_id != nullptr) test_id = tags.test_id->value;
            if (msg_type == "1" && tags.test_id == nullptr) return fail(Status{ErrorCode::missing_field, 112});
        }

        if (config_.check_sending_time) {
            std::int64_t sent_ms{}, cur_ms{};
            if (!detail::direct_parse_utc_ms(tags.sending_time->value, sent_ms))
                return fail(Status{ErrorCode::invalid_timestamp, 52});
            if (!wall_time_ms_valid_) {
                if (!detail::direct_parse_utc_ms(wall_time_, cached_wall_ms_))
                    return fail(Status{ErrorCode::invalid_timestamp, 52});
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
        std::string_view logout_text{};

        if (msg_type == "A") {
            if (tags.encrypt == nullptr || tags.encrypt->value != "0")
                return fail(Status{ErrorCode::protocol_error, 98});
            std::uint64_t secs{};
            if (tags.hb_int == nullptr || !detail::direct_parse_u64(tags.hb_int->value, secs) ||
                secs == 0 || secs > std::numeric_limits<std::uint32_t>::max() / 1000U)
                return fail(Status{ErrorCode::protocol_error, 108});
            peer_hb_ms = secs * 1000;
            if (config_.version == FixVersion::fixt11 &&
                (tags.appl_ver == nullptr || tags.appl_ver->value != config_.default_appl_ver_id))
                return fail(Status{ErrorCode::unsupported_protocol, 1137});
            const bool peer_reset = tags.reset_flag != nullptr && tags.reset_flag->value == "Y";
            if (tags.reset_flag != nullptr && tags.reset_flag->value != "Y" && tags.reset_flag->value != "N")
                return fail(Status{ErrorCode::invalid_value, 141});
            if (peer_reset && (!config_.allow_sequence_reset || !local_epoch_reset_started_ || sequence != 1))
                return fail(Status{ErrorCode::sequence_reset_denied, 141});
            if (!peer_reset && config_.request_sequence_reset && core_.state() == SessionState::awaiting_logon)
                return fail(Status{ErrorCode::sequence_reset_denied, 141});
        } else if (msg_type == "2") {
            if (tags.begin_seq == nullptr || !detail::direct_parse_u64(tags.begin_seq->value, resend_begin) ||
                tags.end_seq == nullptr || !detail::direct_parse_u64(tags.end_seq->value, resend_end) ||
                resend_begin == 0 || (resend_end != 0 && resend_end < resend_begin))
                return fail(Status{ErrorCode::protocol_error, 7});
        } else if (msg_type == "4") {
            if (tags.gap_fill != nullptr && tags.gap_fill->value != "Y" && tags.gap_fill->value != "N")
                return fail(Status{ErrorCode::invalid_value, 123});
            const bool is_gf = tags.gap_fill != nullptr && tags.gap_fill->value == "Y";
            if (tags.new_seq == nullptr || !detail::direct_parse_u64(tags.new_seq->value, next_inbound))
                return fail(Status{ErrorCode::protocol_error, 36});
            if (!is_gf) {
                if (next_inbound <= core_.next_inbound()) return fail(Status{ErrorCode::protocol_error, 36});
                seq_reset_ev = true;
                effective_seq = core_.next_inbound();
            } else {
                if (!poss_dup || !orig_valid || next_inbound <= sequence)
                    return fail(Status{ErrorCode::protocol_error, 123});
                if (next_inbound <= core_.next_inbound()) dup_gapfill = true;
                else {
                    gapfill_ev = true;
                    if (sequence < core_.next_inbound()) effective_seq = core_.next_inbound();
                }
            }
        } else if (msg_type == "5") {
            if (tags.text != nullptr) logout_text = tags.text->value;
        }

        std::optional<std::uint64_t> ack_cand{};
        if (tags.peer_ack != nullptr) {
            std::uint64_t av{};
            if (!detail::direct_parse_u64(tags.peer_ack->value, av))
                return fail(Status{ErrorCode::invalid_value, 369});
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

        bool deferred_logon = is_deferred;
        if (msg_type == "A" && !deferred_logon && core_.state() == SessionState::awaiting_logon &&
            sequence > core_.next_inbound()) {
            if (deferred_logon_pending_) {
                if (sequence == deferred_logon_seq_ &&
                    fv.bytes == std::string_view{deferred_logon_buf_.data(), deferred_logon_size_})
                    return Status::success();
                return fail(Status{ErrorCode::protocol_error, 35});
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
                return fail(gap_out.status.ok() ? Status{ErrorCode::protocol_error, 34} : gap_out.status);

            detail::CoreEvent ooo{};
            ooo.kind = detail::CoreEventKind::out_of_order_logon;
            ooo.now_ms = now_ms;
            ooo.first = peer_hb_ms;
            const auto lout = core_.process(ooo);
            if (!lout.status.ok()) return fail(lout.status);
            if (const auto st = apply_actions(lout, nullptr, 0, 0, now_ms); !st.ok()) return st;
            return apply_actions(gap_out, nullptr, 0, 0, now_ms);
        }
        if (msg_type == "A" && deferred_logon_pending_ && sequence == deferred_logon_seq_) {
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
        if (msg_type == "2") { cand.first = resend_begin; cand.last = resend_end; }
        const auto disp = core_.process(cand);
        if (!disp.status.ok()) return fail(disp.status);
        if (disp.inbound == detail::InboundDisposition::duplicate) return Status::success();

        if (ack_cand) {
            const auto max_out = journal_.next_outbound_seq() == 0 ? 0 : journal_.next_outbound_seq() - 1;
            if (*ack_cand < peer_ack_seq_ || *ack_cand > max_out)
                return fail(Status{ErrorCode::protocol_error, 369});
        }

        if (disp.inbound == detail::InboundDisposition::gap) {
            return apply_actions(disp, nullptr, 0, 0, now_ms);
        }
        if (disp.inbound != detail::InboundDisposition::accept)
            return fail(Status{ErrorCode::protocol_error});

        const std::uint64_t persisted_next = (seq_reset_ev || gapfill_ev) ? next_inbound : 0;
        const auto appended = journal_.append(RecordKind::inbound, effective_seq, 0, 0,
                                              fv.bytes, persisted_next);
        if (!appended.ok()) return fail(appended.status());
        const RecordId inbound_id = appended.value().id;
        if (ack_cand) peer_ack_seq_ = *ack_cand;

        // If a business message arrives during awaiting_logon (gap recovery before deferred Logon),
        // defer delivery to recovery_ids_ so handler sees it in order upon becoming active.
        if (!detail::direct_is_admin(msg_type) && core_.state() == SessionState::awaiting_logon) {
            recovery_ids_.push_back(inbound_id);
        } else if (msg_type == "2") {
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
        comm.first = msg_type == "2" ? resend_begin : peer_hb_ms;
        comm.last = resend_end;
        comm.text = (msg_type == "1" || msg_type == "0") ? test_id : logout_text;
        comm.poss_dup = poss_dup;
        comm.orig_sending_time_valid = orig_valid;
        comm.is_deferred_logon = deferred_logon || merged_gf_covered_logon;
        const auto actions = core_.process(comm);
        if (!actions.status.ok()) return fail(actions.status);

        if (const auto st = apply_actions(actions, detail::direct_is_admin(msg_type) ? nullptr : &message,
                                          inbound_id, effective_seq, now_ms,
                                          msg_type, tags.msg_type->offset); !st.ok()) return st;

        if (detail::direct_is_admin(msg_type) && msg_type != "2") {
            if (const auto st = append_delivery(inbound_id, effective_seq); !st.ok()) return fail(st);
        }
        if (msg_type == "3") {
            if constexpr (requires { handler_.on_error(Status{ErrorCode::peer_rejected}); }) {
                try { handler_.on_error(Status{ErrorCode::peer_rejected}); } catch (...) {}
            }
        }
        update_retention_floor();

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
    detail::SessionCore core_;
    detail::FastUtcClock clock_{};

    std::vector<char> rx_buf_;
    std::size_t rx_head_{};
    std::size_t rx_tail_{};

    std::vector<char> encode_buf_;
    std::vector<char> replay_buf_;
    std::vector<char> deferred_logon_buf_;
    std::vector<char> logon_tx_buf_;
    std::vector<char> admin_arena_;
    std::vector<FieldView> parse_fields_;
    std::vector<TxEntry> tx_ring_;
    std::vector<ReplayRange> replay_ring_;
    std::vector<RecordId> recovery_ids_;

    std::size_t tx_head_{};
    std::size_t tx_count_{};
    std::size_t replay_head_{};
    std::size_t replay_count_{};
    bool replay_active_{};
    bool replay_prepared_{};
    std::uint64_t replay_next_{};
    std::uint64_t replay_end_{};
    std::uint64_t replay_seq_{};
    std::uint64_t replay_advance_{};
    RecordId replay_dep_{};
    std::size_t replay_bytes_{};
    std::size_t replay_offset_{};
    std::array<char, 32> replay_msg_type_{};
    std::uint8_t replay_msg_type_size_{};

    std::size_t logon_tx_size_{};
    std::size_t deferred_logon_size_{};
    std::uint64_t deferred_logon_seq_{};
    bool deferred_logon_pending_{};
    bool local_epoch_reset_started_{};

    std::array<char, 32> wall_time_buf_{};
    std::size_t wall_time_size_{};
    std::string_view wall_time_{};
    std::int64_t cached_wall_ms_{};
    bool wall_time_ms_valid_{};
    std::uint64_t now_ms_{};
    std::uint64_t peer_ack_seq_{};

    bool should_close_{};
    bool fatal_{};
    Status last_status_{};
    Status recovery_status_{};
};

} // namespace nimblefix
