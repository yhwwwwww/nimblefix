#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <nimblefix/codec.hpp>
#include <nimblefix/journal.hpp>
#include <nimblefix/status.hpp>

namespace nimblefix {

enum class SessionRole : std::uint8_t {
    initiator,
    acceptor,
};

enum class SessionState : std::uint8_t {
    disconnected,
    awaiting_logon,
    active,
    logout_pending,
    closed,
    failed,
};

enum class ReceiptState : std::uint8_t {
    appended,
    written,
    failed,
    expired,
};

struct SessionConfig {
    SessionRole role{SessionRole::initiator};
    std::string name{};
    std::string storage_directory{};
    std::string session_key{}; // Stable FIX version/CompID/qualifier identity.
    std::string local_comp_id{};
    std::string remote_comp_id{};

    std::uint32_t heartbeat_ms{30'000};
    std::uint32_t logon_timeout_ms{10'000};
    std::uint32_t write_stall_ms{30'000};
    std::uint32_t logout_timeout_ms{5'000};
    std::uint32_t max_pending_age_ms{}; // Zero disables message-age expiry.

    std::size_t max_frame_bytes{default_max_frame_bytes};
    std::size_t max_pending_messages{256};
    std::size_t max_pending_bytes{4U * 1024U * 1024U};
    std::size_t admin_reserve_messages{8};
    std::size_t admin_reserve_bytes{4U * default_max_frame_bytes};
    std::size_t max_pending_inbound_messages{128};
    std::size_t max_pending_inbound_bytes{4U * 1024U * 1024U};
    std::size_t receipt_capacity{1024};

    std::size_t journal_capacity_bytes{16U * 1024U * 1024U};
    std::size_t journal_max_records{65'536};
    JournalTestHook journal_test_hook{};
};

struct DeliveryInfo {
    RecordId record_id{};
    std::uint64_t msg_seq_num{};
    bool is_recovery{};
};

struct SendReceipt {
    std::uint64_t id{};
    std::uint64_t msg_seq_num{};
    ReceiptState state{ReceiptState::appended};
    Status status{};
};

struct WriteResult {
    std::size_t bytes_written{};
    bool would_block{};
    Status status{};
};

using WriteFn = WriteResult (*)(void* context, std::span<const char> bytes) noexcept;

class SessionDriver;

// A callback-scoped handle. The engine owns the session and this object is not
// valid after its callback returns.
class SessionContext {
public:
    SessionContext(const SessionContext&) = delete;
    SessionContext& operator=(const SessionContext&) = delete;
    SessionContext(SessionContext&&) = delete;
    SessionContext& operator=(SessionContext&&) = delete;

    [[nodiscard]] Result<SendReceipt> send(detail::MessageView message) noexcept;

    template <class Message>
        requires requires(const Message& message) { message.view(); }
    [[nodiscard]] Result<SendReceipt> send(const Message& message) noexcept {
        return send(message.view());
    }

    // Session logout may be requested from the active callback. Message
    // delivery callbacks use send() for durable replies; logout there would
    // be replayed as a new session action after an interrupted callback.
    [[nodiscard]] Status logout(std::string_view reason = {}) noexcept;
    [[nodiscard]] SessionState state() const noexcept;
    [[nodiscard]] bool is_recovery() const noexcept { return delivery_.is_recovery; }
    [[nodiscard]] RecordId inbound_record_id() const noexcept { return delivery_.record_id; }
    [[nodiscard]] std::uint64_t inbound_msg_seq_num() const noexcept { return delivery_.msg_seq_num; }

private:
    friend class SessionDriver;
    SessionContext(SessionDriver& driver, std::uint64_t now_ms,
        std::string_view wall_time, DeliveryInfo delivery) noexcept
        : driver_(&driver), now_ms_(now_ms), wall_time_(wall_time), delivery_(delivery) {}

    SessionDriver* driver_{};
    std::uint64_t now_ms_{};
    std::string_view wall_time_{};
    DeliveryInfo delivery_{};
    std::size_t send_ordinal_{};
};

struct SessionCallbacks {
    // The message view and SessionContext are valid only for this call. Return
    // success to append the delivery-complete journal record.
    std::function<Status(SessionContext&, const FlatMessageView&, const DeliveryInfo&)> on_message{};
    std::function<Status(SessionContext&)> on_active{};
    std::function<void(Status)> on_error{};
    std::function<void()> on_capacity{};
};

namespace detail {
class SessionCore;
}

class SessionDriver {
public:
    SessionDriver(const SessionDriver&) = delete;
    SessionDriver& operator=(const SessionDriver&) = delete;
    SessionDriver(SessionDriver&&) noexcept;
    SessionDriver& operator=(SessionDriver&&) noexcept;
    ~SessionDriver();

    [[nodiscard]] static Result<SessionDriver> create(SessionConfig config,
        SessionCallbacks callbacks = {}) noexcept;

    // The transport calls connected after a new byte stream is ready. Times
    // are monotonic milliseconds and a FIX UTC timestamp string respectively.
    [[nodiscard]] Status connected(std::uint64_t now_ms, std::string_view wall_time) noexcept;
    [[nodiscard]] Status receive(std::string_view complete_frame, std::uint64_t now_ms,
        std::string_view wall_time) noexcept;
    [[nodiscard]] Status tick(std::uint64_t now_ms, std::string_view wall_time) noexcept;
    [[nodiscard]] Status disconnected(Status reason, std::uint64_t now_ms) noexcept;
    [[nodiscard]] Status logout(std::string_view reason, std::uint64_t now_ms,
        std::string_view wall_time) noexcept;

    // The message is encoded and journaled before this call returns. Rejected
    // sends do not consume or retain the caller's message.
    [[nodiscard]] Result<SendReceipt> send(detail::MessageView message,
        std::uint64_t now_ms, std::string_view wall_time) noexcept;

    template <class Message>
        requires requires(const Message& message) { message.view(); }
    [[nodiscard]] Result<SendReceipt> send(const Message& message,
        std::uint64_t now_ms, std::string_view wall_time) noexcept {
        return send(message.view(), now_ms, wall_time);
    }

    // Performs at most byte_budget bytes of transport work. Short writes retain
    // their offset; would_block leaves the queue unchanged.
    [[nodiscard]] Status flush(WriteFn write, void* context, std::uint64_t now_ms,
        std::size_t byte_budget) noexcept;

    [[nodiscard]] Result<SendReceipt> receipt(std::uint64_t id) const noexcept;
    [[nodiscard]] SessionState state() const noexcept;
    [[nodiscard]] Status status() const noexcept;
    [[nodiscard]] bool needs_write() const noexcept;
    [[nodiscard]] bool should_close() const noexcept;
    [[nodiscard]] bool can_send() const noexcept;
    [[nodiscard]] std::uint64_t next_deadline_ms() const noexcept;
    [[nodiscard]] std::uint64_t next_inbound_seq() const noexcept;
    [[nodiscard]] std::uint64_t next_outbound_seq() const noexcept;
    [[nodiscard]] std::size_t pending_messages() const noexcept;
    [[nodiscard]] std::size_t pending_bytes() const noexcept;

private:
    friend class SessionContext;
    struct Impl;
    explicit SessionDriver(std::unique_ptr<Impl> impl) noexcept;
    [[nodiscard]] Result<SendReceipt> send_dependent(detail::MessageView message,
        std::uint64_t now_ms, std::string_view wall_time, RecordId dependency,
        std::size_t dependency_ordinal, bool recovery) noexcept;
    std::unique_ptr<Impl> impl_{};
};

} // namespace nimblefix
