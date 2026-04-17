#include "fastfix/session/session_core.h"

#include <functional>
#include <limits>

namespace fastfix::session {

namespace {

constexpr std::size_t kHashCombineMix = 0x9e3779b9U;

auto
HashOptional(const std::optional<std::string>& value) -> std::size_t
{
  if (!value.has_value()) {
    return 0U;
  }
  return std::hash<std::string>{}(*value);
}

auto
CombineHash(std::size_t seed, std::size_t value) -> std::size_t
{
  return seed ^ (value + kHashCombineMix + (seed << 6U) + (seed >> 2U));
}

} // namespace

auto
SessionKeyHash::operator()(const SessionKey& key) const -> std::size_t
{
  std::size_t seed = std::hash<std::string>{}(key.begin_string);
  seed = CombineHash(seed, std::hash<std::string>{}(key.sender_comp_id));
  seed = CombineHash(seed, std::hash<std::string>{}(key.target_comp_id));
  seed = CombineHash(seed, HashOptional(key.sender_location_id));
  seed = CombineHash(seed, HashOptional(key.target_location_id));
  seed = CombineHash(seed, HashOptional(key.session_qualifier));
  return seed;
}

SessionCore::SessionCore(SessionConfig config)
  : config_(std::move(config))
{
}

auto
SessionCore::BeginConnect() -> base::Status
{
  if (state_ != SessionState::kDisconnected) {
    return base::Status::InvalidArgument("connect can only begin from the disconnected state");
  }

  state_ = SessionState::kConnecting;
  return base::Status::Ok();
}

auto
SessionCore::OnTransportConnected() -> base::Status
{
  if (state_ != SessionState::kDisconnected && state_ != SessionState::kConnecting) {
    return base::Status::InvalidArgument("transport can only connect from the disconnected or connecting state");
  }

  state_ = SessionState::kConnected;
  return base::Status::Ok();
}

auto
SessionCore::Close() -> base::Status
{
  pending_resend_.reset();
  state_ = SessionState::kClosed;
  return base::Status::Ok();
}

auto
SessionCore::BeginRecovery() -> base::Status
{
  if (state_ == SessionState::kRecovering) {
    return base::Status::InvalidArgument("session is already recovering");
  }

  resume_state_ = state_;
  state_ = SessionState::kRecovering;
  return base::Status::Ok();
}

auto
SessionCore::FinishRecovery() -> base::Status
{
  if (state_ != SessionState::kRecovering) {
    return base::Status::InvalidArgument("session is not recovering");
  }

  state_ = resume_state_;
  return base::Status::Ok();
}

auto
SessionCore::BeginLogon() -> base::Status
{
  if (state_ != SessionState::kConnected) {
    return base::Status::InvalidArgument("logon can only begin from the connected state");
  }

  state_ = SessionState::kPendingLogon;
  return base::Status::Ok();
}

auto
SessionCore::RestoreSequenceState(std::uint32_t next_in_seq, std::uint32_t next_out_seq) -> base::Status
{
  if (state_ != SessionState::kDisconnected && state_ != SessionState::kConnected &&
      state_ != SessionState::kPendingLogon && state_ != SessionState::kActive && state_ != SessionState::kRecovering) {
    return base::Status::InvalidArgument("sequence state can only be restored before session activation");
  }

  if (next_in_seq == 0 || next_out_seq == 0) {
    return base::Status::InvalidArgument("sequence numbers must be positive");
  }

  next_in_seq_ = next_in_seq;
  next_out_seq_ = next_out_seq;
  pending_resend_.reset();
  return base::Status::Ok();
}

auto
SessionCore::AdvanceInboundExpectedSeq(std::uint32_t next_in_seq) -> base::Status
{
  if (next_in_seq == 0) {
    return base::Status::InvalidArgument("sequence numbers must be positive");
  }
  if (next_in_seq < next_in_seq_) {
    return base::Status::InvalidArgument("cannot move inbound sequence backwards");
  }

  next_in_seq_ = next_in_seq;
  if (state_ == SessionState::kResendProcessing && pending_resend_.has_value() &&
      next_in_seq_ > pending_resend_->end_seq) {
    pending_resend_.reset();
    state_ = resume_state_;
    resend_completed_ = true;
  }
  return base::Status::Ok();
}

auto
SessionCore::OnLogonAccepted() -> base::Status
{
  if (state_ != SessionState::kConnected && state_ != SessionState::kPendingLogon) {
    return base::Status::InvalidArgument("logon can only be accepted from the connected or pending-logon state");
  }

  state_ = SessionState::kActive;
  return base::Status::Ok();
}

auto
SessionCore::BeginLogout() -> base::Status
{
  if (state_ != SessionState::kActive && state_ != SessionState::kResendProcessing) {
    return base::Status::InvalidArgument("logout can only begin from the active or resend-processing state");
  }

  pending_resend_.reset();
  state_ = SessionState::kAwaitingLogout;
  return base::Status::Ok();
}

auto
SessionCore::OnTransportClosed() -> base::Status
{
  if (state_ != SessionState::kClosed) {
    state_ = SessionState::kDisconnected;
  }
  return base::Status::Ok();
}

auto
SessionCore::AllocateOutboundSeq() -> base::Result<std::uint32_t>
{
  if (state_ != SessionState::kConnected && state_ != SessionState::kPendingLogon && state_ != SessionState::kActive &&
      state_ != SessionState::kAwaitingLogout && state_ != SessionState::kResendProcessing) {
    return base::Status::InvalidArgument("outbound sequence numbers can only "
                                         "be allocated for an active session");
  }

  if (next_out_seq_ == std::numeric_limits<std::uint32_t>::max()) {
    return base::Status::InvalidArgument("outbound sequence number overflow; session reset required");
  }
  return next_out_seq_++;
}

auto
SessionCore::ObserveInboundSeq(std::uint32_t seq_num) -> base::Status
{
  if (state_ != SessionState::kConnected && state_ != SessionState::kPendingLogon && state_ != SessionState::kActive &&
      state_ != SessionState::kAwaitingLogout && state_ != SessionState::kResendProcessing) {
    return base::Status::InvalidArgument("inbound sequence numbers can only be observed for an active session");
  }
  if (seq_num < next_in_seq_) {
    return base::Status::InvalidArgument("received a duplicate or stale inbound sequence number");
  }
  if (seq_num > next_in_seq_) {
    if (state_ == SessionState::kResendProcessing && pending_resend_.has_value()) {
      // Already recovering: extend the gap range rather than overwriting
      // resume_state_. Overwriting resume_state_ here would cause
      // CompleteResend() to restore to kResendProcessing instead of the
      // original active state.
      const auto new_end = seq_num - 1;
      if (new_end > pending_resend_->end_seq) {
        pending_resend_->end_seq = new_end;
      }
    } else {
      pending_resend_ = ResendRange{ .begin_seq = next_in_seq_, .end_seq = seq_num - 1 };
      resume_state_ = state_;
      state_ = SessionState::kResendProcessing;
    }
    return base::Status::InvalidArgument("inbound sequence gap detected");
  }

  ++next_in_seq_;
  if (state_ == SessionState::kResendProcessing && pending_resend_.has_value() &&
      next_in_seq_ > pending_resend_->end_seq) {
    pending_resend_.reset();
    state_ = resume_state_;
    resend_completed_ = true;
  }
  return base::Status::Ok();
}

auto
SessionCore::BeginResend(std::uint32_t begin_seq, std::uint32_t end_seq) -> base::Status
{
  if (state_ != SessionState::kActive && state_ != SessionState::kAwaitingLogout) {
    return base::Status::InvalidArgument("resend can only begin from an active session");
  }
  if (begin_seq == 0 || end_seq == 0 || begin_seq > end_seq) {
    return base::Status::InvalidArgument("invalid resend range");
  }

  pending_resend_ = ResendRange{ .begin_seq = begin_seq, .end_seq = end_seq };
  resume_state_ = state_;
  state_ = SessionState::kResendProcessing;
  return base::Status::Ok();
}

auto
SessionCore::CompleteResend() -> base::Status
{
  if (state_ != SessionState::kResendProcessing) {
    return base::Status::InvalidArgument("session is not processing a resend");
  }

  pending_resend_.reset();
  state_ = resume_state_;
  resend_completed_ = true;
  return base::Status::Ok();
}

auto
SessionCore::RecordInboundActivity(std::uint64_t timestamp_ns) -> base::Status
{
  last_inbound_ns_ = timestamp_ns;
  return base::Status::Ok();
}

auto
SessionCore::RecordOutboundActivity(std::uint64_t timestamp_ns) -> base::Status
{
  last_outbound_ns_ = timestamp_ns;
  return base::Status::Ok();
}

auto
SessionCore::Snapshot() const -> SessionSnapshot
{
  return SessionSnapshot{
    .session_id = config_.session_id,
    .state = state_,
    .profile_id = config_.profile_id,
    .next_in_seq = next_in_seq_,
    .next_out_seq = next_out_seq_,
    .last_inbound_ns = last_inbound_ns_,
    .last_outbound_ns = last_outbound_ns_,
    .has_pending_resend = pending_resend_.has_value(),
    .pending_resend = pending_resend_.value_or(ResendRange{}),
  };
}

auto
SessionCore::handle(std::uint32_t worker_id) const -> SessionHandle
{
  return SessionHandle(config_.session_id, worker_id);
}

auto
SessionCore::CheckDayCut(std::uint64_t now_ns) -> void
{
  const auto mode = day_cut_config_.mode;
  if (mode != DayCutMode::kFixedLocalTime && mode != DayCutMode::kFixedUtcTime) {
    return;
  }

  // Convert nanoseconds since epoch to seconds.
  const auto epoch_seconds = static_cast<std::int64_t>(now_ns / 1'000'000'000ULL);

  // Apply timezone offset for local-time mode.
  const auto adjusted_seconds =
    (mode == DayCutMode::kFixedLocalTime) ? epoch_seconds + day_cut_config_.utc_offset_seconds : epoch_seconds;

  // Compute day number and time-of-day.
  constexpr std::int64_t kSecondsPerDay = 86400;
  const auto day_number = static_cast<std::int32_t>(adjusted_seconds / kSecondsPerDay);
  const auto time_of_day = adjusted_seconds % kSecondsPerDay;
  const auto current_hour = static_cast<std::int32_t>(time_of_day / 3600);
  const auto current_minute = static_cast<std::int32_t>((time_of_day % 3600) / 60);

  // Has the reset time been reached today?
  const bool past_reset =
    (current_hour > day_cut_config_.reset_hour) ||
    (current_hour == day_cut_config_.reset_hour && current_minute >= day_cut_config_.reset_minute);

  // The effective day-cut date: if past reset time, we are in the "new" session
  // day.
  const auto effective_date = past_reset ? day_number : day_number - 1;

  if (last_day_cut_date_ < 0) {
    // First check: initialise without triggering.
    last_day_cut_date_ = effective_date;
    return;
  }

  if (effective_date <= last_day_cut_date_) {
    return; // Already triggered for this session day.
  }

  last_day_cut_date_ = effective_date;
  static_cast<void>(TriggerDayCut());
}

auto
SessionCore::TriggerDayCut() -> base::Status
{
  if (state_ != SessionState::kDisconnected && state_ != SessionState::kActive) {
    return base::Status::InvalidArgument("day-cut can only be triggered when session is disconnected or active");
  }

  next_in_seq_ = 1;
  next_out_seq_ = 1;
  pending_resend_.reset();
  return base::Status::Ok();
}

} // namespace fastfix::session
