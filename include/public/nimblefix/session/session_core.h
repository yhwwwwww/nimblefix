#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "nimblefix/base/result.h"
#include "nimblefix/base/status.h"
#include "nimblefix/session/session_handle.h"
#include "nimblefix/session/session_key.h"
#include "nimblefix/session/session_snapshot.h"
#include "nimblefix/session/transport_profile.h"

namespace nimble::session {

enum class DayCutMode : std::uint32_t
{
  kNoAutoReset = 0, // No automatic reset; application triggers manually
  kFixedLocalTime,  // Reset at a fixed local time (e.g., 17:00 EST)
  kFixedUtcTime,    // Reset at a fixed UTC time
  kExternalControl, // Reset only when explicitly triggered via API
};

struct DayCutConfig
{
  DayCutMode mode{ DayCutMode::kNoAutoReset };
  std::int32_t reset_hour{ 0 };         // Hour (0-23) for fixed-time modes
  std::int32_t reset_minute{ 0 };       // Minute (0-59) for fixed-time modes
  std::int32_t utc_offset_seconds{ 0 }; // For kFixedLocalTime: local timezone offset
};

struct SessionConfig
{
  // Stable runtime/store identity. Static configs must provide a positive id;
  // dynamic acceptor factories may return 0 and let Engine assign one.
  std::uint64_t session_id{ 0 };
  // Session identity from the local engine's perspective.
  SessionKey key;
  // Dictionary/profile id used to encode and decode application messages.
  std::uint64_t profile_id{ 0 };
  // FIXT-only application version. Leave empty for FIX 4.x transport sessions.
  std::string default_appl_ver_id;
  // HeartBtInt(108) for logon and liveness timers.
  std::uint32_t heartbeat_interval_seconds{ 30 };
  // true for outbound initiator sessions, false for inbound acceptor sessions.
  bool is_initiator{ false };
};

class SessionCore
{
public:
  explicit SessionCore(SessionConfig config);

  [[nodiscard]] const SessionKey& key() const { return config_.key; }

  [[nodiscard]] std::uint64_t session_id() const { return config_.session_id; }

  [[nodiscard]] std::uint64_t profile_id() const { return config_.profile_id; }

  [[nodiscard]] SessionState state() const { return state_; }

  auto BeginConnect() -> base::Status;
  auto OnTransportConnected() -> base::Status;
  auto Close() -> base::Status;
  auto BeginRecovery() -> base::Status;
  auto FinishRecovery() -> base::Status;
  auto BeginLogon() -> base::Status;
  auto RestoreSequenceState(std::uint32_t next_in_seq, std::uint32_t next_out_seq) -> base::Status;
  auto AdvanceInboundExpectedSeq(std::uint32_t next_in_seq) -> base::Status;
  auto OnLogonAccepted() -> base::Status;
  auto BeginLogout() -> base::Status;
  auto OnTransportClosed() -> base::Status;
  auto AllocateOutboundSeq() -> base::Result<std::uint32_t>;
  auto ObserveInboundSeq(std::uint32_t seq_num) -> base::Status;
  auto BeginResend(std::uint32_t begin_seq, std::uint32_t end_seq) -> base::Status;
  auto CompleteResend() -> base::Status;
  auto RecordInboundActivity(std::uint64_t timestamp_ns) -> base::Status;
  auto RecordOutboundActivity(std::uint64_t timestamp_ns) -> base::Status;

  /// Check whether the day-cut boundary has been crossed since the last check.
  /// Only effective for kFixedLocalTime and kFixedUtcTime modes.
  /// \param now_ns wall-clock nanoseconds since Unix epoch (system_clock).
  auto CheckDayCut(std::uint64_t now_ns) -> void;

  /// Externally triggered session reset: resets sequence numbers to 1.
  /// Callable for kExternalControl mode or internally by timer-based modes.
  auto TriggerDayCut() -> base::Status;

  auto SetDayCutConfig(const DayCutConfig& config) -> void { day_cut_config_ = config; }

  [[nodiscard]] auto day_cut_config() const -> const DayCutConfig& { return day_cut_config_; }

  void set_transport_profile(const TransportSessionProfile* profile) { transport_profile_ = profile; }

  [[nodiscard]] auto transport_profile() const -> const TransportSessionProfile* { return transport_profile_; }

  [[nodiscard]] auto pending_resend() const -> const std::optional<ResendRange>& { return pending_resend_; }

  /// Returns true (once) after resend processing completes, allowing
  /// AdminProtocol to know when to take post-resend actions. Consumes the flag.
  [[nodiscard]] auto ConsumeResendCompleted() -> bool
  {
    if (resend_completed_) {
      resend_completed_ = false;
      return true;
    }
    return false;
  }

  [[nodiscard]] auto Snapshot() const -> SessionSnapshot;
  [[nodiscard]] auto handle(std::uint32_t worker_id) const -> SessionHandle;

private:
  SessionConfig config_;
  SessionState state_{ SessionState::kDisconnected };
  SessionState resume_state_{ SessionState::kDisconnected };
  std::uint32_t next_in_seq_{ 1 };
  std::uint32_t next_out_seq_{ 1 };
  std::uint64_t last_inbound_ns_{ 0 };
  std::uint64_t last_outbound_ns_{ 0 };
  std::optional<ResendRange> pending_resend_{};
  bool resend_completed_{ false };
  DayCutConfig day_cut_config_{};
  std::int32_t last_day_cut_date_{ -1 }; // Julian-style day number to avoid re-triggering
  const TransportSessionProfile* transport_profile_{ nullptr };
};

} // namespace nimble::session
