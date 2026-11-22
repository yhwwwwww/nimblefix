#include "core.hpp"

#include <charconv>
#include <limits>

namespace nimblefix::detail {

std::uint64_t SessionCore::saturating_add(std::uint64_t value, std::uint64_t amount) const noexcept {
    if (amount > std::numeric_limits<std::uint64_t>::max() - value) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return value + amount;
}

std::uint64_t SessionCore::peer_test_interval_ms() const noexcept {
    // FIX 4.4 / FIXT 1.1: HeartBtInt + reasonable transmission margin (20%).
    return saturating_add(peer_heartbeat_ms_, peer_heartbeat_ms_ / 5U);
}

void SessionCore::enter_active(CoreOutput& output) noexcept {
    if (state_ == SessionState::active) return;
    state_ = SessionState::active;
    logon_deadline_ = 0;
    logon_reply_pending_ = false;
    output.push(CoreAction{CoreActionKind::become_active});
}

void SessionCore::enter_logout(std::string_view reason, std::uint64_t now_ms,
    CoreOutput& output) noexcept {
    if (state_ == SessionState::closed || state_ == SessionState::failed ||
        state_ == SessionState::logout_pending) {
        return;
    }
    state_ = SessionState::logout_pending;
    // A locally initiated Logout must remain open for the peer's reply (or
    // logout timeout). The inbound Logout path sets this true when it needs
    // to write an acknowledgement before closing.
    close_after_logout_ = false;
    local_logout_written_ = false;
    logout_deadline_ = saturating_add(now_ms, logout_timeout_ms_);
    output.push(CoreAction{CoreActionKind::send_logout, 0, 0, reason});
}

void SessionCore::write_error(Status status, CoreOutput& output) noexcept {
    if (failure_.ok()) failure_ = status;
    state_ = SessionState::failed;
    should_close_ = true;
    output.status = failure_;
    output.push(CoreAction{CoreActionKind::close_transport});
}

bool SessionCore::is_admin(std::string_view msg_type) const noexcept {
    return msg_type == "A" || msg_type == "0" || msg_type == "1" || msg_type == "2" ||
        msg_type == "3" || msg_type == "4" || msg_type == "5";
}

void SessionCore::update_test_id() noexcept {
    ++test_counter_;
    const auto result = std::to_chars(test_id_buffer_.data(), test_id_buffer_.data() + test_id_buffer_.size(),
        test_counter_);
    test_id_size_ = result.ec == std::errc{} ? static_cast<std::size_t>(result.ptr - test_id_buffer_.data()) : 0;
}

CoreOutput SessionCore::process(const CoreEvent& event) noexcept {
    CoreOutput output{};
    switch (event.kind) {
    case CoreEventKind::connected:
        if (connected_ || state_ == SessionState::closed || state_ == SessionState::failed) {
            output.status = Status{ErrorCode::not_ready};
            break;
        }
        connected_ = true;
        should_close_ = false;
        state_ = SessionState::awaiting_logon;
        logon_deadline_ = saturating_add(event.now_ms, logon_timeout_ms_);
        last_rx_ms_ = event.now_ms;
        last_tx_ms_ = event.now_ms;
        local_logon_sent_ = false;
        remote_logon_seen_ = false;
        logon_reply_pending_ = false;
        test_pending_ = false;
        heartbeat_pending_ = false;
        resend_pending_ = false;
        resend_end_ = 0;
        resend_target_ = 0;
        local_logout_written_ = false;
        if (role_ == SessionRole::initiator) output.push(CoreAction{CoreActionKind::send_logon});
        break;

    case CoreEventKind::disconnected:
        connected_ = false;
        state_ = SessionState::disconnected;
        should_close_ = false;
        logon_deadline_ = 0;
        logout_deadline_ = 0;
        test_deadline_ = 0;
        test_pending_ = false;
        heartbeat_pending_ = false;
        resend_pending_ = false;
        resend_end_ = 0;
        resend_target_ = 0;
        local_logon_sent_ = false;
        remote_logon_seen_ = false;
        logon_reply_pending_ = false;
        break;

    case CoreEventKind::inbound_candidate: {
        if (!connected_ || state_ == SessionState::closed || state_ == SessionState::failed) {
            output.status = Status{ErrorCode::not_ready};
            break;
        }
        // Pre-Logon security boundary: an unauthenticated acceptor MUST NOT process
        // any message type other than Logon (35=A) before remote Logon is seen.
        if (state_ == SessionState::awaiting_logon && !remote_logon_seen_) {
            const bool initiator_pre_logon_allowed = role_ == SessionRole::initiator && local_logon_sent_ &&
                (event.msg_type == "5" || event.msg_type == "2" || event.msg_type == "4");
            if (event.msg_type != "A" && !initiator_pre_logon_allowed) {
                write_error(Status{ErrorCode::protocol_error, 35}, output);
                break;
            }
        }
        if (event.sequence < next_inbound_) {
            if (event.poss_dup && event.orig_sending_time_valid) {
                output.inbound = InboundDisposition::duplicate;
                last_rx_ms_ = event.now_ms;
            } else {
                write_error(Status{ErrorCode::duplicate_sequence, 34}, output);
            }
            break;
        }
        if (event.sequence > next_inbound_) {
            output.inbound = InboundDisposition::gap;
            last_rx_ms_ = event.now_ms;
            // Mutual-gap deadlock prevention: honor an authenticated peer's ResendRequest
            // even when its MsgSeqNum is higher than our expected inbound sequence.
            if (event.msg_type == "2" && (remote_logon_seen_ ||
                    (role_ == SessionRole::initiator && local_logon_sent_))) {
                output.push(CoreAction{CoreActionKind::start_replay, event.first, event.last});
            }
            if (event.msg_type == "A" && event.last != 0) {
                // High-sequence Logon: request bounded gap [next_inbound_, logon_seq - 1].
                if (!resend_pending_ || (resend_end_ != 0 && event.last > resend_end_)) {
                    resend_pending_ = true;
                    resend_end_ = event.last;
                    resend_target_ = event.last;
                    output.push(CoreAction{CoreActionKind::send_resend_request, next_inbound_, resend_end_});
                }
            } else {
                // Standard FIX gap recovery: emit a single open-ended ResendRequest (EndSeqNo=0)
                // and suppress duplicate requests while resend_pending_ is active.
                if (event.sequence > resend_target_) resend_target_ = event.sequence;
                if (!resend_pending_) {
                    resend_pending_ = true;
                    resend_end_ = 0;
                    output.push(CoreAction{CoreActionKind::send_resend_request, next_inbound_, 0});
                }
            }
            break;
        }
        const bool replayed_business = remote_logon_seen_ && event.poss_dup && event.orig_sending_time_valid;
        if (state_ == SessionState::awaiting_logon && event.msg_type != "A") {
            const bool allowed_post_remote_logon = remote_logon_seen_ &&
                (event.msg_type == "0" || event.msg_type == "2" || event.msg_type == "4" ||
                 event.msg_type == "5" || replayed_business);
            const bool allowed_initiator_pre_logon = role_ == SessionRole::initiator && local_logon_sent_ &&
                (event.msg_type == "2" || event.msg_type == "4" || event.msg_type == "5");
            if (!allowed_post_remote_logon && !allowed_initiator_pre_logon) {
                write_error(Status{ErrorCode::protocol_error, 35}, output);
                break;
            }
        }
        if (state_ == SessionState::active && event.msg_type == "A" && !event.is_deferred_logon) {
            write_error(Status{ErrorCode::protocol_error, 35}, output);
            break;
        }
        output.inbound = InboundDisposition::accept;
        break;
    }

    case CoreEventKind::inbound_committed: {
        const auto next = event.next_sequence == 0 ? event.sequence + 1 : event.next_sequence;
        if (event.sequence != next_inbound_ || next <= event.sequence) {
            write_error(Status{ErrorCode::protocol_error, 34}, output);
            break;
        }
        next_inbound_ = next;
        last_rx_ms_ = event.now_ms;
        if (resend_pending_) {
            const auto completion_seq = resend_end_ != 0 ? resend_end_ : resend_target_;
            if (next_inbound_ > completion_seq) {
                resend_pending_ = false;
                resend_end_ = 0;
                resend_target_ = 0;
            }
        }

        if (event.msg_type == "A" && event.is_deferred_logon) {
            if (state_ != SessionState::awaiting_logon || !remote_logon_seen_ || !local_logon_sent_) {
                write_error(Status{ErrorCode::protocol_error, 35}, output);
            } else {
                enter_active(output);
            }
            break;
        }

        if (event.msg_type == "A") {
            if (state_ != SessionState::awaiting_logon || remote_logon_seen_) {
                write_error(Status{ErrorCode::protocol_error, 35}, output);
                break;
            }
            remote_logon_seen_ = true;
            peer_heartbeat_ms_ = event.first == 0 ? heartbeat_ms_ : static_cast<std::uint32_t>(event.first);
            if (role_ == SessionRole::acceptor) {
                heartbeat_ms_ = peer_heartbeat_ms_;
                logon_reply_pending_ = true;
                output.push(CoreAction{CoreActionKind::send_logon});
            } else if (local_logon_sent_) {
                enter_active(output);
            } else {
                write_error(Status{ErrorCode::protocol_error, 35}, output);
            }
            break;
        }

        const bool replayed_business = remote_logon_seen_ && event.poss_dup && event.orig_sending_time_valid;
        if (state_ != SessionState::active) {
            const bool allowed_post_remote_logon = remote_logon_seen_ &&
                (event.msg_type == "5" || event.msg_type == "2" || event.msg_type == "4" ||
                 event.msg_type == "0" || replayed_business);
            const bool allowed_initiator_pre_logon = role_ == SessionRole::initiator && local_logon_sent_ &&
                (event.msg_type == "2" || event.msg_type == "4" || event.msg_type == "5");
            if (!allowed_post_remote_logon && !allowed_initiator_pre_logon) {
                write_error(Status{ErrorCode::protocol_error, 35}, output);
                break;
            }
        }
        if (event.msg_type == "0") {
            if (test_pending_ && event.text.size() == test_id_size_ &&
                event.text == std::string_view{test_id_buffer_.data(), test_id_size_}) {
                test_pending_ = false;
                test_deadline_ = 0;
            }
        } else if (event.msg_type == "1") {
            output.push(CoreAction{CoreActionKind::send_heartbeat, 0, 0, event.text});
        } else if (event.msg_type == "2") {
            output.push(CoreAction{CoreActionKind::start_replay, event.first, event.last});
        } else if (event.msg_type == "5") {
            if (state_ == SessionState::logout_pending) {
                if (local_logout_written_) {
                    state_ = SessionState::closed;
                    should_close_ = true;
                    output.push(CoreAction{CoreActionKind::close_transport});
                } else {
                    // Simultaneous Logout: the peer's Logout acknowledges
                    // ours, but our already queued frame still must reach
                    // the transport before closing this byte stream.
                    close_after_logout_ = true;
                }
            } else {
                state_ = SessionState::logout_pending;
                close_after_logout_ = true;
                logout_deadline_ = saturating_add(event.now_ms, logout_timeout_ms_);
                output.push(CoreAction{CoreActionKind::send_logout, 0, 0, event.text});
            }
        } else if (!is_admin(event.msg_type) && state_ == SessionState::active) {
            output.push(CoreAction{CoreActionKind::deliver_message, event.sequence, event.next_sequence,
                event.text});
        }
        break;
    }

    case CoreEventKind::out_of_order_logon:
        if (!connected_ || state_ != SessionState::awaiting_logon || remote_logon_seen_ || event.first == 0) {
            output.status = Status{ErrorCode::protocol_error, 35};
            break;
        }
        remote_logon_seen_ = true;
        peer_heartbeat_ms_ = static_cast<std::uint32_t>(event.first);
        if (role_ == SessionRole::acceptor) heartbeat_ms_ = peer_heartbeat_ms_;
        last_rx_ms_ = event.now_ms;
        if (role_ == SessionRole::acceptor) {
            logon_reply_pending_ = false;
            output.push(CoreAction{CoreActionKind::send_logon, 1}); // Reply now; activate after the gap closes.
        }
        break;

    case CoreEventKind::outbound_appended:
        if (event.sequence != next_outbound_ || event.sequence == 0) {
            write_error(Status{ErrorCode::protocol_error, 34}, output);
            break;
        }
        if (next_outbound_ == std::numeric_limits<std::uint64_t>::max()) {
            write_error(Status{ErrorCode::too_large, 34}, output);
            break;
        }
        ++next_outbound_;
        if (event.msg_type == "A") local_logon_sent_ = true;
        if (event.msg_type == "0") heartbeat_pending_ = true;
        break;

    case CoreEventKind::outbound_written:
        last_tx_ms_ = event.now_ms;
        if (event.msg_type == "0") heartbeat_pending_ = false;
        if (event.msg_type == "1") {
            test_pending_ = true;
            test_deadline_ = saturating_add(event.now_ms, peer_heartbeat_ms_);
        }
        if (event.msg_type == "5") {
            local_logout_written_ = true;
            if (close_after_logout_) {
                state_ = SessionState::closed;
                should_close_ = true;
                output.push(CoreAction{CoreActionKind::close_transport});
            }
        }
        break;

    case CoreEventKind::outbound_skipped:
        if (event.msg_type == "0") heartbeat_pending_ = false;
        if (event.msg_type == "1") {
            test_pending_ = false;
            test_deadline_ = 0;
        }
        if (event.msg_type == "2") {
            resend_pending_ = false;
            resend_end_ = 0;
            resend_target_ = 0;
        }
        if (event.msg_type == "5" && state_ == SessionState::logout_pending) {
            if (close_after_logout_) {
                state_ = SessionState::closed;
                should_close_ = true;
                output.push(CoreAction{CoreActionKind::close_transport});
            } else {
                output.push(CoreAction{CoreActionKind::send_logout, 0, 0, "Logout"});
            }
        }
        break;

    case CoreEventKind::logon_reply_appended:
        if (role_ != SessionRole::acceptor || state_ != SessionState::awaiting_logon ||
            !remote_logon_seen_ || !logon_reply_pending_) {
            output.status = Status{ErrorCode::protocol_error, 35};
            break;
        }
        enter_active(output);
        break;

    case CoreEventKind::tick:
        if (state_ == SessionState::awaiting_logon && event.now_ms >= logon_deadline_) {
            write_error(Status{ErrorCode::logon_timeout}, output);
            break;
        }
        if (state_ == SessionState::logout_pending && event.now_ms >= logout_deadline_) {
            write_error(failure_.ok() ? Status{ErrorCode::logout_timeout} : failure_, output);
            break;
        }
        if (state_ != SessionState::active) break;
        if (test_pending_ && test_deadline_ != 0 && event.now_ms >= test_deadline_) {
            failure_ = Status{ErrorCode::heartbeat_timeout};
            enter_logout("TestRequest timeout", event.now_ms, output);
            break;
        }
        if (!test_pending_ && peer_heartbeat_ms_ != 0 &&
            event.now_ms >= saturating_add(last_rx_ms_, peer_test_interval_ms())) {
            update_test_id();
            test_pending_ = true;
            test_deadline_ = 0; // Start the response timeout only after the request is written.
            output.push(CoreAction{CoreActionKind::send_test_request, 0, 0,
                std::string_view{test_id_buffer_.data(), test_id_size_}});
        }
        if (output.action_count == 0 && !heartbeat_pending_ && !event.has_pending_tx &&
            heartbeat_ms_ != 0 && event.now_ms >= saturating_add(last_tx_ms_, heartbeat_ms_)) {
            output.push(CoreAction{CoreActionKind::send_heartbeat});
            heartbeat_pending_ = true;
        }
        break;

    case CoreEventKind::local_logout:
        if (state_ != SessionState::active && state_ != SessionState::awaiting_logon) {
            output.status = Status{ErrorCode::not_ready};
            break;
        }
        enter_logout(event.text, event.now_ms, output);
        if (event.poss_dup) close_after_logout_ = true;
        break;

    case CoreEventKind::sequence_reset_committed:
        // Reset-mode SequenceReset ignores the wire MsgSeqNum. The driver
        // validates NewSeqNo and journals the previous receive cursor plus
        // the new cursor in the same committed inbound record first.
        if (!connected_ || state_ == SessionState::closed || state_ == SessionState::failed ||
            event.next_sequence <= next_inbound_) {
            write_error(Status{ErrorCode::protocol_error, 36}, output);
            break;
        }
        next_inbound_ = event.next_sequence;
        last_rx_ms_ = event.now_ms;
        if (resend_pending_) {
            const auto completion_seq = resend_end_ != 0 ? resend_end_ : resend_target_;
            if (next_inbound_ > completion_seq) {
                resend_pending_ = false;
                resend_end_ = 0;
                resend_target_ = 0;
            }
        }
        if (event.is_deferred_logon && state_ == SessionState::awaiting_logon &&
            remote_logon_seen_ && local_logon_sent_) {
            enter_active(output);
        }
        break;

    case CoreEventKind::reset_epoch:
        if (state_ != SessionState::disconnected && state_ != SessionState::awaiting_logon) {
            output.status = Status{ErrorCode::sequence_reset_denied, 141};
            break;
        }
        next_inbound_ = next_outbound_ = 1;
        local_logon_sent_ = remote_logon_seen_ = logon_reply_pending_ = false;
        test_pending_ = heartbeat_pending_ = resend_pending_ = false;
        local_logout_written_ = close_after_logout_ = false;
        test_deadline_ = resend_end_ = resend_target_ = 0;
        failure_ = Status::success();
        break;
    }
    return output;
}

std::uint64_t SessionCore::next_deadline() const noexcept {
    if (state_ == SessionState::awaiting_logon) return logon_deadline_;
    if (state_ == SessionState::logout_pending) return logout_deadline_;
    if (state_ != SessionState::active) return std::numeric_limits<std::uint64_t>::max();
    auto deadline = heartbeat_pending_ || heartbeat_ms_ == 0 || (test_pending_ && test_deadline_ == 0)
        ? std::numeric_limits<std::uint64_t>::max()
        : saturating_add(last_tx_ms_, heartbeat_ms_);
    const auto peer_deadline = saturating_add(last_rx_ms_, peer_test_interval_ms());
    if (!test_pending_ && peer_heartbeat_ms_ != 0 && peer_deadline < deadline) deadline = peer_deadline;
    if (test_pending_ && test_deadline_ != 0 && test_deadline_ < deadline) deadline = test_deadline_;
    return deadline;
}

} // namespace nimblefix::detail
