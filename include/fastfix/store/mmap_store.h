#pragma once

#include <filesystem>
#include <memory>

#include "fastfix/store/session_store.h"

namespace fastfix::store {

class MmapSessionStore : public SessionStore {
  public:
    struct Impl;

    explicit MmapSessionStore(std::filesystem::path path);
    ~MmapSessionStore() override;

    auto Open() -> base::Status;

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
    auto SaveRecoveryState(const SessionRecoveryState& state) -> base::Status override;
    auto LoadRecoveryState(std::uint64_t session_id) const
        -> base::Result<SessionRecoveryState> override;

  private:
    auto AppendOutboundLike(std::uint32_t record_type, const MessageRecord& record) -> base::Status;
    auto AppendOutboundLikeView(std::uint32_t record_type, const MessageRecordView& record) -> base::Status;
    auto AppendRecoveryState(const SessionRecoveryState& state) -> base::Status;

    std::filesystem::path path_;
    std::unique_ptr<Impl> impl_;
};

}  // namespace fastfix::store