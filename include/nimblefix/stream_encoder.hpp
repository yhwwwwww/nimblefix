#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <nimblefix/decimal.hpp>
#include <nimblefix/detail/message_core.hpp>
#include <nimblefix/protocol.hpp>
#include <nimblefix/status.hpp>

namespace nimblefix {

struct SessionHeader;

namespace detail {

inline constexpr char kStreamDigitPairs[201] =
    "0001020304050607080910111213141516171819"
    "2021222324252627282930313233343536373839"
    "4041424344454647484950515253545556575859"
    "6061626364656667686970717273747576777879"
    "8081828384858687888990919293949596979899";

[[nodiscard]] inline char* write_u64_fast(char* p, std::uint64_t v) noexcept {
    char tmp[24];
    char* end = tmp + sizeof(tmp);
    char* cur = end;
    while (v >= 100ULL) {
        const auto idx = static_cast<std::size_t>((v % 100ULL) * 2ULL);
        v /= 100ULL;
        cur -= 2;
        cur[0] = kStreamDigitPairs[idx];
        cur[1] = kStreamDigitPairs[idx + 1];
    }
    if (v >= 10ULL) {
        const auto idx = static_cast<std::size_t>(v * 2ULL);
        cur -= 2;
        cur[0] = kStreamDigitPairs[idx];
        cur[1] = kStreamDigitPairs[idx + 1];
    } else {
        *--cur = static_cast<char>('0' + v);
    }
    const auto n = static_cast<std::size_t>(end - cur);
    std::memcpy(p, cur, n);
    return p + n;
}

[[nodiscard]] inline char* write_i64_fast(char* p, std::int64_t v) noexcept {
    if (v < 0) {
        *p++ = '-';
        const auto mag = std::uint64_t{0} - std::bit_cast<std::uint64_t>(v);
        return write_u64_fast(p, mag);
    }
    return write_u64_fast(p, static_cast<std::uint64_t>(v));
}

// Caller must guarantee at least 24 bytes of space at p and dec.scale() <= 18.
[[nodiscard]] inline char* write_decimal_fast(char* p, Decimal dec) noexcept {
    const auto coeff = dec.coefficient();
    const auto scale = static_cast<std::size_t>(dec.scale());
    std::uint64_t mag = coeff < 0
        ? std::uint64_t{0} - std::bit_cast<std::uint64_t>(coeff)
        : static_cast<std::uint64_t>(coeff);
    if (coeff < 0) *p++ = '-';
    if (scale == 0) return write_u64_fast(p, mag);

    char tmp[24];
    char* end = tmp + sizeof(tmp);
    char* cur = end;
    while (mag >= 100ULL) {
        const auto idx = static_cast<std::size_t>((mag % 100ULL) * 2ULL);
        mag /= 100ULL;
        cur -= 2;
        cur[0] = kStreamDigitPairs[idx];
        cur[1] = kStreamDigitPairs[idx + 1];
    }
    if (mag >= 10ULL) {
        const auto idx = static_cast<std::size_t>(mag * 2ULL);
        cur -= 2;
        cur[0] = kStreamDigitPairs[idx];
        cur[1] = kStreamDigitPairs[idx + 1];
    } else {
        *--cur = static_cast<char>('0' + mag);
    }
    const auto digits = static_cast<std::size_t>(end - cur);
    if (digits > scale) {
        const auto int_len = digits - scale;
        std::memcpy(p, cur, int_len);
        p += int_len;
        *p++ = '.';
        std::memcpy(p, cur + int_len, scale);
        p += scale;
    } else {
        *p++ = '0';
        *p++ = '.';
        const auto zeros = scale - digits;
        if (zeros != 0) {
            std::memset(p, '0', zeros);
            p += zeros;
        }
        std::memcpy(p, cur, digits);
        p += digits;
    }
    return p;
}

// Auto-vectorizes cleanly to SSE2 psadbw under -O3.
[[nodiscard]] inline unsigned fast_checksum_bytes(const char* data, std::size_t n) noexcept {
    const auto* raw = reinterpret_cast<const unsigned char*>(data);
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < n; ++i) sum += raw[i];
    return sum & 0xffU;
}

[[nodiscard]] inline std::size_t count_decimal_digits(std::size_t v) noexcept {
    if (v < 10U) return 1U;
    if (v < 100U) return 2U;
    if (v < 1000U) return 3U;
    if (v < 10000U) return 4U;
    std::size_t digits = 5U;
    for (v /= 100000U; v != 0; v /= 10U) ++digits;
    return digits;
}

namespace stream {

[[nodiscard]] inline bool is_valid_ymd8(std::string_view value) noexcept {
    if (value.size() != 8) return false;
    for (const char c : value) {
        if (static_cast<unsigned>(c - '0') > 9U) return false;
    }
    const unsigned year = static_cast<unsigned>((value[0] - '0') * 1000 + (value[1] - '0') * 100 +
        (value[2] - '0') * 10 + (value[3] - '0'));
    const unsigned month = static_cast<unsigned>((value[4] - '0') * 10 + (value[5] - '0'));
    const unsigned day = static_cast<unsigned>((value[6] - '0') * 10 + (value[7] - '0'));
    if (month < 1U || month > 12U) return false;
    constexpr unsigned month_days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    unsigned max_day = month_days[month - 1U];
    if (month == 2U && ((year % 4U == 0U && year % 100U != 0U) || year % 400U == 0U)) ++max_day;
    return day >= 1U && day <= max_day;
}

[[nodiscard]] inline bool is_valid_utc_time(std::string_view value) noexcept {
    if (value.size() != 8 && value.size() != 12 && value.size() != 15 && value.size() != 18) return false;
    for (std::size_t i = 0; i < 8; ++i) {
        if (i == 2 || i == 5) {
            if (value[i] != ':') return false;
        } else if (static_cast<unsigned>(value[i] - '0') > 9U) {
            return false;
        }
    }
    const auto two = [&](std::size_t at) noexcept {
        return static_cast<unsigned>((value[at] - '0') * 10 + (value[at + 1] - '0'));
    };
    if (two(0) > 23U || two(3) > 59U || two(6) > 60U) return false;
    if (value.size() > 8) {
        if (value[8] != '.') return false;
        for (std::size_t i = 9; i < value.size(); ++i) {
            if (static_cast<unsigned>(value[i] - '0') > 9U) return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool is_valid_month_year(std::string_view value) noexcept {
    if (value.size() != 6 && value.size() != 8) return false;
    for (std::size_t i = 0; i < 6; ++i) {
        if (static_cast<unsigned>(value[i] - '0') > 9U) return false;
    }
    const unsigned month = static_cast<unsigned>((value[4] - '0') * 10 + (value[5] - '0'));
    if (month < 1U || month > 12U) return false;
    if (value.size() == 6) return true;
    if (value[6] == 'w') return value[7] >= '1' && value[7] <= '5';
    if (static_cast<unsigned>(value[6] - '0') > 9U || static_cast<unsigned>(value[7] - '0') > 9U) return false;
    const unsigned day = static_cast<unsigned>((value[6] - '0') * 10 + (value[7] - '0'));
    const unsigned year = static_cast<unsigned>((value[0] - '0') * 1000 + (value[1] - '0') * 100 +
        (value[2] - '0') * 10 + (value[3] - '0'));
    constexpr unsigned month_days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    unsigned max_day = month_days[month - 1U];
    if (month == 2U && ((year % 4U == 0U && year % 100U != 0U) || year % 400U == 0U)) ++max_day;
    return day >= 1U && day <= max_day;
}

[[nodiscard]] inline bool is_valid_mvs_syntax(std::string_view value, bool single_char_tokens) noexcept {
    if (!is_valid_text(value)) return false;
    for (std::size_t first = 0; first < value.size();) {
        const auto sep = value.find(' ', first);
        const auto last = sep == std::string_view::npos ? value.size() : sep;
        if (last == first || (single_char_tokens && last - first != 1)) return false;
        first = sep == std::string_view::npos ? value.size() : sep + 1;
        if (first == value.size() && sep != std::string_view::npos) return false;
    }
    return true;
}

} // namespace stream

// Strictly caches ONLY session-immutable transport identity:
//   "8=<BeginString>\x019=" and "49=<SenderCompID>\x0156=<TargetCompID>\x01".
// Per-message header fields such as 50=SenderSubID / 57=TargetSubID /
// 142=SenderLocationID / 115=OnBehalfOfCompID are NEVER baked into this prefix;
// they are written dynamically per message via w.header() before body fields.
struct StaticSessionPrefix {
    FixVersion version{FixVersion::fix44};
    std::array<char, 16> begin_prefix{}; // "8=FIX.4.4\x019="
    std::size_t begin_prefix_size{};
    std::array<char, 256> comp_ids{};    // "49=<local>\x0156=<remote>\x01"
    std::size_t comp_ids_size{};
    bool valid{false};

    [[nodiscard]] Status init(FixVersion ver, std::string_view sender, std::string_view target) noexcept {
        valid = false;
        const auto begin = begin_string(ver);
        if (begin.empty() || !is_valid_text(sender) || !is_valid_text(target)) {
            return Status{ErrorCode::invalid_header};
        }
        const std::size_t bp_len = 2U + begin.size() + 3U; // "8=" + begin + SOH + "9="
        const std::size_t ci_len = 3U + sender.size() + 1U + 3U + target.size() + 1U;
        if (bp_len > begin_prefix.size() || ci_len > comp_ids.size()) {
            return Status{ErrorCode::capacity_exceeded};
        }
        version = ver;
        char* bp = begin_prefix.data();
        bp[0] = '8';
        bp[1] = '=';
        std::memcpy(bp + 2, begin.data(), begin.size());
        bp[2 + begin.size()] = '\x01';
        bp[3 + begin.size()] = '9';
        bp[4 + begin.size()] = '=';
        begin_prefix_size = bp_len;

        char* cp = comp_ids.data();
        std::memcpy(cp, "49=", 3); cp += 3;
        std::memcpy(cp, sender.data(), sender.size()); cp += sender.size();
        *cp++ = '\x01';
        std::memcpy(cp, "56=", 3); cp += 3;
        std::memcpy(cp, target.data(), target.size()); cp += target.size();
        *cp++ = '\x01';
        comp_ids_size = ci_len;
        valid = true;
        return Status::success();
    }
};

// Low-level sequential wire cursor shared by all generated StreamWriters.
// Zero allocations, zero intermediate SlotValue/PlanField buffers.
class StreamBuffer {
public:
    static constexpr std::size_t checksum_suffix_size = 7;

    // Optional spill/grow callback invoked only if the initial buffer is exhausted.
    // The callback must preserve [old_data, old_data + current_written) in the returned span.
    using GrowFn = std::span<char> (*)(
        void* ctx, const char* old_data, std::size_t current_written,
        std::size_t min_total_capacity) noexcept;

    // Body-only streaming mode (for encode_body_stream / DirectSendQueue / Sender / ProducerLane).
    explicit StreamBuffer(std::span<char> out, GrowFn grow_fn = nullptr, void* grow_ctx = nullptr) noexcept
        : base_(out.data()), p_(out.data()), end_(out.data() + out.size()),
          cap_(out.size()), grow_fn_(grow_fn), grow_ctx_(grow_ctx), body_mode_(true) {}

    void set_grow_callback(GrowFn grow_fn, void* grow_ctx) noexcept {
        grow_fn_ = grow_fn;
        grow_ctx_ = grow_ctx;
    }

    [[nodiscard]] const char* data() const noexcept { return base_; }

    // Full-frame mode with arbitrary caller-provided SessionHeader (validates header upfront).
    template <class Header>
    static StreamBuffer begin_frame(std::span<char> out, std::string_view msg_type,
                                    const Header& header, GrowFn grow_fn = nullptr,
                                    void* grow_ctx = nullptr) noexcept {
        StreamBuffer buf{out, grow_fn, grow_ctx};
        buf.body_mode_ = false;
        const auto begin = begin_string(header.version);
        if (begin.empty()) { buf.fail(ErrorCode::invalid_header, 8); return buf; }
        if (msg_type.empty() || !is_valid_text(msg_type)) { buf.fail(ErrorCode::invalid_value, 35); return buf; }
        if (!is_valid_text(header.sender_comp_id) || !is_valid_text(header.target_comp_id) ||
            header.msg_seq_num == 0) {
            buf.fail(ErrorCode::invalid_header);
            return buf;
        }
        if (!is_valid_text(header.sending_time) || !is_valid_utc_timestamp(header.sending_time)) {
            buf.fail(ErrorCode::invalid_timestamp, 52);
            return buf;
        }
        if (header.poss_dup && (!is_valid_text(header.orig_sending_time) ||
            !is_valid_utc_timestamp(header.orig_sending_time) ||
            compare_utc_timestamps(header.orig_sending_time, header.sending_time) > 0)) {
            buf.fail(ErrorCode::invalid_timestamp, 122);
            return buf;
        }
        if (!header.poss_dup && !header.orig_sending_time.empty()) {
            buf.fail(ErrorCode::invalid_header, 122);
            return buf;
        }

        buf.begin_ = begin;
        // Guess 3-digit BodyLength ("8=" + begin + SOH + "9=000" + SOH) -> zero memmove for 100..999 B frames.
        const std::size_t guessed = 2U + begin.size() + 1U + 2U + 3U + 1U;
        if (buf.cap_ <= guessed + checksum_suffix_size) {
            if (!buf.grow_to(0, guessed + checksum_suffix_size + 1U)) {
                buf.fail(ErrorCode::capacity_exceeded);
                return buf;
            }
        }
        buf.body_start_ = guessed;
        buf.p_ = buf.base_ + guessed;
        buf.end_ = buf.base_ + buf.cap_ - checksum_suffix_size;

        buf.write_tag_str("35=", 3, msg_type);
        buf.write_tag_str("49=", 3, header.sender_comp_id);
        buf.write_tag_str("56=", 3, header.target_comp_id);
        buf.write_tag_u64("34=", 3, header.msg_seq_num);
        if (header.poss_dup) buf.write_raw("43=Y\x01", 5);
        buf.write_tag_str("52=", 3, header.sending_time);
        if (header.last_msg_seq_num_processed) {
            buf.write_tag_u64("369=", 4, *header.last_msg_seq_num_processed);
        }
        if (header.poss_dup) {
            buf.write_tag_str("122=", 4, header.orig_sending_time);
        }
        return buf;
    }

    // Session hot-path frame mode: uses pre-validated StaticSessionPrefix (49=/56=) and
    // already-validated wall_time (e.g. from FastUtcClock or DirectInitiator::set_wall_time).
    static StreamBuffer begin_session_frame(std::span<char> out, std::string_view msg_type,
                                            const StaticSessionPrefix& prefix,
                                            std::uint64_t msg_seq_num,
                                            std::string_view prevalidated_wall_time,
                                            std::optional<std::uint64_t> last_processed_seq = std::nullopt,
                                            GrowFn grow_fn = nullptr,
                                            void* grow_ctx = nullptr) noexcept {
        StreamBuffer buf{out, grow_fn, grow_ctx};
        buf.body_mode_ = false;
        if (!prefix.valid || msg_seq_num == 0) {
            buf.fail(ErrorCode::invalid_header);
            return buf;
        }
        buf.begin_ = begin_string(prefix.version);
        const std::size_t guessed = prefix.begin_prefix_size + 4U; // "8=...9=" + "000" + SOH
        if (buf.cap_ <= guessed + checksum_suffix_size) {
            if (!buf.grow_to(0, guessed + checksum_suffix_size + 1U)) {
                buf.fail(ErrorCode::capacity_exceeded);
                return buf;
            }
        }
        buf.body_start_ = guessed;
        buf.p_ = buf.base_ + guessed;
        buf.end_ = buf.base_ + buf.cap_ - checksum_suffix_size;

        buf.write_tag_str("35=", 3, msg_type);
        buf.write_raw(prefix.comp_ids.data(), prefix.comp_ids_size);
        buf.write_tag_u64("34=", 3, msg_seq_num);
        buf.write_tag_str("52=", 3, prevalidated_wall_time);
        if (last_processed_seq) {
            buf.write_tag_u64("369=", 4, *last_processed_seq);
        }
        return buf;
    }

    [[nodiscard]] Status status() const noexcept { return err_; }
    [[nodiscard]] bool ok() const noexcept { return err_.ok(); }

    void fail(ErrorCode code, int tag = 0) noexcept {
        if (err_.ok()) err_ = Status{code, tag};
    }

    void fail(Status status) noexcept {
        if (err_.ok() && !status.ok()) err_ = status;
    }

    // Dynamic standard header guard: optional header fields (e.g. 50=SenderSubID,
    // 57=TargetSubID) must precede all message body fields.
    [[nodiscard]] bool enter_header_field(std::uint32_t tag) noexcept {
        if (!err_.ok()) return false;
        if (pending_len_tag_ != 0) {
            fail(ErrorCode::invalid_value, static_cast<int>(pending_len_tag_));
            return false;
        }
        if (body_started_) {
            fail(ErrorCode::invalid_header, static_cast<int>(tag));
            return false;
        }
        return true;
    }

    void enter_body_field(std::uint32_t expected_data_tag = 0) noexcept {
        body_started_ = true;
        if (pending_len_tag_ != 0 && pending_data_tag_ != expected_data_tag) {
            fail(ErrorCode::invalid_value, static_cast<int>(pending_len_tag_));
        }
    }

    // Track explicit LENGTH field preceding its paired binary DATA field.
    void register_pending_length(std::uint32_t len_tag, std::uint32_t data_tag, std::uint64_t length) noexcept {
        if (length == 0) {
            fail(ErrorCode::invalid_value, static_cast<int>(len_tag));
            return;
        }
        pending_len_tag_ = len_tag;
        pending_data_tag_ = data_tag;
        pending_len_value_ = length;
    }

    // Emit binary DATA field: verifies explicit preceding LENGTH if set, or auto-emits LENGTH pair.
    void write_binary_data(std::uint32_t len_tag, const char* len_prefix, std::size_t len_prefix_size,
                           std::uint32_t data_tag, const char* data_prefix, std::size_t data_prefix_size,
                           std::string_view data) noexcept {
        if (!err_.ok()) return;
        body_started_ = true;
        if (data.empty() || data.data() == nullptr) {
            fail(ErrorCode::invalid_value, static_cast<int>(data_tag));
            return;
        }
        if (pending_len_tag_ != 0) {
            if (pending_len_tag_ != len_tag || pending_data_tag_ != data_tag || pending_len_value_ != data.size()) {
                fail(ErrorCode::invalid_value, static_cast<int>(pending_len_tag_));
                return;
            }
            pending_len_tag_ = 0;
            pending_data_tag_ = 0;
            pending_len_value_ = 0;
        } else {
            write_tag_u64(len_prefix, len_prefix_size, data.size());
        }
        write_tag_str(data_prefix, data_prefix_size, data);
    }

    void write_raw(const char* src, std::size_t n) noexcept {
        if (!err_.ok()) return;
        if (!ensure_space(n)) return;
        if (n != 0) {
            std::memcpy(p_, src, n);
            p_ += n;
        }
    }

    void write_tag_str(const char* prefix, std::size_t plen, std::string_view v) noexcept {
        if (!err_.ok()) return;
        const std::size_t need = plen + v.size() + 1U;
        if (!ensure_space(need)) return;
        std::memcpy(p_, prefix, plen);
        p_ += plen;
        if (!v.empty()) {
            std::memcpy(p_, v.data(), v.size());
            p_ += v.size();
        }
        *p_++ = '\x01';
    }

    void write_tag_char(const char* prefix, std::size_t plen, char c) noexcept {
        if (!err_.ok()) return;
        const std::size_t need = plen + 2U;
        if (!ensure_space(need)) return;
        std::memcpy(p_, prefix, plen);
        p_[plen] = c;
        p_[plen + 1U] = '\x01';
        p_ += need;
    }

    void write_tag_u64(const char* prefix, std::size_t plen, std::uint64_t v) noexcept {
        if (!err_.ok()) return;
        const std::size_t avail = static_cast<std::size_t>(end_ - p_);
        if (avail < plen + 22U) {
            // Exact-fit fallback for tight buffers.
            char tmp[24];
            char* written = write_u64_fast(tmp, v);
            const auto dlen = static_cast<std::size_t>(written - tmp);
            write_tag_str(prefix, plen, std::string_view{tmp, dlen});
            return;
        }
        std::memcpy(p_, prefix, plen);
        p_ = write_u64_fast(p_ + plen, v);
        *p_++ = '\x01';
    }

    void write_tag_i64(const char* prefix, std::size_t plen, std::int64_t v) noexcept {
        if (!err_.ok()) return;
        const std::size_t avail = static_cast<std::size_t>(end_ - p_);
        if (avail < plen + 22U) {
            char tmp[24];
            char* written = write_i64_fast(tmp, v);
            const auto dlen = static_cast<std::size_t>(written - tmp);
            write_tag_str(prefix, plen, std::string_view{tmp, dlen});
            return;
        }
        std::memcpy(p_, prefix, plen);
        p_ = write_i64_fast(p_ + plen, v);
        *p_++ = '\x01';
    }

    void write_tag_decimal(const char* prefix, std::size_t plen, Decimal d, int tag_num) noexcept {
        if (!err_.ok()) return;
        if (d.scale() > 18U) {
            fail(ErrorCode::invalid_decimal, tag_num);
            return;
        }
        const std::size_t avail = static_cast<std::size_t>(end_ - p_);
        if (avail < plen + 26U) {
            char tmp[32];
            char* written = write_decimal_fast(tmp, d);
            const auto dlen = static_cast<std::size_t>(written - tmp);
            write_tag_str(prefix, plen, std::string_view{tmp, dlen});
            return;
        }
        std::memcpy(p_, prefix, plen);
        p_ = write_decimal_fast(p_ + plen, d);
        *p_++ = '\x01';
    }

    // High-level typed emit helpers shared by all generated StreamWriters
    // (keeps generated headers compact while remaining 100% header-inlined).
    template <std::uint32_t Tag, class V, class Validator = std::nullptr_t>
    [[nodiscard]] bool emit_str(const char* prefix, std::size_t plen, V&& value,
                                Validator valid_fn = nullptr) noexcept {
        using U = unwrap_optional_t<V>;
        static_assert(supported_value_v<U>, "unsupported static FIX field value type");
        static_assert(wire_value_compatible_v<WireType::string, U>,
            "value type does not match the generated FIX field type");
        if constexpr (is_optional_v<V>) {
            if (!value.has_value()) return false;
            return emit_str<Tag>(prefix, plen, *std::forward<V>(value), valid_fn);
        } else {
            enter_body_field();
            const std::string_view sv = as_string_view(std::forward<V>(value));
            if (!is_valid_text(sv)) { fail(ErrorCode::invalid_value, static_cast<int>(Tag)); return true; }
            if constexpr (!std::is_same_v<Validator, std::nullptr_t>) {
                if (!valid_fn(sv)) { fail(ErrorCode::invalid_value, static_cast<int>(Tag)); return true; }
            }
            write_tag_str(prefix, plen, sv);
            return true;
        }
    }

    template <std::uint32_t Tag, class V>
    [[nodiscard]] bool emit_utc_timestamp(const char* prefix, std::size_t plen, V&& value) noexcept {
        using U = unwrap_optional_t<V>;
        static_assert(supported_value_v<U>, "unsupported static FIX field value type");
        static_assert(wire_value_compatible_v<WireType::utc_timestamp, U>,
            "value type does not match the generated FIX field type");
        if constexpr (is_optional_v<V>) {
            if (!value.has_value()) return false;
            return emit_utc_timestamp<Tag>(prefix, plen, *std::forward<V>(value));
        } else {
            enter_body_field();
            const std::string_view sv = as_string_view(std::forward<V>(value));
            if (!is_valid_utc_timestamp(sv)) { fail(ErrorCode::invalid_timestamp, static_cast<int>(Tag)); return true; }
            write_tag_str(prefix, plen, sv);
            return true;
        }
    }

    template <std::uint32_t Tag, class V, class Validator = std::nullptr_t>
    [[nodiscard]] bool emit_char(const char* prefix, std::size_t plen, V&& value,
                                 Validator valid_fn = nullptr) noexcept {
        using U = unwrap_optional_t<V>;
        static_assert(supported_value_v<U>, "unsupported static FIX field value type");
        static_assert(wire_value_compatible_v<WireType::character, U>,
            "value type does not match the generated FIX field type");
        if constexpr (is_optional_v<V>) {
            if (!value.has_value()) return false;
            return emit_char<Tag>(prefix, plen, *std::forward<V>(value), valid_fn);
        } else {
            enter_body_field();
            char ch{};
            if constexpr (is_boolean_v<U>) ch = value ? 'Y' : 'N';
            else ch = value;
            if constexpr (!std::is_same_v<Validator, std::nullptr_t>) {
                if (!valid_fn(ch)) { fail(ErrorCode::invalid_value, static_cast<int>(Tag)); return true; }
            } else {
                if (static_cast<unsigned char>(ch) < 0x20U || ch == 0x7f) {
                    fail(ErrorCode::invalid_value, static_cast<int>(Tag)); return true;
                }
            }
            write_tag_char(prefix, plen, ch);
            return true;
        }
    }

    template <std::uint32_t Tag, std::uint32_t PairDataTag = 0, class V, class Validator = std::nullptr_t>
    [[nodiscard]] bool emit_int(const char* prefix, std::size_t plen, V&& value,
                                Validator valid_fn = nullptr) noexcept {
        using U = unwrap_optional_t<V>;
        static_assert(supported_value_v<U>, "unsupported static FIX field value type");
        static_assert(wire_value_compatible_v<WireType::integer, U>,
            "value type does not match the generated FIX field type");
        if constexpr (is_optional_v<V>) {
            if (!value.has_value()) return false;
            return emit_int<Tag, PairDataTag>(prefix, plen, *std::forward<V>(value), valid_fn);
        } else {
            enter_body_field();
            std::int64_t iv{};
            if constexpr (is_unsigned_integer_v<U>) {
                if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                    fail(ErrorCode::invalid_value, static_cast<int>(Tag)); return true;
                }
                iv = static_cast<std::int64_t>(value);
            } else {
                iv = static_cast<std::int64_t>(value);
            }
            if constexpr (!std::is_same_v<Validator, std::nullptr_t>) {
                if (!valid_fn(iv)) { fail(ErrorCode::invalid_value, static_cast<int>(Tag)); return true; }
            }
            if constexpr (PairDataTag != 0) {
                register_pending_length(Tag, PairDataTag, static_cast<std::uint64_t>(iv));
            }
            write_tag_i64(prefix, plen, iv);
            return true;
        }
    }

    template <std::uint32_t Tag, class V>
    [[nodiscard]] bool emit_decimal(const char* prefix, std::size_t plen, V&& value) noexcept {
        using U = unwrap_optional_t<V>;
        static_assert(supported_value_v<U>, "unsupported static FIX field value type");
        static_assert(wire_value_compatible_v<WireType::decimal, U>,
            "value type does not match the generated FIX field type");
        if constexpr (is_optional_v<V>) {
            if (!value.has_value()) return false;
            return emit_decimal<Tag>(prefix, plen, *std::forward<V>(value));
        } else {
            enter_body_field();
            Decimal dv{};
            if constexpr (is_decimal_v<U>) dv = value;
            else if constexpr (is_unsigned_integer_v<U>) {
                if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                    fail(ErrorCode::invalid_value, static_cast<int>(Tag)); return true;
                }
                dv = Decimal{static_cast<std::int64_t>(value), 0};
            } else {
                dv = Decimal{static_cast<std::int64_t>(value), 0};
            }
            write_tag_decimal(prefix, plen, dv, static_cast<int>(Tag));
            return true;
        }
    }

    template <std::uint32_t LenTag, std::uint32_t DataTag, class V>
    [[nodiscard]] bool emit_data(const char* len_prefix, std::size_t lplen,
                                 const char* data_prefix, std::size_t dplen, V&& value) noexcept {
        using U = unwrap_optional_t<V>;
        static_assert(supported_value_v<U>, "unsupported static FIX field value type");
        static_assert(wire_value_compatible_v<WireType::binary_data, U>,
            "value type does not match the generated FIX field type");
        if constexpr (is_optional_v<V>) {
            if (!value.has_value()) return false;
            return emit_data<LenTag, DataTag>(len_prefix, lplen, data_prefix, dplen, *std::forward<V>(value));
        } else {
            const std::string_view sv = as_string_view(std::forward<V>(value));
            write_binary_data(LenTag, len_prefix, lplen, DataTag, data_prefix, dplen, sv);
            return true;
        }
    }

    [[nodiscard]] Status check_pending_pairs() noexcept {
        if (!err_.ok()) return err_;
        if (pending_len_tag_ != 0) {
            fail(ErrorCode::invalid_value, static_cast<int>(pending_len_tag_));
        }
        return err_;
    }

    [[nodiscard]] Result<std::size_t> finish_body() noexcept {
        if (const auto st = check_pending_pairs(); !st.ok()) return st;
        return static_cast<std::size_t>(p_ - base_);
    }

    [[nodiscard]] Result<std::size_t> finish_frame() noexcept {
        if (const auto st = check_pending_pairs(); !st.ok()) return st;
        const std::size_t body_len = static_cast<std::size_t>(p_ - (base_ + body_start_));
        const std::size_t digits = count_decimal_digits(body_len);
        const std::size_t actual_start = 2U + begin_.size() + 1U + 2U + digits + 1U;
        const std::size_t total_size = actual_start + body_len + checksum_suffix_size;
        if (total_size > cap_) {
            const std::size_t written = static_cast<std::size_t>(p_ - base_);
            if (!grow_to(written, total_size)) {
                return Status{ErrorCode::capacity_exceeded};
            }
        }
        if (actual_start != body_start_) {
            std::memmove(base_ + actual_start, base_ + body_start_, body_len);
        }
        char* h = base_;
        h[0] = '8';
        h[1] = '=';
        std::memcpy(h + 2, begin_.data(), begin_.size());
        h += 2U + begin_.size();
        *h++ = '\x01';
        *h++ = '9';
        *h++ = '=';
        h = write_u64_fast(h, body_len);
        *h++ = '\x01';

        const std::size_t pre_csum = actual_start + body_len;
        const unsigned csum = fast_checksum_bytes(base_, pre_csum);
        char* tail = base_ + pre_csum;
        tail[0] = '1';
        tail[1] = '0';
        tail[2] = '=';
        tail[3] = static_cast<char>('0' + (csum / 100U) % 10U);
        tail[4] = static_cast<char>('0' + (csum / 10U) % 10U);
        tail[5] = static_cast<char>('0' + csum % 10U);
        tail[6] = '\x01';
        return total_size;
    }

private:
    [[gnu::noinline, gnu::cold]] [[nodiscard]] bool grow_to(std::size_t written, std::size_t min_cap) noexcept {
        if (grow_fn_ == nullptr) return false;
        const auto next = grow_fn_(grow_ctx_, base_, written, min_cap);
        if (next.data() == nullptr || next.size() < min_cap) return false;
        const std::size_t suffix = body_mode_ ? 0U : checksum_suffix_size;
        base_ = next.data();
        p_ = base_ + written;
        cap_ = next.size();
        end_ = base_ + cap_ - suffix;
        return true;
    }

    [[gnu::noinline, gnu::cold]] [[nodiscard]] bool ensure_space_slow(std::size_t need) noexcept {
        const std::size_t written = static_cast<std::size_t>(p_ - base_);
        const std::size_t suffix = body_mode_ ? 0U : checksum_suffix_size;
        if (need > std::numeric_limits<std::size_t>::max() - written - suffix) {
            fail(ErrorCode::too_large);
            return false;
        }
        if (!grow_to(written, written + need + suffix)) {
            fail(ErrorCode::capacity_exceeded);
            return false;
        }
        return true;
    }

    [[nodiscard]] bool ensure_space(std::size_t need) noexcept {
        if (need <= static_cast<std::size_t>(end_ - p_)) [[likely]] return true;
        return ensure_space_slow(need);
    }

    char* base_{};
    char* p_{};
    char* end_{};
    std::size_t cap_{};
    std::size_t body_start_{};
    GrowFn grow_fn_{nullptr};
    void* grow_ctx_{nullptr};
    std::string_view begin_{};
    std::uint32_t pending_len_tag_{};
    std::uint32_t pending_data_tag_{};
    std::uint64_t pending_len_value_{};
    bool body_mode_{false};
    bool body_started_{false};
    Status err_{Status::success()};
};

// Dynamic per-message FIX Standard Header writer (tags NOT managed by the session engine).
// Invoked via w.header() BEFORE writing body fields. Writing any header field after
// a body field has been written fails with ErrorCode::invalid_header.
class StandardHeaderWriter {
public:
    explicit StandardHeaderWriter(StreamBuffer& buf) noexcept : buf_(buf) {}

