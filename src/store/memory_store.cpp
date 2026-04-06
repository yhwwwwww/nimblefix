#include "fastfix/store/memory_store.h"

#include <algorithm>

namespace fastfix::store {

namespace {

auto ValidateRecordView(const MessageRecordView& record) -> base::Status {
    if (record.session_id == 0) {
        return base::Status::InvalidArgument("message record is missing session_id");
    }
    if (record.seq_num == 0) {
        return base::Status::InvalidArgument("message record is missing seq_num");
    }
    return base::Status::Ok();
}

auto AppendPayload(
    std::vector<std::byte>* payload_arena,
    std::span<const std::byte> payload) -> std::pair<std::size_t, std::size_t> {
    const auto offset = payload_arena->size();
    payload_arena->insert(payload_arena->end(), payload.begin(), payload.end());
    return {offset, payload.size()};
}

auto MakeStoredRecord(
    std::vector<std::byte>* payload_arena,
    const MessageRecordView& record) -> MemorySessionStore::StoredMessageRecord {
    const auto [payload_offset, payload_size] = AppendPayload(payload_arena, record.payload);
    return MemorySessionStore::StoredMessageRecord{
        .seq_num = record.seq_num,
        .timestamp_ns = record.timestamp_ns,
        .flags = record.flags,
        .payload_offset = payload_offset,
        .payload_size = payload_size,
    };
}

auto MaterializeRecord(
    std::uint64_t session_id,
    const std::vector<std::byte>& payload_arena,
    const MemorySessionStore::StoredMessageRecord& record) -> MessageRecord {
    MessageRecord owned;
    owned.session_id = session_id;
    owned.seq_num = record.seq_num;
    owned.timestamp_ns = record.timestamp_ns;
    owned.flags = record.flags;
    owned.payload.insert(
        owned.payload.end(),
        payload_arena.begin() + static_cast<std::ptrdiff_t>(record.payload_offset),
        payload_arena.begin() + static_cast<std::ptrdiff_t>(record.payload_offset + record.payload_size));
    return owned;
}

auto ViewRecord(
    std::uint64_t session_id,
    const std::vector<std::byte>& payload_arena,
    const MemorySessionStore::StoredMessageRecord& record) -> MessageRecordView {
    return MessageRecordView{
        .session_id = session_id,
        .seq_num = record.seq_num,
        .timestamp_ns = record.timestamp_ns,
        .flags = record.flags,
        .payload = std::span<const std::byte>(
            payload_arena.data() + static_cast<std::ptrdiff_t>(record.payload_offset),
            record.payload_size),
    };
}

auto UpsertBySequence(
    std::vector<MemorySessionStore::StoredMessageRecord>& records,
    std::vector<std::byte>* payload_arena,
    const MessageRecordView& record) -> void {
    auto it = std::lower_bound(
        records.begin(),
        records.end(),
        record.seq_num,
        [](const auto& existing, std::uint32_t seq_num) { return existing.seq_num < seq_num; });
    if (it != records.end() && it->seq_num == record.seq_num) {
        *it = MakeStoredRecord(payload_arena, record);
        return;
    }
    records.insert(it, MakeStoredRecord(payload_arena, record));
}

}  // namespace

auto MemorySessionStore::SaveOutbound(const MessageRecord& record) -> base::Status {
    return SaveOutboundView(record.view());
}

auto MemorySessionStore::SaveOutboundView(const MessageRecordView& record) -> base::Status {
    auto status = ValidateRecordView(record);
    if (!status.ok()) {
        return status;
    }

    auto& session = sessions_[record.session_id];
    UpsertBySequence(session.outbound, &session.payload_arena, record);
    return base::Status::Ok();
}

auto MemorySessionStore::SaveInbound(const MessageRecord& record) -> base::Status {
    return SaveInboundView(record.view());
}

auto MemorySessionStore::SaveInboundView(const MessageRecordView& record) -> base::Status {
    auto status = ValidateRecordView(record);
    if (!status.ok()) {
        return status;
    }

    auto& session = sessions_[record.session_id];
    session.inbound.push_back(MakeStoredRecord(&session.payload_arena, record));
    return base::Status::Ok();
}

auto MemorySessionStore::LoadOutboundRange(
    std::uint64_t session_id,
    std::uint32_t begin_seq,
    std::uint32_t end_seq) const -> base::Result<std::vector<MessageRecord>> {
    if (begin_seq == 0 || end_seq == 0 || begin_seq > end_seq) {
        return base::Status::InvalidArgument("invalid outbound load range");
    }

    std::vector<MessageRecord> result;
    const auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        return result;
    }

