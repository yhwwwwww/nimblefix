#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

#include "nimblefix/store/session_store.h"

namespace nimble::store {

enum class DurableStoreRolloverMode : std::uint32_t
{
  kDisabled = 0,
  kUtcDay,
  kExternal,
  kLocalTime,
};

struct DurableBatchStoreOptions
{
  std::size_t flush_threshold{ 8 };
  DurableStoreRolloverMode rollover_mode{ DurableStoreRolloverMode::kUtcDay };
  std::size_t max_archived_segments{ 0 };
  /// UTC offset in seconds for kLocalTime mode. E.g., +28800 for UTC+8.
  std::int32_t local_utc_offset_seconds{ 0 };
  /// If true and rollover_mode is kLocalTime, detect offset from system
  /// timezone at Open().
  bool use_system_timezone{ true };
};

class DurableBatchSessionStore : public SessionStore
{
public:
  struct Impl;

  explicit DurableBatchSessionStore(std::filesystem::path root, DurableBatchStoreOptions options = {});
  ~DurableBatchSessionStore() override;

  auto Open() -> base::Status;
  auto Flush() -> base::Status override;
  auto Rollover() -> base::Status override;

  auto SaveOutbound(const MessageRecord& record) -> base::Status override;
  auto SaveInbound(const MessageRecord& record) -> base::Status override;
  auto SaveOutboundView(const MessageRecordView& record) -> base::Status override;
  auto SaveInboundView(const MessageRecordView& record) -> base::Status override;
  auto LoadOutboundRange(std::uint64_t session_id, std::uint32_t begin_seq, std::uint32_t end_seq) const
    -> base::Result<std::vector<MessageRecord>> override;
  auto LoadOutboundRangeViews(std::uint64_t session_id, std::uint32_t begin_seq, std::uint32_t end_seq) const
    -> base::Result<MessageRecordViewRange> override;
  auto SaveRecoveryState(const SessionRecoveryState& state) -> base::Status override;
  auto LoadRecoveryState(std::uint64_t session_id) const -> base::Result<SessionRecoveryState> override;
  auto Refresh() -> base::Status override;
  auto ResetSession(std::uint64_t session_id) -> base::Status override;

private:
  auto AppendMessage(bool outbound, const MessageRecord& record) -> base::Status;
  auto AppendMessageView(bool outbound, const MessageRecordView& record) -> base::Status;
  auto QueueRecoveryState(const SessionRecoveryState& state) -> base::Status;

  std::filesystem::path root_;
  DurableBatchStoreOptions options_;
  mutable std::unique_ptr<Impl> impl_;
};

} // namespace nimble::store