    // 50=SenderSubID
    template <class V> StandardHeaderWriter& sender_sub_id(V&& v) noexcept { return str_field<50>("50=", 3, std::forward<V>(v)); }
    // 57=TargetSubID
    template <class V> StandardHeaderWriter& target_sub_id(V&& v) noexcept { return str_field<57>("57=", 3, std::forward<V>(v)); }
    // 142=SenderLocationID
    template <class V> StandardHeaderWriter& sender_location_id(V&& v) noexcept { return str_field<142>("142=", 4, std::forward<V>(v)); }
    // 143=TargetLocationID
    template <class V> StandardHeaderWriter& target_location_id(V&& v) noexcept { return str_field<143>("143=", 4, std::forward<V>(v)); }
    // 115=OnBehalfOfCompID
    template <class V> StandardHeaderWriter& on_behalf_of_comp_id(V&& v) noexcept { return str_field<115>("115=", 4, std::forward<V>(v)); }
    // 116=OnBehalfOfSubID
    template <class V> StandardHeaderWriter& on_behalf_of_sub_id(V&& v) noexcept { return str_field<116>("116=", 4, std::forward<V>(v)); }
    // 144=OnBehalfOfLocationID
    template <class V> StandardHeaderWriter& on_behalf_of_location_id(V&& v) noexcept { return str_field<144>("144=", 4, std::forward<V>(v)); }
    // 128=DeliverToCompID
    template <class V> StandardHeaderWriter& deliver_to_comp_id(V&& v) noexcept { return str_field<128>("128=", 4, std::forward<V>(v)); }
    // 129=DeliverToSubID
    template <class V> StandardHeaderWriter& deliver_to_sub_id(V&& v) noexcept { return str_field<129>("129=", 4, std::forward<V>(v)); }
    // 145=DeliverToLocationID
    template <class V> StandardHeaderWriter& deliver_to_location_id(V&& v) noexcept { return str_field<145>("145=", 4, std::forward<V>(v)); }
    // 97=PossResend
    StandardHeaderWriter& poss_resend(bool v) noexcept {
        if (!buf_.enter_header_field(97)) return *this;
        buf_.write_tag_char("97=", 3, v ? 'Y' : 'N');
        return *this;
    }
    // 347=MessageEncoding
    template <class V> StandardHeaderWriter& message_encoding(V&& v) noexcept { return str_field<347>("347=", 4, std::forward<V>(v)); }
    // 1128=ApplVerID (FIXT.1.1 per-message override)
    template <class V> StandardHeaderWriter& appl_ver_id(V&& v) noexcept { return str_field<1128>("1128=", 5, std::forward<V>(v)); }

private:
    template <std::uint32_t Tag, class V>
    StandardHeaderWriter& str_field(const char* prefix, std::size_t plen, V&& value) noexcept {
        using U = unwrap_optional_t<V>;
        static_assert(is_string_v<U>, "standard header field requires a string value");
        if constexpr (is_optional_v<V>) {
            if (!value.has_value()) return *this;
            return str_field<Tag>(prefix, plen, *std::forward<V>(value));
        } else {
            if (!buf_.enter_header_field(Tag)) return *this;
            const std::string_view v = as_string_view(std::forward<V>(value));
            if (!is_valid_text(v)) {
                buf_.fail(ErrorCode::invalid_value, static_cast<int>(Tag));
                return *this;
            }
            buf_.write_tag_str(prefix, plen, v);
            return *this;
        }
    }

