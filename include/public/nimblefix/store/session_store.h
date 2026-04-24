#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "nimblefix/base/inline_split_vector.h"
#include "nimblefix/base/result.h"
#include "nimblefix/base/status.h"

namespace nimble::store {

struct MessageRecordView;

/// Per-record FIX persistence flags used for replay decisions.
enum class MessageRecordFlags : std::uint16_t
{
  kNone = 0,
  kAdmin = 1U << 0,
  kPossDup = 1U << 1,
  kGapFill = 1U << 2,
};

/// Owned persisted FIX frame plus replay metadata.
///
/// Design intent: stores keep the full wire payload for resend/replay while the
/// runtime keeps higher-level sequence state separately.
///
/// `body_start_offset` is an optimization hint for replay encoders. It marks
/// the first byte of the application body inside `payload`; `0` means unknown.
struct MessageRecord
{
  std::uint64_t session_id{ 0 };
  std::uint32_t seq_num{ 0 };
  std::uint64_t timestamp_ns{ 0 };
  std::uint16_t flags{ 0 };
  std::vector<std::byte> payload;
  std::uint32_t body_start_offset{ 0 }; // byte offset where application body begins; 0 = unknown

  [[nodiscard]] bool is_admin() const { return (flags & static_cast<std::uint16_t>(MessageRecordFlags::kAdmin)) != 0; }

  [[nodiscard]] bool is_gap_fill() const
  {
    return (flags & static_cast<std::uint16_t>(MessageRecordFlags::kGapFill)) != 0;
  }

  [[nodiscard]] auto view() const -> MessageRecordView;
};

/// Borrowed view over a persisted FIX frame.
///
/// Performance: avoids copying payload bytes when a concrete store can expose a
/// stable backing span. Use `ToOwned()` when the caller must keep the data past
/// the returned range's lifetime.
struct MessageRecordView
{
  std::uint64_t session_id{ 0 };
  std::uint32_t seq_num{ 0 };
  std::uint64_t timestamp_ns{ 0 };
  std::uint16_t flags{ 0 };
  std::span<const std::byte> payload;
  std::uint32_t body_start_offset{ 0 }; // byte offset where application body begins; 0 = unknown

  [[nodiscard]] bool is_admin() const { return (flags & static_cast<std::uint16_t>(MessageRecordFlags::kAdmin)) != 0; }

  [[nodiscard]] bool is_gap_fill() const
  {
    return (flags & static_cast<std::uint16_t>(MessageRecordFlags::kGapFill)) != 0;
  }

