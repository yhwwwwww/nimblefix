#pragma once

#include <cstddef>
#include <unordered_map>
#include <vector>

#include "fastfix/store/session_store.h"

namespace fastfix::store {

class MemorySessionStore : public SessionStore {
  public:
    struct StoredMessageRecord {
        std::uint32_t seq_num{0};
        std::uint64_t timestamp_ns{0};
        std::uint16_t flags{0};
        std::size_t payload_offset{0};
        std::size_t payload_size{0};
    };

    auto SaveOutbound(const MessageRecord& record) -> base::Status override;
    auto SaveInbound(const MessageRecord& record) -> base::Status override;
    auto SaveOutboundView(const MessageRecordView& record) -> base::Status override;
    auto SaveInboundView(const MessageRecordView& record) -> base::Status override;
    auto LoadOutboundRange(
        std::uint64_t session_id,
        std::uint32_t begin_seq,
        std::uint32_t end_seq) const -> base::Result<std::vector<MessageRecord>> override;
    auto LoadOutboundRangeViews(
      std::uint64_t session_id,
      std::uint32_t begin_seq,
      std::uint32_t end_seq) const -> base::Result<MessageRecordViewRange> override;
    auto LoadOutboundRangeViews(
      std::uint64_t session_id,
      std::uint32_t begin_seq,
      std::uint32_t end_seq,
      MessageRecordViewRange* range) const -> base::Status override;
    auto ReserveAdditionalSessionStorage(
        std::uint64_t session_id,
        std::size_t inbound_records,
        std::size_t outbound_records,
        std::size_t payload_bytes) -> void;
    auto SaveRecoveryState(const SessionRecoveryState& state) -> base::Status override;
    auto SaveInboundViewAndRecoveryState(
        const MessageRecordView& record,
        const SessionRecoveryState& state) -> base::Status override;
    auto LoadRecoveryState(std::uint64_t session_id) const
        -> base::Result<SessionRecoveryState> override;

  private:
    static constexpr std::size_t kInitialRecordCapacity = 128U;
    static constexpr std::size_t kInitialPayloadArenaBytes = 16U * 1024U;

    struct SessionData {
      SessionData() {
        outbound.reserve(kInitialRecordCapacity);
        inbound.reserve(kInitialRecordCapacity);
        payload_arena.reserve(kInitialPayloadArenaBytes);
      }

      std::vector<StoredMessageRecord> outbound;
      std::vector<StoredMessageRecord> inbound;
      std::vector<std::byte> payload_arena;
        SessionRecoveryState recovery;
        bool has_recovery{false};
    };

    std::unordered_map<std::uint64_t, SessionData> sessions_;
};

}  // namespace fastfix::store