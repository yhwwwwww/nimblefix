#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <nimblefix/codec.hpp>
#include <nimblefix/journal.hpp>
#include <nimblefix/settings.hpp>
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
    accepted,
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

    // Tail-only runtime/storage settings keep older designated initializers
    // source-compatible. A null settings value preserves the direct Driver
    // compatibility behavior used by low-level tests and embedders.
    std::optional<SessionSettings> settings{};
    bool tls_verified{};
    std::size_t journal_segment_bytes{};
    std::size_t journal_max_segments{};
    std::size_t retain_outbound_messages{};
    // Engine acceptor paths may authenticate the first Logon before creating
    // a Driver so they can safely route the connection by session identity.
    // Direct Driver users leave this false and let the Driver authenticate.
    bool logon_preauthenticated{};
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

namespace detail {
class EngineRuntime;
class SenderSharedState;
}

// A bounded asynchronous delivery lease. The frame refers either to the
// committed journal mapping or to the configured descriptor-ring byte arena;
// owner keeps that storage alive until the worker releases the lease.
struct DeliveryLease {
    DeliveryInfo info{};
    std::string_view frame{};
    JournalLease journal_lease{};
};

struct WriteResult {
    std::size_t bytes_written{};
    bool would_block{};
    Status status{};
};

using WriteFn = WriteResult (*)(void* context, std::span<const char> bytes) noexcept;
using WriteBatchFn = WriteResult (*)(void* context, std::span<const std::span<const char>> chunks) noexcept;

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
        static_assert(!requires { requires std::remove_cvref_t<Message>::is_group_entry; },
            "repeating group entries cannot be sent as standalone FIX messages");
        return send(message.view());
    }

    template <class Message, class Fn>
        requires detail::streamable_message<Message>
    [[nodiscard]] Result<SendReceipt> send_stream(Fn&& fn) noexcept;

    // Session logout may be requested from the active callback. Message
    // delivery callbacks use send() for durable replies; logout there would
    // be replayed as a new session action after an interrupted callback.
    [[nodiscard]] Status logout(std::string_view reason = {}) noexcept;
    [[nodiscard]] SessionState state() const noexcept;
    [[nodiscard]] bool is_recovery() const noexcept { return delivery_.is_recovery; }
    [[nodiscard]] std::string_view session_name() const noexcept { return session_name_; }
    [[nodiscard]] std::string_view local_comp_id() const noexcept { return local_comp_id_; }
    [[nodiscard]] std::string_view remote_comp_id() const noexcept { return remote_comp_id_; }
    [[nodiscard]] RecordId inbound_record_id() const noexcept { return delivery_.record_id; }
    [[nodiscard]] std::uint64_t inbound_msg_seq_num() const noexcept { return delivery_.msg_seq_num; }
    [[nodiscard]] std::size_t accepted_sends() const noexcept { return send_ordinal_; }

private:
    friend class SessionDriver;
    friend class detail::EngineRuntime;

    using SendFunction = Result<SendReceipt>(*)(void*, detail::MessageView,
        std::uint64_t, std::string_view, DeliveryInfo, std::size_t,
        std::uint64_t) noexcept;

    SessionContext(SessionDriver& driver, std::uint64_t now_ms,
        std::string_view wall_time, DeliveryInfo delivery) noexcept;

    SessionContext(std::shared_ptr<detail::SenderSharedState> sender,
        SendFunction send_function, SessionState state, std::uint64_t now_ms,
        std::string_view wall_time, DeliveryInfo delivery,
        std::string_view session_name, std::string_view local_comp_id,
        std::string_view remote_comp_id,
        std::uint64_t generation = 0) noexcept
        : send_context_(sender.get()), send_function_(send_function),
          sender_lifetime_(std::move(sender)), now_ms_(now_ms), wall_time_(wall_time),
          delivery_(delivery), state_snapshot_(state), generation_(generation),
          session_name_(session_name), local_comp_id_(local_comp_id),
          remote_comp_id_(remote_comp_id) {}

    static Result<SendReceipt> send_inline(void* context, detail::MessageView message,
        std::uint64_t now_ms, std::string_view wall_time, DeliveryInfo delivery,
        std::size_t ordinal, std::uint64_t generation) noexcept;

    SessionDriver* driver_{};
    void* send_context_{};
    SendFunction send_function_{};
    std::shared_ptr<detail::SenderSharedState> sender_lifetime_{};
    std::uint64_t now_ms_{};
    std::string_view wall_time_{};
    DeliveryInfo delivery_{};
    std::size_t send_ordinal_{};
    SessionState state_snapshot_{SessionState::closed};
    std::uint64_t generation_{};
    std::string_view session_name_{};
    std::string_view local_comp_id_{};
    std::string_view remote_comp_id_{};
};

