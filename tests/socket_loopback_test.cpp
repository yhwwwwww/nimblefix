#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include "nimblefix/advanced/message_data_writer.h"
#include "nimblefix/codec/fix_tags.h"
#include "nimblefix/session/admin_protocol.h"
#include "nimblefix/store/memory_store.h"
#include "nimblefix/transport/tcp_transport.h"
#include "nimblefix/transport/transport_connection.h"

#include "test_support.h"

namespace {

auto
NowNs() -> std::uint64_t
{
  return static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

#if defined(NIMBLEFIX_ENABLE_TLS)

constexpr std::string_view kLoopbackCertificatePem = R"pem(-----BEGIN CERTIFICATE-----
MIIDJTCCAg2gAwIBAgIUGwWSXUiHLnMRWUC3m/2sbanswl4wDQYJKoZIhvcNAQEL
BQAwFDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI2MDQyNTEyNTY0MVoXDTM2MDQy
MjEyNTY0MVowFDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEF
AAOCAQ8AMIIBCgKCAQEAqvWtY62HfdTq4zU2jzfi5nDsz7QK92wjT9rgzSrXM2lR
bnZpvpne3CT1SDgkENlXB+S53U4nUuGCbdI2opjtN9mRsCFYCKWMjTOHxpuJmPcj
hMs8AjcGPbwy48cY16LcjeNB8Ozn+DLATBrdv74aGFQOQ1cuTKP/4wuz20r3LsSv
titKLQzQ7jEGyP/cFebcq4sENkrM+KkZhExxu+gujo4em/PG3ZIOiH6XU0iRogru
XoPb/2VNLAUWxeM6tcgLJxBhnmnOfoGc0Nk1nV5AVsUlqGah6twTFZe1G0xA127h
90auXBgvodCGaEozZ6xiSEA18VkpJz+ohN3DNzZ82QIDAQABo28wbTAdBgNVHQ4E
FgQUH8XGUDn0aBbzLXqiE3bxpwZfSbwwHwYDVR0jBBgwFoAUH8XGUDn0aBbzLXqi
E3bxpwZfSbwwDwYDVR0TAQH/BAUwAwEB/zAaBgNVHREEEzARgglsb2NhbGhvc3SH
BH8AAAEwDQYJKoZIhvcNAQELBQADggEBADRGMgrWPZf4Qx2CwYVBof+8X5yzIgvA
ziMr/iVpaFlyEjYzJpkvT7cNhNlgYvP29pZE+KRNN7bnI1Yp4Ej27NN6Xtdb5LwL
rh6Omgs1frki0tbRu0ZGB2f+Q1Fl9Y74Vl2pg+Gp+sdwdMYQW3XHvxBeYAHaZem3
NB84tzZdYUCpuLW07DjhMAGPcW4wmBB7c7fblZ7YQarOyawc+PR7asvdaXX9/5lZ
hDjQIrU1AqKUM2Kc6XV3xkAuNHQh7RWOU8kQOFzNXDbURXr3TOgqRHx0DgRjp2uj
tLs1SgfuYPa6qCLBL0YigkCW1TGtFkw4Wotfg3lhHoZmNvIE01aWNxY=
-----END CERTIFICATE-----
)pem";

constexpr std::string_view kLoopbackPrivateKeyPem = R"pem(-----BEGIN PRIVATE KEY-----
MIIEvwIBADANBgkqhkiG9w0BAQEFAASCBKkwggSlAgEAAoIBAQCq9a1jrYd91Orj
NTaPN+LmcOzPtAr3bCNP2uDNKtczaVFudmm+md7cJPVIOCQQ2VcH5LndTidS4YJt
0jaimO032ZGwIVgIpYyNM4fGm4mY9yOEyzwCNwY9vDLjxxjXotyN40Hw7Of4MsBM
Gt2/vhoYVA5DVy5Mo//jC7PbSvcuxK+2K0otDNDuMQbI/9wV5tyriwQ2Ssz4qRmE
THG76C6Ojh6b88bdkg6IfpdTSJGiCu5eg9v/ZU0sBRbF4zq1yAsnEGGeac5+gZzQ
2TWdXkBWxSWoZqHq3BMVl7UbTEDXbuH3Rq5cGC+h0IZoSjNnrGJIQDXxWSknP6iE
3cM3NnzZAgMBAAECggEAA+UG5y9r18UC+NwcexTF2YQXEQBEA7D1+Pq+hk4EiwpK
LZ8K96mftxoscFG/GJcq8WYXieAe6zdx9jiEwB2FwfD17bJExCWpVwomfLLMZqyy
pXLP0ikYvk1MR34gpcDzD1RvCyMKgc/+K32tMZIOHCHGFWimCF7wFGcO2N8TVIBi
+oqGG6h4z/pM4hQTT52BHpo6pBmulZ9jptDJSx4fqSi36ljEwoqAkf5kkysO/k4k
bxYqDe3M4oT498nbImpiqFkRjir/ZqVZIQK9GeqRppyd3kCDrjWA6GKrGcsY04x/
BIYaNbpVg3N6qU9uLp2B3WmKu213fvvSVr0v8cItAQKBgQDr8eAcmXfnvn8obRjt
HKkcBAYbKkiFtaVcaJ9KboaXVq+3NfgbHnsSYCTWppPX2KEYliBX+0/UVLJK3GiY
jmGCw7+v7uum6jzW8A90jFIczvT3Jkpm9NXEPVsGCSN/QhcD+qW0Ogy4icqBvaTn
qGIrNUnPUVet4Ll0GyoiHfce+QKBgQC5fbwQOXZPIcliJu0KHk9Jo1gdoXj8yv4I
hLXhMFTwrOpk0LCM1E6FauLUICp+6Uhxm1yiAbVPol1YCpoyT5spGR303Cab5RKS
YGHTeQNL9ziFzrC7b+fwNsm3vJA3RTaBZCyYxNhToxWaOMalwm34KNcrTX8CJITy
/ObH3ANk4QKBgQDB6L5cItDFl+zfZ5IVxPlCuhfemYiSwy+M27sWK/HXTPoKo4Mt
noZdGsEL3EkjGrmDAFbCmBsKkTUaizw3LMT8+C2AxOXM/zNTHmZFTdFqNbhjqod5
R/yrVBWLx1TyEHnj3kny7cZon23b5OUzMlLD2f64MMzTbR5dSrn84g2n2QKBgQCp
pxNr73KC+876A8pHt+Mi4dBFAZvr8imYVvEXLqJxomWbobfohoHuywz9oRHdE7bb
mZKG5nMTi9g+HyxbGa47T6qzeuuhKEntMVQoHAVk5I+A6sOAG+ESNroWX9OziY1J
mPqlG10UWhP3AzjFAOid0ZTDGVUx+37R03esklFUYQKBgQCdosNAxYnuzD1I57E3
rxNuQ47lauLbUUw2R8ZH0DzDeWrhS2Q2EyrlMhifsHmjP1eiL/OY4CHkMrPO1ynW
Tt/gYkSGSvq8sdoWFrOTe+X3ME4ZLe6j7BwIgs4LwZ6Rwms66bzI8eaML2/xKLdL
fufkaJmHWQJWWpyAoqse6sYsWA==
-----END PRIVATE KEY-----
)pem";

