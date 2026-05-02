#include <chrono>
#include <memory>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "nimblefix/advanced/message_builder.h"
#include "nimblefix/codec/fix_codec.h"
#include "nimblefix/codec/fix_tags.h"
#include "nimblefix/session/admin_protocol.h"
#include "nimblefix/store/memory_store.h"

#include "test_support.h"

namespace {

using namespace nimble::codec::tags;

class RecordingValidationCallback final : public nimble::session::ValidationCallback
{
public:
  bool accept_unknown{ true };
  bool accept_malformed{ true };
  std::uint32_t unknown_count{ 0U };
  std::uint32_t malformed_count{ 0U };
  std::uint32_t warning_count{ 0U };
  std::uint64_t last_session_id{ 0U };
  std::uint32_t last_tag{ 0U };
  std::string last_value;
  std::string last_msg_type;
  std::string last_issue_text;

  auto OnUnknownField(std::uint64_t session_id, std::uint32_t tag, std::string_view value, std::string_view msg_type)
    -> bool override
  {
    ++unknown_count;
    last_session_id = session_id;
    last_tag = tag;
    last_value = std::string(value);
    last_msg_type = std::string(msg_type);
    return accept_unknown;
  }

  auto OnMalformedField(std::uint64_t session_id,
                        std::uint32_t tag,
                        std::string_view value,
                        std::string_view msg_type,
                        std::string_view issue_text) -> bool override
  {
    ++malformed_count;
    last_session_id = session_id;
    last_tag = tag;
    last_value = std::string(value);
    last_msg_type = std::string(msg_type);
    last_issue_text = std::string(issue_text);
    return accept_malformed;
  }

  auto OnValidationWarning(std::uint64_t session_id,
                           const nimble::codec::ValidationIssue& issue,
                           std::string_view msg_type) -> void override
  {
    ++warning_count;
    last_session_id = session_id;
    last_tag = issue.tag;
    last_msg_type = std::string(msg_type);
    last_issue_text = issue.text;
  }
};

auto
MakeAcceptorProtocolConfig(std::uint64_t session_id,
                           const nimble::profile::NormalizedDictionaryView& dictionary,
                           nimble::session::ValidationPolicy validation_policy,
                           std::shared_ptr<nimble::session::ValidationCallback> validation_callback = {})
  -> nimble::session::AdminProtocolConfig
{
  return nimble::session::AdminProtocolConfig{
    .session =
      nimble::session::SessionConfig{
        .session_id = session_id,
        .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
        .profile_id = dictionary.profile().header().profile_id,
        .heartbeat_interval_seconds = 30U,
        .is_initiator = false,
      },
    .begin_string = "FIX.4.4",
    .sender_comp_id = "SELL",
    .target_comp_id = "BUY",
    .heartbeat_interval_seconds = 30U,
    .validation_policy = validation_policy,
    .validation_callback = std::move(validation_callback),
  };
}

auto
InboundApplicationFrame(std::string_view body_fields) -> std::vector<std::byte>
{
  return ::nimble::tests::EncodeFixFrame(body_fields);
}

auto
RequireAcceptedApplication(nimble::session::ProtocolEvent event) -> void
{
  REQUIRE(event.outbound_frames.empty());
  REQUIRE(event.application_messages.size() == 1U);
}

auto
RequireSessionReject(const nimble::profile::NormalizedDictionaryView& dictionary,
                     nimble::session::ProtocolEvent event,
                     std::uint32_t ref_tag_id) -> void
{
  REQUIRE(event.outbound_frames.size() == 1U);
  REQUIRE(event.application_messages.empty());
  auto decoded = nimble::codec::DecodeFixMessage(event.outbound_frames.front().bytes, dictionary);
  REQUIRE(decoded.ok());
  REQUIRE(decoded.value().header.msg_type == "3");
  REQUIRE(decoded.value().message.view().get_int(kRefTagID).value() == ref_tag_id);
}

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
  options.poss_resend = message.view().get_boolean(kPossResend).value_or(false);
  options.sending_time = "20260402-12:00:00.000";
  options.orig_sending_time = std::move(orig_sending_time);
  return nimble::codec::EncodeFixMessage(message, dictionary, options);
}

auto
ActivateAcceptorSession(nimble::session::AdminProtocol* protocol,
                        const nimble::profile::NormalizedDictionaryView& dictionary,
                        std::string begin_string,
                        std::string default_appl_ver_id = {}) -> nimble::base::Status;

TEST_CASE("admin protocol sends pre-encoded application payload", "[admin-protocol]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }

  nimble::store::MemorySessionStore store;
  nimble::session::AdminProtocol protocol(
    nimble::session::AdminProtocolConfig{
      .session =
        nimble::session::SessionConfig{
          .session_id = 5004U,
          .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
          .profile_id = dictionary.value().profile().header().profile_id,
          .heartbeat_interval_seconds = 30U,
          .is_initiator = false,
        },
      .begin_string = "FIX.4.4",
      .sender_comp_id = "SELL",
      .target_comp_id = "BUY",
      .heartbeat_interval_seconds = 30U,
    },
    dictionary.value(),
    &store);

  REQUIRE(protocol.OnTransportConnected(1U).ok());
  REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

  const auto encoded_body = nimble::tests::Bytes("11=ORD-ENC\x01"
                                                 "55=AAPL\x01");
  nimble::session::EncodedApplicationMessage encoded(
    "D", std::span<const std::byte>(encoded_body.data(), encoded_body.size()));

  auto outbound =
    protocol.SendEncodedApplication(encoded, 100U, { .sender_sub_id = "DESK-ENC", .target_sub_id = "ROUTE-ENC" });
  REQUIRE(outbound.ok());

  auto decoded = nimble::codec::DecodeFixMessage(outbound.value().bytes, dictionary.value());
  REQUIRE(decoded.ok());
  REQUIRE(decoded.value().header.msg_type == "D");
  REQUIRE(decoded.value().header.msg_seq_num == 2U);
  REQUIRE(decoded.value().header.sender_sub_id == "DESK-ENC");
  REQUIRE(decoded.value().header.target_sub_id == "ROUTE-ENC");
  REQUIRE(decoded.value().message.view().get_string(kClOrdID).value() == "ORD-ENC");
  REQUIRE(decoded.value().message.view().get_string(kSymbol).value() == "AAPL");

  auto stored = store.LoadOutboundRange(5004U, 2U, 2U);
  REQUIRE(stored.ok());
  REQUIRE(stored.value().size() == 1U);
  REQUIRE(stored.value().front().payload ==
          std::vector<std::byte>(outbound.value().bytes.view().begin(), outbound.value().bytes.view().end()));

  nimble::message::MessageBuilder resend_builder("2");
  resend_builder.set_string(kMsgType, "2").set_int(kBeginSeqNo, 2).set_int(kEndSeqNo, 2);
  auto inbound =
    EncodeInboundFrame(std::move(resend_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 2U, false);
  REQUIRE(inbound.ok());

  auto event = protocol.OnInbound(inbound.value(), 200U);
  REQUIRE(event.ok());
  REQUIRE(event.value().outbound_frames.size() == 1U);

  auto replay = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
  REQUIRE(replay.ok());
  REQUIRE(replay.value().header.msg_type == "D");
  REQUIRE(replay.value().header.sender_sub_id == "DESK-ENC");
  REQUIRE(replay.value().header.target_sub_id == "ROUTE-ENC");
  REQUIRE(replay.value().message.view().get_string(kClOrdID).value() == "ORD-ENC");
  REQUIRE(replay.value().message.view().get_string(kSymbol).value() == "AAPL");
}

TEST_CASE("admin protocol treats SenderSubID and TargetSubID as per-message envelope", "[admin-protocol]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }

  nimble::store::MemorySessionStore store;
  nimble::session::AdminProtocol protocol(
    nimble::session::AdminProtocolConfig{
      .session =
        nimble::session::SessionConfig{
          .session_id = 5005U,
          .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
          .profile_id = dictionary.value().profile().header().profile_id,
          .heartbeat_interval_seconds = 30U,
          .is_initiator = false,
        },
      .begin_string = "FIX.4.4",
      .sender_comp_id = "SELL",
      .target_comp_id = "BUY",
      .heartbeat_interval_seconds = 30U,
    },
    dictionary.value(),
    &store);

  REQUIRE(protocol.OnTransportConnected(1U).ok());
  REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

  nimble::message::MessageBuilder first_builder("D");
  first_builder.set_string(kMsgType, "D").set_string(kClOrdID, "ORD-A");
  auto first = protocol.SendApplication(
    std::move(first_builder).build(), 100U, { .sender_sub_id = "DESK-A", .target_sub_id = "ROUTE-A" });
  REQUIRE(first.ok());

  nimble::message::MessageBuilder second_builder("D");
  second_builder.set_string(kMsgType, "D").set_string(kClOrdID, "ORD-B");
  auto second = protocol.SendApplication(
    std::move(second_builder).build(), 110U, { .sender_sub_id = "DESK-B", .target_sub_id = "ROUTE-B" });
  REQUIRE(second.ok());

  nimble::message::MessageBuilder third_builder("D");
  third_builder.set_string(kMsgType, "D").set_string(kClOrdID, "ORD-C");
  auto third = protocol.SendApplication(std::move(third_builder).build(), 120U);
  REQUIRE(third.ok());

  auto decoded_first = nimble::codec::DecodeFixMessage(first.value().bytes, dictionary.value());
  auto decoded_second = nimble::codec::DecodeFixMessage(second.value().bytes, dictionary.value());
  auto decoded_third = nimble::codec::DecodeFixMessage(third.value().bytes, dictionary.value());
  REQUIRE(decoded_first.ok());
  REQUIRE(decoded_second.ok());
  REQUIRE(decoded_third.ok());

  REQUIRE(decoded_first.value().header.msg_seq_num == 2U);
  REQUIRE(decoded_second.value().header.msg_seq_num == 3U);
  REQUIRE(decoded_third.value().header.msg_seq_num == 4U);

  REQUIRE(decoded_first.value().header.sender_sub_id == "DESK-A");
  REQUIRE(decoded_first.value().header.target_sub_id == "ROUTE-A");
  REQUIRE(decoded_second.value().header.sender_sub_id == "DESK-B");
  REQUIRE(decoded_second.value().header.target_sub_id == "ROUTE-B");
  REQUIRE(decoded_third.value().header.sender_sub_id.empty());
  REQUIRE(decoded_third.value().header.target_sub_id.empty());
}

TEST_CASE("admin protocol initializes session warmup count", "[admin-protocol][warmup]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }

  nimble::store::MemorySessionStore store;
  nimble::session::AdminProtocol protocol(
    nimble::session::AdminProtocolConfig{
      .session =
        nimble::session::SessionConfig{
          .session_id = 5010U,
          .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
          .profile_id = dictionary.value().profile().header().profile_id,
          .heartbeat_interval_seconds = 30U,
          .is_initiator = false,
        },
      .begin_string = "FIX.4.4",
      .sender_comp_id = "SELL",
      .target_comp_id = "BUY",
      .heartbeat_interval_seconds = 30U,
      .warmup_message_count = 2U,
    },
    dictionary.value(),
    &store);

  REQUIRE(protocol.OnTransportConnected(1U).ok());
  REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());
  REQUIRE(protocol.session().is_warmup());
  REQUIRE(protocol.session().Snapshot().is_warmup);
}

auto
ActivateAcceptorSession(nimble::session::AdminProtocol* protocol,
                        const nimble::profile::NormalizedDictionaryView& dictionary,
                        std::string begin_string,
                        std::string default_appl_ver_id) -> nimble::base::Status
{
  nimble::message::MessageBuilder logon_builder("A");
  logon_builder.set_string(kMsgType, "A").set_int(kEncryptMethod, 0).set_int(kHeartBtInt, 30);
  auto inbound = EncodeInboundFrame(std::move(logon_builder).build(),
                                    dictionary,
                                    std::move(begin_string),
                                    "BUY",
                                    "SELL",
                                    1U,
                                    false,
                                    std::move(default_appl_ver_id));
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

} // namespace

