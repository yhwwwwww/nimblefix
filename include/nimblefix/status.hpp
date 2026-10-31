#pragma once

#include <cstddef>
#include <string_view>
#include <utility>
#include <variant>

namespace nimblefix {

enum class ErrorCode {
    none,
    need_more,
    invalid_frame,
    invalid_body_length,
    invalid_checksum,
    invalid_tag,
    invalid_value,
    invalid_header,
    too_large,
    capacity_exceeded,
    missing_field,
    duplicate_field,
    managed_field,
    unsupported,
    invalid_decimal,
    invalid_timestamp,
    required_field_missing,
    io_error,
    storage_full,
    store_locked,
    corrupt_store,
    identity_mismatch,
    unsupported_version,
    invalid_config,
    not_ready,
    queue_full,
    engine_stopped,
    callback_failed,
    protocol_error,
    sequence_gap,
    duplicate_sequence,
    write_stalled,
    logon_timeout,
    heartbeat_timeout,
    connection_closed,
    receipt_expired,
    wrong_thread,
    logout_timeout,
    authentication_failed,
    tls_error,
    certificate_error,
    tls_handshake_timeout,
    unsupported_protocol,
    schema_mismatch,
    backlog_timeout,
    message_unhandled,
    history_unavailable,
    reader_expired,
    schedule_closed,
    sequence_reset_denied,
    peer_rejected,
};

struct [[nodiscard]] Status {
    ErrorCode code{ErrorCode::none};
    int tag{};
    std::size_t offset{};

    [[nodiscard]] constexpr bool ok() const noexcept { return code == ErrorCode::none; }
    [[nodiscard]] static constexpr Status success() noexcept { return {}; }
    [[nodiscard]] constexpr std::string_view message() const noexcept {
        switch (code) {
        case ErrorCode::none: return "success";
        case ErrorCode::need_more: return "incomplete frame";
        case ErrorCode::invalid_frame: return "malformed FIX frame";
        case ErrorCode::invalid_body_length: return "invalid BodyLength";
        case ErrorCode::invalid_checksum: return "invalid CheckSum";
        case ErrorCode::invalid_tag: return "invalid field tag";
        case ErrorCode::invalid_value: return "invalid field value";
        case ErrorCode::invalid_header: return "invalid FIX header";
        case ErrorCode::too_large: return "frame exceeds configured limit";
        case ErrorCode::capacity_exceeded: return "bounded storage exhausted";
        case ErrorCode::missing_field: return "field is missing";
        case ErrorCode::duplicate_field: return "field occurs more than once";
        case ErrorCode::managed_field: return "field is managed by the engine";
        case ErrorCode::unsupported: return "unsupported capability";
        case ErrorCode::invalid_decimal: return "invalid exact decimal";
        case ErrorCode::invalid_timestamp: return "invalid UTC timestamp";
        case ErrorCode::required_field_missing: return "required field is missing";
        case ErrorCode::io_error: return "system I/O operation failed";
        case ErrorCode::storage_full: return "bounded journal capacity exhausted";
        case ErrorCode::store_locked: return "journal already has a writer";
        case ErrorCode::corrupt_store: return "committed journal data is corrupt";
        case ErrorCode::identity_mismatch: return "persistent session identity differs";
        case ErrorCode::unsupported_version: return "unsupported storage format version";
        case ErrorCode::invalid_config: return "invalid engine or session configuration";
        case ErrorCode::not_ready: return "session is not active";
        case ErrorCode::queue_full: return "bounded send capacity exhausted";
        case ErrorCode::engine_stopped: return "engine has stopped";
        case ErrorCode::callback_failed: return "application callback failed";
        case ErrorCode::protocol_error: return "FIX session protocol violation";
        case ErrorCode::sequence_gap: return "FIX sequence gap requires recovery";
        case ErrorCode::duplicate_sequence: return "unexpected lower FIX sequence";
        case ErrorCode::write_stalled: return "transport write progress timed out";
        case ErrorCode::logon_timeout: return "FIX Logon timed out";
        case ErrorCode::heartbeat_timeout: return "FIX heartbeat response timed out";
        case ErrorCode::connection_closed: return "transport connection closed";
        case ErrorCode::receipt_expired: return "send receipt retention expired";
        case ErrorCode::wrong_thread: return "operation requires the session owner thread";
        case ErrorCode::logout_timeout: return "FIX Logout acknowledgement timed out";
        case ErrorCode::authentication_failed: return "FIX Logon authentication failed";
        case ErrorCode::tls_error: return "TLS protocol operation failed";
        case ErrorCode::certificate_error: return "TLS certificate verification failed";
        case ErrorCode::tls_handshake_timeout: return "TLS handshake timed out";
        case ErrorCode::unsupported_protocol: return "unsupported FIX transport or application version";
        case ErrorCode::schema_mismatch: return "XML and generated schema definitions differ";
        case ErrorCode::backlog_timeout: return "bounded application backlog timed out";
        case ErrorCode::message_unhandled: return "no registered handler for this message type";
        case ErrorCode::history_unavailable: return "requested FIX replay history is no longer retained";
        case ErrorCode::reader_expired: return "journal reader exceeded its permitted lag";
        case ErrorCode::schedule_closed: return "FIX session is outside its configured schedule";
        case ErrorCode::peer_rejected: return "peer rejected a FIX message";
        case ErrorCode::sequence_reset_denied: return "FIX sequence reset is not permitted by policy";
        }
        return "unknown error";
    }
};

template <class T>
class [[nodiscard]] Result {
public:
    Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
    Result(Status error) : storage_(std::in_place_index<1>, error.ok()
        ? Status{ErrorCode::invalid_value} : error) {}

    [[nodiscard]] bool ok() const noexcept { return storage_.index() == 0; }
    [[nodiscard]] Status status() const noexcept {
        return ok() ? Status::success() : std::get<1>(storage_);
    }
    [[nodiscard]] Status error() const noexcept { return status(); }
    [[nodiscard]] T& value() & { return std::get<0>(storage_); }
    [[nodiscard]] const T& value() const& { return std::get<0>(storage_); }
    [[nodiscard]] T&& value() && { return std::get<0>(std::move(storage_)); }

private:
    std::variant<T, Status> storage_;
};

} // namespace nimblefix
