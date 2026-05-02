#include <catch2/catch_test_macros.hpp>

#include "nimblefix/session/session_core.h"

#include "test_support.h"

using nimble::session::SessionConfig;
using nimble::session::SessionCore;
using nimble::session::SessionState;

namespace {

auto
MakeConfig() -> SessionConfig
{
  SessionConfig config;
  config.session_id = 111U;
  config.profile_id = 42U;
  config.is_initiator = true;
  config.key.begin_string = "FIX.4.4";
  config.key.sender_comp_id = "BUY";
  config.key.target_comp_id = "SELL";
  return config;
}

auto
MakeActive() -> SessionCore
{
  SessionCore s(MakeConfig());
  (void)s.OnTransportConnected();
  (void)s.OnLogonAccepted();
  return s;
}

auto
MakeResendProcessing() -> SessionCore
{
  auto s = MakeActive();
  (void)s.BeginResend(2U, 5U);
  return s;
}

auto
MakeAwaitingLogout() -> SessionCore
{
  auto s = MakeActive();
  (void)s.BeginLogout();
  return s;
}

} // namespace

// ---------------------------------------------------------------------------
// Original integration test (preserved)
// ---------------------------------------------------------------------------

TEST_CASE("session-core", "[session-core]")
{
  auto config = MakeConfig();

  SessionCore session(config);
  REQUIRE(session.state() == SessionState::kDisconnected);
  REQUIRE(session.OnTransportConnected().ok());
  REQUIRE(session.OnLogonAccepted().ok());

  auto outbound = session.AllocateOutboundSeq();
  REQUIRE(outbound.ok());
  REQUIRE(outbound.value() == 1U);
  REQUIRE(session.ObserveInboundSeq(1U).ok());
  REQUIRE(session.BeginLogout().ok());

  const auto snapshot = session.Snapshot();
  REQUIRE(snapshot.next_in_seq == 2U);
  REQUIRE(snapshot.next_out_seq == 2U);
  REQUIRE(snapshot.state == SessionState::kAwaitingLogout);

  SessionCore resend_session(config);
  REQUIRE(resend_session.OnTransportConnected().ok());
  REQUIRE(resend_session.OnLogonAccepted().ok());
  REQUIRE(resend_session.BeginResend(2U, 3U).ok());
  REQUIRE(resend_session.BeginLogout().ok());

  const auto resend_snapshot = resend_session.Snapshot();
  REQUIRE(resend_snapshot.state == SessionState::kAwaitingLogout);
  REQUIRE(!resend_snapshot.has_pending_resend);

  const auto handle = session.handle(3U);
  REQUIRE(handle.valid());
  REQUIRE(handle.session_id() == 111U);
  REQUIRE(handle.worker_id() == 3U);

  REQUIRE(session.OnTransportClosed().ok());
  REQUIRE(session.state() == SessionState::kDisconnected);
}

// ===========================================================================
// Legal state transitions — happy paths
// ===========================================================================

TEST_CASE("session-core: Disconnected -> Connected", "[session-core][legal]")
{
  SessionCore s(MakeConfig());
  REQUIRE(s.state() == SessionState::kDisconnected);
  REQUIRE(s.OnTransportConnected().ok());
  REQUIRE(s.state() == SessionState::kConnected);
}

TEST_CASE("session-core: Connected -> PendingLogon", "[session-core][legal]")
{
  SessionCore s(MakeConfig());
  REQUIRE(s.OnTransportConnected().ok());
  REQUIRE(s.BeginLogon().ok());
  REQUIRE(s.state() == SessionState::kPendingLogon);
}

TEST_CASE("session-core: Connected -> Active (direct logon accept)", "[session-core][legal]")
{
  SessionCore s(MakeConfig());
  REQUIRE(s.OnTransportConnected().ok());
  REQUIRE(s.OnLogonAccepted().ok());
  REQUIRE(s.state() == SessionState::kActive);
}

TEST_CASE("session-core: PendingLogon -> Active", "[session-core][legal]")
{
  SessionCore s(MakeConfig());
  REQUIRE(s.OnTransportConnected().ok());
  REQUIRE(s.BeginLogon().ok());
  REQUIRE(s.state() == SessionState::kPendingLogon);
  REQUIRE(s.OnLogonAccepted().ok());
  REQUIRE(s.state() == SessionState::kActive);
}

TEST_CASE("session-core: Active -> AwaitingLogout", "[session-core][legal]")
{
  auto s = MakeActive();
  REQUIRE(s.BeginLogout().ok());
  REQUIRE(s.state() == SessionState::kAwaitingLogout);
}

