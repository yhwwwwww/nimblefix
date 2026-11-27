#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nimblefix/codec.hpp>
#include <nimblefix/protocol.hpp>
#include <nimblefix/schema.hpp>

namespace nimblefix {

struct LogonField {
    std::uint32_t tag{};
    std::string value;
    bool sensitive{};
};

struct LogonConfig {
    std::string username;
    std::string password;
    std::vector<LogonField> extra_fields;
    bool request_sequence_reset{};
};

enum class AuthenticationMethod : std::uint8_t { none, credentials, callback };

struct AuthenticationConfig {
    AuthenticationMethod method{AuthenticationMethod::none};
    std::string username;
    std::string password;
    bool require_tls{true};
    std::function<Status(const FlatMessageView &)> verify;
};

// UTC windows avoid implicit local timezone or DST changes. Bit 0 is Sunday.
// An overnight window belongs to the weekday on which it starts.
struct SessionSchedule {
    bool enabled{};
    std::uint8_t weekdays{all_weekdays_mask};
    std::uint32_t start_second_utc{};
    std::uint32_t end_second_utc{seconds_per_day};
    // Opt-in EOD/BOD sequence reset on schedule window transition.
    // Requires enabled == true and SessionSettings::allow_sequence_reset == true.
    bool reset_on_schedule_boundary{false};

    [[nodiscard]] constexpr Status validate() const noexcept {
        if (!enabled)
            return Status::success();
        if (weekdays == 0 || (weekdays & ~all_weekdays_mask) != 0 ||
            start_second_utc >= seconds_per_day || end_second_utc > seconds_per_day ||
            start_second_utc == end_second_utc)
            return {ErrorCode::invalid_config};
        return Status::success();
    }

    // Returns the UTC epoch-second start of the schedule window containing `time`,
    // or 0 if `time` is outside the schedule (or the schedule is disabled/invalid).
    [[nodiscard]] std::int64_t window_id(std::chrono::system_clock::time_point time) const noexcept {
        using namespace std::chrono;
        if (!enabled || !validate().ok())
            return 0;
        const auto day = floor<days>(time);
        const auto second = duration_cast<seconds>(time - day).count();
        const auto current_day = weekday(day).c_encoding();
        const auto allowed = [this](unsigned index) noexcept { return (weekdays & (1U << index)) != 0; };
        if (start_second_utc < end_second_utc) {
            if (!allowed(current_day) || second < start_second_utc || second >= end_second_utc)
                return 0;
            const auto start_tp = day + seconds{start_second_utc};
            const auto epoch_sec = duration_cast<seconds>(start_tp.time_since_epoch()).count();
            return epoch_sec == 0 ? 1 : epoch_sec;
        }
        if (second >= start_second_utc) {
            if (!allowed(current_day)) return 0;
            const auto start_tp = day + seconds{start_second_utc};
            const auto epoch_sec = duration_cast<seconds>(start_tp.time_since_epoch()).count();
            return epoch_sec == 0 ? 1 : epoch_sec;
        }
        if (second < end_second_utc && allowed((current_day + 6U) % 7U)) {
            const auto start_tp = (day - days{1}) + seconds{start_second_utc};
            const auto epoch_sec = duration_cast<seconds>(start_tp.time_since_epoch()).count();
            return epoch_sec == 0 ? 1 : epoch_sec;
        }
        return 0;
    }

    [[nodiscard]] bool contains(std::chrono::system_clock::time_point time) const noexcept {
        if (!enabled)
            return true;
        return window_id(time) != 0;
    }
};

struct FlowControlConfig {
    std::size_t max_pending_messages{256};
    std::size_t max_pending_bytes{4U * 1024U * 1024U};
    std::size_t max_pending_inbound_messages{128};
    std::size_t max_pending_inbound_bytes{4U * 1024U * 1024U};
    std::uint32_t max_pending_age_ms{30'000};
    std::uint32_t max_delivery_age_ms{30'000};
    std::uint32_t receive_pause_timeout_ms{30'000};
    std::uint32_t write_stall_ms{30'000};
};

struct SessionSettings {
    FixVersion version{FixVersion::fix44};
    std::string qualifier;
    std::string default_appl_ver_id{appl_ver_id::fix50sp2};
    LogonConfig logon;
    AuthenticationConfig authentication;
    SessionSchedule schedule;
    std::optional<FlowControlConfig> flow_control;
    bool allow_sequence_reset{};
    bool emit_last_processed_seq{true};
    bool check_sending_time{true};
    std::uint32_t max_sending_time_skew_ms{120'000};
    TimestampPrecision timestamp_precision{TimestampPrecision::milliseconds};
    bool verify_checksum{true};
    UnexpectedFieldPolicy unexpected_field_policy{UnexpectedFieldPolicy::reject};
    // Optional per-session schema override for heterogeneous multi-schema engines.
    // The referenced SchemaDescriptor (and wire hints storage) must outlive the Engine/SessionDriver.
    const SchemaDescriptor* schema{nullptr};
    WireHints schema_wire_hints{};

    template <class Schema>
        requires requires {
            { Schema::descriptor() } -> std::same_as<const SchemaDescriptor&>;
            { Schema::wire_hints() } -> std::same_as<WireHints>;
        }
    SessionSettings& use_schema() noexcept {
        schema = &Schema::descriptor();
        schema_wire_hints = Schema::wire_hints();
        return *this;
    }

    SessionSettings& use_schema(const SchemaDescriptor& descriptor, WireHints hints = {}) noexcept {
        schema = &descriptor;
        schema_wire_hints = hints;
        return *this;
    }
};

[[nodiscard]] Status validate_session_settings(const SessionSettings &settings, bool acceptor,
                                               bool tls_enabled) noexcept;
[[nodiscard]] Status authenticate_logon(const AuthenticationConfig &authentication,
                                        const FlatMessageView &message, bool tls_enabled) noexcept;

} // namespace nimblefix
