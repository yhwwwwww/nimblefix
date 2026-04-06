#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "fastfix/base/result.h"
#include "fastfix/base/status.h"
#include "fastfix/session/session_handle.h"
#include "fastfix/session/session_key.h"
#include "fastfix/session/session_snapshot.h"

namespace fastfix::session {

struct SessionConfig {
    std::uint64_t session_id{0};
    SessionKey key;
    std::uint64_t profile_id{0};
    std::string default_appl_ver_id;
    std::uint32_t heartbeat_interval_seconds{30};
    bool is_initiator{false};
};

class SessionCore {
  public:
    explicit SessionCore(SessionConfig config);

    [[nodiscard]] const SessionKey& key() const {
        return config_.key;
    }

    [[nodiscard]] std::uint64_t session_id() const {
        return config_.session_id;
    }

    [[nodiscard]] std::uint64_t profile_id() const {
        return config_.profile_id;
    }

    [[nodiscard]] SessionState state() const {
        return state_;
    }

    auto OnTransportConnected() -> base::Status;
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

    [[nodiscard]] auto pending_resend() const -> const std::optional<ResendRange>& {
        return pending_resend_;
    }

    [[nodiscard]] auto Snapshot() const -> SessionSnapshot;
    [[nodiscard]] auto handle(std::uint32_t worker_id) const -> SessionHandle;

  private:
    SessionConfig config_;
    SessionState state_{SessionState::kDisconnected};
    SessionState resume_state_{SessionState::kDisconnected};
    std::uint32_t next_in_seq_{1};
    std::uint32_t next_out_seq_{1};
    std::uint64_t last_inbound_ns_{0};
    std::uint64_t last_outbound_ns_{0};
    std::optional<ResendRange> pending_resend_{};
};

}  // namespace fastfix::session
