#pragma once

#include <cstdint>

namespace fastfix::session {

enum class SessionState
{
  kDisconnected = 0,
  kConnecting,
  kConnected,
  kPendingLogon,
  kRecovering,
  kActive,
  kResendProcessing,
  kAwaitingLogout,
  kClosed,
};

struct ResendRange
{
  std::uint32_t begin_seq{ 0 };
  std::uint32_t end_seq{ 0 };
};

struct SessionSnapshot
{
  std::uint64_t session_id{ 0 };
  SessionState state{ SessionState::kDisconnected };
  std::uint64_t profile_id{ 0 };
  std::uint32_t next_in_seq{ 1 };
  std::uint32_t next_out_seq{ 1 };
  std::uint64_t last_inbound_ns{ 0 };
  std::uint64_t last_outbound_ns{ 0 };
  bool has_pending_resend{ false };
  ResendRange pending_resend{};
};

} // namespace fastfix::session