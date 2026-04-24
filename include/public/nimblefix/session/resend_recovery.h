#pragma once

#include <cstdint>
#include <vector>

#include "nimblefix/base/result.h"
#include "nimblefix/session/session_core.h"
#include "nimblefix/store/session_store.h"

namespace nimble::session {

/// Startup/restart recovery policy for one session.
///
/// `kWarmRestart` loads persisted sequence state. `kColdStart` and
/// `kNoRecovery` both restart at sequence 1, but `kNoRecovery` is the explicit
/// "no store attached" mode. `kMemoryOnly` trusts the already-live in-memory
/// state and performs no store I/O.
enum class RecoveryMode : std::uint32_t
{
  kMemoryOnly = 0,
  kWarmRestart,
  kColdStart,
  kNoRecovery, // No store, no recovery; sequences start at 1 (lowest-latency
               // mode)
};

/// One replay action segment inside a resend response plan.
///
/// `begin_seq` and `end_seq` are inclusive. Replay chunks carry the exact
/// application messages to resend; gap-fill chunks carry no payloads.
enum class ReplayActionKind : std::uint32_t
{
  kReplay = 0,
  kGapFill,
};

struct ReplayChunk
{
  ReplayActionKind kind{ ReplayActionKind::kGapFill };
  std::uint32_t begin_seq{ 0 };
  std::uint32_t end_seq{ 0 };
  std::vector<store::MessageRecord> messages;
};

/// Full resend/replay plan for one request range.
///
/// Design intent: separate the store scan from actual outbound encoding so the
/// runtime can inspect or batch the replay work before sending frames.
struct ReplayPlan
{
  std::uint64_t session_id{ 0 };
  std::uint32_t request_begin_seq{ 0 };
  std::uint32_t request_end_seq{ 0 };
  std::vector<ReplayChunk> chunks;
};

/// Build an inclusive resend plan from the outbound store.
///
/// Boundary condition: `end_seq == 0` means "infinity" per FIX and is expanded
/// to the store's highest known outbound sequence.
///
/// \param store Session store used for replay lookups.
/// \param session_id Runtime session id.
/// \param begin_seq First requested sequence number, inclusive.
/// \param end_seq Last requested sequence number, inclusive, or `0` for infinity.
/// \return Replay plan on success, otherwise an error status.
auto
BuildReplayPlan(const store::SessionStore& store,
                std::uint64_t session_id,
                std::uint32_t begin_seq,
                std::uint32_t end_seq) -> base::Result<ReplayPlan>;

/// Restore session sequence state according to the configured recovery mode.
///
/// \param session Session state machine to restore.
/// \param store Session store used for recovery reads.
/// \param mode Recovery policy.
/// \return `Ok()` on success, otherwise an error status.
auto
RecoverSession(SessionCore& session, const store::SessionStore& store, RecoveryMode mode) -> base::Status;

} // namespace nimble::session