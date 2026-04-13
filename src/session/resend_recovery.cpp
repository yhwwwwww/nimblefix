#include "fastfix/session/resend_recovery.h"

#include <cstddef>

namespace fastfix::session {

namespace {

auto FlushChunk(ReplayPlan& plan, ReplayChunk& chunk) -> void {
    if (chunk.begin_seq == 0 || chunk.end_seq == 0) {
        return;
    }
    plan.chunks.push_back(std::move(chunk));
    chunk = ReplayChunk{};
}

}  // namespace

auto BuildReplayPlan(
    const store::SessionStore& store,
    std::uint64_t session_id,
    std::uint32_t begin_seq,
    std::uint32_t end_seq) -> base::Result<ReplayPlan> {
    if (session_id == 0) {
        return base::Status::InvalidArgument("replay plan requires a valid session id");
    }
    if (begin_seq == 0 || end_seq == 0 || begin_seq > end_seq) {
        return base::Status::InvalidArgument("invalid replay range");
    }

    auto loaded = store.LoadOutboundRangeViews(session_id, begin_seq, end_seq);
    if (!loaded.ok()) {
        return loaded.status();
    }

    ReplayPlan plan;
    plan.session_id = session_id;
    plan.request_begin_seq = begin_seq;
    plan.request_end_seq = end_seq;

    ReplayChunk current;
    std::size_t record_index = 0U;
    const auto& records = loaded.value().records;
    for (std::uint32_t seq_num = begin_seq; seq_num <= end_seq; ++seq_num) {
        const store::MessageRecordView* record = nullptr;
        if (record_index < records.size() && records[record_index].seq_num == seq_num) {
            record = &records[record_index];
        }
        const auto should_gap_fill =
            record == nullptr || record->is_admin() || record->is_gap_fill();
        const auto next_kind = should_gap_fill ? ReplayActionKind::kGapFill : ReplayActionKind::kReplay;

        if (current.begin_seq == 0) {
            current.kind = next_kind;
            current.begin_seq = seq_num;
            current.end_seq = seq_num;
        } else if (current.kind != next_kind) {
            FlushChunk(plan, current);
            current.kind = next_kind;
            current.begin_seq = seq_num;
            current.end_seq = seq_num;
        } else {
            current.end_seq = seq_num;
        }

        if (!should_gap_fill) {
            current.messages.push_back(record->ToOwned());
        }
        if (record != nullptr) {
            ++record_index;
        }
    }

    FlushChunk(plan, current);
    return plan;
}

auto RecoverSession(SessionCore& session, const store::SessionStore& store, RecoveryMode mode)
    -> base::Status {
    auto status = session.BeginRecovery();
    if (!status.ok()) {
        return status;
    }

    if (mode == RecoveryMode::kMemoryOnly) {
        return session.FinishRecovery();
    }

    if (mode == RecoveryMode::kNoRecovery || mode == RecoveryMode::kColdStart) {
        status = session.RestoreSequenceState(1U, 1U);
        if (!status.ok()) {
            return status;
        }
        status = session.RecordInboundActivity(0U);
        if (!status.ok()) {
            return status;
        }
        status = session.RecordOutboundActivity(0U);
        if (!status.ok()) {
            return status;
        }
        return session.FinishRecovery();
    }

    auto recovery = store.LoadRecoveryState(session.session_id());
    if (!recovery.ok()) {
        return recovery.status();
    }

    status = session.RestoreSequenceState(recovery.value().next_in_seq, recovery.value().next_out_seq);
    if (!status.ok()) {
        return status;
    }
    status = session.RecordInboundActivity(recovery.value().last_inbound_ns);
    if (!status.ok()) {
        return status;
    }
    status = session.RecordOutboundActivity(recovery.value().last_outbound_ns);
    if (!status.ok()) {
        return status;
    }
    return session.FinishRecovery();
}

}  // namespace fastfix::session