TEST_CASE("session-core: Active -> ResendProcessing (BeginResend)", "[session-core][legal]")
{
  auto s = MakeActive();
  REQUIRE(s.BeginResend(1U, 3U).ok());
  REQUIRE(s.state() == SessionState::kResendProcessing);
  REQUIRE(s.pending_resend().has_value());
  REQUIRE(s.pending_resend()->begin_seq == 1U);
  REQUIRE(s.pending_resend()->end_seq == 3U);
}

TEST_CASE("session-core: ResendProcessing -> Active (CompleteResend)", "[session-core][legal]")
{
  auto s = MakeResendProcessing();
  REQUIRE(s.state() == SessionState::kResendProcessing);
  REQUIRE(s.CompleteResend().ok());
  REQUIRE(s.state() == SessionState::kActive);
  REQUIRE(!s.pending_resend().has_value());
}

TEST_CASE("session-core: ResendProcessing -> AwaitingLogout (BeginLogout)", "[session-core][legal]")
{
  auto s = MakeResendProcessing();
  REQUIRE(s.BeginLogout().ok());
  REQUIRE(s.state() == SessionState::kAwaitingLogout);
  REQUIRE(!s.pending_resend().has_value());
}

TEST_CASE("session-core: AwaitingLogout -> ResendProcessing (BeginResend)", "[session-core][legal]")
{
  auto s = MakeAwaitingLogout();
  REQUIRE(s.BeginResend(1U, 2U).ok());
  REQUIRE(s.state() == SessionState::kResendProcessing);
}

TEST_CASE("session-core: ResendProcessing -> AwaitingLogout (CompleteResend "
          "restores resume)",
          "[session-core][legal]")
{
  // Enter resend from AwaitingLogout, then CompleteResend should return to
  // AwaitingLogout
  auto s = MakeAwaitingLogout();
  REQUIRE(s.BeginResend(1U, 2U).ok());
  REQUIRE(s.state() == SessionState::kResendProcessing);
  REQUIRE(s.CompleteResend().ok());
  REQUIRE(s.state() == SessionState::kAwaitingLogout);
}

TEST_CASE("session-core: OnTransportClosed from every state", "[session-core][legal]")
{
  SECTION("from Disconnected")
  {
    SessionCore s(MakeConfig());
    REQUIRE(s.OnTransportClosed().ok());
    REQUIRE(s.state() == SessionState::kDisconnected);
  }
  SECTION("from Connected")
  {
    SessionCore s(MakeConfig());
    REQUIRE(s.OnTransportConnected().ok());
    REQUIRE(s.OnTransportClosed().ok());
    REQUIRE(s.state() == SessionState::kDisconnected);
  }
  SECTION("from PendingLogon")
  {
    SessionCore s(MakeConfig());
    REQUIRE(s.OnTransportConnected().ok());
    REQUIRE(s.BeginLogon().ok());
    REQUIRE(s.OnTransportClosed().ok());
    REQUIRE(s.state() == SessionState::kDisconnected);
  }
  SECTION("from Active")
  {
    auto s = MakeActive();
    REQUIRE(s.OnTransportClosed().ok());
    REQUIRE(s.state() == SessionState::kDisconnected);
  }
  SECTION("from ResendProcessing")
  {
    auto s = MakeResendProcessing();
    REQUIRE(s.OnTransportClosed().ok());
    REQUIRE(s.state() == SessionState::kDisconnected);
  }
  SECTION("from AwaitingLogout")
  {
    auto s = MakeAwaitingLogout();
    REQUIRE(s.OnTransportClosed().ok());
    REQUIRE(s.state() == SessionState::kDisconnected);
  }
  SECTION("from Recovering")
  {
    auto s = MakeActive();
    REQUIRE(s.BeginRecovery().ok());
    REQUIRE(s.OnTransportClosed().ok());
    REQUIRE(s.state() == SessionState::kDisconnected);
  }
}

// ---------------------------------------------------------------------------
// Recovery transitions (BeginRecovery from each non-Recovering state)
// ---------------------------------------------------------------------------

TEST_CASE("session-core: BeginRecovery from Disconnected", "[session-core][legal][recovery]")
{
  SessionCore s(MakeConfig());
  REQUIRE(s.BeginRecovery().ok());
  REQUIRE(s.state() == SessionState::kRecovering);
  REQUIRE(s.FinishRecovery().ok());
  REQUIRE(s.state() == SessionState::kDisconnected);
}

TEST_CASE("session-core: BeginRecovery from Connected", "[session-core][legal][recovery]")
{
  SessionCore s(MakeConfig());
  REQUIRE(s.OnTransportConnected().ok());
  REQUIRE(s.BeginRecovery().ok());
  REQUIRE(s.state() == SessionState::kRecovering);
  REQUIRE(s.FinishRecovery().ok());
  REQUIRE(s.state() == SessionState::kConnected);
}

