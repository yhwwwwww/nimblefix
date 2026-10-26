#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <nimblefix/session.hpp>

namespace nimblefix::detail {

enum class CoreEventKind : std::uint8_t {
    connected,
    disconnected,
    inbound_candidate,
    inbound_committed,
    out_of_order_logon,
    outbound_appended,
    outbound_written,
    outbound_skipped,
    logon_reply_appended,
    tick,
    local_logout,
};

enum class CoreActionKind : std::uint8_t {
    send_logon,
    send_logout,
    send_heartbeat,
    send_test_request,
    send_resend_request,
    start_replay,
    become_active,
    deliver_message,
    close_transport,
};

enum class InboundDisposition : std::uint8_t {
    none,
    accept,
    duplicate,
    gap,
};

struct CoreAction {
    CoreActionKind kind{};
    std::uint64_t first{};
    std::uint64_t last{};
    std::string_view text{};
};

struct CoreOutput {
    Status status{};
    InboundDisposition inbound{InboundDisposition::none};
    std::array<CoreAction, 4> actions{};
    std::size_t action_count{};

    void push(CoreAction action) noexcept {
        if (action_count < actions.size()) actions[action_count++] = action;
        else status = Status{ErrorCode::capacity_exceeded};
    }
};

struct CoreEvent {
    CoreEventKind kind{};
    std::uint64_t now_ms{};
    std::uint64_t sequence{};
    std::uint64_t next_sequence{};
    std::uint64_t first{};
    std::uint64_t last{};
    std::string_view msg_type{};
    std::string_view text{};
    Status status{};
    bool flag{};
    bool flag2{};
    bool flag3{};
};

class SessionCore {
public:
    SessionCore(SessionRole role, std::uint32_t heartbeat_ms,
        std::uint32_t logon_timeout_ms, std::uint32_t logout_timeout_ms,
        std::uint64_t next_inbound, std::uint64_t next_outbound) noexcept
        : role_(role), heartbeat_ms_(heartbeat_ms), logon_timeout_ms_(logon_timeout_ms),
          logout_timeout_ms_(logout_timeout_ms), next_inbound_(next_inbound), next_outbound_(next_outbound) {}

    [[nodiscard]] CoreOutput process(const CoreEvent& event) noexcept;
    [[nodiscard]] SessionState state() const noexcept { return state_; }
    [[nodiscard]] std::uint64_t next_inbound() const noexcept { return next_inbound_; }
    [[nodiscard]] std::uint64_t next_outbound() const noexcept { return next_outbound_; }
    [[nodiscard]] std::uint64_t next_deadline() const noexcept;
    [[nodiscard]] bool should_close() const noexcept { return should_close_; }
    [[nodiscard]] Status failure() const noexcept { return failure_; }

private:
    [[nodiscard]] std::uint64_t saturating_add(std::uint64_t value, std::uint64_t amount) const noexcept;
    void enter_active(CoreOutput& output) noexcept;
    void enter_logout(std::string_view reason, std::uint64_t now_ms, CoreOutput& output) noexcept;
    void write_error(Status status, CoreOutput& output) noexcept;
    [[nodiscard]] bool is_admin(std::string_view msg_type) const noexcept;
    void update_test_id() noexcept;

    SessionRole role_{};
    SessionState state_{SessionState::disconnected};
    std::uint32_t heartbeat_ms_{};
    std::uint32_t peer_heartbeat_ms_{};
    std::uint32_t logon_timeout_ms_{};
    std::uint32_t logout_timeout_ms_{};
    std::uint64_t next_inbound_{1};
    std::uint64_t next_outbound_{1};
    std::uint64_t logon_deadline_{};
    std::uint64_t logout_deadline_{};
    std::uint64_t last_rx_ms_{};
    std::uint64_t last_tx_ms_{};
    std::uint64_t test_deadline_{};
    std::uint64_t resend_end_{};
    std::uint64_t test_counter_{};
    std::array<char, 32> test_id_buffer_{};
    std::size_t test_id_size_{};
    Status failure_{};
    bool connected_{};
    bool local_logon_sent_{};
    bool remote_logon_seen_{};
    bool logon_reply_pending_{};
    bool test_pending_{};
    bool heartbeat_pending_{};
    bool resend_pending_{};
    bool close_after_logout_{};
    bool local_logout_written_{};
    bool should_close_{};
};

} // namespace nimblefix::detail
