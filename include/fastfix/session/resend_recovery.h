#pragma once

#include <cstdint>
#include <vector>

#include "fastfix/base/result.h"
#include "fastfix/session/session_core.h"
#include "fastfix/store/session_store.h"

namespace fastfix::session {

enum class RecoveryMode : std::uint32_t {
    kMemoryOnly = 0,
    kWarmRestart,
    kColdStart,
    kNoRecovery,  // No store, no recovery; sequences start at 1 (lowest-latency mode)
};

enum class ReplayActionKind : std::uint32_t {
    kReplay = 0,
    kGapFill,
};

struct ReplayChunk {
    ReplayActionKind kind{ReplayActionKind::kGapFill};
    std::uint32_t begin_seq{0};
    std::uint32_t end_seq{0};
    std::vector<store::MessageRecord> messages;
};

struct ReplayPlan {
    std::uint64_t session_id{0};
    std::uint32_t request_begin_seq{0};
    std::uint32_t request_end_seq{0};
    std::vector<ReplayChunk> chunks;
};

auto BuildReplayPlan(
    const store::SessionStore& store,
    std::uint64_t session_id,
    std::uint32_t begin_seq,
    std::uint32_t end_seq) -> base::Result<ReplayPlan>;

auto RecoverSession(SessionCore& session, const store::SessionStore& store, RecoveryMode mode)
    -> base::Status;

}  // namespace fastfix::session