TEST_CASE("session-core: BeginRecovery from PendingLogon", "[session-core][legal][recovery]")
{
  SessionCore s(MakeConfig());
  REQUIRE(s.OnTransportConnected().ok());
  REQUIRE(s.BeginLogon().ok());
  REQUIRE(s.BeginRecovery().ok());
  REQUIRE(s.state() == SessionState::kRecovering);
  REQUIRE(s.FinishRecovery().ok());
  REQUIRE(s.state() == SessionState::kPendingLogon);
}

TEST_CASE("session-core: BeginRecovery from Active", "[session-core][legal][recovery]")
{
  auto s = MakeActive();
  REQUIRE(s.BeginRecovery().ok());
  REQUIRE(s.state() == SessionState::kRecovering);
  REQUIRE(s.FinishRecovery().ok());
  REQUIRE(s.state() == SessionState::kActive);
}

TEST_CASE("session-core: BeginRecovery from AwaitingLogout", "[session-core][legal][recovery]")
{
  auto s = MakeAwaitingLogout();
  REQUIRE(s.BeginRecovery().ok());
  REQUIRE(s.state() == SessionState::kRecovering);
  REQUIRE(s.FinishRecovery().ok());
  REQUIRE(s.state() == SessionState::kAwaitingLogout);
}

TEST_CASE("session-core: BeginRecovery from ResendProcessing", "[session-core][legal][recovery]")
{
  auto s = MakeResendProcessing();
  REQUIRE(s.BeginRecovery().ok());
  REQUIRE(s.state() == SessionState::kRecovering);
  REQUIRE(s.FinishRecovery().ok());
  REQUIRE(s.state() == SessionState::kResendProcessing);
}

// ===========================================================================
// Implicit transitions — gap detection & advance past resend end
// ===========================================================================

TEST_CASE("session-core: ObserveInboundSeq gap triggers ResendProcessing", "[session-core][legal][gap]")
{
  auto s = MakeActive();
  // Expect seq 1, observe seq 3 → gap [1,2]
  auto status = s.ObserveInboundSeq(3U);
  REQUIRE(!status.ok()); // gap detected is signalled as error
  REQUIRE(s.state() == SessionState::kResendProcessing);
  REQUIRE(s.pending_resend().has_value());
  REQUIRE(s.pending_resend()->begin_seq == 1U);
  REQUIRE(s.pending_resend()->end_seq == 2U);
}

TEST_CASE("session-core: AdvanceInboundExpectedSeq past resend end auto-completes", "[session-core][legal][gap]")
{
  auto s = MakeActive();
  // Create gap: expect 1, see 4 → gap [1, 3], state = ResendProcessing, resume
  // = Active
  (void)s.ObserveInboundSeq(4U);
  REQUIRE(s.state() == SessionState::kResendProcessing);
  REQUIRE(s.pending_resend()->end_seq == 3U);

  // Advance past end_seq
  REQUIRE(s.AdvanceInboundExpectedSeq(4U).ok());
  REQUIRE(s.state() == SessionState::kActive);
  REQUIRE(!s.pending_resend().has_value());
}

TEST_CASE("session-core: AdvanceInboundExpectedSeq within range stays "
          "ResendProcessing",
          "[session-core][legal][gap]")
{
  auto s = MakeActive();
  (void)s.ObserveInboundSeq(5U); // gap [1, 4]
  REQUIRE(s.state() == SessionState::kResendProcessing);

  // Advance to 3 — still within gap range [1,4]
  REQUIRE(s.AdvanceInboundExpectedSeq(3U).ok());
  REQUIRE(s.state() == SessionState::kResendProcessing);
  REQUIRE(s.pending_resend().has_value());
}

// ===========================================================================
// Full lifecycle: Disconnected → Connected → PendingLogon → Active →
//                 AwaitingLogout → Disconnected
// ===========================================================================

TEST_CASE("session-core: full initiator lifecycle", "[session-core][legal][lifecycle]")
{
  SessionCore s(MakeConfig());
  REQUIRE(s.state() == SessionState::kDisconnected);

  REQUIRE(s.OnTransportConnected().ok());
  REQUIRE(s.state() == SessionState::kConnected);

  REQUIRE(s.BeginLogon().ok());
  REQUIRE(s.state() == SessionState::kPendingLogon);

  REQUIRE(s.OnLogonAccepted().ok());
  REQUIRE(s.state() == SessionState::kActive);

  REQUIRE(s.BeginLogout().ok());
  REQUIRE(s.state() == SessionState::kAwaitingLogout);

  REQUIRE(s.OnTransportClosed().ok());
  REQUIRE(s.state() == SessionState::kDisconnected);
}