TEST_CASE("admin-protocol", "[admin-protocol]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }

  {
    nimble::store::MemorySessionStore store;
    REQUIRE(store
              .SaveRecoveryState(nimble::store::SessionRecoveryState{
                .session_id = 5008U,
                .next_in_seq = 7U,
                .next_out_seq = 11U,
                .last_inbound_ns = 77U,
                .last_outbound_ns = 88U,
                .active = false,
              })
              .ok());
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5008U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());

    nimble::message::MessageBuilder logon_builder("A");
    logon_builder.set_string(kMsgType, "A").set_int(kEncryptMethod, 0).set_int(kHeartBtInt, 30);
    auto inbound =
      EncodeInboundFrame(std::move(logon_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 7U, false);
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().session_active);
    REQUIRE(event.value().outbound_frames.size() == 1U);

    auto decoded = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "A");
    REQUIRE(decoded.value().header.msg_seq_num == 11U);

    const auto snapshot = protocol.session().Snapshot();
    REQUIRE(snapshot.next_in_seq == 8U);
    REQUIRE(snapshot.next_out_seq == 12U);
  }

  {
    nimble::store::MemorySessionStore store;
    REQUIRE(store
              .SaveRecoveryState(nimble::store::SessionRecoveryState{
                .session_id = 5009U,
                .next_in_seq = 7U,
                .next_out_seq = 11U,
                .last_inbound_ns = 77U,
                .last_outbound_ns = 88U,
                .active = false,
              })
              .ok());
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5009U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());

    nimble::message::MessageBuilder logon_builder("A");
    logon_builder.set_string(kMsgType, "A")
      .set_int(kEncryptMethod, 0)
      .set_int(kHeartBtInt, 30)
      .set_boolean(kResetSeqNumFlag, true);
    auto inbound =
      EncodeInboundFrame(std::move(logon_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 1U, false);
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().session_active);
    REQUIRE(event.value().outbound_frames.size() == 1U);

    auto decoded = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "A");
    REQUIRE(decoded.value().header.msg_seq_num == 1U);
    REQUIRE(decoded.value().message.view().get_boolean(kResetSeqNumFlag).value());

    const auto snapshot = protocol.session().Snapshot();
    REQUIRE(snapshot.next_in_seq == 2U);
    REQUIRE(snapshot.next_out_seq == 2U);
  }

  {
    nimble::store::MemorySessionStore store;
    REQUIRE(store
              .SaveRecoveryState(nimble::store::SessionRecoveryState{
                .session_id = 5010U,
                .next_in_seq = 7U,
                .next_out_seq = 11U,
                .last_inbound_ns = 70U,
                .last_outbound_ns = 80U,
                .active = false,
              })
              .ok());
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5010U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = true,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
        .refresh_on_logon = true,
        .send_next_expected_msg_seq_num = true,
      },
      dictionary.value(),
      &store);

    REQUIRE(store
              .SaveRecoveryState(nimble::store::SessionRecoveryState{
                .session_id = 5010U,
                .next_in_seq = 13U,
                .next_out_seq = 17U,
                .last_inbound_ns = 170U,
                .last_outbound_ns = 180U,
                .active = false,
              })
              .ok());

    auto event = protocol.OnTransportConnected(5U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.size() == 1U);

    auto decoded = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "A");
    REQUIRE(decoded.value().header.msg_seq_num == 17U);
    REQUIRE(decoded.value().message.view().get_int(kNextExpectedMsgSeqNum).value() == 13);
  }

  {
    nimble::store::MemorySessionStore store;
    REQUIRE(store
              .SaveRecoveryState(nimble::store::SessionRecoveryState{
                .session_id = 5011U,
                .next_in_seq = 5U,
                .next_out_seq = 9U,
                .last_inbound_ns = 50U,
                .last_outbound_ns = 90U,
                .active = true,
              })
              .ok());
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5011U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = true,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
        .reset_seq_num_on_disconnect = true,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(5U).ok());
    REQUIRE(protocol.OnTransportClosed().ok());

    const auto snapshot = protocol.session().Snapshot();
    REQUIRE(snapshot.next_in_seq == 1U);
    REQUIRE(snapshot.next_out_seq == 1U);

    auto recovery = store.LoadRecoveryState(5011U);
    REQUIRE(recovery.ok());
    REQUIRE(recovery.value().next_in_seq == 1U);
    REQUIRE(recovery.value().next_out_seq == 1U);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5012U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
        .reset_seq_num_on_logout = true,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());
    auto logout = protocol.BeginLogout({}, 10U);
    REQUIRE(logout.ok());
    REQUIRE(protocol.OnTransportClosed().ok());

    const auto snapshot = protocol.session().Snapshot();
    REQUIRE(snapshot.next_in_seq == 1U);
    REQUIRE(snapshot.next_out_seq == 1U);

    auto recovery = store.LoadRecoveryState(5012U);
    REQUIRE(recovery.ok());
    REQUIRE(recovery.value().next_in_seq == 1U);
    REQUIRE(recovery.value().next_out_seq == 1U);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5001U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    nimble::message::MessageBuilder heartbeat_builder("0");
    heartbeat_builder.set_string(kMsgType, "0");
    auto inbound =
      EncodeInboundFrame(std::move(heartbeat_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 1U, true);
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.size() == 1U);
    REQUIRE(!event.value().disconnect);

    auto decoded = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "3");
    REQUIRE(decoded.value().message.view().get_int(kRefSeqNum).value() == 1);
    REQUIRE(decoded.value().message.view().get_int(kRefTagID).value() == kOrigSendingTime);
    REQUIRE(decoded.value().message.view().get_string(kRefMsgType).value() == "0");
    REQUIRE(decoded.value().message.view().get_int(kRejectReason).value() == 1);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5033U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    nimble::message::MessageBuilder heartbeat_builder("0");
    heartbeat_builder.set_string(kMsgType, "0");
    auto inbound = EncodeInboundFrame(std::move(heartbeat_builder).build(),
                                      dictionary.value(),
                                      "FIX.4.4",
                                      "BUY",
                                      "SELL",
                                      1U,
                                      true,
                                      {},
                                      "20260402-11:59:00.000");
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.empty());
    REQUIRE(!event.value().disconnect);

    const auto snapshot = protocol.session().Snapshot();
    REQUIRE(snapshot.last_inbound_ns == 10U);

    auto recovery = store.LoadRecoveryState(5033U);
    REQUIRE(recovery.ok());
    REQUIRE(recovery.value().last_inbound_ns == 10U);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5047U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    nimble::message::MessageBuilder heartbeat_builder("0");
    heartbeat_builder.set_string(kMsgType, "0");
    auto inbound = EncodeInboundFrame(std::move(heartbeat_builder).build(),
                                      dictionary.value(),
                                      "FIX.4.4",
                                      "BUY",
                                      "SELL",
                                      2U,
                                      true,
                                      {},
                                      "20260402-12:00:01.000");
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.size() == 1U);
    REQUIRE(!event.value().disconnect);

    auto decoded = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "3");
    REQUIRE(decoded.value().message.view().get_int(kRefTagID).value() == kSendingTime);
    REQUIRE(decoded.value().message.view().get_string(kRefMsgType).value() == "0");
    REQUIRE(decoded.value().message.view().get_int(kRejectReason).value() == 10);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5021U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    const auto inbound = ::nimble::tests::EncodeFixFrame("35=0|34=2|49=BUY|56=SELL|52=20260402-12:00:00.000|49=BUY|");
    auto event = protocol.OnInbound(inbound, 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.size() == 1U);
    REQUIRE(!event.value().disconnect);

    auto decoded = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "3");
    REQUIRE(decoded.value().message.view().get_int(kRefTagID).value() == kSenderCompID);
    REQUIRE(decoded.value().message.view().get_string(kRefMsgType).value() == "0");
    REQUIRE(decoded.value().message.view().get_int(kRejectReason).value() == 13);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5006U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
        .validation_policy = nimble::session::ValidationPolicy::Compatible(),
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    nimble::message::MessageBuilder heartbeat_builder("0");
    heartbeat_builder.set_string(kMsgType, "0");
    auto inbound =
      EncodeInboundFrame(std::move(heartbeat_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 1U, true);
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.empty());
    REQUIRE(!event.value().disconnect);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5022U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
        .validation_policy = nimble::session::ValidationPolicy::Compatible(),
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    const auto inbound = ::nimble::tests::EncodeFixFrame("35=0|34=2|49=BUY|56=SELL|52=20260402-12:00:00.000|49=BUY|");
    auto event = protocol.OnInbound(inbound, 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.empty());
    REQUIRE(!event.value().disconnect);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5002U,
            .key = nimble::session::SessionKey{ "FIXT.1.1", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .default_appl_ver_id = "9",
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIXT.1.1",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .default_appl_ver_id = "9",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());

    nimble::message::MessageBuilder logon_builder("A");
    logon_builder.set_string(kMsgType, "A").set_int(kEncryptMethod, 0).set_int(kHeartBtInt, 30);
    auto inbound = EncodeInboundFrame(
      std::move(logon_builder).build(), dictionary.value(), "FIXT.1.1", "OTHER", "SELL", 1U, false, "9");
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.size() == 2U);
    REQUIRE(event.value().disconnect);
    REQUIRE(!event.value().errors.empty());

    auto reject = nimble::codec::DecodeFixMessage(event.value().outbound_frames[0].bytes, dictionary.value());
    REQUIRE(reject.ok());
    REQUIRE(reject.value().header.msg_type == "3");
    REQUIRE(reject.value().message.view().get_int(kRefTagID).value() == kSenderCompID);
    REQUIRE(reject.value().message.view().get_int(kRejectReason).value() == 9);
    REQUIRE(reject.value().message.view().get_string(kRefMsgType).value() == "A");

    auto logout = nimble::codec::DecodeFixMessage(event.value().outbound_frames[1].bytes, dictionary.value());
    REQUIRE(logout.ok());
    REQUIRE(logout.value().header.msg_type == "5");
    REQUIRE(logout.value().message.view().get_string(kText).value() == "unexpected SenderCompID on inbound frame");
    REQUIRE(protocol.session().Snapshot().next_in_seq == 2U);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5046U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());

    nimble::message::MessageBuilder logon_builder("A");
    logon_builder.set_string(kMsgType, "A").set_int(kEncryptMethod, 0).set_int(kHeartBtInt, 30);
    auto inbound =
      EncodeInboundFrame(std::move(logon_builder).build(), dictionary.value(), "FIX.4.2", "BUY", "SELL", 1U, false);
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.size() == 1U);
    REQUIRE(event.value().disconnect);

    auto decoded = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "5");
    REQUIRE(decoded.value().message.view().get_string(kText).value() == "unexpected BeginString on inbound frame");
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5039U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
        .validation_policy = nimble::session::ValidationPolicy::Permissive(),
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());

    nimble::message::MessageBuilder logon_builder("A");
    logon_builder.set_string(kMsgType, "A").set_int(kEncryptMethod, 0).set_int(kHeartBtInt, 30);
    auto inbound =
      EncodeInboundFrame(std::move(logon_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "OTHER", 1U, false);
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().session_active);
    REQUIRE(event.value().outbound_frames.size() == 1U);
    REQUIRE(!event.value().disconnect);

    auto decoded = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "A");
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5010U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());

    nimble::message::MessageBuilder logon_builder("A");
    logon_builder.set_string(kMsgType, "A").set_int(kEncryptMethod, 0);
    auto inbound =
      EncodeInboundFrame(std::move(logon_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 1U, false);
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.size() == 1U);
    REQUIRE(event.value().disconnect);

    auto decoded = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "5");
    REQUIRE(decoded.value().message.view().get_string(kText).value() == "Logon requires HeartBtInt");
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5023U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());

    nimble::message::MessageBuilder logon_builder("A");
    logon_builder.set_string(kMsgType, "A").set_int(kEncryptMethod, 1).set_int(kHeartBtInt, 30);
    auto inbound =
      EncodeInboundFrame(std::move(logon_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 1U, false);
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.size() == 1U);
    REQUIRE(event.value().disconnect);

    auto decoded = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "5");
    REQUIRE(decoded.value().message.view().get_string(kText).value() == "Logon EncryptMethod must be 0");
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5040U,
            .key = nimble::session::SessionKey{ "FIXT.1.1", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .default_appl_ver_id = "9",
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIXT.1.1",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .default_appl_ver_id = "9",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());

    nimble::message::MessageBuilder logon_builder("A");
    logon_builder.set_string(kMsgType, "A").set_int(kEncryptMethod, 0).set_int(kHeartBtInt, 30);
    auto inbound =
      EncodeInboundFrame(std::move(logon_builder).build(), dictionary.value(), "FIXT.1.1", "BUY", "SELL", 1U, false);
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.size() == 1U);
    REQUIRE(event.value().disconnect);

    auto decoded = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "5");
    REQUIRE(decoded.value().message.view().get_string(kText).value() == "FIXT.1.1 logon requires DefaultApplVerID");
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5041U,
            .key = nimble::session::SessionKey{ "FIXT.1.1", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .default_appl_ver_id = "9",
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIXT.1.1",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .default_appl_ver_id = "9",
        .heartbeat_interval_seconds = 30U,
        .validation_policy = nimble::session::ValidationPolicy::Compatible(),
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());

    nimble::message::MessageBuilder logon_builder("A");
    logon_builder.set_string(kMsgType, "A").set_int(kEncryptMethod, 0).set_int(kHeartBtInt, 30);
    auto inbound =
      EncodeInboundFrame(std::move(logon_builder).build(), dictionary.value(), "FIXT.1.1", "BUY", "SELL", 1U, false);
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().session_active);
    REQUIRE(event.value().outbound_frames.size() == 1U);
    REQUIRE(!event.value().disconnect);

    auto decoded = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "A");
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5042U,
            .key = nimble::session::SessionKey{ "FIXT.1.1", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .default_appl_ver_id = "9",
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIXT.1.1",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .default_appl_ver_id = "9",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());

    nimble::message::MessageBuilder logon_builder("A");
    logon_builder.set_string(kMsgType, "A").set_int(kEncryptMethod, 0).set_int(kHeartBtInt, 30);
    auto inbound = EncodeInboundFrame(
      std::move(logon_builder).build(), dictionary.value(), "FIXT.1.1", "BUY", "SELL", 1U, false, "7");
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.size() == 1U);
    REQUIRE(event.value().disconnect);

    auto decoded = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "5");
    REQUIRE(decoded.value().message.view().get_string(kText).value() == "unexpected DefaultApplVerID on inbound frame");
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5043U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());

    nimble::message::MessageBuilder logon_builder("A");
    logon_builder.set_string(kMsgType, "A").set_int(kEncryptMethod, 0).set_int(kHeartBtInt, 30);
    auto inbound =
      EncodeInboundFrame(std::move(logon_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 1U, true);
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.size() == 1U);
    REQUIRE(event.value().disconnect);

    auto decoded = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "5");
    REQUIRE(decoded.value().message.view().get_string(kText).value() == "PossDupFlag requires OrigSendingTime");
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5044U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());

    const auto inbound = ::nimble::tests::EncodeFixFrame("35=A|34=1|49=BUY|56=SELL|52=20260402-"
                                                         "12:00:00.000|98=0|108=30|9999=BAD|");
    auto event = protocol.OnInbound(inbound, 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.size() == 1U);
    REQUIRE(event.value().disconnect);

    auto decoded = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "5");
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5036U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());

    nimble::message::MessageBuilder heartbeat_builder("0");
    heartbeat_builder.set_string(kMsgType, "0");
    auto inbound =
      EncodeInboundFrame(std::move(heartbeat_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 1U, false);
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.empty());
    REQUIRE(event.value().disconnect);
    REQUIRE(!event.value().errors.empty());

    const auto snapshot = protocol.session().Snapshot();
    REQUIRE(snapshot.next_in_seq == 1U);
    REQUIRE(snapshot.next_out_seq == 1U);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5037U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());

    nimble::message::MessageBuilder resend_builder("2");
    resend_builder.set_string(kMsgType, "2").set_int(kBeginSeqNo, 1).set_int(kEndSeqNo, 0);
    auto inbound =
      EncodeInboundFrame(std::move(resend_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 2U, false);
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.empty());
    REQUIRE(event.value().disconnect);
    REQUIRE(!event.value().errors.empty());

    const auto snapshot = protocol.session().Snapshot();
    REQUIRE(snapshot.state == nimble::session::SessionState::kConnected);
    REQUIRE(snapshot.next_in_seq == 1U);
    REQUIRE(snapshot.next_out_seq == 1U);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5004U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    nimble::message::MessageBuilder heartbeat_builder("0");
    heartbeat_builder.set_string(kMsgType, "0");
    auto inbound =
      EncodeInboundFrame(std::move(heartbeat_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 1U, false);
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.size() == 1U);
    REQUIRE(event.value().disconnect);

    auto decoded = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "5");
    REQUIRE(decoded.value().message.view().get_string(kText).value() == "received stale inbound FIX sequence number");
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5038U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    nimble::message::MessageBuilder logon_builder("A");
    logon_builder.set_string(kMsgType, "A").set_int(kEncryptMethod, 0).set_int(kHeartBtInt, 30);
    auto inbound =
      EncodeInboundFrame(std::move(logon_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 2U, false);
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.size() == 1U);
    REQUIRE(event.value().disconnect);

    auto decoded = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "5");
    REQUIRE(decoded.value().message.view().get_string(kText).value() ==
            "received unexpected Logon after session activation");

    const auto snapshot = protocol.session().Snapshot();
    REQUIRE(snapshot.state == nimble::session::SessionState::kAwaitingLogout);
    REQUIRE(snapshot.next_in_seq == 3U);
    REQUIRE(snapshot.next_out_seq == 3U);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5024U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    nimble::message::MessageBuilder test_request_builder("1");
    test_request_builder.set_string(kMsgType, "1");
    auto inbound = EncodeInboundFrame(
      std::move(test_request_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 2U, false);
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.size() == 1U);
    REQUIRE(!event.value().disconnect);

    auto decoded = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "3");
    REQUIRE(decoded.value().message.view().get_int(kRefTagID).value() == kTestReqID);
    REQUIRE(decoded.value().message.view().get_string(kRefMsgType).value() == "1");
    REQUIRE(decoded.value().message.view().get_int(kRejectReason).value() == 1);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5025U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    nimble::message::MessageBuilder resend_builder("2");
    resend_builder.set_string(kMsgType, "2").set_int(kBeginSeqNo, 9).set_int(kEndSeqNo, 4);
    auto inbound =
      EncodeInboundFrame(std::move(resend_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 2U, false);
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.size() == 1U);
    REQUIRE(!event.value().disconnect);

    auto decoded = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "3");
    REQUIRE(decoded.value().message.view().get_int(kRefTagID).value() == kEndSeqNo);
    REQUIRE(decoded.value().message.view().get_string(kRefMsgType).value() == "2");
    REQUIRE(decoded.value().message.view().get_int(kRejectReason).value() == 5);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5005U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    nimble::message::MessageBuilder reset_builder("4");
    reset_builder.set_string(kMsgType, "4");
    auto inbound =
      EncodeInboundFrame(std::move(reset_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 2U, false);
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.size() == 1U);
    REQUIRE(!event.value().disconnect);

    auto decoded = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "3");
    REQUIRE(decoded.value().message.view().get_int(kRefTagID).value() == kNewSeqNo);
    REQUIRE(decoded.value().message.view().get_string(kRefMsgType).value() == "4");
    REQUIRE(decoded.value().message.view().get_int(kRejectReason).value() == 1);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5026U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    nimble::message::MessageBuilder reset_builder("4");
    reset_builder.set_string(kMsgType, "4").set_int(kNewSeqNo, 1);
    auto inbound =
      EncodeInboundFrame(std::move(reset_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 2U, false);
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.size() == 1U);
    REQUIRE(!event.value().disconnect);

    auto decoded = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "3");
    REQUIRE(decoded.value().message.view().get_int(kRefTagID).value() == kNewSeqNo);
    REQUIRE(decoded.value().message.view().get_string(kRefMsgType).value() == "4");
    REQUIRE(decoded.value().message.view().get_int(kRejectReason).value() == 5);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5034U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    nimble::message::MessageBuilder heartbeat_builder("0");
    heartbeat_builder.set_string(kMsgType, "0");
    auto gap_inbound =
      EncodeInboundFrame(std::move(heartbeat_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 5U, false);
    REQUIRE(gap_inbound.ok());

    auto gap_event = protocol.OnInbound(gap_inbound.value(), 10U);
    REQUIRE(gap_event.ok());
    REQUIRE(gap_event.value().outbound_frames.size() == 1U);

    auto resend = nimble::codec::DecodeFixMessage(gap_event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(resend.ok());
    REQUIRE(resend.value().header.msg_type == "2");
    REQUIRE(resend.value().message.view().get_int(kBeginSeqNo).value() == 2);
    REQUIRE(resend.value().message.view().get_int(kEndSeqNo).value() == 4);

    nimble::message::MessageBuilder first_gap_fill_builder("4");
    first_gap_fill_builder.set_string(kMsgType, "4").set_boolean(kGapFillFlag, true).set_int(kNewSeqNo, 4);
    auto first_gap_fill = EncodeInboundFrame(
      std::move(first_gap_fill_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 2U, false);
    REQUIRE(first_gap_fill.ok());

    auto first_gap_fill_event = protocol.OnInbound(first_gap_fill.value(), 20U);
    REQUIRE(first_gap_fill_event.ok());
    REQUIRE(first_gap_fill_event.value().outbound_frames.empty());
    REQUIRE(!first_gap_fill_event.value().disconnect);

    const auto after_first_gap_fill = protocol.session().Snapshot();
    REQUIRE(after_first_gap_fill.state == nimble::session::SessionState::kResendProcessing);
    REQUIRE(after_first_gap_fill.has_pending_resend);
    REQUIRE(after_first_gap_fill.pending_resend.begin_seq == 2U);
    REQUIRE(after_first_gap_fill.pending_resend.end_seq == 4U);
    REQUIRE(after_first_gap_fill.next_in_seq == 4U);

    nimble::message::MessageBuilder overlapping_gap_fill_builder("4");
    overlapping_gap_fill_builder.set_string(kMsgType, "4").set_boolean(kGapFillFlag, true).set_int(kNewSeqNo, 6);
    auto overlapping_gap_fill = EncodeInboundFrame(std::move(overlapping_gap_fill_builder).build(),
                                                   dictionary.value(),
                                                   "FIX.4.4",
                                                   "BUY",
                                                   "SELL",
                                                   3U,
                                                   true,
                                                   {},
                                                   "20260402-11:59:59.000");
    REQUIRE(overlapping_gap_fill.ok());

    auto overlapping_gap_fill_event = protocol.OnInbound(overlapping_gap_fill.value(), 30U);
    REQUIRE(overlapping_gap_fill_event.ok());
    REQUIRE(overlapping_gap_fill_event.value().outbound_frames.empty());
    REQUIRE(!overlapping_gap_fill_event.value().disconnect);

    const auto after_overlapping_gap_fill = protocol.session().Snapshot();
    REQUIRE(after_overlapping_gap_fill.state == nimble::session::SessionState::kActive);
    REQUIRE(!after_overlapping_gap_fill.has_pending_resend);
    REQUIRE(after_overlapping_gap_fill.next_in_seq == 6U);
    REQUIRE(after_overlapping_gap_fill.last_inbound_ns == 30U);

    auto recovery = store.LoadRecoveryState(5034U);
    REQUIRE(recovery.ok());
    REQUIRE(recovery.value().next_in_seq == 6U);
    REQUIRE(recovery.value().last_inbound_ns == 30U);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5035U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    nimble::message::MessageBuilder first_gap_fill_builder("4");
    first_gap_fill_builder.set_string(kMsgType, "4").set_boolean(kGapFillFlag, true).set_int(kNewSeqNo, 5);
    auto first_gap_fill = EncodeInboundFrame(
      std::move(first_gap_fill_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 2U, false);
    REQUIRE(first_gap_fill.ok());

    auto first_gap_fill_event = protocol.OnInbound(first_gap_fill.value(), 10U);
    REQUIRE(first_gap_fill_event.ok());
    REQUIRE(first_gap_fill_event.value().outbound_frames.empty());
    REQUIRE(!first_gap_fill_event.value().disconnect);

    nimble::message::MessageBuilder duplicate_gap_fill_builder("4");
    duplicate_gap_fill_builder.set_string(kMsgType, "4").set_boolean(kGapFillFlag, true).set_int(kNewSeqNo, 5);
    auto duplicate_gap_fill = EncodeInboundFrame(std::move(duplicate_gap_fill_builder).build(),
                                                 dictionary.value(),
                                                 "FIX.4.4",
                                                 "BUY",
                                                 "SELL",
                                                 3U,
                                                 true,
                                                 {},
                                                 "20260402-11:59:59.000");
    REQUIRE(duplicate_gap_fill.ok());

    auto duplicate_gap_fill_event = protocol.OnInbound(duplicate_gap_fill.value(), 20U);
    REQUIRE(duplicate_gap_fill_event.ok());
    REQUIRE(duplicate_gap_fill_event.value().outbound_frames.empty());
    REQUIRE(!duplicate_gap_fill_event.value().disconnect);

    const auto snapshot = protocol.session().Snapshot();
    REQUIRE(snapshot.state == nimble::session::SessionState::kActive);
    REQUIRE(snapshot.next_in_seq == 5U);
    REQUIRE(snapshot.last_inbound_ns == 20U);

    auto recovery = store.LoadRecoveryState(5035U);
    REQUIRE(recovery.ok());
    REQUIRE(recovery.value().next_in_seq == 5U);
    REQUIRE(recovery.value().last_inbound_ns == 20U);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5045U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    nimble::message::MessageBuilder first_gap_fill_builder("4");
    first_gap_fill_builder.set_string(kMsgType, "4").set_boolean(kGapFillFlag, true).set_int(kNewSeqNo, 5);
    auto first_gap_fill = EncodeInboundFrame(
      std::move(first_gap_fill_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 2U, false);
    REQUIRE(first_gap_fill.ok());
    REQUIRE(protocol.OnInbound(first_gap_fill.value(), 10U).ok());
    REQUIRE(protocol.session().Snapshot().next_in_seq == 5U);

    nimble::message::MessageBuilder stale_gap_fill_builder("4");
    stale_gap_fill_builder.set_string(kMsgType, "4").set_boolean(kGapFillFlag, true).set_int(kNewSeqNo, 6);
    auto stale_gap_fill = EncodeInboundFrame(
      std::move(stale_gap_fill_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 3U, false);
    REQUIRE(stale_gap_fill.ok());

    auto stale_gap_fill_event = protocol.OnInbound(stale_gap_fill.value(), 20U);
    REQUIRE(stale_gap_fill_event.ok());
    REQUIRE(stale_gap_fill_event.value().outbound_frames.size() == 1U);
    REQUIRE(stale_gap_fill_event.value().disconnect);

    auto decoded =
      nimble::codec::DecodeFixMessage(stale_gap_fill_event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "5");
    REQUIRE(decoded.value().message.view().get_string(kText).value() ==
            "MsgSeqNum too low, expecting 5 but received 3");
    REQUIRE(protocol.session().Snapshot().next_in_seq == 5U);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5003U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    nimble::message::MessageBuilder app_builder("D");
    app_builder.set_string(kMsgType, "D").set_string(kClOrdID, "ORD-1");
    auto outbound = protocol.SendApplication(std::move(app_builder).build(), 100U);
    REQUIRE(outbound.ok());

    auto original = nimble::codec::DecodeFixMessage(outbound.value().bytes, dictionary.value());
    REQUIRE(original.ok());
    REQUIRE(!original.value().header.sending_time.empty());

    nimble::message::MessageBuilder resend_builder("2");
    resend_builder.set_string(kMsgType, "2").set_int(kBeginSeqNo, 2).set_int(kEndSeqNo, 2);
    auto inbound =
      EncodeInboundFrame(std::move(resend_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 2U, false);
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 200U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.size() == 1U);

    auto replay = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(replay.ok());
    REQUIRE(replay.value().header.msg_type == "D");
    REQUIRE(replay.value().header.poss_dup);
    REQUIRE(replay.value().header.orig_sending_time == original.value().header.sending_time);
    REQUIRE(replay.value().message.view().get_string(kOrigSendingTime).has_value());
    REQUIRE(replay.value().message.view().get_string(kOrigSendingTime).value() == original.value().header.sending_time);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5030U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    auto logout = protocol.BeginLogout({}, 100U);
    REQUIRE(logout.ok());
    REQUIRE(nimble::codec::DecodeFixMessage(logout.value().bytes, dictionary.value()).ok());

    const auto before_replay = protocol.session().Snapshot();
    REQUIRE(before_replay.state == nimble::session::SessionState::kAwaitingLogout);
    REQUIRE(before_replay.next_out_seq == 3U);

    nimble::message::MessageBuilder resend_builder("2");
    resend_builder.set_string(kMsgType, "2").set_int(kBeginSeqNo, 2).set_int(kEndSeqNo, 2);
    auto inbound =
      EncodeInboundFrame(std::move(resend_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 2U, false);
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 200U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.size() == 1U);
    REQUIRE(!event.value().disconnect);

    auto replay = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(replay.ok());
    REQUIRE(replay.value().header.msg_type == "4");
    REQUIRE(replay.value().header.msg_seq_num == 2U);
    REQUIRE(replay.value().message.view().get_boolean(kGapFillFlag).value());
    REQUIRE(replay.value().message.view().get_int(kNewSeqNo).value() == 3);

    const auto after_replay = protocol.session().Snapshot();
    REQUIRE(after_replay.next_in_seq == 3U);
    REQUIRE(after_replay.next_out_seq == before_replay.next_out_seq);

    auto stored = store.LoadOutboundRange(5030U, 1U, 10U);
    REQUIRE(stored.ok());
    REQUIRE(stored.value().size() == 2U);
    REQUIRE(stored.value()[0].seq_num == 1U);
    REQUIRE(stored.value()[1].seq_num == 2U);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5031U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    nimble::message::MessageBuilder logout_builder("5");
    logout_builder.set_string(kMsgType, "5");
    auto inbound =
      EncodeInboundFrame(std::move(logout_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 2U, false);
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.size() == 1U);
    REQUIRE(event.value().disconnect);

    auto decoded = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "5");

    const auto snapshot = protocol.session().Snapshot();
    REQUIRE(snapshot.state == nimble::session::SessionState::kAwaitingLogout);
    REQUIRE(snapshot.next_in_seq == 3U);
    REQUIRE(snapshot.next_out_seq == 3U);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5032U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    nimble::message::MessageBuilder heartbeat_builder("0");
    heartbeat_builder.set_string(kMsgType, "0");
    auto inbound =
      EncodeInboundFrame(std::move(heartbeat_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 4U, false);
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.size() == 1U);
    REQUIRE(!event.value().disconnect);

    auto resend = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(resend.ok());
    REQUIRE(resend.value().header.msg_type == "2");
    REQUIRE(resend.value().message.view().get_int(kBeginSeqNo).value() == 2);
    REQUIRE(resend.value().message.view().get_int(kEndSeqNo).value() == 3);

    const auto snapshot = protocol.session().Snapshot();
    REQUIRE(snapshot.state == nimble::session::SessionState::kResendProcessing);
    REQUIRE(snapshot.has_pending_resend);
    REQUIRE(snapshot.pending_resend.begin_seq == 2U);
    REQUIRE(snapshot.pending_resend.end_seq == 3U);
    REQUIRE(snapshot.last_inbound_ns == 10U);

    const auto deadline = protocol.NextTimerDeadline(10U);
    REQUIRE(deadline.has_value());
    REQUIRE(deadline.value() == 30000000010ULL);

    auto timer_event = protocol.OnTimer(30000000011ULL);
    REQUIRE(timer_event.ok());
    REQUIRE(timer_event.value().outbound_frames.size() == 1U);

    auto heartbeat =
      nimble::codec::DecodeFixMessage(timer_event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(heartbeat.ok());
    REQUIRE(heartbeat.value().header.msg_type == "0");

    timer_event = protocol.OnTimer(60000000011ULL);
    REQUIRE(timer_event.ok());
    REQUIRE(timer_event.value().outbound_frames.size() == 1U);

    auto test_request =
      nimble::codec::DecodeFixMessage(timer_event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(test_request.ok());
    REQUIRE(test_request.value().header.msg_type == "1");
    REQUIRE(test_request.value().message.view().get_string(kTestReqID).has_value());
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5007U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    nimble::message::MessageBuilder app_builder("ZZ");
    app_builder.set_string(kMsgType, "ZZ");
    auto inbound =
      EncodeInboundFrame(std::move(app_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 2U, false);
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.size() == 1U);
    REQUIRE(event.value().application_messages.empty());
    REQUIRE(!event.value().disconnect);

    auto decoded = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "3");
    REQUIRE(decoded.value().message.view().get_int(kRefTagID).value() == kMsgType);
    REQUIRE(decoded.value().message.view().get_string(kRefMsgType).value() == "ZZ");
    REQUIRE(decoded.value().message.view().get_int(kRejectReason).value() == 11);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5008U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .supported_app_msg_types = { "D" },
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    nimble::message::MessageBuilder app_builder("AE");
    app_builder.set_string(kMsgType, "AE");
    auto inbound =
      EncodeInboundFrame(std::move(app_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 2U, false);
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.size() == 1U);
    REQUIRE(event.value().application_messages.empty());
    REQUIRE(!event.value().disconnect);

    auto decoded = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "j");
    REQUIRE(decoded.value().message.view().get_string(kRefMsgType).value() == "AE");
    REQUIRE(decoded.value().message.view().get_int(380).value() == 3);
    REQUIRE(protocol.session().Snapshot().next_in_seq == 3U);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5009U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
        .application_messages_available = false,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    nimble::message::MessageBuilder app_builder("D");
    app_builder.set_string(kMsgType, "D");
    auto inbound =
      EncodeInboundFrame(std::move(app_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 2U, false);
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.size() == 1U);
    REQUIRE(event.value().application_messages.empty());
    REQUIRE(!event.value().disconnect);

    auto decoded = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "j");
    REQUIRE(decoded.value().message.view().get_string(kRefMsgType).value() == "D");
    REQUIRE(decoded.value().message.view().get_int(380).value() == 4);
    REQUIRE(protocol.session().Snapshot().next_in_seq == 3U);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5124U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    const auto inbound = ::nimble::tests::EncodeFixFrame(
      "35=D|34=2|49=BUY|56=SELL|52=20260402-12:00:00.000|11=ORD-14M|55=AAPL|54=1|60=20260402-12:00:00.000|40=2|");
    auto event = protocol.OnInbound(inbound, 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.size() == 1U);
    REQUIRE(event.value().application_messages.empty());
    REQUIRE(!event.value().disconnect);

    auto decoded = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "j");
    REQUIRE(decoded.value().message.view().get_string(kRefMsgType).value() == "D");
    REQUIRE(decoded.value().message.view().get_int(380).value() == 5);
    REQUIRE(decoded.value().message.view().get_string(kText).value().find("Price") != std::string_view::npos);
    REQUIRE(protocol.session().Snapshot().next_in_seq == 3U);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5011U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
        .validation_policy = nimble::session::ValidationPolicy::Compatible(),
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    nimble::message::MessageBuilder app_builder("ZZ");
    app_builder.set_string(kMsgType, "ZZ");
    auto inbound =
      EncodeInboundFrame(std::move(app_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 2U, false);
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.empty());
    REQUIRE(event.value().application_messages.size() == 1U);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5012U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    const auto inbound =
      ::nimble::tests::EncodeFixFrame("35=D|34=2|49=BUY|56=SELL|52=20260402-12:00:00.000|55=AAPL|9999=BAD|");
    auto event = protocol.OnInbound(inbound, 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.size() == 1U);
    REQUIRE(event.value().application_messages.empty());

    auto decoded = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "3");
    REQUIRE(decoded.value().message.view().get_int(kRefTagID).value() == 9999);
    REQUIRE(decoded.value().message.view().get_string(kRefMsgType).value() == "D");
    REQUIRE(decoded.value().message.view().get_int(kRejectReason).value() == 0);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5120U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    const auto inbound = ::nimble::tests::EncodeFixFrame("35=2|34=2|49=BUY|56=SELL|52=20260402-12:00:00.000|7=1|16=|");
    auto event = protocol.OnInbound(inbound, 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.size() == 1U);
    REQUIRE(event.value().application_messages.empty());

    auto decoded = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "3");
    REQUIRE(decoded.value().message.view().get_int(kRefTagID).value() == kEndSeqNo);
    REQUIRE(decoded.value().message.view().get_string(kRefMsgType).value() == "2");
    REQUIRE(decoded.value().message.view().get_int(kRejectReason).value() == 4);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5121U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    const auto inbound =
      ::nimble::tests::EncodeFixFrame("35=2|34=2|49=BUY|56=SELL|52=20260402-12:00:00.000|7=ABC|16=0|");
    auto event = protocol.OnInbound(inbound, 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.size() == 1U);
    REQUIRE(event.value().application_messages.empty());

    auto decoded = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "3");
    REQUIRE(decoded.value().message.view().get_int(kRefTagID).value() == kBeginSeqNo);
    REQUIRE(decoded.value().message.view().get_string(kRefMsgType).value() == "2");
    REQUIRE(decoded.value().message.view().get_int(kRejectReason).value() == 6);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5122U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    const auto inbound = ::nimble::tests::EncodeFixFrame("35=2|34=2|49=BUY|56=SELL|52=20260402-12:00:00.000|16=0|7=1|");
    auto event = protocol.OnInbound(inbound, 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.size() == 1U);
    REQUIRE(event.value().application_messages.empty());

    auto decoded = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "3");
    REQUIRE(decoded.value().message.view().get_int(kRefTagID).value() == kBeginSeqNo);
    REQUIRE(decoded.value().message.view().get_string(kRefMsgType).value() == "2");
    REQUIRE(decoded.value().message.view().get_int(kRejectReason).value() == 14);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5123U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    const auto inbound = ::nimble::tests::EncodeFixFrame(
      "35=D|34=2|49=BUY|56=SELL|52=20260402-12:00:00.000|11=ORD1|453=1|447=D|448=PTY1|452=7|");
    auto event = protocol.OnInbound(inbound, 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.size() == 1U);
    REQUIRE(event.value().application_messages.empty());

    auto decoded = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "3");
    REQUIRE(decoded.value().message.view().get_int(kRefTagID).value() == kPartyIDSource);
    REQUIRE(decoded.value().message.view().get_string(kRefMsgType).value() == "D");
    REQUIRE(decoded.value().message.view().get_int(kRejectReason).value() == 15);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5125U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    const auto inbound =
      ::nimble::tests::EncodeFixFrame("35=0|34=2|49=BUY|56=SELL|52=20260402-12:00:00.000|58=CLEAR|90=4|91=ABCD|");
    auto event = protocol.OnInbound(inbound, 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.size() == 1U);
    REQUIRE(event.value().application_messages.empty());
    REQUIRE(!event.value().disconnect);

    auto decoded = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "3");
    REQUIRE(decoded.value().message.view().get_int(kRefTagID).value() == kSecureData);
    REQUIRE(decoded.value().message.view().get_string(kRefMsgType).value() == "0");
    REQUIRE(decoded.value().message.view().get_int(kRejectReason).value() == 7);
    REQUIRE(protocol.session().Snapshot().next_in_seq == 3U);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5013U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    const auto inbound = ::nimble::tests::EncodeFixFrame("35=D|34=2|49=BUY|56=SELL|52=20260402-"
                                                         "12:00:00.000|55=AAPL|448=ORPHAN|");
    auto event = protocol.OnInbound(inbound, 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.size() == 1U);
    REQUIRE(event.value().application_messages.empty());

    auto decoded = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "3");
    REQUIRE(decoded.value().message.view().get_int(kRefTagID).value() == kPartyID);
    REQUIRE(decoded.value().message.view().get_string(kRefMsgType).value() == "D");
    REQUIRE(decoded.value().message.view().get_int(kRejectReason).value() == 2);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5014U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    const auto inbound =
      ::nimble::tests::EncodeFixFrame("35=D|34=2|49=BUY|56=SELL|52=20260402-12:00:00.000|55=AAPL|55=MSFT|");
    auto event = protocol.OnInbound(inbound, 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.size() == 1U);
    REQUIRE(event.value().application_messages.empty());

    auto decoded = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "3");
    REQUIRE(decoded.value().message.view().get_int(kRefTagID).value() == kSymbol);
    REQUIRE(decoded.value().message.view().get_string(kRefMsgType).value() == "D");
    REQUIRE(decoded.value().message.view().get_int(kRejectReason).value() == 13);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5015U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    const auto inbound =
      ::nimble::tests::EncodeFixFrame("35=D|34=2|49=BUY|56=SELL|52=20260402-12:00:00.000|453=1|448=PTY1|447="
                                      "D|452=7|55=AAPL|");
    auto event = protocol.OnInbound(inbound, 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.size() == 1U);
    REQUIRE(event.value().application_messages.empty());

    auto decoded = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "3");
    REQUIRE(decoded.value().message.view().get_int(kRefTagID).value() == kClOrdID);
    REQUIRE(decoded.value().message.view().get_string(kRefMsgType).value() == "D");
    REQUIRE(decoded.value().message.view().get_int(kRejectReason).value() == 1);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5019U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    const auto inbound =
      ::nimble::tests::EncodeFixFrame("35=D|34=2|49=BUY|56=SELL|52=20260402-12:00:00.000|11=ORD1|453=1|");
    auto event = protocol.OnInbound(inbound, 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.size() == 1U);
    REQUIRE(event.value().application_messages.empty());

    auto decoded = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "3");
    REQUIRE(decoded.value().message.view().get_int(kRefTagID).value() == kNoPartyIDs);
    REQUIRE(decoded.value().message.view().get_string(kRefMsgType).value() == "D");
    REQUIRE(decoded.value().message.view().get_int(kRejectReason).value() == 16);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5016U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
        .validation_policy = nimble::session::ValidationPolicy::Compatible(),
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    const auto inbound =
      ::nimble::tests::EncodeFixFrame("35=D|34=2|49=BUY|56=SELL|52=20260402-12:00:00.000|11=ORD1|55=AAPL|54=1|60="
                                      "20260402-12:00:00.000|40=1|9999=BAD|");
    auto event = protocol.OnInbound(inbound, 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.empty());
    REQUIRE(event.value().application_messages.size() == 1U);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5020U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
        .validation_policy = nimble::session::ValidationPolicy::Compatible(),
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    const auto inbound =
      ::nimble::tests::EncodeFixFrame("35=D|34=2|49=BUY|56=SELL|52=20260402-12:00:00.000|11=ORD1|55=AAPL|54=1|60="
                                      "20260402-12:00:00.000|40=1|453=1|448=PARTY1|447=D|452=1|");
    auto event = protocol.OnInbound(inbound, 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.empty());
    REQUIRE(event.value().application_messages.size() == 1U);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5017U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
        .validation_policy = nimble::session::ValidationPolicy::Compatible(),
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    const auto inbound = ::nimble::tests::EncodeFixFrame(
      "35=D|34=2|49=BUY|56=SELL|52=20260402-12:00:00.000|11=ORD1|55=AAPL|55=MSFT|54=1|60="
      "20260402-12:00:00.000|40=1|");
    auto event = protocol.OnInbound(inbound, 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.empty());
    REQUIRE(event.value().application_messages.size() == 1U);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5018U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
        .validation_policy = nimble::session::ValidationPolicy::Compatible(),
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    const auto inbound =
      ::nimble::tests::EncodeFixFrame("35=D|34=2|49=BUY|56=SELL|52=20260402-12:00:00.000|11=ORD1|54=1|60="
                                      "20260402-12:00:00.000|40=1|453=1|448=PTY1|447=D|452=7|55=AAPL|");
    auto event = protocol.OnInbound(inbound, 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.empty());
    REQUIRE(event.value().application_messages.size() == 1U);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5008U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    nimble::message::MessageBuilder app_builder("D");
    app_builder.set_string(kMsgType, "D");
    auto inbound =
      EncodeInboundFrame(std::move(app_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 2U, false);
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.size() == 1U);
    REQUIRE(event.value().application_messages.empty());

    auto decoded = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "3");
    REQUIRE(decoded.value().message.view().get_int(kRefTagID).value() == kClOrdID);
    REQUIRE(decoded.value().message.view().get_string(kRefMsgType).value() == "D");
    REQUIRE(decoded.value().message.view().get_int(kRejectReason).value() == 1);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5009U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    nimble::message::MessageBuilder app_builder("D");
    app_builder.set_string(kMsgType, "D").set_string(kSymbol, "AAPL");
    auto party = app_builder.add_group_entry(kNoPartyIDs);
    party.set_string(kPartyID, "PTY1").set_int(kPartyRole, 7);
    auto inbound =
      EncodeInboundFrame(std::move(app_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 2U, false);
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.size() == 1U);
    REQUIRE(event.value().application_messages.empty());

    auto decoded = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "3");
    REQUIRE(decoded.value().message.view().get_int(kRefTagID).value() == kClOrdID);
    REQUIRE(decoded.value().message.view().get_string(kRefMsgType).value() == "D");
    REQUIRE(decoded.value().message.view().get_int(kRejectReason).value() == 1);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5010U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
        .validation_policy = nimble::session::ValidationPolicy::Permissive(),
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    nimble::message::MessageBuilder app_builder("D");
    app_builder.set_string(kMsgType, "D");
    auto inbound =
      EncodeInboundFrame(std::move(app_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 2U, false);
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.empty());
    REQUIRE(event.value().application_messages.size() == 1U);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 5011U,
            .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
            .profile_id = dictionary.value().profile().header().profile_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = "FIX.4.4",
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .heartbeat_interval_seconds = 30U,
        .validation_policy = nimble::session::ValidationPolicy::Permissive(),
      },
      dictionary.value(),
      &store);

    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    nimble::message::MessageBuilder app_builder("D");
    app_builder.set_string(kMsgType, "D").set_string(kSymbol, "AAPL");
    auto party = app_builder.add_group_entry(kNoPartyIDs);
    party.set_string(kPartyID, "PTY1").set_char(kPartyIDSource, 'D').set_int(kPartyRole, 1);
    auto inbound =
      EncodeInboundFrame(std::move(app_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 2U, false);
    REQUIRE(inbound.ok());

    auto inbound_frame = std::move(inbound).value();
    auto inbound_result = protocol.OnInbound(std::move(inbound_frame), 10U);
    REQUIRE(inbound_result.ok());

    auto event = std::move(inbound_result).value();
    REQUIRE(event.outbound_frames.empty());
    REQUIRE(event.application_messages.size() == 1U);

    auto moved_event = std::move(event);
    REQUIRE(moved_event.application_messages.size() == 1U);
    REQUIRE(moved_event.application_messages.front().view().get_string(kSymbol).value() == "AAPL");
    const auto parties = moved_event.application_messages.front().view().group(kNoPartyIDs);
    REQUIRE(parties.has_value());
    REQUIRE(parties->size() == 1U);
    REQUIRE((*parties)[0].get_char(kPartyIDSource).value() == 'D');
    REQUIRE((*parties)[0].get_string(kPartyID).value() == "PTY1");
  }
}