    for (const auto& record : it->second.outbound) {
        if (record.seq_num < begin_seq) {
            continue;
        }
        if (record.seq_num > end_seq) {
            break;
        }
        result.push_back(MaterializeRecord(session_id, it->second.payload_arena, record));
    }
    return result;
}

auto MemorySessionStore::LoadOutboundRangeViews(
    std::uint64_t session_id,
    std::uint32_t begin_seq,
    std::uint32_t end_seq) const -> base::Result<MessageRecordViewRange> {
    if (begin_seq == 0 || end_seq == 0 || begin_seq > end_seq) {
        return base::Status::InvalidArgument("invalid outbound load range");
    }

    MessageRecordViewRange result;
    const auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        return result;
    }

    result.records.reserve(static_cast<std::size_t>(end_seq - begin_seq + 1U));
    for (const auto& record : it->second.outbound) {
        if (record.seq_num < begin_seq) {
            continue;
        }
        if (record.seq_num > end_seq) {
            break;
        }
        result.records.push_back(ViewRecord(session_id, it->second.payload_arena, record));
    }
    return result;
}

auto MemorySessionStore::LoadOutboundRangeViews(
    std::uint64_t session_id,
    std::uint32_t begin_seq,
    std::uint32_t end_seq,
    MessageRecordViewRange* result) const -> base::Status {
    if (result == nullptr) {
        return base::Status::InvalidArgument("message record view range output is null");
    }
    if (begin_seq == 0 || end_seq == 0 || begin_seq > end_seq) {
        return base::Status::InvalidArgument("invalid outbound load range");
    }

    result->clear();
    const auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        return base::Status::Ok();
    }

    result->records.reserve(static_cast<std::size_t>(end_seq - begin_seq + 1U));
    for (const auto& record : it->second.outbound) {
        if (record.seq_num < begin_seq) {
            continue;
        }
        if (record.seq_num > end_seq) {
            break;
        }
        result->records.push_back(ViewRecord(session_id, it->second.payload_arena, record));
    }
    return base::Status::Ok();
}

auto MemorySessionStore::ReserveAdditionalSessionStorage(
    std::uint64_t session_id,
    std::size_t inbound_records,
    std::size_t outbound_records,
    std::size_t payload_bytes) -> void {
    auto& session = sessions_[session_id];
    if (inbound_records != 0U) {
        session.inbound.reserve(session.inbound.size() + inbound_records);
    }
    if (outbound_records != 0U) {
        session.outbound.reserve(session.outbound.size() + outbound_records);
    }
    if (payload_bytes != 0U) {
        session.payload_arena.reserve(session.payload_arena.size() + payload_bytes);
    }
}

auto MemorySessionStore::SaveRecoveryState(const SessionRecoveryState& state) -> base::Status {
    if (state.session_id == 0) {
        return base::Status::InvalidArgument("recovery state is missing session_id");
    }

    auto& session = sessions_[state.session_id];
    session.recovery = state;
    session.has_recovery = true;
    return base::Status::Ok();
}

auto MemorySessionStore::LoadRecoveryState(std::uint64_t session_id) const
    -> base::Result<SessionRecoveryState> {
    const auto it = sessions_.find(session_id);
    if (it == sessions_.end() || !it->second.has_recovery) {
        return base::Status::NotFound("recovery state not found");
    }

    return it->second.recovery;
}

}  // namespace fastfix::store