// ===========================================================================
// Illegal transitions — each gets an independent test case
// ===========================================================================

TEST_CASE("session-core: illegal OnTransportConnected from Connected", "[session-core][illegal]")
{
  SessionCore s(MakeConfig());
  REQUIRE(s.OnTransportConnected().ok());
  REQUIRE(!s.OnTransportConnected().ok());
  REQUIRE(s.state() == SessionState::kConnected);
}

TEST_CASE("session-core: illegal OnTransportConnected from PendingLogon", "[session-core][illegal]")
{
  SessionCore s(MakeConfig());
  REQUIRE(s.OnTransportConnected().ok());
  REQUIRE(s.BeginLogon().ok());
  REQUIRE(!s.OnTransportConnected().ok());
  REQUIRE(s.state() == SessionState::kPendingLogon);
}

TEST_CASE("session-core: illegal OnTransportConnected from Active", "[session-core][illegal]")
{
  auto s = MakeActive();
  REQUIRE(!s.OnTransportConnected().ok());
  REQUIRE(s.state() == SessionState::kActive);
}

TEST_CASE("session-core: illegal OnTransportConnected from AwaitingLogout", "[session-core][illegal]")
{
  auto s = MakeAwaitingLogout();
  REQUIRE(!s.OnTransportConnected().ok());
  REQUIRE(s.state() == SessionState::kAwaitingLogout);
}

TEST_CASE("session-core: illegal OnTransportConnected from ResendProcessing", "[session-core][illegal]")
{
  auto s = MakeResendProcessing();
  REQUIRE(!s.OnTransportConnected().ok());
  REQUIRE(s.state() == SessionState::kResendProcessing);
}

TEST_CASE("session-core: illegal OnTransportConnected from Recovering", "[session-core][illegal]")
{
  auto s = MakeActive();
  REQUIRE(s.BeginRecovery().ok());
  REQUIRE(!s.OnTransportConnected().ok());
  REQUIRE(s.state() == SessionState::kRecovering);
}

TEST_CASE("session-core: illegal BeginLogon from Disconnected", "[session-core][illegal]")
{
  SessionCore s(MakeConfig());
  REQUIRE(!s.BeginLogon().ok());
  REQUIRE(s.state() == SessionState::kDisconnected);
}

TEST_CASE("session-core: illegal BeginLogon from PendingLogon (double logon)", "[session-core][illegal]")
{
  SessionCore s(MakeConfig());
  REQUIRE(s.OnTransportConnected().ok());
  REQUIRE(s.BeginLogon().ok());
  REQUIRE(!s.BeginLogon().ok());
  REQUIRE(s.state() == SessionState::kPendingLogon);
}

TEST_CASE("session-core: illegal BeginLogon from Active", "[session-core][illegal]")
{
  auto s = MakeActive();
  REQUIRE(!s.BeginLogon().ok());
  REQUIRE(s.state() == SessionState::kActive);
}

TEST_CASE("session-core: illegal BeginLogon from AwaitingLogout", "[session-core][illegal]")
{
  auto s = MakeAwaitingLogout();
  REQUIRE(!s.BeginLogon().ok());
  REQUIRE(s.state() == SessionState::kAwaitingLogout);
}

TEST_CASE("session-core: illegal OnLogonAccepted from Active (duplicate logon)", "[session-core][illegal]")
{
  auto s = MakeActive();
  REQUIRE(!s.OnLogonAccepted().ok());
  REQUIRE(s.state() == SessionState::kActive);
}

TEST_CASE("session-core: illegal OnLogonAccepted from Disconnected", "[session-core][illegal]")
{
  SessionCore s(MakeConfig());
  REQUIRE(!s.OnLogonAccepted().ok());
  REQUIRE(s.state() == SessionState::kDisconnected);
}

TEST_CASE("session-core: illegal OnLogonAccepted from AwaitingLogout", "[session-core][illegal]")
{
  auto s = MakeAwaitingLogout();
  REQUIRE(!s.OnLogonAccepted().ok());
  REQUIRE(s.state() == SessionState::kAwaitingLogout);
}

TEST_CASE("session-core: illegal OnLogonAccepted from ResendProcessing", "[session-core][illegal]")
{
  auto s = MakeResendProcessing();
  REQUIRE(!s.OnLogonAccepted().ok());
  REQUIRE(s.state() == SessionState::kResendProcessing);
}