TEST_CASE("malformed raw inbound frames are ignored without consuming sequence", "[admin-protocol]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }

  nimble::store::MemorySessionStore store;
  nimble::session::AdminProtocol protocol(
    nimble::session::AdminProtocolConfig{
      .session =
        nimble::session::SessionConfig{
          .session_id = 6000U,
          .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
          .profile_id = dictionary.value().profile().header().profile_id,
          .heartbeat_interval_seconds = 30U,
          .is_initiator = false,
        },
      .begin_string = "FIX.4.4",
      .sender_comp_id = "SELL",
      .target_comp_id = "BUY",
      .heartbeat_interval_seconds = 30U,
    },
    dictionary.value(),
    &store);

  REQUIRE(protocol.OnTransportConnected(1U).ok());
  REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());
  REQUIRE(protocol.session().Snapshot().next_in_seq == 2U);

  auto malformed = nimble::tests::EncodeFixFrame("35=0|34=2|49=BUY|56=SELL|52=20260402-12:00:00.000|");
  REQUIRE(malformed.size() > 2U);
  const auto last_digit = std::to_integer<unsigned char>(malformed[malformed.size() - 2U]);
  malformed[malformed.size() - 2U] = std::byte{ static_cast<unsigned char>(last_digit == '0' ? '1' : '0') };

  auto malformed_event = protocol.OnInbound(std::span<const std::byte>(malformed.data(), malformed.size()), 20U);
  REQUIRE(malformed_event.ok());
  REQUIRE(malformed_event.value().outbound_frames.empty());
  REQUIRE(malformed_event.value().application_messages.empty());
  REQUIRE(!malformed_event.value().disconnect);
  REQUIRE(!malformed_event.value().warnings.empty());
  REQUIRE(protocol.session().Snapshot().next_in_seq == 2U);
  REQUIRE(protocol.session().Snapshot().last_inbound_ns == 20U);

  const auto valid = nimble::tests::EncodeFixFrame("35=0|34=2|49=BUY|56=SELL|52=20260402-12:00:01.000|");
  auto valid_event = protocol.OnInbound(std::span<const std::byte>(valid.data(), valid.size()), 30U);
  REQUIRE(valid_event.ok());
  REQUIRE(valid_event.value().outbound_frames.empty());
  REQUIRE(valid_event.value().application_messages.empty());
  REQUIRE(!valid_event.value().disconnect);
  REQUIRE(protocol.session().Snapshot().next_in_seq == 3U);
  REQUIRE(protocol.session().Snapshot().last_inbound_ns == 30U);
}

