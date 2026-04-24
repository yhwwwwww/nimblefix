#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

#include "nimblefix/store/session_store.h"

namespace nimble::store {

/// Segment rollover policy for `DurableBatchSessionStore`.
enum class DurableStoreRolloverMode : std::uint32_t
{
  kDisabled = 0,
  kUtcDay,
  kExternal,
  kLocalTime,
};

/// Configuration for the durable batch store.
///
/// Design intent: batch append writes for lower steady-state write overhead
/// than syncing every frame while still retaining crash-recoverable replay logs.
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

/// Log-structured durable session store with batched flush and segment rollover.
///
/// Design intent: provide persistence stronger than `MemorySessionStore` while
/// reducing the per-message sync cost of `MmapSessionStore` in high-throughput
/// deployments.
///
/// Performance: writes may remain queued until `flush_threshold` or `Flush()`.
/// Read APIs flush pending entries first so replay sees a consistent sequence.
/// `LoadOutboundRangeViews()` copies payload bytes into the returned range,
/// making the views stable for the range lifetime.
class DurableBatchSessionStore : public SessionStore
{
public:
  struct Impl;

  /// \param root Root directory that owns active and archived segments.
  /// \param options Flush and rollover policy.
  explicit DurableBatchSessionStore(std::filesystem::path root, DurableBatchStoreOptions options = {});
  ~DurableBatchSessionStore() override;

  /// Open or create the store and rebuild indexes from the log segments.
  auto Open() -> base::Status;

  /// Flush queued entries to durable segment files.
  auto Flush() -> base::Status override;

  /// Rotate the active segment according to the configured rollover mode.
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

  /// Re-scan segment state from disk.
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