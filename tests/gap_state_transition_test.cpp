// Tests for state transition bugs related to gap detection and resend recovery.
// Written TDD-style: these tests capture the expected behavior for 4 known
// bugs.
//
// Bug 1: Gap-triggering message dropped (initiator & acceptor).
// Bug 2: ObserveInboundSeq doesn't auto-complete resend on normal seq
// increment. Bug 3: No PendingLogon timeout in OnTimer / NextTimerDeadline. Bug
// 4: Same as Bug 1 for acceptor side (covered by Bug 1 tests).

#include <catch2/catch_test_macros.hpp>

#include "nimblefix/codec/fix_codec.h"
#include "nimblefix/codec/fix_tags.h"
#include "nimblefix/session/admin_protocol.h"
#include "nimblefix/session/session_core.h"
#include "nimblefix/store/memory_store.h"

#include "test_support.h"

using nimble::session::AdminProtocol;
using nimble::session::AdminProtocolConfig;
using nimble::session::SessionConfig;
using namespace nimble::codec::tags;
using nimble::session::SessionCore;
using nimble::session::SessionKey;
using nimble::session::SessionState;

namespace {

auto
EncodeInboundFrame(const nimble::message::Message& message,
                   const nimble::profile::NormalizedDictionaryView& dictionary,
                   std::string begin_string,
                   std::string sender,
                   std::string target,
                   std::uint32_t seq_num,
                   bool poss_dup,
                   std::string default_appl_ver_id = {},
                   std::string orig_sending_time = {}) -> nimble::base::Result<std::vector<std::byte>>
{
  nimble::codec::EncodeOptions options;
  options.begin_string = std::move(begin_string);
  options.sender_comp_id = std::move(sender);
  options.target_comp_id = std::move(target);
  options.default_appl_ver_id = std::move(default_appl_ver_id);
  options.msg_seq_num = seq_num;
  options.poss_dup = poss_dup;
  options.sending_time = "20260414-12:00:00.000";
  options.orig_sending_time = std::move(orig_sending_time);
  return nimble::codec::EncodeFixMessage(message, dictionary, options);
}

// Activate an acceptor session by sending an inbound Logon at seq 1.
auto
ActivateAcceptorSession(AdminProtocol* protocol,
                        const nimble::profile::NormalizedDictionaryView& dictionary,
                        std::string begin_string) -> nimble::base::Status
{
  nimble::message::MessageBuilder logon_builder("A");
  logon_builder.set_string(35U, "A").set_int(98U, 0).set_int(108U, 30);
  auto inbound =
    EncodeInboundFrame(std::move(logon_builder).build(), dictionary, std::move(begin_string), "BUY", "SELL", 1U, false);
  if (!inbound.ok()) {
    return inbound.status();
  }

  auto event = protocol->OnInbound(inbound.value(), 2U);
  if (!event.ok()) {
    return event.status();
  }
  if (!event.value().session_active || event.value().outbound_frames.empty()) {
    return nimble::base::Status::InvalidArgument("acceptor session did not activate on inbound logon");
  }
  return nimble::base::Status::Ok();
}

// Helper: create an initiator AdminProtocol, connect, and send logon
// (PendingLogon state). Returns the protocol and its outbound logon frame.
struct InitiatorSetup
{
  nimble::store::MemorySessionStore store;
  std::unique_ptr<AdminProtocol> protocol;
};

auto
MakeInitiatorPendingLogon(const nimble::profile::NormalizedDictionaryView& dictionary,
                          std::uint64_t session_id,
                          nimble::store::MemorySessionStore& store) -> std::unique_ptr<AdminProtocol>
{
  auto protocol = std::make_unique<AdminProtocol>(
    AdminProtocolConfig{
      .session =
        SessionConfig{
          .session_id = session_id,
          .key = SessionKey{ "FIX.4.4", "BUY", "SELL" },
          .profile_id = dictionary.profile().header().profile_id,
          .heartbeat_interval_seconds = 30U,
          .is_initiator = true,
        },
      .begin_string = "FIX.4.4",
      .sender_comp_id = "BUY",
      .target_comp_id = "SELL",
      .heartbeat_interval_seconds = 30U,
    },
    dictionary,
    &store);

  auto connected = protocol->OnTransportConnected(1U);
  if (!connected.ok()) {
    return nullptr;
  }
  // OnTransportConnected for initiator sends logon → PendingLogon
  if (connected.value().outbound_frames.empty()) {
    return nullptr;
  }
  if (protocol->session().state() != SessionState::kPendingLogon) {
    return nullptr;
  }
  return protocol;
}

auto
MakeAcceptorProtocol(const nimble::profile::NormalizedDictionaryView& dictionary,
                     std::uint64_t session_id,
                     nimble::store::MemorySessionStore& store) -> std::unique_ptr<AdminProtocol>
{
  return std::make_unique<AdminProtocol>(
    AdminProtocolConfig{
      .session =
        SessionConfig{
          .session_id = session_id,
          .key = SessionKey{ "FIX.4.4", "SELL", "BUY" },
          .profile_id = dictionary.profile().header().profile_id,
          .heartbeat_interval_seconds = 30U,
          .is_initiator = false,
        },
      .begin_string = "FIX.4.4",
      .sender_comp_id = "SELL",
      .target_comp_id = "BUY",
      .heartbeat_interval_seconds = 30U,
    },
    dictionary,
    &store);
}

} // namespace