TEST_CASE("session-core: illegal OnLogonAccepted from Recovering", "[session-core][illegal]")
{
  auto s = MakeActive();
  REQUIRE(s.BeginRecovery().ok());
  REQUIRE(!s.OnLogonAccepted().ok());
  REQUIRE(s.state() == SessionState::kRecovering);
}

TEST_CASE("session-core: illegal BeginLogout from Disconnected", "[session-core][illegal]")
{
  SessionCore s(MakeConfig());
  REQUIRE(!s.BeginLogout().ok());
  REQUIRE(s.state() == SessionState::kDisconnected);
}

TEST_CASE("session-core: illegal BeginLogout from Connected", "[session-core][illegal]")
{
  SessionCore s(MakeConfig());
  REQUIRE(s.OnTransportConnected().ok());
  REQUIRE(!s.BeginLogout().ok());
  REQUIRE(s.state() == SessionState::kConnected);
}

TEST_CASE("session-core: illegal BeginLogout from PendingLogon", "[session-core][illegal]")
{
  SessionCore s(MakeConfig());
  REQUIRE(s.OnTransportConnected().ok());
  REQUIRE(s.BeginLogon().ok());
  REQUIRE(!s.BeginLogout().ok());
  REQUIRE(s.state() == SessionState::kPendingLogon);
}

TEST_CASE("session-core: illegal BeginLogout from AwaitingLogout (double logout)", "[session-core][illegal]")
{
  auto s = MakeAwaitingLogout();
  REQUIRE(!s.BeginLogout().ok());
  REQUIRE(s.state() == SessionState::kAwaitingLogout);
}

TEST_CASE("session-core: illegal BeginRecovery from Recovering (double recovery)", "[session-core][illegal]")
{
  auto s = MakeActive();
  REQUIRE(s.BeginRecovery().ok());
  REQUIRE(!s.BeginRecovery().ok());
  REQUIRE(s.state() == SessionState::kRecovering);
}

TEST_CASE("session-core: illegal FinishRecovery from Disconnected", "[session-core][illegal]")
{
  SessionCore s(MakeConfig());
  REQUIRE(!s.FinishRecovery().ok());
  REQUIRE(s.state() == SessionState::kDisconnected);
}

TEST_CASE("session-core: illegal FinishRecovery from Active", "[session-core][illegal]")
{
  auto s = MakeActive();
  REQUIRE(!s.FinishRecovery().ok());
  REQUIRE(s.state() == SessionState::kActive);
}

TEST_CASE("session-core: illegal BeginResend from Disconnected", "[session-core][illegal]")
{
  SessionCore s(MakeConfig());
  REQUIRE(!s.BeginResend(1U, 2U).ok());
  REQUIRE(s.state() == SessionState::kDisconnected);
}

TEST_CASE("session-core: illegal BeginResend from Connected", "[session-core][illegal]")
{
  SessionCore s(MakeConfig());
  REQUIRE(s.OnTransportConnected().ok());
  REQUIRE(!s.BeginResend(1U, 2U).ok());
  REQUIRE(s.state() == SessionState::kConnected);
}

TEST_CASE("session-core: illegal BeginResend from PendingLogon", "[session-core][illegal]")
{
  SessionCore s(MakeConfig());
  REQUIRE(s.OnTransportConnected().ok());
  REQUIRE(s.BeginLogon().ok());
  REQUIRE(!s.BeginResend(1U, 2U).ok());
  REQUIRE(s.state() == SessionState::kPendingLogon);
}

TEST_CASE("session-core: illegal BeginResend from ResendProcessing", "[session-core][illegal]")
{
  auto s = MakeResendProcessing();
  REQUIRE(!s.BeginResend(6U, 7U).ok());
  REQUIRE(s.state() == SessionState::kResendProcessing);
}

TEST_CASE("session-core: illegal BeginResend with invalid range", "[session-core][illegal]")
{
  auto s = MakeActive();
  REQUIRE(!s.BeginResend(0U, 5U).ok());
  REQUIRE(!s.BeginResend(5U, 0U).ok());
  REQUIRE(!s.BeginResend(5U, 3U).ok());
  REQUIRE(s.state() == SessionState::kActive);
}

TEST_CASE("session-core: illegal CompleteResend from Active", "[session-core][illegal]")
{
  auto s = MakeActive();
  REQUIRE(!s.CompleteResend().ok());
  REQUIRE(s.state() == SessionState::kActive);
}

TEST_CASE("session-core: illegal CompleteResend from Disconnected", "[session-core][illegal]")
{
  SessionCore s(MakeConfig());
  REQUIRE(!s.CompleteResend().ok());
  REQUIRE(s.state() == SessionState::kDisconnected);
}