struct SessionCallbacks {
    // The message view and SessionContext are valid only for this call. Return
    // success to append the delivery-complete journal record.
    std::function<Status(SessionContext&, const FlatMessageView&, const DeliveryInfo&)> on_message{};
    // Schema validation is a separate protocol boundary from business callback
    // failure, so invalid typed input never reaches application code.
    std::function<Status(const FlatMessageView&)> validate_message{};
    WireHints wire_hints{};
    std::function<Status()> on_deferred_active{};
    bool deferred_active{};
    // When present, the driver calls this on the owner with a committed inbound
    // record. A successful callback must call defer_delivery() after enqueueing
    // that record; queue_full leaves it pending for a later retry.
    std::function<Status(const DeliveryLease&)> on_deferred_message{};
    bool deferred_delivery{};
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
    // Fast path for callers that already ran inspect_frame() on complete_frame.
    [[nodiscard]] Status receive_verified(FrameView verified_frame, std::uint64_t now_ms,
        std::string_view wall_time) noexcept;
    [[nodiscard]] Status defer_delivery(DeliveryInfo info) noexcept;
    [[nodiscard]] Status defer_active() noexcept;
    [[nodiscard]] Status complete_active(Status callback_status,
        std::uint64_t now_ms, std::string_view wall_time) noexcept;
    [[nodiscard]] Status complete_delivery(DeliveryInfo info, Status callback_status,
        std::size_t accepted_replies, std::uint64_t now_ms,
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
        static_assert(!requires { requires std::remove_cvref_t<Message>::is_group_entry; },
            "repeating group entries cannot be sent as standalone FIX messages");
        return send(message.view(), now_ms, wall_time);
    }

    template <class Message, class Fn>
        requires detail::streamable_message<Message>
    [[nodiscard]] Result<SendReceipt> send_stream(Fn&& fn,
        std::uint64_t now_ms, std::string_view wall_time) noexcept {
        static_assert(!requires { requires Message::is_group_entry; },
            "repeating group entries cannot be sent as standalone FIX messages");
        auto& ref = fn;
        return send_stream_dependent(Message::msg_type,
            +[](void* ctx, std::span<char> out, const detail::StaticSessionPrefix& prefix,
                std::uint64_t seq, std::string_view wt,
                std::optional<std::uint64_t> last_proc,
                detail::StreamBuffer::GrowFn grow_fn, void* grow_ctx) noexcept -> Result<std::size_t> {
                return encode_session_stream<Message>(
                    out, prefix, seq, wt, last_proc,
                    *static_cast<std::remove_reference_t<Fn>*>(ctx), grow_fn, grow_ctx);
            },
            &ref, now_ms, wall_time, 0, 0, false);
    }

    // Performs at most byte_budget bytes of transport work. Short writes retain
    // their offset; would_block leaves the queue unchanged.
    [[nodiscard]] Status flush(WriteFn write, void* context, std::uint64_t now_ms,
        std::size_t byte_budget) noexcept;
    [[nodiscard]] Status flush_batch(WriteBatchFn write_batch, WriteFn write_single,
        void* context, std::uint64_t now_ms, std::size_t byte_budget) noexcept;