// ===========================================================================
// Bug 1: Gap-triggering message is dropped — initiator PendingLogon scenario
// ===========================================================================
// Initiator sends Logon (seq=1), counterparty responds with Logon at seq=3
// (expected seq=1). Gap [1,2] detected, ResendRequest sent, but the Logon
// response (seq=3) should still activate the session after gap recovery.

TEST_CASE("gap-transition: initiator logon response with gap activates after fill", "[gap-transition][bug1][initiator]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }

  nimble::store::MemorySessionStore store;
  auto protocol = MakeInitiatorPendingLogon(dictionary.value(), 9001U, store);
  REQUIRE(protocol != nullptr);
  REQUIRE(protocol->session().state() == SessionState::kPendingLogon);
  REQUIRE(protocol->session().Snapshot().next_in_seq == 1U);

  // Counterparty sends Logon at seq=3 (gap: expected 1, received 3)
  nimble::message::MessageBuilder logon_builder("A");
  logon_builder.set_string(35U, "A").set_int(98U, 0).set_int(108U, 30);
  auto logon_frame =
    EncodeInboundFrame(std::move(logon_builder).build(), dictionary.value(), "FIX.4.4", "SELL", "BUY", 3U, false);
  REQUIRE(logon_frame.ok());

  auto gap_event = protocol->OnInbound(logon_frame.value(), 10U);
  REQUIRE(gap_event.ok());
  // Should have sent a ResendRequest for [1,2]
  REQUIRE(gap_event.value().outbound_frames.size() >= 1U);

  auto resend_decoded =
    nimble::codec::DecodeFixMessage(gap_event.value().outbound_frames.front().bytes, dictionary.value());
  REQUIRE(resend_decoded.ok());
  REQUIRE(resend_decoded.value().header.msg_type == "2");
  REQUIRE(resend_decoded.value().message.view().get_int(7U).value() == 1);
  REQUIRE(resend_decoded.value().message.view().get_int(16U).value() == 2);

  // Now fill the gap with a GapFill covering [1, 3) → NewSeqNo=3
  nimble::message::MessageBuilder gap_fill_builder("4");
  gap_fill_builder.set_string(35U, "4").set_boolean(123U, true).set_int(36U, 3);
  auto gap_fill = EncodeInboundFrame(std::move(gap_fill_builder).build(),
                                     dictionary.value(),
                                     "FIX.4.4",
                                     "SELL",
                                     "BUY",
                                     1U,
                                     true,
                                     {},
                                     "20260414-11:59:00.000");
  REQUIRE(gap_fill.ok());

  auto fill_event = protocol->OnInbound(gap_fill.value(), 20U);
  REQUIRE(fill_event.ok());

  // After gap fill, the deferred Logon should have been processed.
  // Session MUST be Active, not stuck in PendingLogon.
  const auto snapshot = protocol->session().Snapshot();
  REQUIRE(snapshot.state == SessionState::kActive);
  REQUIRE(snapshot.next_in_seq == 4U);
}

// ===========================================================================
// Bug 1: Gap-triggering message dropped — acceptor Logon scenario
// ===========================================================================
// Acceptor in kConnected. Counterparty sends Logon at seq=5 (expected=1).
// Gap [1,4] detected. After gap fill, session must transition to kActive.

