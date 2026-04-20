#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <future>
#include <iostream>
#include <thread>

#include "nimblefix/codec/fix_tags.h"
#include "nimblefix/session/admin_protocol.h"
#include "nimblefix/store/memory_store.h"
#include "nimblefix/transport/tcp_transport.h"

#include "test_support.h"

namespace {

auto
NowNs() -> std::uint64_t
{
  return static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

} // namespace

TEST_CASE("socket-loopback", "[socket-loopback]")
{
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }

  auto acceptor = nimble::transport::TcpAcceptor::Listen("127.0.0.1", 0U);
  REQUIRE(acceptor.ok());
  const auto listen_port = acceptor.value().port();

  std::promise<nimble::base::Status> acceptor_result;
  auto acceptor_future = acceptor_result.get_future();

  std::jthread acceptor_thread(
    [acceptor_socket = std::move(acceptor).value(), &acceptor_result, &dictionary]() mutable {
      nimble::store::MemorySessionStore store;
      nimble::session::AdminProtocol protocol(
        nimble::session::AdminProtocolConfig{
          .session =
            nimble::session::SessionConfig{
              .session_id = 2001U,
              .key = nimble::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
              .profile_id = dictionary.value().profile().header().profile_id,
              .heartbeat_interval_seconds = 1U,
              .is_initiator = false,
            },
          .begin_string = "FIX.4.4",
          .sender_comp_id = "SELL",
          .target_comp_id = "BUY",
          .heartbeat_interval_seconds = 1U,
          .validation_policy = nimble::session::ValidationPolicy::Permissive(),
        },
        dictionary.value(),
        &store);

      auto inbound = acceptor_socket.Accept(std::chrono::seconds(5));
      if (!inbound.ok()) {
        acceptor_result.set_value(inbound.status());
        return;
      }

      auto status = protocol.OnTransportConnected(NowNs());
      if (!status.ok()) {
        acceptor_result.set_value(status.status());
        return;
      }

      auto connection = std::move(inbound).value();
      while (true) {
        auto frame = connection.ReceiveFrame(std::chrono::seconds(5));
        if (!frame.ok()) {
          acceptor_result.set_value(frame.status());
          return;
        }

        auto event = protocol.OnInbound(frame.value(), NowNs());
        if (!event.ok()) {
          acceptor_result.set_value(event.status());
          return;
        }

        for (const auto& outbound : event.value().outbound_frames) {
          auto send_status = connection.Send(outbound.bytes, std::chrono::seconds(5));
          if (!send_status.ok()) {
            acceptor_result.set_value(send_status);
            return;
          }
        }

        for (const auto& app_message : event.value().application_messages) {
          auto echo = protocol.SendApplication(app_message, NowNs());
          if (!echo.ok()) {
            acceptor_result.set_value(echo.status());
            return;
          }
          auto send_status = connection.Send(echo.value().bytes, std::chrono::seconds(5));
          if (!send_status.ok()) {
            acceptor_result.set_value(send_status);
            return;
          }
        }

        if (event.value().disconnect) {
          protocol.OnTransportClosed();
          connection.Close();
          acceptor_result.set_value(nimble::base::Status::Ok());
          return;
        }
      }
    });

  auto connection = nimble::transport::TcpConnection::Connect("127.0.0.1", listen_port, std::chrono::seconds(5));
  REQUIRE(connection.ok());

  nimble::store::MemorySessionStore initiator_store;
  nimble::session::AdminProtocol initiator(
    nimble::session::AdminProtocolConfig{
      .session =
        nimble::session::SessionConfig{
          .session_id = 1001U,
          .key = nimble::session::SessionKey{ "FIX.4.4", "BUY", "SELL" },
          .profile_id = dictionary.value().profile().header().profile_id,
          .heartbeat_interval_seconds = 1U,
          .is_initiator = true,
        },
      .begin_string = "FIX.4.4",
      .sender_comp_id = "BUY",
      .target_comp_id = "SELL",
      .heartbeat_interval_seconds = 1U,
      .validation_policy = nimble::session::ValidationPolicy::Permissive(),
    },
    dictionary.value(),
    &initiator_store);

  auto start = initiator.OnTransportConnected(NowNs());
  REQUIRE(start.ok());
  for (const auto& outbound : start.value().outbound_frames) {
    REQUIRE(connection.value().Send(outbound.bytes, std::chrono::seconds(5)).ok());
  }

  bool sent_app = false;
  bool received_echo = false;
  while (!received_echo || initiator.session().state() != nimble::session::SessionState::kAwaitingLogout) {
    auto frame = connection.value().ReceiveFrame(std::chrono::seconds(5));
    REQUIRE(frame.ok());

    auto event = initiator.OnInbound(frame.value(), NowNs());
    if (!event.ok()) {
      std::cerr << "initiator inbound error: " << event.status().message() << '\n';
    }
    REQUIRE(event.ok());

    for (const auto& outbound : event.value().outbound_frames) {
      REQUIRE(connection.value().Send(outbound.bytes, std::chrono::seconds(5)).ok());
    }

    if (event.value().session_active && !sent_app) {
      nimble::message::MessageBuilder builder("D");
      builder.set_string(nimble::codec::tags::kMsgType, "D");
      auto party = builder.add_group_entry(nimble::codec::tags::kNoPartyIDs);
      party.set_string(nimble::codec::tags::kPartyID, "PARTY-A")
        .set_char(nimble::codec::tags::kPartyIDSource, 'D')
        .set_int(nimble::codec::tags::kPartyRole, 3);
      auto outbound = initiator.SendApplication(std::move(builder).build(), NowNs());
      REQUIRE(outbound.ok());
      REQUIRE(connection.value().Send(outbound.value().bytes, std::chrono::seconds(5)).ok());
      sent_app = true;
      continue;
    }

    if (!event.value().application_messages.empty()) {
      const auto group = event.value().application_messages.front().view().group(nimble::codec::tags::kNoPartyIDs);
      REQUIRE(group.has_value());
      REQUIRE(group->size() == 1U);
      REQUIRE((*group)[0].get_string(nimble::codec::tags::kPartyID).value() == "PARTY-A");
      received_echo = true;

      auto logout = initiator.BeginLogout({}, NowNs());
      REQUIRE(logout.ok());
      REQUIRE(connection.value().Send(logout.value().bytes, std::chrono::seconds(5)).ok());
      continue;
    }
  }

  auto logout_ack = connection.value().ReceiveFrame(std::chrono::seconds(5));
  REQUIRE(logout_ack.ok());
  auto logout_event = initiator.OnInbound(logout_ack.value(), NowNs());
  if (!logout_event.ok()) {
    std::cerr << "initiator logout error: " << logout_event.status().message() << '\n';
  }
  REQUIRE(logout_event.ok());
  REQUIRE(logout_event.value().disconnect);
  initiator.OnTransportClosed();
  connection.value().Close();

  REQUIRE(acceptor_future.get().ok());
}