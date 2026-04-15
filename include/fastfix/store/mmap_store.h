#pragma once

#include <filesystem>
#include <memory>

#include "fastfix/store/session_store.h"

namespace fastfix::store {

enum class SyncPolicy {
    kNone,         // OS page-cache flush only; no explicit sync.
    kEveryWrite,   // fdatasync after every pwrite (safest, default).
    kBatchFlush,   // Caller-initiated flush via Flush().
};

class MmapSessionStore : public SessionStore {
  public:
    struct Impl;

    explicit MmapSessionStore(
        std::filesystem::path path,
        SyncPolicy sync_policy = SyncPolicy::kEveryWrite);
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
    auto Refresh() -> base::Status override;
    auto ResetSession(std::uint64_t session_id) -> base::Status override;

    auto Flush() -> base::Status override;

  private:
    auto AppendOutboundLike(std::uint32_t record_type, const MessageRecord& record) -> base::Status;
    auto AppendOutboundLikeView(std::uint32_t record_type, const MessageRecordView& record) -> base::Status;
    auto AppendRecoveryState(const SessionRecoveryState& state) -> base::Status;
    auto EnsureMapping() -> base::Status;
    auto GrowMapping(std::size_t new_size) -> base::Status;

    std::filesystem::path path_;
    SyncPolicy sync_policy_;
    mutable std::unique_ptr<Impl> impl_;
};

}  // namespace fastfix::store