struct TlsFixturePaths
{
  std::filesystem::path root;
  std::filesystem::path certificate;
  std::filesystem::path private_key;
};

auto
WriteTextFile(const std::filesystem::path& path, std::string_view text) -> void
{
  std::ofstream out(path, std::ios::trunc);
  out << text;
}

auto
WriteTlsFixture(std::string_view name) -> TlsFixturePaths
{
  auto root = std::filesystem::temp_directory_path() / ("nimblefix-tls-loopback-" + std::string(name));
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  TlsFixturePaths paths{
    .root = root,
    .certificate = root / "loopback-cert.pem",
    .private_key = root / "loopback-key.pem",
  };
  WriteTextFile(paths.certificate, kLoopbackCertificatePem);
  WriteTextFile(paths.private_key, kLoopbackPrivateKeyPem);
  return paths;
}

auto
MakeTlsServerConfig(const TlsFixturePaths& paths) -> nimble::runtime::TlsServerConfig
{
  return nimble::runtime::TlsServerConfig{
    .enabled = true,
    .certificate_chain_file = paths.certificate,
    .private_key_file = paths.private_key,
    .min_version = nimble::runtime::TlsProtocolVersion::kTls12,
    .max_version = nimble::runtime::TlsProtocolVersion::kTls13,
  };
}