TEST_CASE("session-core: illegal AllocateOutboundSeq from Disconnected", "[session-core][illegal]")
{
  SessionCore s(MakeConfig());
  REQUIRE(!s.AllocateOutboundSeq().ok());
}

TEST_CASE("session-core: illegal AllocateOutboundSeq from Recovering", "[session-core][illegal]")
{
  auto s = MakeActive();
  REQUIRE(s.BeginRecovery().ok());
  REQUIRE(!s.AllocateOutboundSeq().ok());
}

TEST_CASE("session-core: illegal ObserveInboundSeq from Disconnected", "[session-core][illegal]")
{
  SessionCore s(MakeConfig());
  REQUIRE(!s.ObserveInboundSeq(1U).ok());
}

TEST_CASE("session-core: illegal ObserveInboundSeq from Recovering", "[session-core][illegal]")
{
  auto s = MakeActive();
  REQUIRE(s.BeginRecovery().ok());
  REQUIRE(!s.ObserveInboundSeq(1U).ok());
}

TEST_CASE("session-core: illegal ObserveInboundSeq duplicate/stale seq", "[session-core][illegal]")
{
  auto s = MakeActive();
  REQUIRE(s.ObserveInboundSeq(1U).ok());
  REQUIRE(!s.ObserveInboundSeq(1U).ok()); // duplicate
}

TEST_CASE("session-core: illegal AdvanceInboundExpectedSeq backwards", "[session-core][illegal]")
{
  auto s = MakeActive();
  REQUIRE(s.ObserveInboundSeq(1U).ok());          // next_in_seq_ = 2
  REQUIRE(!s.AdvanceInboundExpectedSeq(1U).ok()); // can't go backwards
}

TEST_CASE("session-core: illegal AdvanceInboundExpectedSeq zero", "[session-core][illegal]")
{
  auto s = MakeActive();
  REQUIRE(!s.AdvanceInboundExpectedSeq(0U).ok());
}

// ===========================================================================
// Timeout-equivalent paths — transport close during pending states
// ===========================================================================

TEST_CASE("session-core: PendingLogon timeout -> disconnect", "[session-core][timeout]")
{
  SessionCore s(MakeConfig());
  REQUIRE(s.OnTransportConnected().ok());
  REQUIRE(s.BeginLogon().ok());
  REQUIRE(s.state() == SessionState::kPendingLogon);
  // Simulate timeout: transport layer closes connection
  REQUIRE(s.OnTransportClosed().ok());
  REQUIRE(s.state() == SessionState::kDisconnected);
}

TEST_CASE("session-core: AwaitingLogout timeout -> disconnect", "[session-core][timeout]")
{
  auto s = MakeAwaitingLogout();
  REQUIRE(s.state() == SessionState::kAwaitingLogout);
  // Simulate timeout: transport layer closes connection
  REQUIRE(s.OnTransportClosed().ok());
  REQUIRE(s.state() == SessionState::kDisconnected);
}

TEST_CASE("session-core: heartbeat activity recording", "[session-core][timeout]")
{
  auto s = MakeActive();
  REQUIRE(s.RecordInboundActivity(1000U).ok());
  REQUIRE(s.RecordOutboundActivity(2000U).ok());
  auto snap = s.Snapshot();
  REQUIRE(snap.last_inbound_ns == 1000U);
  REQUIRE(snap.last_outbound_ns == 2000U);

  REQUIRE(s.RecordInboundActivity(3000U).ok());
  snap = s.Snapshot();
  REQUIRE(snap.last_inbound_ns == 3000U);
  REQUIRE(snap.last_outbound_ns == 2000U);
}

// ===========================================================================
// Edge cases
// ===========================================================================

TEST_CASE("session-core: disconnect during ResendProcessing clears state", "[session-core][edge]")
{
  auto s = MakeResendProcessing();
  REQUIRE(s.pending_resend().has_value());
  REQUIRE(s.OnTransportClosed().ok());
  REQUIRE(s.state() == SessionState::kDisconnected);
  // Note: pending_resend is NOT auto-cleared by OnTransportClosed (just state)
}

TEST_CASE("session-core: reconnect after clean disconnect", "[session-core][edge]")
{
  SessionCore s(MakeConfig());
  // First connection cycle
  REQUIRE(s.OnTransportConnected().ok());
  REQUIRE(s.OnLogonAccepted().ok());
  REQUIRE(s.BeginLogout().ok());
  REQUIRE(s.OnTransportClosed().ok());
  REQUIRE(s.state() == SessionState::kDisconnected);

  // Second connection cycle
  REQUIRE(s.OnTransportConnected().ok());
  REQUIRE(s.state() == SessionState::kConnected);
  REQUIRE(s.BeginLogon().ok());
  REQUIRE(s.OnLogonAccepted().ok());
  REQUIRE(s.state() == SessionState::kActive);
}