TEST_CASE("SendingTime outside threshold rejects then logs out", "[admin-protocol]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }

  const auto base_time = std::chrono::sys_days{ std::chrono::year{ 2026 } / 4 / 25 } + std::chrono::hours{ 12 };
  const auto logon_timestamp_ns = static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::nanoseconds>(base_time.time_since_epoch()).count());
  const auto stale_timestamp_ns =
    static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 (base_time + std::chrono::minutes{ 2 } + std::chrono::seconds{ 1 }).time_since_epoch())
                                 .count());

  nimble::store::MemorySessionStore store;
  nimble::session::AdminProtocol protocol(
    nimble::session::AdminProtocolConfig{
      .session =
        nimble::session::SessionConfig{
          .session_id = 6002U,
          .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
          .profile_id = dictionary.value().profile().header().profile_id,
          .heartbeat_interval_seconds = 30U,
          .is_initiator = false,
        },
      .begin_string = "FIX.4.4",
      .sender_comp_id = "SELL",
      .target_comp_id = "BUY",
      .heartbeat_interval_seconds = 30U,
      .sending_time_threshold_seconds = 60U,
    },
    dictionary.value(),
    &store);

  REQUIRE(protocol.OnTransportConnected(logon_timestamp_ns - 1U).ok());

  const auto inbound_logon =
    nimble::tests::EncodeFixFrame("35=A|34=1|49=BUY|56=SELL|52=20260425-12:00:00.000|98=0|108=30|");
  auto logon_event =
    protocol.OnInbound(std::span<const std::byte>(inbound_logon.data(), inbound_logon.size()), logon_timestamp_ns);
  REQUIRE(logon_event.ok());
  REQUIRE(logon_event.value().outbound_frames.size() == 1U);
  REQUIRE(!logon_event.value().disconnect);

  const auto stale_heartbeat = nimble::tests::EncodeFixFrame("35=0|34=2|49=BUY|56=SELL|52=20260425-12:00:00.000|");
  auto stale_event =
    protocol.OnInbound(std::span<const std::byte>(stale_heartbeat.data(), stale_heartbeat.size()), stale_timestamp_ns);
  REQUIRE(stale_event.ok());
  REQUIRE(stale_event.value().outbound_frames.size() == 2U);
  REQUIRE(stale_event.value().application_messages.empty());
  REQUIRE(stale_event.value().disconnect);
  REQUIRE(protocol.session().Snapshot().next_in_seq == 3U);
  REQUIRE(protocol.session().Snapshot().next_out_seq == 4U);

  auto reject = nimble::codec::DecodeFixMessage(stale_event.value().outbound_frames[0].bytes, dictionary.value());
  REQUIRE(reject.ok());
  REQUIRE(reject.value().header.msg_type == "3");
  REQUIRE(reject.value().message.view().get_int(kRefTagID).value() == kSendingTime);
  REQUIRE(reject.value().message.view().get_int(kRejectReason).value() == 10);
  REQUIRE(reject.value().message.view().get_string(kRefMsgType).value() == "0");

  auto logout = nimble::codec::DecodeFixMessage(stale_event.value().outbound_frames[1].bytes, dictionary.value());
  REQUIRE(logout.ok());
  REQUIRE(logout.value().header.msg_type == "5");
  REQUIRE(logout.value().message.view().get_string(kText).value().find("configured tolerance") != std::string::npos);
}