  [[nodiscard]] auto ToOwned() const -> MessageRecord
  {
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

/// Batch of borrowed or owned message-record views.
///
/// Exactly one storage strategy is active at a time:
/// - `owned_storage` holds deep-copied records
/// - `payload_storage` holds copied payload bytes for views in `records`
/// - `borrowed_payload_owner` pins a concrete store's shared backing storage
///
/// Lifetime: `records[i].payload` is valid only while this range object stays
/// alive and any additional concrete-store caveats remain satisfied.
struct MessageRecordViewRange
{
  std::vector<MessageRecord> owned_storage;
  std::vector<std::byte> payload_storage;
  std::shared_ptr<const void> borrowed_payload_owner;
  base::InlineSplitVector<MessageRecordView, kMessageRecordViewRangeInlineCapacity> records;

  auto clear() -> void
  {
    owned_storage.clear();
    payload_storage.clear();
    borrowed_payload_owner.reset();
    records.clear();
  }

  [[nodiscard]] bool empty() const { return records.empty(); }

  [[nodiscard]] auto begin() const { return records.begin(); }

  [[nodiscard]] auto end() const { return records.end(); }
};

inline auto
MessageRecord::view() const -> MessageRecordView
{
  return MessageRecordView{
    .session_id = session_id,
    .seq_num = seq_num,
    .timestamp_ns = timestamp_ns,
    .flags = flags,
    .payload = std::span<const std::byte>(payload.data(), payload.size()),
    .body_start_offset = body_start_offset,
  };
}

struct SessionRecoveryState
{
  std::uint64_t session_id{ 0 };
  std::uint32_t next_in_seq{ 1 };
  std::uint32_t next_out_seq{ 1 };
  std::uint64_t last_inbound_ns{ 0 };
  std::uint64_t last_outbound_ns{ 0 };
  bool active{ false };
};

/// Abstract persistence contract for one or more FIX sessions.
///
/// Design intent: all concrete stores must support two jobs:
/// - outbound replay by inclusive sequence range
/// - recovery of next inbound/outbound sequence state
///
/// Performance/lifecycle contract:
/// - `Save*View()` may avoid payload copies when the store can consume the span
///   immediately
/// - `LoadOutboundRangeViews()` may either borrow store-backed payloads or copy
///   them into the returned range; callers must treat the returned range as the
///   payload owner
/// - `begin_seq`/`end_seq` parameters are inclusive and must be non-zero
///
/// Boundary condition: `ResetSession(session_id)` must only affect the target
/// session, never unrelated sessions.
class SessionStore
{
public:
  virtual ~SessionStore() = default;

  /// Force any pending writes to durable media when the backend supports it.
  ///
  /// \return `Ok()` on success, otherwise an error status.
  virtual auto Flush() -> base::Status { return base::Status::Ok(); }

  /// Rotate/compact backing storage according to the concrete store policy.
  ///
  /// \return `Ok()` on success, otherwise an error status.
  virtual auto Rollover() -> base::Status { return base::Status::Ok(); }

  /// Refresh any on-disk or shared backing state.
  ///
  /// \return `Ok()` on success, otherwise an error status.
  virtual auto Refresh() -> base::Status { return base::Status::Ok(); }

  /// Remove persisted data for one session.
  ///
  /// \param session_id Runtime session id to clear.
  /// \return `Ok()` on success, otherwise an error status.
  virtual auto ResetSession(std::uint64_t session_id) -> base::Status = 0;

  /// Persist one outbound frame.
  ///
  /// \param record Owned message record.
  /// \return `Ok()` on success, otherwise an error status.
  virtual auto SaveOutbound(const MessageRecord& record) -> base::Status = 0;

  /// Persist one inbound frame.
  ///
  /// \param record Owned message record.
  /// \return `Ok()` on success, otherwise an error status.
  virtual auto SaveInbound(const MessageRecord& record) -> base::Status = 0;

  /// Persist one outbound frame from a borrowed payload view.
  ///
  /// \param record Borrowed message record.
  /// \return `Ok()` on success, otherwise an error status.
  virtual auto SaveOutboundView(const MessageRecordView& record) -> base::Status
  {
    return SaveOutbound(record.ToOwned());
  }

  /// Persist one inbound frame from a borrowed payload view.
  ///
  /// \param record Borrowed message record.
  /// \return `Ok()` on success, otherwise an error status.
  virtual auto SaveInboundView(const MessageRecordView& record) -> base::Status
  {
    return SaveInbound(record.ToOwned());
  }

  /// Load owned outbound frames for an inclusive replay range.
  ///
  /// \param session_id Runtime session id.
  /// \param begin_seq First outbound sequence number to load, inclusive.
  /// \param end_seq Last outbound sequence number to load, inclusive.
  /// \return Owned records in sequence order, or an error status.
  virtual auto LoadOutboundRange(std::uint64_t session_id, std::uint32_t begin_seq, std::uint32_t end_seq) const
    -> base::Result<std::vector<MessageRecord>> = 0;

  /// Load outbound frames as borrowed or range-owned views.
  ///
  /// \param session_id Runtime session id.
  /// \param begin_seq First outbound sequence number to load, inclusive.
  /// \param end_seq Last outbound sequence number to load, inclusive.
  /// \return Range that owns or pins the payload bytes behind `records`.
  virtual auto LoadOutboundRangeViews(std::uint64_t session_id, std::uint32_t begin_seq, std::uint32_t end_seq) const
    -> base::Result<MessageRecordViewRange>
  {
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

  /// Populate an existing range object with outbound frame views.
  ///
  /// \param session_id Runtime session id.
  /// \param begin_seq First outbound sequence number to load, inclusive.
  /// \param end_seq Last outbound sequence number to load, inclusive.
  /// \param range Output object that becomes the owner/pinner of payload bytes.
  /// \return `Ok()` on success, otherwise an error status.
  virtual auto LoadOutboundRangeViews(std::uint64_t session_id,
                                      std::uint32_t begin_seq,
                                      std::uint32_t end_seq,
                                      MessageRecordViewRange* range) const -> base::Status
  {
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

  /// Persist recovery state for one session.
  ///
  /// \param state Recovery snapshot to store.
  /// \return `Ok()` on success, otherwise an error status.
  virtual auto SaveRecoveryState(const SessionRecoveryState& state) -> base::Status = 0;

  /// Persist one inbound frame and the updated recovery state together.
  ///
  /// \param record Borrowed inbound record.
  /// \param state Recovery snapshot to store.
  /// \return `Ok()` on success, otherwise an error status.
  virtual auto SaveInboundViewAndRecoveryState(const MessageRecordView& record, const SessionRecoveryState& state)
    -> base::Status
  {
    auto status = SaveInboundView(record);
    if (!status.ok()) {
      return status;
    }
    return SaveRecoveryState(state);
  }

  /// Persist one outbound frame and the updated recovery state together.
  ///
  /// \param record Borrowed outbound record.
  /// \param state Recovery snapshot to store.
  /// \return `Ok()` on success, otherwise an error status.
  virtual auto SaveOutboundViewAndRecoveryState(const MessageRecordView& record, const SessionRecoveryState& state)
    -> base::Status
  {
    auto status = SaveOutboundView(record);
    if (!status.ok()) {
      return status;
    }
    return SaveRecoveryState(state);
  }

  /// Load the latest recovery state for one session.
  ///
  /// \param session_id Runtime session id.
  /// \return Recovery state on success, otherwise an error status.
  virtual auto LoadRecoveryState(std::uint64_t session_id) const -> base::Result<SessionRecoveryState> = 0;
};

} // namespace nimble::store