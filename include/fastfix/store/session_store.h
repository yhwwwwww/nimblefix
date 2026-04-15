#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "fastfix/base/inline_split_vector.h"
#include "fastfix/base/result.h"
#include "fastfix/base/status.h"

namespace fastfix::store {

struct MessageRecordView;

enum class MessageRecordFlags : std::uint16_t {
    kNone = 0,
    kAdmin = 1U << 0,
    kPossDup = 1U << 1,
    kGapFill = 1U << 2,
};

struct MessageRecord {
    std::uint64_t session_id{0};
    std::uint32_t seq_num{0};
    std::uint64_t timestamp_ns{0};
    std::uint16_t flags{0};
    std::vector<std::byte> payload;
    std::uint32_t body_start_offset{0};  // byte offset where application body begins; 0 = unknown

    [[nodiscard]] bool is_admin() const {
        return (flags & static_cast<std::uint16_t>(MessageRecordFlags::kAdmin)) != 0;
    }

    [[nodiscard]] bool is_gap_fill() const {
        return (flags & static_cast<std::uint16_t>(MessageRecordFlags::kGapFill)) != 0;
    }

    [[nodiscard]] auto view() const -> MessageRecordView;
};

struct MessageRecordView {
    std::uint64_t session_id{0};
    std::uint32_t seq_num{0};
    std::uint64_t timestamp_ns{0};
    std::uint16_t flags{0};
    std::span<const std::byte> payload;
    std::uint32_t body_start_offset{0};  // byte offset where application body begins; 0 = unknown

    [[nodiscard]] bool is_admin() const {
        return (flags & static_cast<std::uint16_t>(MessageRecordFlags::kAdmin)) != 0;
    }

    [[nodiscard]] bool is_gap_fill() const {
        return (flags & static_cast<std::uint16_t>(MessageRecordFlags::kGapFill)) != 0;
    }

    [[nodiscard]] auto ToOwned() const -> MessageRecord {
        MessageRecord record;
        record.session_id = session_id;
        record.seq_num = seq_num;
        record.timestamp_ns = timestamp_ns;
        record.flags = flags;
        record.payload = std::vector<std::byte>(payload.begin(), payload.end());
        record.body_start_offset = body_start_offset;
        return record;
    }
};

inline constexpr std::size_t kMessageRecordViewRangeInlineCapacity = 4U;

struct MessageRecordViewRange {
    std::vector<MessageRecord> owned_storage;
    std::vector<std::byte> payload_storage;
    base::InlineSplitVector<MessageRecordView, kMessageRecordViewRangeInlineCapacity> records;

    auto clear() -> void {
        owned_storage.clear();
        payload_storage.clear();
        records.clear();
    }

    [[nodiscard]] bool empty() const {
        return records.empty();
    }

    [[nodiscard]] auto begin() const {
        return records.begin();
    }

    [[nodiscard]] auto end() const {
        return records.end();
    }
};

inline auto MessageRecord::view() const -> MessageRecordView {
    return MessageRecordView{
        .session_id = session_id,
        .seq_num = seq_num,
        .timestamp_ns = timestamp_ns,
        .flags = flags,
        .payload = std::span<const std::byte>(payload.data(), payload.size()),
        .body_start_offset = body_start_offset,
    };
}

struct SessionRecoveryState {
    std::uint64_t session_id{0};
    std::uint32_t next_in_seq{1};
    std::uint32_t next_out_seq{1};
    std::uint64_t last_inbound_ns{0};
    std::uint64_t last_outbound_ns{0};
    bool active{false};
};

class SessionStore {
  public:
    virtual ~SessionStore() = default;

    virtual auto Flush() -> base::Status {
        return base::Status::Ok();
    }

    virtual auto Rollover() -> base::Status {
        return base::Status::Ok();
    }

    virtual auto Refresh() -> base::Status {
        return base::Status::Ok();
    }

    virtual auto ResetSession(std::uint64_t session_id) -> base::Status = 0;

    virtual auto SaveOutbound(const MessageRecord& record) -> base::Status = 0;
    virtual auto SaveInbound(const MessageRecord& record) -> base::Status = 0;
    virtual auto SaveOutboundView(const MessageRecordView& record) -> base::Status {
        return SaveOutbound(record.ToOwned());
    }
    virtual auto SaveInboundView(const MessageRecordView& record) -> base::Status {
        return SaveInbound(record.ToOwned());
    }
    virtual auto LoadOutboundRange(
        std::uint64_t session_id,
        std::uint32_t begin_seq,
        std::uint32_t end_seq) const -> base::Result<std::vector<MessageRecord>> = 0;
    virtual auto LoadOutboundRangeViews(
        std::uint64_t session_id,
        std::uint32_t begin_seq,
        std::uint32_t end_seq) const -> base::Result<MessageRecordViewRange> {
        auto loaded = LoadOutboundRange(session_id, begin_seq, end_seq);
        if (!loaded.ok()) {
            return loaded.status();
        }

        MessageRecordViewRange range;
        range.owned_storage = std::move(loaded).value();
        range.records.reserve(range.owned_storage.size());
        for (const auto& record : range.owned_storage) {
            range.records.push_back(record.view());
        }
        return range;
    }
    virtual auto LoadOutboundRangeViews(
        std::uint64_t session_id,
        std::uint32_t begin_seq,
        std::uint32_t end_seq,
        MessageRecordViewRange* range) const -> base::Status {
        if (range == nullptr) {
            return base::Status::InvalidArgument("message record view range output is null");
        }

        auto loaded = LoadOutboundRangeViews(session_id, begin_seq, end_seq);
        if (!loaded.ok()) {
            return loaded.status();
        }

        *range = std::move(loaded).value();
        return base::Status::Ok();
    }
    virtual auto SaveRecoveryState(const SessionRecoveryState& state) -> base::Status = 0;
    virtual auto SaveInboundViewAndRecoveryState(
        const MessageRecordView& record,
        const SessionRecoveryState& state) -> base::Status {
        auto status = SaveInboundView(record);
        if (!status.ok()) {
            return status;
        }
        return SaveRecoveryState(state);
    }
    virtual auto SaveOutboundViewAndRecoveryState(
        const MessageRecordView& record,
        const SessionRecoveryState& state) -> base::Status {
        auto status = SaveOutboundView(record);
        if (!status.ok()) {
            return status;
        }
        return SaveRecoveryState(state);
    }
    virtual auto LoadRecoveryState(std::uint64_t session_id) const
        -> base::Result<SessionRecoveryState> = 0;
};

}  // namespace fastfix::store