TEST_CASE("PossResend flag detected on app message", "[admin-protocol]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }

  nimble::store::MemorySessionStore store;
  nimble::session::AdminProtocol protocol(
    nimble::session::AdminProtocolConfig{
      .session =
        nimble::session::SessionConfig{
          .session_id = 6001U,
          .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
          .profile_id = dictionary.value().profile().header().profile_id,
          .heartbeat_interval_seconds = 30U,
          .is_initiator = false,
        },
      .begin_string = "FIX.4.4",
      .sender_comp_id = "SELL",
      .target_comp_id = "BUY",
      .heartbeat_interval_seconds = 30U,
      .validation_policy = nimble::session::ValidationPolicy::Permissive(),
    },
    dictionary.value(),
    &store);

  REQUIRE(protocol.OnTransportConnected(1U).ok());
  REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

  nimble::message::MessageBuilder app_builder("D");
  app_builder.set_string(kMsgType, "D").set_string(kSymbol, "MSFT").set_boolean(kPossResend, true);
  auto inbound =
    EncodeInboundFrame(std::move(app_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 2U, false);
  REQUIRE(inbound.ok());

  auto event = protocol.OnInbound(inbound.value(), 10U);
  REQUIRE(event.ok());
  REQUIRE(event.value().application_messages.size() == 1U);
  REQUIRE(event.value().poss_resend);
}

TEST_CASE("PossResend flag not set on normal message", "[admin-protocol]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }

  nimble::store::MemorySessionStore store;
  nimble::session::AdminProtocol protocol(
    nimble::session::AdminProtocolConfig{
      .session =
        nimble::session::SessionConfig{
          .session_id = 6002U,
          .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
          .profile_id = dictionary.value().profile().header().profile_id,
          .heartbeat_interval_seconds = 30U,
          .is_initiator = false,
        },
      .begin_string = "FIX.4.4",
      .sender_comp_id = "SELL",
      .target_comp_id = "BUY",
      .heartbeat_interval_seconds = 30U,
      .validation_policy = nimble::session::ValidationPolicy::Permissive(),
    },
    dictionary.value(),
    &store);

  REQUIRE(protocol.OnTransportConnected(1U).ok());
  REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

  nimble::message::MessageBuilder app_builder("D");
  app_builder.set_string(kMsgType, "D").set_string(kSymbol, "AAPL");
  auto inbound =
    EncodeInboundFrame(std::move(app_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 2U, false);
  REQUIRE(inbound.ok());

  auto event = protocol.OnInbound(inbound.value(), 10U);
  REQUIRE(event.ok());
  REQUIRE(event.value().application_messages.size() == 1U);
  REQUIRE_FALSE(event.value().poss_resend);
}

TEST_CASE("PossResend message still processed by application", "[admin-protocol]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }

  nimble::store::MemorySessionStore store;
  nimble::session::AdminProtocol protocol(
    nimble::session::AdminProtocolConfig{
      .session =
        nimble::session::SessionConfig{
          .session_id = 6003U,
          .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
          .profile_id = dictionary.value().profile().header().profile_id,
          .heartbeat_interval_seconds = 30U,
          .is_initiator = false,
        },
      .begin_string = "FIX.4.4",
      .sender_comp_id = "SELL",
      .target_comp_id = "BUY",
      .heartbeat_interval_seconds = 30U,
      .validation_policy = nimble::session::ValidationPolicy::Permissive(),
    },
    dictionary.value(),
    &store);

  REQUIRE(protocol.OnTransportConnected(1U).ok());
  REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

  nimble::message::MessageBuilder app_builder("D");
  app_builder.set_string(kMsgType, "D").set_string(kSymbol, "GOOG").set_boolean(kPossResend, true);
  auto inbound =
    EncodeInboundFrame(std::move(app_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 2U, false);
  REQUIRE(inbound.ok());

  auto event = protocol.OnInbound(inbound.value(), 10U);
  REQUIRE(event.ok());
  // PossResend messages must NOT be silently dropped — they are passed to the
  // application layer.
  REQUIRE(event.value().application_messages.size() == 1U);
  REQUIRE(event.value().poss_resend);
  // The application message content is intact.
  REQUIRE(event.value().application_messages.front().view().get_string(kSymbol).value() == "GOOG");
  REQUIRE(event.value().application_messages.front().view().get_boolean(kPossResend).value());
  // No reject or disconnect — the message is delivered normally.
  REQUIRE(event.value().outbound_frames.empty());
  REQUIRE_FALSE(event.value().disconnect);
}

// ==================== ResendRequest Tests ====================

TEST_CASE("ResendRequest happy path", "[admin-protocol]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }

  nimble::store::MemorySessionStore store;
  nimble::session::AdminProtocol protocol(
    nimble::session::AdminProtocolConfig{
      .session =
        nimble::session::SessionConfig{
          .session_id = 7001U,
          .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
          .profile_id = dictionary.value().profile().header().profile_id,
          .heartbeat_interval_seconds = 30U,
          .is_initiator = false,
        },
      .begin_string = "FIX.4.4",
      .sender_comp_id = "SELL",
      .target_comp_id = "BUY",
      .heartbeat_interval_seconds = 30U,
    },
    dictionary.value(),
    &store);

  REQUIRE(protocol.OnTransportConnected(1U).ok());
  REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

  // Send 3 outbound application messages (seqs 2, 3, 4)
  for (int i = 0; i < 3; ++i) {
    nimble::message::MessageBuilder app_builder("D");
    app_builder.set_string(kMsgType, "D").set_string(kSymbol, "AAPL");
    auto sent = protocol.SendApplication(std::move(app_builder).build(), 100U + static_cast<std::uint64_t>(i));
    REQUIRE(sent.ok());
  }
  REQUIRE(protocol.session().Snapshot().next_out_seq == 5U);

  // Counterparty requests resend of seqs 2-4
  nimble::message::MessageBuilder resend_builder("2");
  resend_builder.set_string(kMsgType, "2").set_int(kBeginSeqNo, 2).set_int(kEndSeqNo, 4);
  auto inbound =
    EncodeInboundFrame(std::move(resend_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 2U, false);
  REQUIRE(inbound.ok());

  auto event = protocol.OnInbound(inbound.value(), 200U);
  REQUIRE(event.ok());
  REQUIRE(event.value().outbound_frames.size() == 3U);
  REQUIRE(!event.value().disconnect);

  // All replayed frames should be app messages with PossDup
  for (std::size_t i = 0U; i < 3U; ++i) {
    auto replayed = nimble::codec::DecodeFixMessage(event.value().outbound_frames[i].bytes, dictionary.value());
    REQUIRE(replayed.ok());
    REQUIRE(replayed.value().header.msg_type == "D");
    REQUIRE(replayed.value().header.poss_dup);
  }
}

TEST_CASE("ResendRequest BeginSeqNo=0 rejected", "[admin-protocol]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }

  nimble::store::MemorySessionStore store;
  nimble::session::AdminProtocol protocol(
    nimble::session::AdminProtocolConfig{
      .session =
        nimble::session::SessionConfig{
          .session_id = 7002U,
          .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
          .profile_id = dictionary.value().profile().header().profile_id,
          .heartbeat_interval_seconds = 30U,
          .is_initiator = false,
        },
      .begin_string = "FIX.4.4",
      .sender_comp_id = "SELL",
      .target_comp_id = "BUY",
      .heartbeat_interval_seconds = 30U,
    },
    dictionary.value(),
    &store);

  REQUIRE(protocol.OnTransportConnected(1U).ok());
  REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

  // BeginSeqNo=0 is invalid per FIX spec — must be positive
  nimble::message::MessageBuilder resend_builder("2");
  resend_builder.set_string(kMsgType, "2").set_int(kBeginSeqNo, 0).set_int(kEndSeqNo, 5);
  auto inbound =
    EncodeInboundFrame(std::move(resend_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 2U, false);
  REQUIRE(inbound.ok());

  auto event = protocol.OnInbound(inbound.value(), 200U);
  REQUIRE(event.ok());
  REQUIRE(event.value().outbound_frames.size() == 1U);
  REQUIRE(!event.value().disconnect);

  auto decoded = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
  REQUIRE(decoded.ok());
  REQUIRE(decoded.value().header.msg_type == "3");
  REQUIRE(decoded.value().message.view().get_int(kRefTagID).value() == kBeginSeqNo);
  REQUIRE(decoded.value().message.view().get_string(kRefMsgType).value() == "2");
  REQUIRE(decoded.value().message.view().get_int(kRejectReason).value() == 5);
}

TEST_CASE("ResendRequest EndSeqNo=0 replays to infinity", "[admin-protocol]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }

  nimble::store::MemorySessionStore store;
  nimble::session::AdminProtocol protocol(
    nimble::session::AdminProtocolConfig{
      .session =
        nimble::session::SessionConfig{
          .session_id = 7003U,
          .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
          .profile_id = dictionary.value().profile().header().profile_id,
          .heartbeat_interval_seconds = 30U,
          .is_initiator = false,
        },
      .begin_string = "FIX.4.4",
      .sender_comp_id = "SELL",
      .target_comp_id = "BUY",
      .heartbeat_interval_seconds = 30U,
    },
    dictionary.value(),
    &store);

  REQUIRE(protocol.OnTransportConnected(1U).ok());
  REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

  // Send 2 outbound app messages (seqs 2, 3)
  for (int i = 0; i < 2; ++i) {
    nimble::message::MessageBuilder app_builder("D");
    app_builder.set_string(kMsgType, "D").set_string(kSymbol, "AAPL");
    auto sent = protocol.SendApplication(std::move(app_builder).build(), 100U + static_cast<std::uint64_t>(i));
    REQUIRE(sent.ok());
  }
  REQUIRE(protocol.session().Snapshot().next_out_seq == 4U);

  // EndSeqNo=0 means "all messages from BeginSeqNo to end"
  nimble::message::MessageBuilder resend_builder("2");
  resend_builder.set_string(kMsgType, "2").set_int(kBeginSeqNo, 1).set_int(kEndSeqNo, 0);
  auto inbound =
    EncodeInboundFrame(std::move(resend_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 2U, false);
  REQUIRE(inbound.ok());

  auto event = protocol.OnInbound(inbound.value(), 200U);
  REQUIRE(event.ok());
  REQUIRE(!event.value().disconnect);

  // Seq 1 is logon (admin) → GapFill; seqs 2,3 are app → replay
  REQUIRE(event.value().outbound_frames.size() == 3U);

  auto gap_fill = nimble::codec::DecodeFixMessage(event.value().outbound_frames[0U].bytes, dictionary.value());
  REQUIRE(gap_fill.ok());
  REQUIRE(gap_fill.value().header.msg_type == "4");
  REQUIRE(gap_fill.value().message.view().get_boolean(kGapFillFlag).value());
  REQUIRE(gap_fill.value().message.view().get_int(kNewSeqNo).value() == 2);

  for (std::size_t i = 1U; i < event.value().outbound_frames.size(); ++i) {
    auto replayed = nimble::codec::DecodeFixMessage(event.value().outbound_frames[i].bytes, dictionary.value());
    REQUIRE(replayed.ok());
    REQUIRE(replayed.value().header.msg_type == "D");
    REQUIRE(replayed.value().header.poss_dup);
  }
}

// ==================== TestRequest Tests ====================

TEST_CASE("TestRequest happy path", "[admin-protocol]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }

  nimble::store::MemorySessionStore store;
  nimble::session::AdminProtocol protocol(
    nimble::session::AdminProtocolConfig{
      .session =
        nimble::session::SessionConfig{
          .session_id = 7004U,
          .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
          .profile_id = dictionary.value().profile().header().profile_id,
          .heartbeat_interval_seconds = 30U,
          .is_initiator = false,
        },
      .begin_string = "FIX.4.4",
      .sender_comp_id = "SELL",
      .target_comp_id = "BUY",
      .heartbeat_interval_seconds = 30U,
    },
    dictionary.value(),
    &store);

  REQUIRE(protocol.OnTransportConnected(1U).ok());
  REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

  // Receive TestRequest with TestReqID
  nimble::message::MessageBuilder test_builder("1");
  test_builder.set_string(kMsgType, "1").set_string(kTestReqID, "HELLO-123");
  auto inbound =
    EncodeInboundFrame(std::move(test_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 2U, false);
  REQUIRE(inbound.ok());

  auto event = protocol.OnInbound(inbound.value(), 10U);
  REQUIRE(event.ok());
  REQUIRE(event.value().outbound_frames.size() == 1U);
  REQUIRE(!event.value().disconnect);

  // Verify Heartbeat response echoes the TestReqID
  auto decoded = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
  REQUIRE(decoded.ok());
  REQUIRE(decoded.value().header.msg_type == "0");
  REQUIRE(decoded.value().message.view().get_string(kTestReqID).value() == "HELLO-123");
}

TEST_CASE("TestRequest timeout triggers disconnect", "[admin-protocol]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }

  nimble::store::MemorySessionStore store;
  nimble::session::AdminProtocol protocol(
    nimble::session::AdminProtocolConfig{
      .session =
        nimble::session::SessionConfig{
          .session_id = 7005U,
          .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
          .profile_id = dictionary.value().profile().header().profile_id,
          .heartbeat_interval_seconds = 30U,
          .is_initiator = false,
        },
      .begin_string = "FIX.4.4",
      .sender_comp_id = "SELL",
      .target_comp_id = "BUY",
      .heartbeat_interval_seconds = 30U,
    },
    dictionary.value(),
    &store);

  REQUIRE(protocol.OnTransportConnected(1U).ok());
  REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

  // Activation happens at timestamp 2; no further inbound activity.
  // After HeartBtInt plus the official 20% grace period, protocol sends TestRequest.
  constexpr std::uint64_t kIntervalNs = 30ULL * 1'000'000'000ULL;
  const std::uint64_t ts_test_request = 2U + kIntervalNs + (kIntervalNs / 5U) + 1U;

  auto timer1 = protocol.OnTimer(ts_test_request);
  REQUIRE(timer1.ok());
  REQUIRE(timer1.value().outbound_frames.size() == 1U);
  REQUIRE(!timer1.value().disconnect);

  auto test_request = nimble::codec::DecodeFixMessage(timer1.value().outbound_frames.front().bytes, dictionary.value());
  REQUIRE(test_request.ok());
  REQUIRE(test_request.value().header.msg_type == "1");
  REQUIRE(test_request.value().message.view().has_field(kTestReqID));

  // No Heartbeat response within another heartbeat_interval → disconnect
  auto timer2 = protocol.OnTimer(ts_test_request + kIntervalNs + 1U);
  REQUIRE(timer2.ok());
  REQUIRE(timer2.value().disconnect);
  REQUIRE(timer2.value().outbound_frames.empty());
}

TEST_CASE("TestRequest duplicate handling", "[admin-protocol]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }

  nimble::store::MemorySessionStore store;
  nimble::session::AdminProtocol protocol(
    nimble::session::AdminProtocolConfig{
      .session =
        nimble::session::SessionConfig{
          .session_id = 7006U,
          .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
          .profile_id = dictionary.value().profile().header().profile_id,
          .heartbeat_interval_seconds = 30U,
          .is_initiator = false,
        },
      .begin_string = "FIX.4.4",
      .sender_comp_id = "SELL",
      .target_comp_id = "BUY",
      .heartbeat_interval_seconds = 30U,
    },
    dictionary.value(),
    &store);

  REQUIRE(protocol.OnTransportConnected(1U).ok());
  REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

  // First TestRequest
  nimble::message::MessageBuilder test1("1");
  test1.set_string(kMsgType, "1").set_string(kTestReqID, "DUP-REQ");
  auto inbound1 = EncodeInboundFrame(std::move(test1).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 2U, false);
  REQUIRE(inbound1.ok());

  auto event1 = protocol.OnInbound(inbound1.value(), 10U);
  REQUIRE(event1.ok());
  REQUIRE(event1.value().outbound_frames.size() == 1U);

  auto hb1 = nimble::codec::DecodeFixMessage(event1.value().outbound_frames.front().bytes, dictionary.value());
  REQUIRE(hb1.ok());
  REQUIRE(hb1.value().header.msg_type == "0");
  REQUIRE(hb1.value().message.view().get_string(kTestReqID).value() == "DUP-REQ");

  // Second TestRequest with same TestReqID at next seq
  nimble::message::MessageBuilder test2("1");
  test2.set_string(kMsgType, "1").set_string(kTestReqID, "DUP-REQ");
  auto inbound2 = EncodeInboundFrame(std::move(test2).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 3U, false);
  REQUIRE(inbound2.ok());

  auto event2 = protocol.OnInbound(inbound2.value(), 20U);
  REQUIRE(event2.ok());
  REQUIRE(event2.value().outbound_frames.size() == 1U);

  auto hb2 = nimble::codec::DecodeFixMessage(event2.value().outbound_frames.front().bytes, dictionary.value());
  REQUIRE(hb2.ok());
  REQUIRE(hb2.value().header.msg_type == "0");
  REQUIRE(hb2.value().message.view().get_string(kTestReqID).value() == "DUP-REQ");
}

// ==================== SequenceReset Tests ====================

TEST_CASE("Acceptor reset_seq_num_on_logon resets local state before Logon response", "[admin-protocol][reset-seq]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }

  nimble::store::MemorySessionStore store;
  REQUIRE(store
            .SaveRecoveryState(nimble::store::SessionRecoveryState{
              .session_id = 7006U,
              .next_in_seq = 11U,
              .next_out_seq = 17U,
              .last_inbound_ns = 0U,
              .last_outbound_ns = 0U,
              .active = false,
            })
            .ok());

  nimble::session::AdminProtocol protocol(
    nimble::session::AdminProtocolConfig{
      .session =
        nimble::session::SessionConfig{
          .session_id = 7006U,
          .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
          .profile_id = dictionary.value().profile().header().profile_id,
          .heartbeat_interval_seconds = 30U,
          .is_initiator = false,
        },
      .begin_string = "FIX.4.4",
      .sender_comp_id = "SELL",
      .target_comp_id = "BUY",
      .heartbeat_interval_seconds = 30U,
      .reset_seq_num_on_logon = true,
    },
    dictionary.value(),
    &store);

  REQUIRE(protocol.OnTransportConnected(1U).ok());

  nimble::message::MessageBuilder logon_builder("A");
  logon_builder.set_string(kMsgType, "A").set_int(kEncryptMethod, 0).set_int(kHeartBtInt, 30);
  auto inbound =
    EncodeInboundFrame(std::move(logon_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 1U, false);
  REQUIRE(inbound.ok());

  auto event = protocol.OnInbound(inbound.value(), 10U);
  REQUIRE(event.ok());
  REQUIRE(event.value().outbound_frames.size() == 1U);

  auto decoded = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
  REQUIRE(decoded.ok());
  REQUIRE(decoded.value().header.msg_type == "A");
  REQUIRE(decoded.value().header.msg_seq_num == 1U);
  REQUIRE(decoded.value().message.view().get_boolean(kResetSeqNumFlag).value());

  const auto snapshot = protocol.session().Snapshot();
  REQUIRE(snapshot.next_in_seq == 2U);
  REQUIRE(snapshot.next_out_seq == 2U);
}

TEST_CASE("SequenceReset-Reset happy path", "[admin-protocol]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }

  nimble::store::MemorySessionStore store;
  nimble::session::AdminProtocol protocol(
    nimble::session::AdminProtocolConfig{
      .session =
        nimble::session::SessionConfig{
          .session_id = 7007U,
          .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
          .profile_id = dictionary.value().profile().header().profile_id,
          .heartbeat_interval_seconds = 30U,
          .is_initiator = false,
        },
      .begin_string = "FIX.4.4",
      .sender_comp_id = "SELL",
      .target_comp_id = "BUY",
      .heartbeat_interval_seconds = 30U,
    },
    dictionary.value(),
    &store);

  REQUIRE(protocol.OnTransportConnected(1U).ok());
  REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());
  REQUIRE(protocol.session().Snapshot().next_in_seq == 2U);

  // SequenceReset-Reset (no GapFillFlag) advances expected inbound seq
  nimble::message::MessageBuilder reset_builder("4");
  reset_builder.set_string(kMsgType, "4").set_int(kNewSeqNo, 10);
  auto inbound =
    EncodeInboundFrame(std::move(reset_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 2U, false);
  REQUIRE(inbound.ok());

  auto event = protocol.OnInbound(inbound.value(), 10U);
  REQUIRE(event.ok());
  REQUIRE(event.value().outbound_frames.empty());
  REQUIRE(!event.value().disconnect);

  const auto after = protocol.session().Snapshot();
  REQUIRE(after.next_in_seq == 10U);
  REQUIRE(after.state == nimble::session::SessionState::kActive);
}

TEST_CASE("SequenceReset-Reset accepts stale reset frames", "[admin-protocol][reset-seq]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }

  nimble::store::MemorySessionStore store;
  nimble::session::AdminProtocol protocol(
    nimble::session::AdminProtocolConfig{
      .session =
        nimble::session::SessionConfig{
          .session_id = 7017U,
          .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
          .profile_id = dictionary.value().profile().header().profile_id,
          .heartbeat_interval_seconds = 30U,
          .is_initiator = false,
        },
      .begin_string = "FIX.4.4",
      .sender_comp_id = "SELL",
      .target_comp_id = "BUY",
      .heartbeat_interval_seconds = 30U,
    },
    dictionary.value(),
    &store);

  REQUIRE(protocol.OnTransportConnected(1U).ok());
  REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

  nimble::message::MessageBuilder gap_fill_builder("4");
  gap_fill_builder.set_string(kMsgType, "4").set_boolean(kGapFillFlag, true).set_int(kNewSeqNo, 5);
  auto gap_fill =
    EncodeInboundFrame(std::move(gap_fill_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 2U, false);
  REQUIRE(gap_fill.ok());
  REQUIRE(protocol.OnInbound(gap_fill.value(), 10U).ok());
  REQUIRE(protocol.session().Snapshot().next_in_seq == 5U);

  nimble::message::MessageBuilder reset_builder("4");
  reset_builder.set_string(kMsgType, "4").set_int(kNewSeqNo, 8);
  auto stale_reset =
    EncodeInboundFrame(std::move(reset_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 3U, false);
  REQUIRE(stale_reset.ok());

  auto event = protocol.OnInbound(stale_reset.value(), 20U);
  REQUIRE(event.ok());
  REQUIRE(event.value().outbound_frames.empty());
  REQUIRE(!event.value().disconnect);
  REQUIRE(protocol.session().Snapshot().next_in_seq == 8U);
}

TEST_CASE("SequenceReset backward rejected", "[admin-protocol]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }

  nimble::store::MemorySessionStore store;
  nimble::session::AdminProtocol protocol(
    nimble::session::AdminProtocolConfig{
      .session =
        nimble::session::SessionConfig{
          .session_id = 7008U,
          .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
          .profile_id = dictionary.value().profile().header().profile_id,
          .heartbeat_interval_seconds = 30U,
          .is_initiator = false,
        },
      .begin_string = "FIX.4.4",
      .sender_comp_id = "SELL",
      .target_comp_id = "BUY",
      .heartbeat_interval_seconds = 30U,
    },
    dictionary.value(),
    &store);

  REQUIRE(protocol.OnTransportConnected(1U).ok());
  REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

  // NewSeqNo=1 is backward (next_in_seq will be 3 after ObserveInboundSeq(2))
  nimble::message::MessageBuilder reset_builder("4");
  reset_builder.set_string(kMsgType, "4").set_int(kNewSeqNo, 1);
  auto inbound =
    EncodeInboundFrame(std::move(reset_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 2U, false);
  REQUIRE(inbound.ok());

  auto event = protocol.OnInbound(inbound.value(), 10U);
  REQUIRE(event.ok());
  REQUIRE(event.value().outbound_frames.size() == 1U);
  REQUIRE(!event.value().disconnect);

  auto decoded = nimble::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
  REQUIRE(decoded.ok());
  REQUIRE(decoded.value().header.msg_type == "3");
  REQUIRE(decoded.value().message.view().get_int(kRefTagID).value() == kNewSeqNo);
  REQUIRE(decoded.value().message.view().get_string(kRefMsgType).value() == "4");
  REQUIRE(decoded.value().message.view().get_int(kRejectReason).value() == 5);
}

TEST_CASE("SequenceReset-GapFill happy path", "[admin-protocol]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }

  nimble::store::MemorySessionStore store;
  nimble::session::AdminProtocol protocol(
    nimble::session::AdminProtocolConfig{
      .session =
        nimble::session::SessionConfig{
          .session_id = 7009U,
          .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
          .profile_id = dictionary.value().profile().header().profile_id,
          .heartbeat_interval_seconds = 30U,
          .is_initiator = false,
        },
      .begin_string = "FIX.4.4",
      .sender_comp_id = "SELL",
      .target_comp_id = "BUY",
      .heartbeat_interval_seconds = 30U,
    },
    dictionary.value(),
    &store);

  REQUIRE(protocol.OnTransportConnected(1U).ok());
  REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());
  REQUIRE(protocol.session().Snapshot().next_in_seq == 2U);

  // GapFill at expected seq advances inbound expected seq past the filled range
  nimble::message::MessageBuilder gap_fill_builder("4");
  gap_fill_builder.set_string(kMsgType, "4").set_boolean(kGapFillFlag, true).set_int(kNewSeqNo, 5);
  auto inbound =
    EncodeInboundFrame(std::move(gap_fill_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 2U, false);
  REQUIRE(inbound.ok());

  auto event = protocol.OnInbound(inbound.value(), 10U);
  REQUIRE(event.ok());
  REQUIRE(event.value().outbound_frames.empty());
  REQUIRE(!event.value().disconnect);

  const auto after = protocol.session().Snapshot();
  REQUIRE(after.next_in_seq == 5U);
  REQUIRE(after.state == nimble::session::SessionState::kActive);
}

TEST_CASE("SequenceReset-GapFill partial fill", "[admin-protocol]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }

  nimble::store::MemorySessionStore store;
  nimble::session::AdminProtocol protocol(
    nimble::session::AdminProtocolConfig{
      .session =
        nimble::session::SessionConfig{
          .session_id = 7010U,
          .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
          .profile_id = dictionary.value().profile().header().profile_id,
          .heartbeat_interval_seconds = 30U,
          .is_initiator = false,
        },
      .begin_string = "FIX.4.4",
      .sender_comp_id = "SELL",
      .target_comp_id = "BUY",
      .heartbeat_interval_seconds = 30U,
    },
    dictionary.value(),
    &store);

  REQUIRE(protocol.OnTransportConnected(1U).ok());
  REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

  // Create a gap: receive heartbeat at seq 5 when expected is 2
  nimble::message::MessageBuilder heartbeat_builder("0");
  heartbeat_builder.set_string(kMsgType, "0");
  auto gap_inbound =
    EncodeInboundFrame(std::move(heartbeat_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 5U, false);
  REQUIRE(gap_inbound.ok());

  auto gap_event = protocol.OnInbound(gap_inbound.value(), 10U);
  REQUIRE(gap_event.ok());
  REQUIRE(gap_event.value().outbound_frames.size() == 1U);

  auto resend_req =
    nimble::codec::DecodeFixMessage(gap_event.value().outbound_frames.front().bytes, dictionary.value());
  REQUIRE(resend_req.ok());
  REQUIRE(resend_req.value().header.msg_type == "2");
  REQUIRE(resend_req.value().message.view().get_int(kBeginSeqNo).value() == 2);
  REQUIRE(resend_req.value().message.view().get_int(kEndSeqNo).value() == 4);

  // Partial GapFill: covers seqs 2-3 only (NewSeqNo=4), seq 4 still missing
  nimble::message::MessageBuilder gap_fill_builder("4");
  gap_fill_builder.set_string(kMsgType, "4").set_boolean(kGapFillFlag, true).set_int(kNewSeqNo, 4);
  auto gap_fill_inbound =
    EncodeInboundFrame(std::move(gap_fill_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 2U, false);
  REQUIRE(gap_fill_inbound.ok());

  auto fill_event = protocol.OnInbound(gap_fill_inbound.value(), 20U);
  REQUIRE(fill_event.ok());
  REQUIRE(fill_event.value().outbound_frames.empty());
  REQUIRE(!fill_event.value().disconnect);

  // Gap not fully resolved — session stays in ResendProcessing
  const auto after = protocol.session().Snapshot();
  REQUIRE(after.state == nimble::session::SessionState::kResendProcessing);
  REQUIRE(after.has_pending_resend);
  REQUIRE(after.next_in_seq == 4U);
}

// ==================== Reject Tests ====================

TEST_CASE("Reject received processed silently", "[admin-protocol]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }

  nimble::store::MemorySessionStore store;
  nimble::session::AdminProtocol protocol(
    nimble::session::AdminProtocolConfig{
      .session =
        nimble::session::SessionConfig{
          .session_id = 7011U,
          .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
          .profile_id = dictionary.value().profile().header().profile_id,
          .heartbeat_interval_seconds = 30U,
          .is_initiator = false,
        },
      .begin_string = "FIX.4.4",
      .sender_comp_id = "SELL",
      .target_comp_id = "BUY",
      .heartbeat_interval_seconds = 30U,
    },
    dictionary.value(),
    &store);

  REQUIRE(protocol.OnTransportConnected(1U).ok());
  REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());
  REQUIRE(protocol.session().Snapshot().next_in_seq == 2U);

  // Receive inbound Reject — protocol processes it silently
  nimble::message::MessageBuilder reject_builder("3");
  reject_builder.set_string(kMsgType, "3").set_int(kRefSeqNum, 1).set_string(kText, "invalid message");
  auto inbound =
    EncodeInboundFrame(std::move(reject_builder).build(), dictionary.value(), "FIX.4.4", "BUY", "SELL", 2U, false);
  REQUIRE(inbound.ok());

  auto event = protocol.OnInbound(inbound.value(), 10U);
  REQUIRE(event.ok());
  REQUIRE(event.value().outbound_frames.empty());
  REQUIRE(!event.value().disconnect);
  // Reject is delivered to the application layer via OnReject callback (17.3)
  REQUIRE(event.value().application_messages.size() == 1U);

  const auto after = protocol.session().Snapshot();
  REQUIRE(after.next_in_seq == 3U);
}

TEST_CASE("admin protocol validation policy controls unknown fields", "[admin-protocol][validation-policy]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }

  const auto inbound = InboundApplicationFrame("35=D|34=2|49=BUY|56=SELL|52=20260402-12:00:00.000|11=ORD1|55="
                                               "AAPL|54=1|60=20260402-12:00:00.000|40=1|9999=BAD|");

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      MakeAcceptorProtocolConfig(6101U, dictionary.value(), nimble::session::ValidationPolicy::Strict()),
      dictionary.value(),
      &store);
    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    auto event = protocol.OnInbound(inbound, 10U);
    REQUIRE(event.ok());
    RequireSessionReject(dictionary.value(), event.value(), 9999U);
  }

  {
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      MakeAcceptorProtocolConfig(6102U, dictionary.value(), nimble::session::ValidationPolicy::Compatible()),
      dictionary.value(),
      &store);
    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    auto event = protocol.OnInbound(inbound, 10U);
    REQUIRE(event.ok());
    RequireAcceptedApplication(std::move(event).value());
  }
}

TEST_CASE("admin protocol validation callback handles log-and-process unknown fields",
          "[admin-protocol][validation-policy]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }

  auto policy = nimble::session::ValidationPolicy::Strict();
  policy.unknown_field_action = nimble::session::UnknownFieldAction::kLogAndProcess;
  auto callback = std::make_shared<RecordingValidationCallback>();

  nimble::store::MemorySessionStore store;
  nimble::session::AdminProtocol protocol(
    MakeAcceptorProtocolConfig(6103U, dictionary.value(), policy, callback), dictionary.value(), &store);
  REQUIRE(protocol.OnTransportConnected(1U).ok());
  REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

  const auto inbound = InboundApplicationFrame("35=D|34=2|49=BUY|56=SELL|52=20260402-12:00:00.000|11=ORD1|55="
                                               "AAPL|54=1|60=20260402-12:00:00.000|40=1|9999=BAD|");
  auto event = protocol.OnInbound(inbound, 10U);
  REQUIRE(event.ok());
  RequireAcceptedApplication(std::move(event).value());
  REQUIRE(callback->unknown_count == 1U);
  REQUIRE(callback->warning_count == 1U);
  REQUIRE(callback->last_session_id == 6103U);
  REQUIRE(callback->last_tag == 9999U);
  REQUIRE(callback->last_value == "BAD");
  REQUIRE(callback->last_msg_type == "D");
}

TEST_CASE("admin protocol validation policy controls malformed fields", "[admin-protocol][validation-policy]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }

  const auto inbound = InboundApplicationFrame("35=D|34=2|49=BUY|56=SELL|52=20260402-12:00:00.000|11=ORD1|55="
                                               "AAPL|54=X|60=20260402-12:00:00.000|40=1|");

  {
    auto policy = nimble::session::ValidationPolicy::Permissive();
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      MakeAcceptorProtocolConfig(6104U, dictionary.value(), policy), dictionary.value(), &store);
    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    auto event = protocol.OnInbound(inbound, 10U);
    REQUIRE(event.ok());
    RequireAcceptedApplication(std::move(event).value());
  }

  {
    auto policy = nimble::session::ValidationPolicy::Strict();
    policy.malformed_field_action = nimble::session::MalformedFieldAction::kLog;
    auto callback = std::make_shared<RecordingValidationCallback>();
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      MakeAcceptorProtocolConfig(6105U, dictionary.value(), policy, callback), dictionary.value(), &store);
    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    auto event = protocol.OnInbound(inbound, 10U);
    REQUIRE(event.ok());
    RequireAcceptedApplication(std::move(event).value());
    REQUIRE(callback->malformed_count == 1U);
    REQUIRE(callback->warning_count == 1U);
    REQUIRE(callback->last_session_id == 6105U);
    REQUIRE(callback->last_tag == kSide);
    REQUIRE(callback->last_value == "X");
    REQUIRE(callback->last_msg_type == "D");
  }
}

TEST_CASE("admin protocol validation policy controls enum validation", "[admin-protocol][validation-policy]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }

  const auto inbound = InboundApplicationFrame("35=D|34=2|49=BUY|56=SELL|52=20260402-12:00:00.000|11=ORD1|55="
                                               "AAPL|54=Z|60=20260402-12:00:00.000|40=1|");

  {
    auto policy = nimble::session::ValidationPolicy::Strict();
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      MakeAcceptorProtocolConfig(6106U, dictionary.value(), policy), dictionary.value(), &store);
    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    auto event = protocol.OnInbound(inbound, 10U);
    REQUIRE(event.ok());
    RequireSessionReject(dictionary.value(), event.value(), kSide);
  }

  {
    auto policy = nimble::session::ValidationPolicy::Strict();
    policy.validate_enum_values = false;
    nimble::store::MemorySessionStore store;
    nimble::session::AdminProtocol protocol(
      MakeAcceptorProtocolConfig(6107U, dictionary.value(), policy), dictionary.value(), &store);
    REQUIRE(protocol.OnTransportConnected(1U).ok());
    REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

    auto event = protocol.OnInbound(inbound, 10U);
    REQUIRE(event.ok());
    RequireAcceptedApplication(std::move(event).value());
  }
}