    StreamBuffer& buf_;
};

// Concept for generated message types supporting streaming encoding.
template <class Message>
concept streamable_message = requires {
    typename Message::stream_writer_type;
    { Message::msg_type } -> std::convertible_to<std::string_view>;
};

} // namespace detail

// Encode one complete FIX frame directly into output using a zero-intermediate-copy
// typed StreamWriter. Validates SessionHeader, all written fields, enums, and required tags.
template <class Message, class Fn>
    requires detail::streamable_message<Message>
[[nodiscard]] Result<std::size_t> encode_stream(
    const SessionHeader& header, std::span<char> output, Fn&& fn) noexcept {
    static_assert(!requires { requires Message::is_group_entry; },
        "repeating group entries cannot be encoded as standalone FIX messages");
    auto buf = detail::StreamBuffer::begin_frame(output, Message::msg_type, header);
    if (!buf.ok()) return buf.status();
    typename Message::stream_writer_type writer{buf};
    fn(writer);
    if (const auto req = writer.validate_required(); !req.ok()) return req;
    return buf.finish_frame();
}

// FIX 4.4 convenience spelling.
template <class Message, class Fn>
    requires detail::streamable_message<Message>
[[nodiscard]] Result<std::size_t> encode_fix44_stream(
    SessionHeader header, std::span<char> output, Fn&& fn) noexcept {
    header.version = FixVersion::fix44;
    return encode_stream<Message>(header, output, std::forward<Fn>(fn));
}

// Stream only application body fields (plus optional dynamic standard header fields via w.header())
// directly into output, without session envelope/checksum.
template <class Message, class Fn>
    requires detail::streamable_message<Message>
[[nodiscard]] Result<std::size_t> encode_body_stream(
    std::span<char> output, Fn&& fn) noexcept {
    static_assert(!requires { requires Message::is_group_entry; },
        "repeating group entries cannot be encoded as standalone FIX messages");
    detail::StreamBuffer buf{output};
    typename Message::stream_writer_type writer{buf};
    fn(writer);
    if (const auto req = writer.validate_required(); !req.ok()) return req;
    return buf.finish_body();
}

template <class Message, class Fn>
    requires detail::streamable_message<Message>
[[nodiscard]] Result<std::size_t> encode_session_stream(
    std::span<char> output, const detail::StaticSessionPrefix& prefix,
    std::uint64_t msg_seq_num, std::string_view prevalidated_wall_time,
    std::optional<std::uint64_t> last_processed_seq, Fn&& fn,
    detail::StreamBuffer::GrowFn grow_fn = nullptr, void* grow_ctx = nullptr) noexcept {
    static_assert(!requires { requires Message::is_group_entry; },
        "repeating group entries cannot be encoded as standalone FIX messages");
    auto buf = detail::StreamBuffer::begin_session_frame(
        output, Message::msg_type, prefix, msg_seq_num, prevalidated_wall_time,
        last_processed_seq, grow_fn, grow_ctx);
    if (!buf.ok()) return buf.status();
    typename Message::stream_writer_type writer{buf};
    fn(writer);
    if (const auto req = writer.validate_required(); !req.ok()) return req;
    return buf.finish_frame();
}

namespace detail {

// Stack-first body streaming helper for Layer 1/2 send_stream() entry points
// (SessionContext, SessionDriver, Sender, ProducerLane). Avoids heap allocation
// for messages up to 4 KiB and spills seamlessly into a heap buffer if exceeded
// so the user's encoding callable is guaranteed to be invoked at most once.
template <class Message, class Fn, class Sink>
    requires streamable_message<Message>
[[nodiscard]] auto stream_body_and_invoke(Fn&& fn, Sink&& sink) noexcept {
    using ReturnType = decltype(sink(std::declval<MessageView>()));
    static_assert(!requires { requires Message::is_group_entry; },
        "repeating group entries cannot be sent as standalone FIX messages");
    struct SpillState {
        std::vector<char> heap_buf{};
        bool too_large{false};
    } spill{};
    auto grow = +[](void* raw, const char* old_data, std::size_t written,
                    std::size_t min_cap) noexcept -> std::span<char> {
        auto* st = static_cast<SpillState*>(raw);
        constexpr std::size_t max_body_cap = 1024U * 1024U;
        if (min_cap > max_body_cap) {
            st->too_large = true;
            return {};
        }
        std::size_t next_cap = st->heap_buf.empty() ? 16384U : st->heap_buf.size() * 2U;
        while (next_cap < min_cap) next_cap *= 2U;
        if (next_cap > max_body_cap) next_cap = max_body_cap;
        try {
            if (st->heap_buf.empty()) {
                st->heap_buf.resize(next_cap);
                if (written != 0) std::memcpy(st->heap_buf.data(), old_data, written);
            } else {
                st->heap_buf.resize(next_cap);
            }
            return std::span<char>{st->heap_buf.data(), st->heap_buf.size()};
        } catch (...) {
            return {};
        }
    };
    char stack_buf[4096];
    StreamBuffer buf{std::span<char>{stack_buf, sizeof(stack_buf)}, grow, &spill};
    typename Message::stream_writer_type writer{buf};
    fn(writer);
    if (const auto req = writer.validate_required(); !req.ok()) {
        if (spill.too_large && req.code == ErrorCode::capacity_exceeded) {
            return ReturnType{Status{ErrorCode::too_large}};
        }
        return ReturnType{req};
    }
    const auto body_len = buf.finish_body();
    if (!body_len.ok()) {
        if (spill.too_large && body_len.status().code == ErrorCode::capacity_exceeded) {
            return ReturnType{Status{ErrorCode::too_large}};
        }
        return ReturnType{body_len.status()};
    }
    return sink(make_prebuilt_body_view(
        Message::msg_type, std::string_view{buf.data(), body_len.value()}));
}

} // namespace detail

} // namespace nimblefix