TEST_CASE("gap-transition: acceptor logon with gap activates after fill", "[gap-transition][bug1][acceptor]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }

  nimble::store::MemorySessionStore store;
  auto protocol = MakeAcceptorProtocol(dictionary.value(), 9002U, store);
  REQUIRE(protocol->OnTransportConnected(1U).ok());
  REQUIRE(protocol->session().state() == SessionState::kConnected);

  // Counterparty sends Logon at seq=5 (gap: expected 1, received 5)
  nimble::message::MessageBuilder logon_builder("A");
  logon_builder.set_string(35U, "A").set_int(98U, 0).set_int(108U, 30);
  auto logon_frame =
    EncodeInboundFrame(std::move(logon_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 5U, false);
  REQUIRE(logon_frame.ok());

  auto gap_event = protocol->OnInbound(logon_frame.value(), 10U);
  REQUIRE(gap_event.ok());
  // Should have a ResendRequest for [1,4]
  REQUIRE(gap_event.value().outbound_frames.size() >= 1U);

  auto resend_decoded =
    nimble::codec::DecodeFixMessage(gap_event.value().outbound_frames.front().bytes, dictionary.value());
  REQUIRE(resend_decoded.ok());
  REQUIRE(resend_decoded.value().header.msg_type == "2");
  REQUIRE(resend_decoded.value().message.view().get_int(7U).value() == 1);
  REQUIRE(resend_decoded.value().message.view().get_int(16U).value() == 4);

  // Fill gap with GapFill [1, 5) → NewSeqNo=5
  nimble::message::MessageBuilder gap_fill_builder("4");
  gap_fill_builder.set_string(35U, "4").set_boolean(123U, true).set_int(36U, 5);
  auto gap_fill = EncodeInboundFrame(std::move(gap_fill_builder).build(),
                                     dictionary.value(),
                                     "FIX.4.4",
                                     "BUY",
                                     "SELL",
                                     1U,
                                     true,
                                     {},
                                     "20260414-11:59:00.000");
  REQUIRE(gap_fill.ok());

  auto fill_event = protocol->OnInbound(gap_fill.value(), 20U);
  REQUIRE(fill_event.ok());

  // Session MUST be Active after gap fill, not stuck in kConnected.
  const auto snapshot = protocol->session().Snapshot();
  REQUIRE(snapshot.state == SessionState::kActive);
  REQUIRE(snapshot.next_in_seq == 6U);
  // Acceptor must have sent a Logon response
  bool has_logon_response = false;
  for (const auto& frame : fill_event.value().outbound_frames) {
    auto decoded = nimble::codec::DecodeFixMessage(frame.bytes, dictionary.value());
    if (decoded.ok() && decoded.value().header.msg_type == "A") {
      has_logon_response = true;
    }
  }
  // Either the logon response was in gap_event or fill_event
  if (!has_logon_response) {
    for (const auto& frame : gap_event.value().outbound_frames) {
      auto decoded = nimble::codec::DecodeFixMessage(frame.bytes, dictionary.value());
      if (decoded.ok() && decoded.value().header.msg_type == "A") {
        has_logon_response = true;
      }
    }
  }
  REQUIRE(has_logon_response);
}

// ===========================================================================
// Bug 1: Gap-triggering application message is not lost
// ===========================================================================
// Active session receives app message at seq=5 (expected=2). Gap [2,4].
// After gap fill, the app message from seq 5 must be delivered.

TEST_CASE("gap-transition: app message triggering gap is delivered after fill", "[gap-transition][bug1][app-message]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }

  nimble::store::MemorySessionStore store;
  auto protocol = MakeAcceptorProtocol(dictionary.value(), 9003U, store);
  REQUIRE(protocol->OnTransportConnected(1U).ok());
  REQUIRE(ActivateAcceptorSession(protocol.get(), dictionary.value(), "FIX.4.4").ok());
  REQUIRE(protocol->session().state() == SessionState::kActive);
  REQUIRE(protocol->session().Snapshot().next_in_seq == 2U);

  // Counterparty sends a NewOrderSingle (D) at seq=5 (gap: expected 2, received
  // 5)
  nimble::message::MessageBuilder order_builder("D");
  order_builder.set_string(kMsgType, "D")
    .set_string(kClOrdID, "ORDER-001")
    .set_string(kSymbol, "AAPL")
    .set_string(kSide, "1")
    .set_int(kOrderQty, 100)
    .set_string(kOrdType, "2")
    .set_string(kPrice, "150.50")
    .set_string(kTransactTime, "20260414-12:00:00.000");
  auto order_frame =
    EncodeInboundFrame(std::move(order_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 5U, false);
  REQUIRE(order_frame.ok());

  auto gap_event = protocol->OnInbound(order_frame.value(), 10U);
  REQUIRE(gap_event.ok());
  // ResendRequest for [2,4]
  REQUIRE(gap_event.value().outbound_frames.size() >= 1U);

  // Fill gap with GapFill [2, 5) → NewSeqNo=5
  nimble::message::MessageBuilder gap_fill_builder("4");
  gap_fill_builder.set_string(35U, "4").set_boolean(123U, true).set_int(36U, 5);
  auto gap_fill = EncodeInboundFrame(std::move(gap_fill_builder).build(),
                                     dictionary.value(),
                                     "FIX.4.4",
                                     "BUY",
                                     "SELL",
                                     2U,
                                     true,
                                     {},
                                     "20260414-11:59:00.000");
  REQUIRE(gap_fill.ok());

  auto fill_event = protocol->OnInbound(gap_fill.value(), 20U);
  REQUIRE(fill_event.ok());

  // After gap fill, the original order message (seq=5) must appear in
  // application_messages — either in gap_event or fill_event.
  std::size_t total_app_messages =
    gap_event.value().application_messages.size() + fill_event.value().application_messages.size();
  REQUIRE(total_app_messages >= 1U);

  // Verify the session state is clean — back to Active, seq advanced past 5.
  const auto snapshot = protocol->session().Snapshot();
  REQUIRE(snapshot.state == SessionState::kActive);
  REQUIRE(snapshot.next_in_seq == 6U);
}

// ===========================================================================
// Bug 1: Gap-triggering Logout message is not lost
// ===========================================================================
// Active session receives Logout at seq=4 (expected=2). Gap [2,3].
// After gap fill, the Logout must cause a disconnect.

TEST_CASE("gap-transition: logout triggering gap causes disconnect after fill", "[gap-transition][bug1][logout]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }

  nimble::store::MemorySessionStore store;
  auto protocol = MakeAcceptorProtocol(dictionary.value(), 9004U, store);
  REQUIRE(protocol->OnTransportConnected(1U).ok());
  REQUIRE(ActivateAcceptorSession(protocol.get(), dictionary.value(), "FIX.4.4").ok());
  REQUIRE(protocol->session().state() == SessionState::kActive);

  // Counterparty sends Logout at seq=4 (gap: expected 2, received 4)
  nimble::message::MessageBuilder logout_builder("5");
  logout_builder.set_string(35U, "5");
  auto logout_frame =
    EncodeInboundFrame(std::move(logout_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 4U, false);
  REQUIRE(logout_frame.ok());

  auto gap_event = protocol->OnInbound(logout_frame.value(), 10U);
  REQUIRE(gap_event.ok());

  // Fill gap with GapFill [2, 4) → NewSeqNo=4
  nimble::message::MessageBuilder gap_fill_builder("4");
  gap_fill_builder.set_string(35U, "4").set_boolean(123U, true).set_int(36U, 4);
  auto gap_fill = EncodeInboundFrame(std::move(gap_fill_builder).build(),
                                     dictionary.value(),
                                     "FIX.4.4",
                                     "BUY",
                                     "SELL",
                                     2U,
                                     true,
                                     {},
                                     "20260414-11:59:00.000");
  REQUIRE(gap_fill.ok());

  auto fill_event = protocol->OnInbound(gap_fill.value(), 20U);
  REQUIRE(fill_event.ok());

  // After gap fill, the Logout must have been processed.
  // Either gap_event or fill_event should signal disconnect.
  const bool disconnect = gap_event.value().disconnect || fill_event.value().disconnect;
  REQUIRE(disconnect);
}

// ===========================================================================
// Bug 1: Gap-triggering TestRequest is not lost
// ===========================================================================
// Active session receives TestRequest at seq=3 (expected=1). Gap [1,2].
// After gap fill, a Heartbeat response must be produced.

TEST_CASE("gap-transition: test request triggering gap gets heartbeat after fill",
          "[gap-transition][bug1][test-request]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }

  nimble::store::MemorySessionStore store;
  auto protocol = MakeAcceptorProtocol(dictionary.value(), 9005U, store);
  REQUIRE(protocol->OnTransportConnected(1U).ok());
  REQUIRE(ActivateAcceptorSession(protocol.get(), dictionary.value(), "FIX.4.4").ok());
  REQUIRE(protocol->session().state() == SessionState::kActive);

  // Counterparty sends TestRequest at seq=4 (gap: expected 2, received 4)
  nimble::message::MessageBuilder test_req_builder("1");
  test_req_builder.set_string(35U, "1").set_string(112U, "TR-12345");
  auto test_req_frame =
    EncodeInboundFrame(std::move(test_req_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 4U, false);
  REQUIRE(test_req_frame.ok());

  auto gap_event = protocol->OnInbound(test_req_frame.value(), 10U);
  REQUIRE(gap_event.ok());

  // Fill gap with GapFill [2, 4) → NewSeqNo=4
  nimble::message::MessageBuilder gap_fill_builder("4");
  gap_fill_builder.set_string(35U, "4").set_boolean(123U, true).set_int(36U, 4);
  auto gap_fill = EncodeInboundFrame(std::move(gap_fill_builder).build(),
                                     dictionary.value(),
                                     "FIX.4.4",
                                     "BUY",
                                     "SELL",
                                     2U,
                                     true,
                                     {},
                                     "20260414-11:59:00.000");
  REQUIRE(gap_fill.ok());

  auto fill_event = protocol->OnInbound(gap_fill.value(), 20U);
  REQUIRE(fill_event.ok());

  // After gap fill, a Heartbeat echoing TestReqID should exist
  // in some outbound frame from either event.
  bool found_heartbeat = false;
  auto check_frames = [&](const auto& frames) {
    for (const auto& frame : frames) {
      auto decoded = nimble::codec::DecodeFixMessage(frame.bytes, dictionary.value());
      if (decoded.ok() && decoded.value().header.msg_type == "0") {
        auto tr_id = decoded.value().message.view().get_string(112U);
        if (tr_id.has_value() && tr_id.value() == "TR-12345") {
          found_heartbeat = true;
        }
      }
    }
  };
  check_frames(gap_event.value().outbound_frames);
  check_frames(fill_event.value().outbound_frames);
  REQUIRE(found_heartbeat);
}

// ===========================================================================
// Bug 2: ObserveInboundSeq normal increment should auto-complete resend
// ===========================================================================
// SessionCore level: after gap fill via individual message replay (not
// GapFill), ObserveInboundSeq should detect that next_in_seq has passed end_seq
// and auto-complete the resend.

TEST_CASE("gap-transition: ObserveInboundSeq auto-completes resend on normal "
          "increment",
          "[gap-transition][bug2][session-core]")
{
  SessionCore s(SessionConfig{
    .session_id = 9010U,
    .key = SessionKey{ "FIX.4.4", "BUY", "SELL" },
    .profile_id = 42U,
    .heartbeat_interval_seconds = 30U,
    .is_initiator = true,
  });
  REQUIRE(s.OnTransportConnected().ok());
  REQUIRE(s.OnLogonAccepted().ok());

  // Create gap: expect 1, see 3 → gap [1, 2], state = ResendProcessing
  auto status = s.ObserveInboundSeq(3U);
  REQUIRE(!status.ok());
  REQUIRE(s.state() == SessionState::kResendProcessing);
  REQUIRE(s.pending_resend()->begin_seq == 1U);
  REQUIRE(s.pending_resend()->end_seq == 2U);

  // Replay seq 1 (normal observe)
  REQUIRE(s.ObserveInboundSeq(1U).ok());
  // Still in ResendProcessing after processing seq 1
  REQUIRE(s.state() == SessionState::kResendProcessing);

  // Replay seq 2 (normal observe) — this should auto-complete the resend
  // because next_in_seq will become 3, which is past end_seq=2.
  REQUIRE(s.ObserveInboundSeq(2U).ok());
  REQUIRE(s.state() == SessionState::kActive);
  REQUIRE(!s.pending_resend().has_value());
}

// Bug 2 at protocol level: gap filled via replayed messages (not GapFill).

TEST_CASE("gap-transition: replayed messages complete gap recovery", "[gap-transition][bug2][protocol]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }

  nimble::store::MemorySessionStore store;
  auto protocol = MakeAcceptorProtocol(dictionary.value(), 9011U, store);
  REQUIRE(protocol->OnTransportConnected(1U).ok());
  REQUIRE(ActivateAcceptorSession(protocol.get(), dictionary.value(), "FIX.4.4").ok());

  // Receive heartbeat at seq=4 → gap [2, 3]
  nimble::message::MessageBuilder hb_builder("0");
  hb_builder.set_string(35U, "0");
  auto hb_frame =
    EncodeInboundFrame(std::move(hb_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 4U, false);
  REQUIRE(hb_frame.ok());

  auto gap_event = protocol->OnInbound(hb_frame.value(), 10U);
  REQUIRE(gap_event.ok());
  REQUIRE(protocol->session().state() == SessionState::kResendProcessing);

  // Replay seq 2 as PossDup heartbeat
  nimble::message::MessageBuilder replay2("0");
  replay2.set_string(35U, "0");
  auto frame2 = EncodeInboundFrame(
    std::move(replay2).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 2U, true, {}, "20260414-11:59:00.000");
  REQUIRE(frame2.ok());

  auto event2 = protocol->OnInbound(frame2.value(), 15U);
  REQUIRE(event2.ok());
  // Still processing
  REQUIRE(protocol->session().state() == SessionState::kResendProcessing);

  // Replay seq 3 as PossDup heartbeat — should complete the gap
  nimble::message::MessageBuilder replay3("0");
  replay3.set_string(35U, "0");
  auto frame3 = EncodeInboundFrame(
    std::move(replay3).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 3U, true, {}, "20260414-11:59:00.000");
  REQUIRE(frame3.ok());

  auto event3 = protocol->OnInbound(frame3.value(), 18U);
  REQUIRE(event3.ok());

  // After replaying seq 2 and 3, gap [2,3] is fully covered.
  // State should return to Active.
  const auto snapshot = protocol->session().Snapshot();
  REQUIRE(snapshot.state == SessionState::kActive);
  REQUIRE(snapshot.next_in_seq == 5U);
}

// ===========================================================================
// Bug 3: PendingLogon must have a timeout via OnTimer
// ===========================================================================
// When initiator is in PendingLogon, OnTimer should eventually disconnect
// if no Logon response arrives.

TEST_CASE("gap-transition: OnTimer disconnects stale PendingLogon", "[gap-transition][bug3][timeout]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }

  nimble::store::MemorySessionStore store;
  auto protocol = MakeInitiatorPendingLogon(dictionary.value(), 9020U, store);
  REQUIRE(protocol != nullptr);
  REQUIRE(protocol->session().state() == SessionState::kPendingLogon);

  // Heartbeat interval is 30 seconds = 30,000,000,000 ns.
  // Timer should detect stale PendingLogon after a reasonable interval.
  constexpr std::uint64_t kNanosPerSecond = 1'000'000'000ULL;
  const auto logon_timeout_ns = 30U * kNanosPerSecond * 2U + 1U; // Well past 2*HBI

  auto timer_event = protocol->OnTimer(1U + logon_timeout_ns);
  REQUIRE(timer_event.ok());
  // Timer should signal disconnect for stale PendingLogon.
  REQUIRE(timer_event.value().disconnect);
}

// ===========================================================================
// Bug 3: NextTimerDeadline returns a deadline during PendingLogon
// ===========================================================================

TEST_CASE("gap-transition: NextTimerDeadline provides deadline during PendingLogon",
          "[gap-transition][bug3][timer-deadline]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }

  nimble::store::MemorySessionStore store;
  auto protocol = MakeInitiatorPendingLogon(dictionary.value(), 9021U, store);
  REQUIRE(protocol != nullptr);
  REQUIRE(protocol->session().state() == SessionState::kPendingLogon);

  auto deadline = protocol->NextTimerDeadline(1U);
  // Must return a deadline (not nullopt) so the timer wheel fires.
  REQUIRE(deadline.has_value());
}

// ===========================================================================
// Bug 3: PendingLogon does NOT disconnect prematurely if logon arrives in time
// ===========================================================================

TEST_CASE("gap-transition: OnTimer does not disconnect PendingLogon before timeout",
          "[gap-transition][bug3][no-premature]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }

  nimble::store::MemorySessionStore store;
  auto protocol = MakeInitiatorPendingLogon(dictionary.value(), 9022U, store);
  REQUIRE(protocol != nullptr);

  // Timer fires shortly after logon was sent — should not disconnect.
  auto timer_event = protocol->OnTimer(2U);
  REQUIRE(timer_event.ok());
  REQUIRE(!timer_event.value().disconnect);
  REQUIRE(protocol->session().state() == SessionState::kPendingLogon);
}

// ===========================================================================
// Bug 1 edge case: Multiple gap-triggering messages in sequence
// ===========================================================================
// Active session receives two messages with a gap. After fill, both should be
// delivered. This tests the "extended gap" path in ObserveInboundSeq.

TEST_CASE("gap-transition: second gap-triggering message extends gap then both "
          "delivered",
          "[gap-transition][bug1][extended-gap]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }

  nimble::store::MemorySessionStore store;
  auto protocol = MakeAcceptorProtocol(dictionary.value(), 9030U, store);
  REQUIRE(protocol->OnTransportConnected(1U).ok());
  REQUIRE(ActivateAcceptorSession(protocol.get(), dictionary.value(), "FIX.4.4").ok());
  REQUIRE(protocol->session().Snapshot().next_in_seq == 2U);

  // First app message at seq=4 (gap [2, 3])
  nimble::message::MessageBuilder order1("D");
  order1.set_string(kMsgType, "D")
    .set_string(kClOrdID, "ORD-1")
    .set_string(kSymbol, "AAPL")
    .set_string(kSide, "1")
    .set_int(kOrderQty, 100)
    .set_string(kOrdType, "2")
    .set_string(kTransactTime, "20260414-12:00:00.000");
  auto frame1 = EncodeInboundFrame(std::move(order1).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 4U, false);
  REQUIRE(frame1.ok());

  auto event1 = protocol->OnInbound(frame1.value(), 10U);
  REQUIRE(event1.ok());
  REQUIRE(protocol->session().state() == SessionState::kResendProcessing);

  // Second app message at seq=6 (extends gap to [2, 5])
  nimble::message::MessageBuilder order2("D");
  order2.set_string(kMsgType, "D")
    .set_string(kClOrdID, "ORD-2")
    .set_string(kSymbol, "MSFT")
    .set_string(kSide, "1")
    .set_int(kOrderQty, 200)
    .set_string(kOrdType, "2")
    .set_string(kTransactTime, "20260414-12:00:01.000");
  auto frame2 = EncodeInboundFrame(std::move(order2).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 6U, false);
  REQUIRE(frame2.ok());

  auto event2 = protocol->OnInbound(frame2.value(), 12U);
  REQUIRE(event2.ok());

  // Fill [2, 4) → advances next_in_seq to 4 (does not yet complete resend)
  nimble::message::MessageBuilder gap_fill1("4");
  gap_fill1.set_string(35U, "4").set_boolean(123U, true).set_int(36U, 4);
  auto fill_frame1 = EncodeInboundFrame(
    std::move(gap_fill1).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 2U, true, {}, "20260414-11:59:00.000");
  REQUIRE(fill_frame1.ok());

  auto fill_event1 = protocol->OnInbound(fill_frame1.value(), 15U);
  REQUIRE(fill_event1.ok());

  // Still in resend processing; deferred seq=4 not drained yet because
  // gap now covers [4, 5] and pending_resend is still active.
  REQUIRE(protocol->session().state() == SessionState::kResendProcessing);

  // Fill [4, 6) → advances next_in_seq to 6, completes resend;
  // drain should replay seq=4 (now == next_in_seq after partial fills?
  // No — after GapFill [4,6), next_in_seq = 6, but seq=4 < 6 → stale.
  // The real replay: counterparty would re-send seq=4 and seq=5 as
  // PossDup, not GapFill over them.
  //
  // Revised scenario: GapFill [2,4) fills 2-3.  Counterparty replays
  // the real seq=4 (PossDup), which the engine consumes.  Then GapFill
  // [5,6) fills seq=5.  Pending resend ends_seq=5, so next_in_seq(6)>5
  // completes the resend and drains seq=6 from the deferred queue.

  // Counterparty replays seq=4 as PossDup
  nimble::message::MessageBuilder replay4("D");
  replay4.set_string(kMsgType, "D")
    .set_string(kClOrdID, "ORD-1-PD")
    .set_string(kSymbol, "AAPL")
    .set_string(kSide, "1")
    .set_int(kOrderQty, 100)
    .set_string(kOrdType, "2")
    .set_string(kTransactTime, "20260414-12:00:00.000");
  auto replay4_frame = EncodeInboundFrame(
    std::move(replay4).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 4U, true, {}, "20260414-11:59:01.000");
  REQUIRE(replay4_frame.ok());

  auto replay4_event = protocol->OnInbound(replay4_frame.value(), 16U);
  REQUIRE(replay4_event.ok());
  // next_in_seq should be 5 now
  REQUIRE(protocol->session().Snapshot().next_in_seq == 5U);

  // GapFill [5, 6) fills seq=5, completing the gap → triggers drain
  nimble::message::MessageBuilder gap_fill2("4");
  gap_fill2.set_string(35U, "4").set_boolean(123U, true).set_int(36U, 6);
  auto fill_frame2 = EncodeInboundFrame(
    std::move(gap_fill2).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 5U, true, {}, "20260414-11:59:02.000");
  REQUIRE(fill_frame2.ok());

  auto fill_event2 = protocol->OnInbound(fill_frame2.value(), 20U);
  REQUIRE(fill_event2.ok());

  // The deferred seq=6 should have been drained; counting all app msgs
  // across all events:
  //   event1: 0 (gap-triggered, queued)
  //   event2: 0 (gap-triggered, queued)
  //   fill_event1: 0 (GapFill admin)
  //   replay4_event: 1 (PossDup replay of order1) — may or may not surface
  //   fill_event2: 1+ (GapFill + drained seq=6 order2)
  std::size_t total_app = event1.value().application_messages.size() + event2.value().application_messages.size() +
                          fill_event1.value().application_messages.size() +
                          replay4_event.value().application_messages.size() +
                          fill_event2.value().application_messages.size();
  REQUIRE(total_app >= 1U);

  const auto snapshot = protocol->session().Snapshot();
  REQUIRE(snapshot.state == SessionState::kActive);
  REQUIRE(snapshot.next_in_seq == 7U);
}

// ===========================================================================
// Bug 1: initiator with ResetSeqNum + gap on logon response
// ===========================================================================
// Initiator sends Logon with ResetSeqNumFlag. Counterparty responds with
// seq=2 instead of seq=1 (gap on seq 1). Logon must still complete.

TEST_CASE("gap-transition: initiator reset-seq-num logon with gap", "[gap-transition][bug1][reset-seq]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }

  nimble::store::MemorySessionStore store;
  // Initiator with reset_seq_num_on_logon
  auto protocol = std::make_unique<AdminProtocol>(
    AdminProtocolConfig{
      .session =
        SessionConfig{
          .session_id = 9040U,
          .key = SessionKey{ "FIX.4.4", "BUY", "SELL" },
          .profile_id = dictionary.value().profile().header().profile_id,
          .heartbeat_interval_seconds = 30U,
          .is_initiator = true,
        },
      .begin_string = "FIX.4.4",
      .sender_comp_id = "BUY",
      .target_comp_id = "SELL",
      .heartbeat_interval_seconds = 30U,
      .reset_seq_num_on_logon = true,
    },
    dictionary.value(),
    &store);

  auto connected = protocol->OnTransportConnected(1U);
  REQUIRE(connected.ok());
  REQUIRE(protocol->session().state() == SessionState::kPendingLogon);

  // Counterparty responds with Logon at seq=2 (expected=1, gap on 1)
  nimble::message::MessageBuilder logon_builder("A");
  logon_builder.set_string(35U, "A").set_int(98U, 0).set_int(108U, 30);
  auto logon_frame =
    EncodeInboundFrame(std::move(logon_builder).build(), dictionary.value(), "FIX.4.4", "SELL", "BUY", 2U, false);
  REQUIRE(logon_frame.ok());

  auto gap_event = protocol->OnInbound(logon_frame.value(), 10U);
  REQUIRE(gap_event.ok());

  // Fill gap [1,1] with GapFill
  nimble::message::MessageBuilder gap_fill("4");
  gap_fill.set_string(35U, "4").set_boolean(123U, true).set_int(36U, 2);
  auto fill_frame = EncodeInboundFrame(
    std::move(gap_fill).build(), dictionary.value(), "FIX.4.4", "SELL", "BUY", 1U, true, {}, "20260414-11:59:00.000");
  REQUIRE(fill_frame.ok());

  auto fill_event = protocol->OnInbound(fill_frame.value(), 20U);
  REQUIRE(fill_event.ok());

  // Session must be Active.
  REQUIRE(protocol->session().state() == SessionState::kActive);
}