    [[nodiscard]] Result<SendReceipt> receipt(std::uint64_t id) const noexcept;
    [[nodiscard]] SessionState state() const noexcept;
    [[nodiscard]] Status status() const noexcept;
    [[nodiscard]] bool needs_write() const noexcept;
    [[nodiscard]] bool should_close() const noexcept;
    [[nodiscard]] bool can_send() const noexcept;
    // Safe before reading another transport frame. Each accepted frame may
    // consume max_frame_bytes from the bounded inbound callback/recovery area.
    [[nodiscard]] bool can_receive() const noexcept;
    [[nodiscard]] std::uint64_t next_deadline_ms() const noexcept;
    [[nodiscard]] std::uint64_t next_inbound_seq() const noexcept;
    [[nodiscard]] std::uint64_t next_outbound_seq() const noexcept;
    [[nodiscard]] std::size_t pending_messages() const noexcept;
    [[nodiscard]] std::size_t pending_bytes() const noexcept;
    [[nodiscard]] std::size_t pending_inbound_messages() const noexcept;
    [[nodiscard]] std::size_t pending_inbound_bytes() const noexcept;

private:
    friend class SessionContext;
    friend class detail::EngineRuntime;
    struct Impl;
    explicit SessionDriver(std::unique_ptr<Impl> impl) noexcept;
    using StreamEncodeFn = Result<std::size_t> (*)(
        void* ctx, std::span<char> out, const detail::StaticSessionPrefix& prefix,
        std::uint64_t seq, std::string_view wall_time,
        std::optional<std::uint64_t> last_processed,
        detail::StreamBuffer::GrowFn grow_fn, void* grow_ctx) noexcept;

    [[nodiscard]] Result<SendReceipt> send_dependent(detail::MessageView message,
        std::uint64_t now_ms, std::string_view wall_time, RecordId dependency,
        std::size_t dependency_ordinal, bool recovery) noexcept;
    [[nodiscard]] Result<SendReceipt> send_stream_dependent(std::string_view msg_type,
        StreamEncodeFn encode_fn, void* encode_ctx,
        std::uint64_t now_ms, std::string_view wall_time, RecordId dependency,
        std::size_t dependency_ordinal, bool recovery) noexcept;
    // Two-phase proactor send helpers (used by EngineRuntime io_uring backend).
    [[nodiscard]] Result<std::size_t> stage_tx(std::span<char> buffer, std::uint64_t now_ms) noexcept;
    [[nodiscard]] Status commit_tx_written(std::size_t bytes_written, std::uint64_t now_ms) noexcept;
    void abort_staged_tx() noexcept;
    std::unique_ptr<Impl> impl_{};
};

template <class Message, class Fn>
    requires detail::streamable_message<Message>
[[nodiscard]] inline Result<SendReceipt> SessionContext::send_stream(Fn&& fn) noexcept {
    static_assert(!requires { requires Message::is_group_entry; },
        "repeating group entries cannot be sent as standalone FIX messages");
    if (driver_ != nullptr && !delivery_.is_recovery &&
        send_ordinal_ != std::numeric_limits<std::size_t>::max()) {
        auto& ref = fn;
        auto result = driver_->send_stream_dependent(Message::msg_type,
            +[](void* ctx, std::span<char> out, const detail::StaticSessionPrefix& prefix,
                std::uint64_t seq, std::string_view wt,
                std::optional<std::uint64_t> last_proc,
                detail::StreamBuffer::GrowFn grow_fn, void* grow_ctx) noexcept -> Result<std::size_t> {
                return encode_session_stream<Message>(
                    out, prefix, seq, wt, last_proc,
                    *static_cast<std::remove_reference_t<Fn>*>(ctx), grow_fn, grow_ctx);
            },
            &ref, now_ms_, wall_time_, delivery_.record_id, send_ordinal_, false);
        if (result.ok()) ++send_ordinal_;
        return result;
    }
    return detail::stream_body_and_invoke<Message>(std::forward<Fn>(fn),
        [this](detail::MessageView view) noexcept { return send(view); });
}

} // namespace nimblefix