TEST_CASE("session-core: disconnect during PendingLogon then reconnect", "[session-core][edge]")
{
  SessionCore s(MakeConfig());
  REQUIRE(s.OnTransportConnected().ok());
  REQUIRE(s.BeginLogon().ok());
  // Disconnect before logon accepted
  REQUIRE(s.OnTransportClosed().ok());
  REQUIRE(s.state() == SessionState::kDisconnected);
  // Reconnect
  REQUIRE(s.OnTransportConnected().ok());
  REQUIRE(s.BeginLogon().ok());
  REQUIRE(s.OnLogonAccepted().ok());
  REQUIRE(s.state() == SessionState::kActive);
}

TEST_CASE("session-core: BeginLogout during ResendProcessing clears pending resend", "[session-core][edge]")
{
  auto s = MakeResendProcessing();
  REQUIRE(s.pending_resend().has_value());
  REQUIRE(s.BeginLogout().ok());
  REQUIRE(s.state() == SessionState::kAwaitingLogout);
  REQUIRE(!s.pending_resend().has_value());
}

TEST_CASE("session-core: gap detection from AwaitingLogout", "[session-core][edge]")
{
  auto s = MakeActive();
  REQUIRE(s.ObserveInboundSeq(1U).ok());
  REQUIRE(s.BeginLogout().ok());
  REQUIRE(s.state() == SessionState::kAwaitingLogout);
  // Gap during logout
  auto status = s.ObserveInboundSeq(5U);
  REQUIRE(!status.ok());
  REQUIRE(s.state() == SessionState::kResendProcessing);
  // Complete resend should return to AwaitingLogout
  REQUIRE(s.CompleteResend().ok());
  REQUIRE(s.state() == SessionState::kAwaitingLogout);
}

TEST_CASE("session-core: sequence state restore", "[session-core][edge]")
{
  SessionCore s(MakeConfig());
  REQUIRE(s.RestoreSequenceState(100U, 200U).ok());
  auto snap = s.Snapshot();
  REQUIRE(snap.next_in_seq == 100U);
  REQUIRE(snap.next_out_seq == 200U);
}

TEST_CASE("session-core: illegal RestoreSequenceState with zero", "[session-core][illegal]")
{
  SessionCore s(MakeConfig());
  REQUIRE(!s.RestoreSequenceState(0U, 1U).ok());
  REQUIRE(!s.RestoreSequenceState(1U, 0U).ok());
}

TEST_CASE("session-core: AllocateOutboundSeq increments", "[session-core][edge]")
{
  auto s = MakeActive();
  auto r1 = s.AllocateOutboundSeq();
  REQUIRE(r1.ok());
  REQUIRE(r1.value() == 1U);
  auto r2 = s.AllocateOutboundSeq();
  REQUIRE(r2.ok());
  REQUIRE(r2.value() == 2U);
  auto r3 = s.AllocateOutboundSeq();
  REQUIRE(r3.ok());
  REQUIRE(r3.value() == 3U);
}

TEST_CASE("session-core: AllocateOutboundSeq from valid non-Active states", "[session-core][edge]")
{
  SECTION("Connected")
  {
    SessionCore s(MakeConfig());
    REQUIRE(s.OnTransportConnected().ok());
    REQUIRE(s.AllocateOutboundSeq().ok());
  }
  SECTION("PendingLogon")
  {
    SessionCore s(MakeConfig());
    REQUIRE(s.OnTransportConnected().ok());
    REQUIRE(s.BeginLogon().ok());
    REQUIRE(s.AllocateOutboundSeq().ok());
  }
  SECTION("AwaitingLogout")
  {
    auto s = MakeAwaitingLogout();
    REQUIRE(s.AllocateOutboundSeq().ok());
  }
  SECTION("ResendProcessing")
  {
    auto s = MakeResendProcessing();
    REQUIRE(s.AllocateOutboundSeq().ok());
  }
}

TEST_CASE("session-core: snapshot reflects all fields", "[session-core][edge]")
{
  auto config = MakeConfig();
  SessionCore s(config);
  REQUIRE(s.OnTransportConnected().ok());
  REQUIRE(s.OnLogonAccepted().ok());
  REQUIRE(s.RecordInboundActivity(42U).ok());
  REQUIRE(s.RecordOutboundActivity(99U).ok());
  REQUIRE(s.ObserveInboundSeq(1U).ok());
  auto seq = s.AllocateOutboundSeq();
  REQUIRE(seq.ok());

  auto snap = s.Snapshot();
  REQUIRE(snap.session_id == 111U);
  REQUIRE(snap.profile_id == 42U);
  REQUIRE(snap.state == SessionState::kActive);
  REQUIRE(snap.next_in_seq == 2U);
  REQUIRE(snap.next_out_seq == 2U);
  REQUIRE(snap.last_inbound_ns == 42U);
  REQUIRE(snap.last_outbound_ns == 99U);
  REQUIRE(!snap.has_pending_resend);
}

