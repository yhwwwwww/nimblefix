#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <thread>

#include "fastfix/profile/artifact_builder.h"
#include "fastfix/profile/normalized_dictionary.h"
#include "fastfix/profile/profile_loader.h"
#include "fastfix/session/admin_protocol.h"
#include "fastfix/store/memory_store.h"
#include "fastfix/transport/tcp_transport.h"

#include "test_support.h"

namespace {

auto NowNs() -> std::uint64_t {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch())
                                              .count());
}

auto LoadLoopbackDictionary() -> fastfix::base::Result<fastfix::profile::NormalizedDictionaryView> {
    fastfix::profile::NormalizedDictionary dictionary;
    dictionary.profile_id = 8001U;
    dictionary.schema_hash = 0x8001800180018001ULL;
    dictionary.fields = {
        {35U, "MsgType", fastfix::profile::ValueType::kString, 0U},
        {49U, "SenderCompID", fastfix::profile::ValueType::kString, 0U},
        {56U, "TargetCompID", fastfix::profile::ValueType::kString, 0U},
        {453U, "NoPartyIDs", fastfix::profile::ValueType::kInt, 0U},
        {448U, "PartyID", fastfix::profile::ValueType::kString, 0U},
        {447U, "PartyIDSource", fastfix::profile::ValueType::kChar, 0U},
        {452U, "PartyRole", fastfix::profile::ValueType::kInt, 0U},
    };
    dictionary.messages = {
        fastfix::profile::MessageDef{
            .msg_type = "D",
            .name = "NewOrderSingle",
            .field_rules = {
                {35U, static_cast<std::uint32_t>(fastfix::profile::FieldRuleFlags::kRequired)},
                {49U, static_cast<std::uint32_t>(fastfix::profile::FieldRuleFlags::kRequired)},
                {56U, static_cast<std::uint32_t>(fastfix::profile::FieldRuleFlags::kRequired)},
                {453U, 0U},
            },
            .flags = 0U,
        },
    };
    dictionary.groups = {
        fastfix::profile::GroupDef{
            .count_tag = 453U,
            .delimiter_tag = 448U,
            .name = "Parties",
            .field_rules = {
                {448U, static_cast<std::uint32_t>(fastfix::profile::FieldRuleFlags::kRequired)},
                {447U, static_cast<std::uint32_t>(fastfix::profile::FieldRuleFlags::kRequired)},
                {452U, static_cast<std::uint32_t>(fastfix::profile::FieldRuleFlags::kRequired)},
            },
            .flags = 0U,
        },
    };

    auto artifact = fastfix::profile::BuildProfileArtifact(dictionary);
    if (!artifact.ok()) {
        return artifact.status();
    }
    const auto artifact_path = std::filesystem::temp_directory_path() / "fastfix-loopback-test.art";
    auto write_status = fastfix::profile::WriteProfileArtifact(artifact_path, artifact.value());
    if (!write_status.ok()) {
        return write_status;
    }
    auto loaded = fastfix::profile::LoadProfileArtifact(artifact_path);
    std::filesystem::remove(artifact_path);
    if (!loaded.ok()) {
        return loaded.status();
    }
    return fastfix::profile::NormalizedDictionaryView::FromProfile(std::move(loaded).value());
}

}  // namespace

TEST_CASE("socket-loopback", "[socket-loopback]") {
    auto dictionary = LoadLoopbackDictionary();
    REQUIRE(dictionary.ok());

    auto acceptor = fastfix::transport::TcpAcceptor::Listen("127.0.0.1", 0U);
    REQUIRE(acceptor.ok());
    const auto listen_port = acceptor.value().port();

    std::promise<fastfix::base::Status> acceptor_result;
    auto acceptor_future = acceptor_result.get_future();

    std::jthread acceptor_thread([
        acceptor_socket = std::move(acceptor).value(),
        &acceptor_result,
        &dictionary]() mutable {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 2001U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
                    .profile_id = dictionary.value().profile().header().profile_id,
                    .heartbeat_interval_seconds = 1U,
                    .is_initiator = false,
                },
                .begin_string = "FIX.4.4",
                .sender_comp_id = "SELL",
                .target_comp_id = "BUY",
                .heartbeat_interval_seconds = 1U,
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
                acceptor_result.set_value(fastfix::base::Status::Ok());
                return;
            }
        }
    });

    auto connection = fastfix::transport::TcpConnection::Connect(
        "127.0.0.1",
        listen_port,
        std::chrono::seconds(5));
    REQUIRE(connection.ok());

    fastfix::store::MemorySessionStore initiator_store;
    fastfix::session::AdminProtocol initiator(
        fastfix::session::AdminProtocolConfig{
            .session = fastfix::session::SessionConfig{
                .session_id = 1001U,
                .key = fastfix::session::SessionKey{"FIX.4.4", "BUY", "SELL"},
                .profile_id = dictionary.value().profile().header().profile_id,
                .heartbeat_interval_seconds = 1U,
                .is_initiator = true,
            },
            .begin_string = "FIX.4.4",
            .sender_comp_id = "BUY",
            .target_comp_id = "SELL",
            .heartbeat_interval_seconds = 1U,
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
    while (!received_echo || initiator.session().state() != fastfix::session::SessionState::kAwaitingLogout) {
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
            fastfix::message::MessageBuilder builder("D");
            builder.set_string(35U, "D");
            auto party = builder.add_group_entry(453U);
            party.set_string(448U, "PARTY-A").set_char(447U, 'D').set_int(452U, 3);
            auto outbound = initiator.SendApplication(std::move(builder).build(), NowNs());
            REQUIRE(outbound.ok());
            REQUIRE(connection.value().Send(outbound.value().bytes, std::chrono::seconds(5)).ok());
            sent_app = true;
            continue;
        }

        if (!event.value().application_messages.empty()) {
            const auto group = event.value().application_messages.front().view().group(453U);
            REQUIRE(group.has_value());
            REQUIRE(group->size() == 1U);
            REQUIRE((*group)[0].get_string(448U).value() == "PARTY-A");
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