auto
MakeTlsClientConfig(const TlsFixturePaths& paths) -> nimble::runtime::TlsClientConfig
{
  return nimble::runtime::TlsClientConfig{
    .enabled = true,
    .server_name = "localhost",
    .expected_peer_name = "localhost",
    .ca_file = paths.certificate,
    .min_version = nimble::runtime::TlsProtocolVersion::kTls12,
    .max_version = nimble::runtime::TlsProtocolVersion::kTls13,
  };
}

auto
SendSplit(nimble::transport::TransportConnection& connection, std::span<const std::byte> bytes, bool zero_copy)
  -> nimble::base::Status
{
  const auto split = bytes.size() / 2U;
  std::array<std::span<const std::byte>, 2> segments{
    bytes.subspan(0U, split),
    bytes.subspan(split),
  };
  if (zero_copy) {
    return connection.SendZeroCopyGather(segments, std::chrono::seconds(5));
  }
  return connection.SendGather(segments, std::chrono::seconds(5));
}

#endif

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
      nimble::message::MessageDataWriter builder("D");
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

TEST_CASE("tls-transport-loopback", "[socket-loopback][tls]")
{
#if !defined(NIMBLEFIX_ENABLE_TLS)
  SKIP("optional TLS support is not compiled in this build");
#else
  auto dictionary = nimble::tests::LoadFix44DictionaryView();
  if (!dictionary.ok()) {
    SKIP("FIX44 artifact not available: " << dictionary.status().message());
  }

  auto fixture = WriteTlsFixture("success");
  const auto server_tls = MakeTlsServerConfig(fixture);
  const auto client_tls = MakeTlsClientConfig(fixture);

  auto acceptor = nimble::transport::TcpAcceptor::Listen("127.0.0.1", 0U);
  REQUIRE(acceptor.ok());
  const auto listen_port = acceptor.value().port();

  std::promise<nimble::base::Status> acceptor_result;
  auto acceptor_future = acceptor_result.get_future();

  std::jthread acceptor_thread(
    [acceptor_socket = std::move(acceptor).value(), &acceptor_result, &dictionary, server_tls]() mutable {
      nimble::store::MemorySessionStore store;
      nimble::session::AdminProtocol protocol(
        nimble::session::AdminProtocolConfig{
          .session =
            nimble::session::SessionConfig{
              .session_id = 2201U,
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
      auto connection_result = nimble::transport::TransportConnection::FromAcceptedTcp(
        std::move(inbound).value(), std::chrono::seconds(5), &server_tls);
      if (!connection_result.ok()) {
        acceptor_result.set_value(connection_result.status());
        return;
      }
      auto connection = std::move(connection_result).value();
      if (!connection.uses_tls() || connection.kind() != nimble::transport::TransportConnectionKind::kTlsServer) {
        acceptor_result.set_value(nimble::base::Status::InvalidArgument("accepted connection is not TLS"));
        return;
      }

      auto status = protocol.OnTransportConnected(NowNs());
      if (!status.ok()) {
        acceptor_result.set_value(status.status());
        return;
      }

      while (true) {
        auto frame = connection.ReceiveFrameView(std::chrono::seconds(5));
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
          auto send_status = SendSplit(connection, echo.value().bytes, true);
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

  auto connection =
    nimble::transport::TransportConnection::Connect("127.0.0.1", listen_port, std::chrono::seconds(5), &client_tls);
  REQUIRE(connection.ok());
  REQUIRE(connection.value().uses_tls());
  REQUIRE(connection.value().kind() == nimble::transport::TransportConnectionKind::kTlsClient);
  const auto session_info = connection.value().tls_session_info();
  REQUIRE(session_info.has_value());
  REQUIRE(!session_info->protocol.empty());
  REQUIRE(!session_info->cipher.empty());

  nimble::store::MemorySessionStore initiator_store;
  nimble::session::AdminProtocol initiator(
    nimble::session::AdminProtocolConfig{
      .session =
        nimble::session::SessionConfig{
          .session_id = 1201U,
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
    auto frame = connection.value().ReceiveFrameView(std::chrono::seconds(5));
    REQUIRE(frame.ok());

    auto event = initiator.OnInbound(frame.value(), NowNs());
    if (!event.ok()) {
      std::cerr << "TLS initiator inbound error: " << event.status().message() << '\n';
    }
    REQUIRE(event.ok());

    for (const auto& outbound : event.value().outbound_frames) {
      REQUIRE(connection.value().Send(outbound.bytes, std::chrono::seconds(5)).ok());
    }

    if (event.value().session_active && !sent_app) {
      nimble::message::MessageDataWriter builder("D");
      builder.set_string(nimble::codec::tags::kMsgType, "D");
      auto party = builder.add_group_entry(nimble::codec::tags::kNoPartyIDs);
      party.set_string(nimble::codec::tags::kPartyID, "TLS-PARTY")
        .set_char(nimble::codec::tags::kPartyIDSource, 'D')
        .set_int(nimble::codec::tags::kPartyRole, 3);
      auto outbound = initiator.SendApplication(std::move(builder).build(), NowNs());
      REQUIRE(outbound.ok());
      REQUIRE(SendSplit(connection.value(), outbound.value().bytes, false).ok());
      sent_app = true;
      continue;
    }

    if (!event.value().application_messages.empty()) {
      const auto group = event.value().application_messages.front().view().group(nimble::codec::tags::kNoPartyIDs);
      REQUIRE(group.has_value());
      REQUIRE(group->size() == 1U);
      REQUIRE((*group)[0].get_string(nimble::codec::tags::kPartyID).value() == "TLS-PARTY");
      received_echo = true;

      auto logout = initiator.BeginLogout({}, NowNs());
      REQUIRE(logout.ok());
      REQUIRE(SendSplit(connection.value(), logout.value().bytes, true).ok());
      continue;
    }
  }

  auto logout_ack = connection.value().ReceiveFrameView(std::chrono::seconds(5));
  REQUIRE(logout_ack.ok());
  auto logout_event = initiator.OnInbound(logout_ack.value(), NowNs());
  if (!logout_event.ok()) {
    std::cerr << "TLS initiator logout error: " << logout_event.status().message() << '\n';
  }
  REQUIRE(logout_event.ok());
  REQUIRE(logout_event.value().disconnect);
  initiator.OnTransportClosed();
  connection.value().Close();

  REQUIRE(acceptor_future.get().ok());
  std::filesystem::remove_all(fixture.root);
#endif
}

TEST_CASE("tls-transport-handshake-failures", "[socket-loopback][tls]")
{
#if !defined(NIMBLEFIX_ENABLE_TLS)
  SKIP("optional TLS support is not compiled in this build");
#else
  auto fixture = WriteTlsFixture("failures");

  auto run_tls_failure = [&](nimble::runtime::TlsServerConfig server_tls, nimble::runtime::TlsClientConfig client_tls) {
    auto acceptor = nimble::transport::TcpAcceptor::Listen("127.0.0.1", 0U);
    REQUIRE(acceptor.ok());
    const auto listen_port = acceptor.value().port();

    std::promise<nimble::base::Status> server_result;
    auto server_future = server_result.get_future();
    std::jthread server_thread([acceptor_socket = std::move(acceptor).value(), server_tls, &server_result]() mutable {
      auto inbound = acceptor_socket.Accept(std::chrono::seconds(5));
      if (!inbound.ok()) {
        server_result.set_value(inbound.status());
        return;
      }
      auto tls_connection = nimble::transport::TransportConnection::FromAcceptedTcp(
        std::move(inbound).value(), std::chrono::seconds(2), &server_tls);
      server_result.set_value(tls_connection.ok() ? nimble::base::Status::Ok() : tls_connection.status());
    });

    auto client =
      nimble::transport::TransportConnection::Connect("127.0.0.1", listen_port, std::chrono::seconds(2), &client_tls);
    const auto server_status = server_future.get();
    REQUIRE((!client.ok() || !server_status.ok()));
    if (client.ok()) {
      client.value().Close();
    }
  };

  SECTION("hostname mismatch")
  {
    auto server_tls = MakeTlsServerConfig(fixture);
    auto client_tls = MakeTlsClientConfig(fixture);
    client_tls.expected_peer_name = "wrong.example.test";
    run_tls_failure(std::move(server_tls), std::move(client_tls));
  }

  SECTION("missing client certificate for mTLS")
  {
    auto server_tls = MakeTlsServerConfig(fixture);
    server_tls.verify_peer = true;
    server_tls.require_client_certificate = true;
    server_tls.ca_file = fixture.certificate;
    auto client_tls = MakeTlsClientConfig(fixture);
    run_tls_failure(std::move(server_tls), std::move(client_tls));
  }

  SECTION("protocol version mismatch")
  {
    auto server_tls = MakeTlsServerConfig(fixture);
    server_tls.min_version = nimble::runtime::TlsProtocolVersion::kTls13;
    server_tls.max_version = nimble::runtime::TlsProtocolVersion::kTls13;
    auto client_tls = MakeTlsClientConfig(fixture);
    client_tls.min_version = nimble::runtime::TlsProtocolVersion::kTls12;
    client_tls.max_version = nimble::runtime::TlsProtocolVersion::kTls12;
    run_tls_failure(std::move(server_tls), std::move(client_tls));
  }

  SECTION("handshake timeout")
  {
    auto server_tls = MakeTlsServerConfig(fixture);
    auto acceptor = nimble::transport::TcpAcceptor::Listen("127.0.0.1", 0U);
    REQUIRE(acceptor.ok());
    const auto listen_port = acceptor.value().port();

    std::promise<nimble::base::Status> server_result;
    auto server_future = server_result.get_future();
    std::jthread server_thread([acceptor_socket = std::move(acceptor).value(), server_tls, &server_result]() mutable {
      auto inbound = acceptor_socket.Accept(std::chrono::seconds(5));
      if (!inbound.ok()) {
        server_result.set_value(inbound.status());
        return;
      }
      auto tls_connection = nimble::transport::TransportConnection::FromAcceptedTcp(
        std::move(inbound).value(), std::chrono::milliseconds(100), &server_tls);
      server_result.set_value(tls_connection.ok() ? nimble::base::Status::Ok() : tls_connection.status());
    });

    auto plain_client = nimble::transport::TcpConnection::Connect("127.0.0.1", listen_port, std::chrono::seconds(2));
    REQUIRE(plain_client.ok());
    const auto server_status = server_future.get();
    REQUIRE(!server_status.ok());
    REQUIRE(server_status.message().find("timed out") != std::string_view::npos);
    plain_client.value().Close();
  }

  std::filesystem::remove_all(fixture.root);
#endif
}