TEST_CASE("session-core warmup counter transitions and snapshots", "[session-core][warmup]")
{
  auto s = MakeActive();
  REQUIRE_FALSE(s.is_warmup());
  REQUIRE_FALSE(s.Snapshot().is_warmup);

  s.SetWarmupCount(2U);
  REQUIRE(s.is_warmup());
  REQUIRE(s.Snapshot().is_warmup);

  REQUIRE(s.ConsumeWarmupMessage());
  REQUIRE(s.is_warmup());
  REQUIRE(s.Snapshot().is_warmup);

  REQUIRE(s.ConsumeWarmupMessage());
  REQUIRE_FALSE(s.is_warmup());
  REQUIRE_FALSE(s.Snapshot().is_warmup);

  REQUIRE_FALSE(s.ConsumeWarmupMessage());
  REQUIRE_FALSE(s.is_warmup());
}

TEST_CASE("session-core warmup count zero stays disabled", "[session-core][warmup]")
{
  auto s = MakeActive();
  s.SetWarmupCount(0U);

  REQUIRE_FALSE(s.is_warmup());
  REQUIRE_FALSE(s.ConsumeWarmupMessage());
  REQUIRE_FALSE(s.Snapshot().is_warmup);
}

TEST_CASE("session-core: handle fields", "[session-core][edge]")
{
  SessionCore s(MakeConfig());
  auto h = s.handle(7U);
  REQUIRE(h.valid());
  REQUIRE(h.session_id() == 111U);
  REQUIRE(h.worker_id() == 7U);
}

TEST_CASE("session-core: key and config accessors", "[session-core][edge]")
{
  auto config = MakeConfig();
  SessionCore s(config);
  REQUIRE(s.session_id() == 111U);
  REQUIRE(s.profile_id() == 42U);
  REQUIRE(s.key().begin_string == "FIX.4.4");
  REQUIRE(s.key().sender_comp_id == "BUY");
  REQUIRE(s.key().target_comp_id == "SELL");
}

// ===========================================================================
// UINT32_MAX sequence number wraparound boundary
// ===========================================================================

TEST_CASE("session-core: outbound seq near UINT32_MAX", "[session-core][edge][wraparound]")
{
  auto config = MakeConfig();
  SessionCore s(config);
  REQUIRE(s.OnTransportConnected().ok());
  REQUIRE(s.OnLogonAccepted().ok());

  // Restore to near UINT32_MAX
  REQUIRE(s.RestoreSequenceState(1U, UINT32_MAX - 2U).ok());

  auto snap = s.Snapshot();
  REQUIRE(snap.next_out_seq == UINT32_MAX - 2U);

  // Allocate: UINT32_MAX-2
  auto seq1 = s.AllocateOutboundSeq();
  REQUIRE(seq1.ok());
  REQUIRE(seq1.value() == UINT32_MAX - 2U);

  // Allocate: UINT32_MAX-1
  auto seq2 = s.AllocateOutboundSeq();
  REQUIRE(seq2.ok());
  REQUIRE(seq2.value() == UINT32_MAX - 1U);

  // Allocate: UINT32_MAX — should fail (8.3: overflow returns error)
  auto seq3 = s.AllocateOutboundSeq();
  REQUIRE(!seq3.ok());
}

TEST_CASE("session-core: inbound seq near UINT32_MAX", "[session-core][edge][wraparound]")
{
  auto config = MakeConfig();
  SessionCore s(config);
  REQUIRE(s.OnTransportConnected().ok());
  REQUIRE(s.OnLogonAccepted().ok());

  // Restore to near UINT32_MAX for inbound
  REQUIRE(s.RestoreSequenceState(UINT32_MAX - 1U, 1U).ok());

  auto snap = s.Snapshot();
  REQUIRE(snap.next_in_seq == UINT32_MAX - 1U);

  // Observe UINT32_MAX - 1
  REQUIRE(s.ObserveInboundSeq(UINT32_MAX - 1U).ok());
  // Observe UINT32_MAX
  REQUIRE(s.ObserveInboundSeq(UINT32_MAX).ok());

  auto post_snap = s.Snapshot();
  REQUIRE(post_snap.next_in_seq == 0U); // wraps to 0
}
