#pragma once

#include <filesystem>
#include <memory>

#include "nimblefix/store/session_store.h"

namespace nimble::store {

enum class SyncPolicy
{
  kNone,       // OS page-cache flush only; no explicit sync.
  kEveryWrite, // fdatasync after every pwrite (safest, default).
  kBatchFlush, // Caller-initiated flush via Flush().
};

/// File-backed session store that keeps an mmap'd replay index in process.
///
/// Design intent: trade a small amount of filesystem complexity for faster
/// warm restart and lower copy cost than fully materialized durable logs.
///
/// Performance: explicit `LoadOutboundRangeViews()` can pin the current mapping
/// and expose borrowed payload spans without copying them. `LoadOutboundRange()`
/// always materializes owned payloads.
///
/// Lifetime: view ranges pin the mapping they were created from via
/// `borrowed_payload_owner`, so their payload spans remain valid until the range
/// is destroyed even if the store later refreshes or remaps.
class MmapSessionStore : public SessionStore
{
public:
  struct Impl;

  /// \param path Backing mmap store file.
  /// \param sync_policy Durability policy for appended writes.
  explicit MmapSessionStore(std::filesystem::path path, SyncPolicy sync_policy = SyncPolicy::kEveryWrite);
  ~MmapSessionStore() override;

  /// Open the backing file and build in-memory indexes.
  ///
  /// Safe to call eagerly or let other APIs open lazily on first use.
  ///
  /// \return `Ok()` on success, otherwise an error status.
  auto Open() -> base::Status;

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

  /// Reload indexes from the current file contents.
  auto Refresh() -> base::Status override;
  auto ResetSession(std::uint64_t session_id) -> base::Status override;

  /// Flush pending mapped/file writes according to `sync_policy_`.
  auto Flush() -> base::Status override;

private:
  auto AppendOutboundLike(std::uint32_t record_type, const MessageRecord& record) -> base::Status;
  auto AppendOutboundLikeView(std::uint32_t record_type, const MessageRecordView& record) -> base::Status;
  auto AppendRecoveryState(const SessionRecoveryState& state) -> base::Status;
  auto EnsureMapping() -> base::Status;
  auto RemapFile(std::size_t new_size) -> base::Status;
  auto CloseResources() -> void;

  std::filesystem::path path_;
  SyncPolicy sync_policy_;
  mutable std::unique_ptr<Impl> impl_;
};

} // namespace nimble::store