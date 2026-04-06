#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include "fastfix/codec/fix_codec.h"
#include "fastfix/profile/artifact_builder.h"
#include "fastfix/profile/profile_loader.h"
#include "fastfix/session/admin_protocol.h"
#include "fastfix/store/memory_store.h"

#include "test_support.h"

namespace {

auto LoadAdminDictionary() -> fastfix::base::Result<fastfix::profile::NormalizedDictionaryView> {
    fastfix::profile::NormalizedDictionary dictionary;
    dictionary.profile_id = 7301U;
    dictionary.schema_hash = 0x7301730173017301ULL;
    dictionary.fields = {
        {35U, "MsgType", fastfix::profile::ValueType::kString, 0U},
        {447U, "PartyIDSource", fastfix::profile::ValueType::kChar, 0U},
        {448U, "PartyID", fastfix::profile::ValueType::kString, 0U},
        {43U, "PossDupFlag", fastfix::profile::ValueType::kBoolean, 0U},
        {97U, "PossResend", fastfix::profile::ValueType::kBoolean, 0U},
        {45U, "RefSeqNum", fastfix::profile::ValueType::kInt, 0U},
        {49U, "SenderCompID", fastfix::profile::ValueType::kString, 0U},
        {52U, "SendingTime", fastfix::profile::ValueType::kTimestamp, 0U},
        {55U, "Symbol", fastfix::profile::ValueType::kString, 0U},
        {56U, "TargetCompID", fastfix::profile::ValueType::kString, 0U},
        {58U, "Text", fastfix::profile::ValueType::kString, 0U},
        {98U, "EncryptMethod", fastfix::profile::ValueType::kInt, 0U},
        {108U, "HeartBtInt", fastfix::profile::ValueType::kInt, 0U},
        {1137U, "DefaultApplVerID", fastfix::profile::ValueType::kString, 0U},
        {122U, "OrigSendingTime", fastfix::profile::ValueType::kTimestamp, 0U},
        {141U, "ResetSeqNumFlag", fastfix::profile::ValueType::kBoolean, 0U},
        {452U, "PartyRole", fastfix::profile::ValueType::kInt, 0U},
        {453U, "NoPartyIDs", fastfix::profile::ValueType::kInt, 0U},
        {371U, "RefTagID", fastfix::profile::ValueType::kInt, 0U},
        {372U, "RefMsgType", fastfix::profile::ValueType::kString, 0U},
        {373U, "SessionRejectReason", fastfix::profile::ValueType::kInt, 0U},
    };
    dictionary.messages = {
        fastfix::profile::MessageDef{.msg_type = "0", .name = "Heartbeat", .field_rules = {}, .flags = 0U},
        fastfix::profile::MessageDef{.msg_type = "2", .name = "ResendRequest", .field_rules = {}, .flags = 0U},
        fastfix::profile::MessageDef{.msg_type = "3", .name = "Reject", .field_rules = {}, .flags = 0U},
        fastfix::profile::MessageDef{.msg_type = "A", .name = "Logon", .field_rules = {}, .flags = 0U},
        fastfix::profile::MessageDef{
            .msg_type = "D",
            .name = "NewOrderSingle",
            .field_rules = {
                {35U, static_cast<std::uint32_t>(fastfix::profile::FieldRuleFlags::kRequired)},
                {55U, static_cast<std::uint32_t>(fastfix::profile::FieldRuleFlags::kRequired)},
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

    const auto artifact_path = std::filesystem::temp_directory_path() / "fastfix-admin-protocol-test.art";
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

auto EncodeInboundFrame(
    const fastfix::message::Message& message,
    const fastfix::profile::NormalizedDictionaryView& dictionary,
    std::string begin_string,
    std::string sender,
    std::string target,
    std::uint32_t seq_num,
    bool poss_dup,
    std::string default_appl_ver_id = {},
    std::string orig_sending_time = {}) -> fastfix::base::Result<std::vector<std::byte>> {
    fastfix::codec::EncodeOptions options;
    options.begin_string = std::move(begin_string);
    options.sender_comp_id = std::move(sender);
    options.target_comp_id = std::move(target);
    options.default_appl_ver_id = std::move(default_appl_ver_id);
    options.msg_seq_num = seq_num;
    options.poss_dup = poss_dup;
    options.sending_time = "20260402-12:00:00.000";
    options.orig_sending_time = std::move(orig_sending_time);
    return fastfix::codec::EncodeFixMessage(message, dictionary, options);
}

auto ActivateAcceptorSession(
    fastfix::session::AdminProtocol* protocol,
    const fastfix::profile::NormalizedDictionaryView& dictionary,
    std::string begin_string,
    std::string default_appl_ver_id = {}) -> fastfix::base::Status {
    fastfix::message::MessageBuilder logon_builder("A");
    logon_builder.set_string(35U, "A").set_int(98U, 0).set_int(108U, 30);
    auto inbound = EncodeInboundFrame(
        std::move(logon_builder).build(),
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
        return fastfix::base::Status::InvalidArgument("acceptor session did not activate on inbound logon");
    }
    return fastfix::base::Status::Ok();
}

}  // namespace

TEST_CASE("admin-protocol", "[admin-protocol]") {
    auto dictionary = LoadAdminDictionary();
    REQUIRE(dictionary.ok());

    {
        fastfix::store::MemorySessionStore store;
        REQUIRE(store.SaveRecoveryState(
            fastfix::store::SessionRecoveryState{
                .session_id = 5008U,
                .next_in_seq = 7U,
                .next_out_seq = 11U,
                .last_inbound_ns = 77U,
                .last_outbound_ns = 88U,
                .active = false,
            }).ok());
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5008U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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

        fastfix::message::MessageBuilder logon_builder("A");
        logon_builder.set_string(35U, "A").set_int(98U, 0).set_int(108U, 30);
        auto inbound = EncodeInboundFrame(
            std::move(logon_builder).build(),
            dictionary.value(),
            "FIX.4.4",
            "BUY",
            "SELL",
            7U,
            false);
        REQUIRE(inbound.ok());

        auto event = protocol.OnInbound(inbound.value(), 10U);
        REQUIRE(event.ok());
        REQUIRE(event.value().session_active);
        REQUIRE(event.value().outbound_frames.size() == 1U);

        auto decoded = fastfix::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
        REQUIRE(decoded.ok());
        REQUIRE(decoded.value().header.msg_type == "A");
        REQUIRE(decoded.value().header.msg_seq_num == 11U);

        const auto snapshot = protocol.session().Snapshot();
        REQUIRE(snapshot.next_in_seq == 8U);
        REQUIRE(snapshot.next_out_seq == 12U);
    }

    {
        fastfix::store::MemorySessionStore store;
        REQUIRE(store.SaveRecoveryState(
            fastfix::store::SessionRecoveryState{
                .session_id = 5009U,
                .next_in_seq = 7U,
                .next_out_seq = 11U,
                .last_inbound_ns = 77U,
                .last_outbound_ns = 88U,
                .active = false,
            }).ok());
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5009U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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

        fastfix::message::MessageBuilder logon_builder("A");
        logon_builder.set_string(35U, "A").set_int(98U, 0).set_int(108U, 30).set_boolean(141U, true);
        auto inbound = EncodeInboundFrame(
            std::move(logon_builder).build(),
            dictionary.value(),
            "FIX.4.4",
            "BUY",
            "SELL",
            1U,
            false);
        REQUIRE(inbound.ok());

        auto event = protocol.OnInbound(inbound.value(), 10U);
        REQUIRE(event.ok());
        REQUIRE(event.value().session_active);
        REQUIRE(event.value().outbound_frames.size() == 1U);

        auto decoded = fastfix::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
        REQUIRE(decoded.ok());
        REQUIRE(decoded.value().header.msg_type == "A");
        REQUIRE(decoded.value().header.msg_seq_num == 1U);
        REQUIRE(decoded.value().message.view().get_boolean(141U).value());

        const auto snapshot = protocol.session().Snapshot();
        REQUIRE(snapshot.next_in_seq == 2U);
        REQUIRE(snapshot.next_out_seq == 2U);
    }

    {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5001U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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

        fastfix::message::MessageBuilder heartbeat_builder("0");
        heartbeat_builder.set_string(35U, "0");
        auto inbound = EncodeInboundFrame(
            std::move(heartbeat_builder).build(),
            dictionary.value(),
            "FIX.4.4",
            "BUY",
            "SELL",
            1U,
            true);
        REQUIRE(inbound.ok());

        auto event = protocol.OnInbound(inbound.value(), 10U);
        REQUIRE(event.ok());
        REQUIRE(event.value().outbound_frames.size() == 1U);
        REQUIRE(!event.value().disconnect);

        auto decoded = fastfix::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
        REQUIRE(decoded.ok());
        REQUIRE(decoded.value().header.msg_type == "3");
        REQUIRE(decoded.value().message.view().get_int(45U).value() == 1);
        REQUIRE(decoded.value().message.view().get_int(371U).value() == 122);
        REQUIRE(decoded.value().message.view().get_string(372U).value() == "0");
        REQUIRE(decoded.value().message.view().get_int(373U).value() == 1);
    }

    {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5033U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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

        fastfix::message::MessageBuilder heartbeat_builder("0");
        heartbeat_builder.set_string(35U, "0");
        auto inbound = EncodeInboundFrame(
            std::move(heartbeat_builder).build(),
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
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5021U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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

        const auto inbound = ::fastfix::tests::EncodeFixFrame(
            "35=0|34=2|49=BUY|56=SELL|52=20260402-12:00:00.000|49=BUY|");
        auto event = protocol.OnInbound(inbound, 10U);
        REQUIRE(event.ok());
        REQUIRE(event.value().outbound_frames.size() == 1U);
        REQUIRE(!event.value().disconnect);

        auto decoded = fastfix::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
        REQUIRE(decoded.ok());
        REQUIRE(decoded.value().header.msg_type == "3");
        REQUIRE(decoded.value().message.view().get_int(371U).value() == 49);
        REQUIRE(decoded.value().message.view().get_string(372U).value() == "0");
        REQUIRE(decoded.value().message.view().get_int(373U).value() == 13);
    }

    {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5006U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
                    .profile_id = dictionary.value().profile().header().profile_id,
                    .heartbeat_interval_seconds = 30U,
                    .is_initiator = false,
                },
                .begin_string = "FIX.4.4",
                .sender_comp_id = "SELL",
                .target_comp_id = "BUY",
                .heartbeat_interval_seconds = 30U,
                .validation_policy = fastfix::session::ValidationPolicy::Compatible(),
            },
            dictionary.value(),
            &store);

        REQUIRE(protocol.OnTransportConnected(1U).ok());
        REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

        fastfix::message::MessageBuilder heartbeat_builder("0");
        heartbeat_builder.set_string(35U, "0");
        auto inbound = EncodeInboundFrame(
            std::move(heartbeat_builder).build(),
            dictionary.value(),
            "FIX.4.4",
            "BUY",
            "SELL",
            1U,
            true);
        REQUIRE(inbound.ok());

        auto event = protocol.OnInbound(inbound.value(), 10U);
        REQUIRE(event.ok());
        REQUIRE(event.value().outbound_frames.empty());
        REQUIRE(!event.value().disconnect);
    }

    {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5022U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
                    .profile_id = dictionary.value().profile().header().profile_id,
                    .heartbeat_interval_seconds = 30U,
                    .is_initiator = false,
                },
                .begin_string = "FIX.4.4",
                .sender_comp_id = "SELL",
                .target_comp_id = "BUY",
                .heartbeat_interval_seconds = 30U,
                .validation_policy = fastfix::session::ValidationPolicy::Compatible(),
            },
            dictionary.value(),
            &store);

        REQUIRE(protocol.OnTransportConnected(1U).ok());
        REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

        const auto inbound = ::fastfix::tests::EncodeFixFrame(
            "35=0|34=2|49=BUY|56=SELL|52=20260402-12:00:00.000|49=BUY|");
        auto event = protocol.OnInbound(inbound, 10U);
        REQUIRE(event.ok());
        REQUIRE(event.value().outbound_frames.empty());
        REQUIRE(!event.value().disconnect);
    }

    {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5002U,
                    .key = fastfix::session::SessionKey{"FIXT.1.1", "SELL", "BUY"},
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

        fastfix::message::MessageBuilder logon_builder("A");
        logon_builder.set_string(35U, "A").set_int(98U, 0).set_int(108U, 30);
        auto inbound = EncodeInboundFrame(
            std::move(logon_builder).build(),
            dictionary.value(),
            "FIXT.1.1",
            "OTHER",
            "SELL",
            1U,
            false,
            "9");
        REQUIRE(inbound.ok());

        auto event = protocol.OnInbound(inbound.value(), 10U);
        REQUIRE(event.ok());
        REQUIRE(event.value().outbound_frames.size() == 1U);
        REQUIRE(event.value().disconnect);

        auto decoded = fastfix::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
        REQUIRE(decoded.ok());
        REQUIRE(decoded.value().header.msg_type == "5");
        REQUIRE(decoded.value().message.view().get_string(58U).value() == "unexpected SenderCompID on inbound frame");
    }

    {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5039U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
                    .profile_id = dictionary.value().profile().header().profile_id,
                    .heartbeat_interval_seconds = 30U,
                    .is_initiator = false,
                },
                .begin_string = "FIX.4.4",
                .sender_comp_id = "SELL",
                .target_comp_id = "BUY",
                .heartbeat_interval_seconds = 30U,
                .validation_policy = fastfix::session::ValidationPolicy::Permissive(),
            },
            dictionary.value(),
            &store);

        REQUIRE(protocol.OnTransportConnected(1U).ok());

        fastfix::message::MessageBuilder logon_builder("A");
        logon_builder.set_string(35U, "A").set_int(98U, 0).set_int(108U, 30);
        auto inbound = EncodeInboundFrame(
            std::move(logon_builder).build(),
            dictionary.value(),
            "FIX.4.4",
            "BUY",
            "OTHER",
            1U,
            false);
        REQUIRE(inbound.ok());

        auto event = protocol.OnInbound(inbound.value(), 10U);
        REQUIRE(event.ok());
        REQUIRE(event.value().session_active);
        REQUIRE(event.value().outbound_frames.size() == 1U);
        REQUIRE(!event.value().disconnect);

        auto decoded = fastfix::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
        REQUIRE(decoded.ok());
        REQUIRE(decoded.value().header.msg_type == "A");
    }

    {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5010U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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

        fastfix::message::MessageBuilder logon_builder("A");
        logon_builder.set_string(35U, "A").set_int(98U, 0);
        auto inbound = EncodeInboundFrame(
            std::move(logon_builder).build(),
            dictionary.value(),
            "FIX.4.4",
            "BUY",
            "SELL",
            1U,
            false);
        REQUIRE(inbound.ok());

        auto event = protocol.OnInbound(inbound.value(), 10U);
        REQUIRE(event.ok());
        REQUIRE(event.value().outbound_frames.size() == 1U);
        REQUIRE(event.value().disconnect);

        auto decoded = fastfix::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
        REQUIRE(decoded.ok());
        REQUIRE(decoded.value().header.msg_type == "5");
        REQUIRE(decoded.value().message.view().get_string(58U).value() == "Logon requires HeartBtInt");
    }

    {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5023U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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

        fastfix::message::MessageBuilder logon_builder("A");
        logon_builder.set_string(35U, "A").set_int(98U, 1).set_int(108U, 30);
        auto inbound = EncodeInboundFrame(
            std::move(logon_builder).build(),
            dictionary.value(),
            "FIX.4.4",
            "BUY",
            "SELL",
            1U,
            false);
        REQUIRE(inbound.ok());

        auto event = protocol.OnInbound(inbound.value(), 10U);
        REQUIRE(event.ok());
        REQUIRE(event.value().outbound_frames.size() == 1U);
        REQUIRE(event.value().disconnect);

        auto decoded = fastfix::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
        REQUIRE(decoded.ok());
        REQUIRE(decoded.value().header.msg_type == "5");
        REQUIRE(decoded.value().message.view().get_string(58U).value() == "Logon EncryptMethod must be 0");
    }

    {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5040U,
                    .key = fastfix::session::SessionKey{"FIXT.1.1", "SELL", "BUY"},
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

        fastfix::message::MessageBuilder logon_builder("A");
        logon_builder.set_string(35U, "A").set_int(98U, 0).set_int(108U, 30);
        auto inbound = EncodeInboundFrame(
            std::move(logon_builder).build(),
            dictionary.value(),
            "FIXT.1.1",
            "BUY",
            "SELL",
            1U,
            false);
        REQUIRE(inbound.ok());

        auto event = protocol.OnInbound(inbound.value(), 10U);
        REQUIRE(event.ok());
        REQUIRE(event.value().outbound_frames.size() == 1U);
        REQUIRE(event.value().disconnect);

        auto decoded = fastfix::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
        REQUIRE(decoded.ok());
        REQUIRE(decoded.value().header.msg_type == "5");
        REQUIRE(decoded.value().message.view().get_string(58U).value() == "FIXT.1.1 logon requires DefaultApplVerID");
    }

    {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5041U,
                    .key = fastfix::session::SessionKey{"FIXT.1.1", "SELL", "BUY"},
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
                .validation_policy = fastfix::session::ValidationPolicy::Compatible(),
            },
            dictionary.value(),
            &store);

        REQUIRE(protocol.OnTransportConnected(1U).ok());

        fastfix::message::MessageBuilder logon_builder("A");
        logon_builder.set_string(35U, "A").set_int(98U, 0).set_int(108U, 30);
        auto inbound = EncodeInboundFrame(
            std::move(logon_builder).build(),
            dictionary.value(),
            "FIXT.1.1",
            "BUY",
            "SELL",
            1U,
            false);
        REQUIRE(inbound.ok());

        auto event = protocol.OnInbound(inbound.value(), 10U);
        REQUIRE(event.ok());
        REQUIRE(event.value().session_active);
        REQUIRE(event.value().outbound_frames.size() == 1U);
        REQUIRE(!event.value().disconnect);

        auto decoded = fastfix::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
        REQUIRE(decoded.ok());
        REQUIRE(decoded.value().header.msg_type == "A");
    }

    {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5042U,
                    .key = fastfix::session::SessionKey{"FIXT.1.1", "SELL", "BUY"},
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

        fastfix::message::MessageBuilder logon_builder("A");
        logon_builder.set_string(35U, "A").set_int(98U, 0).set_int(108U, 30);
        auto inbound = EncodeInboundFrame(
            std::move(logon_builder).build(),
            dictionary.value(),
            "FIXT.1.1",
            "BUY",
            "SELL",
            1U,
            false,
            "7");
        REQUIRE(inbound.ok());

        auto event = protocol.OnInbound(inbound.value(), 10U);
        REQUIRE(event.ok());
        REQUIRE(event.value().outbound_frames.size() == 1U);
        REQUIRE(event.value().disconnect);

        auto decoded = fastfix::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
        REQUIRE(decoded.ok());
        REQUIRE(decoded.value().header.msg_type == "5");
        REQUIRE(decoded.value().message.view().get_string(58U).value() == "unexpected DefaultApplVerID on inbound frame");
    }

    {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5043U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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

        fastfix::message::MessageBuilder logon_builder("A");
        logon_builder.set_string(35U, "A").set_int(98U, 0).set_int(108U, 30);
        auto inbound = EncodeInboundFrame(
            std::move(logon_builder).build(),
            dictionary.value(),
            "FIX.4.4",
            "BUY",
            "SELL",
            1U,
            true);
        REQUIRE(inbound.ok());

        auto event = protocol.OnInbound(inbound.value(), 10U);
        REQUIRE(event.ok());
        REQUIRE(event.value().outbound_frames.size() == 1U);
        REQUIRE(event.value().disconnect);

        auto decoded = fastfix::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
        REQUIRE(decoded.ok());
        REQUIRE(decoded.value().header.msg_type == "5");
        REQUIRE(decoded.value().message.view().get_string(58U).value() == "PossDupFlag requires OrigSendingTime");
    }

    {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5044U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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

        const auto inbound = ::fastfix::tests::EncodeFixFrame(
            "35=A|34=1|49=BUY|56=SELL|52=20260402-12:00:00.000|98=0|108=30|9999=BAD|");
        auto event = protocol.OnInbound(inbound, 10U);
        REQUIRE(event.ok());
        REQUIRE(event.value().outbound_frames.size() == 1U);
        REQUIRE(event.value().disconnect);

        auto decoded = fastfix::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
        REQUIRE(decoded.ok());
        REQUIRE(decoded.value().header.msg_type == "5");
    }

    {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5036U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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

        fastfix::message::MessageBuilder heartbeat_builder("0");
        heartbeat_builder.set_string(35U, "0");
        auto inbound = EncodeInboundFrame(
            std::move(heartbeat_builder).build(),
            dictionary.value(),
            "FIX.4.4",
            "BUY",
            "SELL",
            1U,
            false);
        REQUIRE(inbound.ok());

        auto event = protocol.OnInbound(inbound.value(), 10U);
        REQUIRE(event.ok());
        REQUIRE(event.value().outbound_frames.size() == 1U);
        REQUIRE(event.value().disconnect);

        auto decoded = fastfix::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
        REQUIRE(decoded.ok());
        REQUIRE(decoded.value().header.msg_type == "5");
        REQUIRE(decoded.value().message.view().get_string(58U).value() == "received 0 before Logon completed");

        const auto snapshot = protocol.session().Snapshot();
        REQUIRE(snapshot.next_in_seq == 2U);
        REQUIRE(snapshot.next_out_seq == 2U);
    }

    {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5037U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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

        fastfix::message::MessageBuilder resend_builder("2");
        resend_builder.set_string(35U, "2").set_int(7U, 1).set_int(16U, 0);
        auto inbound = EncodeInboundFrame(
            std::move(resend_builder).build(),
            dictionary.value(),
            "FIX.4.4",
            "BUY",
            "SELL",
            2U,
            false);
        REQUIRE(inbound.ok());

        auto event = protocol.OnInbound(inbound.value(), 10U);
        REQUIRE(event.ok());
        REQUIRE(event.value().outbound_frames.size() == 1U);
        REQUIRE(event.value().disconnect);

        auto decoded = fastfix::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
        REQUIRE(decoded.ok());
        REQUIRE(decoded.value().header.msg_type == "5");
        REQUIRE(decoded.value().message.view().get_string(58U).value() == "received 2 before Logon completed");

        const auto snapshot = protocol.session().Snapshot();
        REQUIRE(snapshot.state == fastfix::session::SessionState::kAwaitingLogout);
        REQUIRE(snapshot.next_in_seq == 1U);
        REQUIRE(snapshot.next_out_seq == 2U);
    }

    {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5004U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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

        fastfix::message::MessageBuilder heartbeat_builder("0");
        heartbeat_builder.set_string(35U, "0");
        auto inbound = EncodeInboundFrame(
            std::move(heartbeat_builder).build(),
            dictionary.value(),
            "FIX.4.4",
            "BUY",
            "SELL",
            1U,
            false);
        REQUIRE(inbound.ok());

        auto event = protocol.OnInbound(inbound.value(), 10U);
        REQUIRE(event.ok());
        REQUIRE(event.value().outbound_frames.size() == 1U);
        REQUIRE(event.value().disconnect);

        auto decoded = fastfix::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
        REQUIRE(decoded.ok());
        REQUIRE(decoded.value().header.msg_type == "5");
        REQUIRE(decoded.value().message.view().get_string(58U).value() == "received stale inbound FIX sequence number");
    }

    {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5038U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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

        fastfix::message::MessageBuilder logon_builder("A");
        logon_builder.set_string(35U, "A").set_int(98U, 0).set_int(108U, 30);
        auto inbound = EncodeInboundFrame(
            std::move(logon_builder).build(),
            dictionary.value(),
            "FIX.4.4",
            "BUY",
            "SELL",
            2U,
            false);
        REQUIRE(inbound.ok());

        auto event = protocol.OnInbound(inbound.value(), 10U);
        REQUIRE(event.ok());
        REQUIRE(event.value().outbound_frames.size() == 1U);
        REQUIRE(event.value().disconnect);

        auto decoded = fastfix::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
        REQUIRE(decoded.ok());
        REQUIRE(decoded.value().header.msg_type == "5");
        REQUIRE(decoded.value().message.view().get_string(58U).value() == "received unexpected Logon after session activation");

        const auto snapshot = protocol.session().Snapshot();
        REQUIRE(snapshot.state == fastfix::session::SessionState::kAwaitingLogout);
        REQUIRE(snapshot.next_in_seq == 3U);
        REQUIRE(snapshot.next_out_seq == 3U);
    }

    {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5024U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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

        fastfix::message::MessageBuilder test_request_builder("1");
        test_request_builder.set_string(35U, "1");
        auto inbound = EncodeInboundFrame(
            std::move(test_request_builder).build(),
            dictionary.value(),
            "FIX.4.4",
            "BUY",
            "SELL",
            2U,
            false);
        REQUIRE(inbound.ok());

        auto event = protocol.OnInbound(inbound.value(), 10U);
        REQUIRE(event.ok());
        REQUIRE(event.value().outbound_frames.size() == 1U);
        REQUIRE(!event.value().disconnect);

        auto decoded = fastfix::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
        REQUIRE(decoded.ok());
        REQUIRE(decoded.value().header.msg_type == "3");
        REQUIRE(decoded.value().message.view().get_int(371U).value() == 112);
        REQUIRE(decoded.value().message.view().get_string(372U).value() == "1");
        REQUIRE(decoded.value().message.view().get_int(373U).value() == 1);
    }

    {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5025U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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

        fastfix::message::MessageBuilder resend_builder("2");
        resend_builder.set_string(35U, "2").set_int(7U, 9).set_int(16U, 4);
        auto inbound = EncodeInboundFrame(
            std::move(resend_builder).build(),
            dictionary.value(),
            "FIX.4.4",
            "BUY",
            "SELL",
            2U,
            false);
        REQUIRE(inbound.ok());

        auto event = protocol.OnInbound(inbound.value(), 10U);
        REQUIRE(event.ok());
        REQUIRE(event.value().outbound_frames.size() == 1U);
        REQUIRE(!event.value().disconnect);

        auto decoded = fastfix::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
        REQUIRE(decoded.ok());
        REQUIRE(decoded.value().header.msg_type == "3");
        REQUIRE(decoded.value().message.view().get_int(371U).value() == 16);
        REQUIRE(decoded.value().message.view().get_string(372U).value() == "2");
        REQUIRE(decoded.value().message.view().get_int(373U).value() == 5);
    }

    {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5005U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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

        fastfix::message::MessageBuilder reset_builder("4");
        reset_builder.set_string(35U, "4");
        auto inbound = EncodeInboundFrame(
            std::move(reset_builder).build(),
            dictionary.value(),
            "FIX.4.4",
            "BUY",
            "SELL",
            2U,
            false);
        REQUIRE(inbound.ok());

        auto event = protocol.OnInbound(inbound.value(), 10U);
        REQUIRE(event.ok());
        REQUIRE(event.value().outbound_frames.size() == 1U);
        REQUIRE(!event.value().disconnect);

        auto decoded = fastfix::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
        REQUIRE(decoded.ok());
        REQUIRE(decoded.value().header.msg_type == "3");
        REQUIRE(decoded.value().message.view().get_int(371U).value() == 36);
        REQUIRE(decoded.value().message.view().get_string(372U).value() == "4");
        REQUIRE(decoded.value().message.view().get_int(373U).value() == 1);
    }

    {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5026U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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

        fastfix::message::MessageBuilder reset_builder("4");
        reset_builder.set_string(35U, "4").set_int(36U, 1);
        auto inbound = EncodeInboundFrame(
            std::move(reset_builder).build(),
            dictionary.value(),
            "FIX.4.4",
            "BUY",
            "SELL",
            2U,
            false);
        REQUIRE(inbound.ok());

        auto event = protocol.OnInbound(inbound.value(), 10U);
        REQUIRE(event.ok());
        REQUIRE(event.value().outbound_frames.size() == 1U);
        REQUIRE(!event.value().disconnect);

        auto decoded = fastfix::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
        REQUIRE(decoded.ok());
        REQUIRE(decoded.value().header.msg_type == "3");
        REQUIRE(decoded.value().message.view().get_int(371U).value() == 36);
        REQUIRE(decoded.value().message.view().get_string(372U).value() == "4");
        REQUIRE(decoded.value().message.view().get_int(373U).value() == 5);
    }

    {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5034U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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

        fastfix::message::MessageBuilder heartbeat_builder("0");
        heartbeat_builder.set_string(35U, "0");
        auto gap_inbound = EncodeInboundFrame(
            std::move(heartbeat_builder).build(),
            dictionary.value(),
            "FIX.4.4",
            "BUY",
            "SELL",
            5U,
            false);
        REQUIRE(gap_inbound.ok());

        auto gap_event = protocol.OnInbound(gap_inbound.value(), 10U);
        REQUIRE(gap_event.ok());
        REQUIRE(gap_event.value().outbound_frames.size() == 1U);

        auto resend = fastfix::codec::DecodeFixMessage(gap_event.value().outbound_frames.front().bytes, dictionary.value());
        REQUIRE(resend.ok());
        REQUIRE(resend.value().header.msg_type == "2");
        REQUIRE(resend.value().message.view().get_int(7U).value() == 2);
        REQUIRE(resend.value().message.view().get_int(16U).value() == 4);

        fastfix::message::MessageBuilder first_gap_fill_builder("4");
        first_gap_fill_builder.set_string(35U, "4").set_boolean(123U, true).set_int(36U, 4);
        auto first_gap_fill = EncodeInboundFrame(
            std::move(first_gap_fill_builder).build(),
            dictionary.value(),
            "FIX.4.4",
            "BUY",
            "SELL",
            2U,
            false);
        REQUIRE(first_gap_fill.ok());

        auto first_gap_fill_event = protocol.OnInbound(first_gap_fill.value(), 20U);
        REQUIRE(first_gap_fill_event.ok());
        REQUIRE(first_gap_fill_event.value().outbound_frames.empty());
        REQUIRE(!first_gap_fill_event.value().disconnect);

        const auto after_first_gap_fill = protocol.session().Snapshot();
        REQUIRE(after_first_gap_fill.state == fastfix::session::SessionState::kResendProcessing);
        REQUIRE(after_first_gap_fill.has_pending_resend);
        REQUIRE(after_first_gap_fill.pending_resend.begin_seq == 2U);
        REQUIRE(after_first_gap_fill.pending_resend.end_seq == 4U);
        REQUIRE(after_first_gap_fill.next_in_seq == 4U);

        fastfix::message::MessageBuilder overlapping_gap_fill_builder("4");
        overlapping_gap_fill_builder.set_string(35U, "4").set_boolean(123U, true).set_int(36U, 6);
        auto overlapping_gap_fill = EncodeInboundFrame(
            std::move(overlapping_gap_fill_builder).build(),
            dictionary.value(),
            "FIX.4.4",
            "BUY",
            "SELL",
            3U,
            false);
        REQUIRE(overlapping_gap_fill.ok());

        auto overlapping_gap_fill_event = protocol.OnInbound(overlapping_gap_fill.value(), 30U);
        REQUIRE(overlapping_gap_fill_event.ok());
        REQUIRE(overlapping_gap_fill_event.value().outbound_frames.empty());
        REQUIRE(!overlapping_gap_fill_event.value().disconnect);

        const auto after_overlapping_gap_fill = protocol.session().Snapshot();
        REQUIRE(after_overlapping_gap_fill.state == fastfix::session::SessionState::kActive);
        REQUIRE(!after_overlapping_gap_fill.has_pending_resend);
        REQUIRE(after_overlapping_gap_fill.next_in_seq == 6U);
        REQUIRE(after_overlapping_gap_fill.last_inbound_ns == 30U);

        auto recovery = store.LoadRecoveryState(5034U);
        REQUIRE(recovery.ok());
        REQUIRE(recovery.value().next_in_seq == 6U);
        REQUIRE(recovery.value().last_inbound_ns == 30U);
    }

    {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5035U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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

        fastfix::message::MessageBuilder first_gap_fill_builder("4");
        first_gap_fill_builder.set_string(35U, "4").set_boolean(123U, true).set_int(36U, 5);
        auto first_gap_fill = EncodeInboundFrame(
            std::move(first_gap_fill_builder).build(),
            dictionary.value(),
            "FIX.4.4",
            "BUY",
            "SELL",
            2U,
            false);
        REQUIRE(first_gap_fill.ok());

        auto first_gap_fill_event = protocol.OnInbound(first_gap_fill.value(), 10U);
        REQUIRE(first_gap_fill_event.ok());
        REQUIRE(first_gap_fill_event.value().outbound_frames.empty());
        REQUIRE(!first_gap_fill_event.value().disconnect);

        fastfix::message::MessageBuilder duplicate_gap_fill_builder("4");
        duplicate_gap_fill_builder.set_string(35U, "4").set_boolean(123U, true).set_int(36U, 5);
        auto duplicate_gap_fill = EncodeInboundFrame(
            std::move(duplicate_gap_fill_builder).build(),
            dictionary.value(),
            "FIX.4.4",
            "BUY",
            "SELL",
            3U,
            false);
        REQUIRE(duplicate_gap_fill.ok());

        auto duplicate_gap_fill_event = protocol.OnInbound(duplicate_gap_fill.value(), 20U);
        REQUIRE(duplicate_gap_fill_event.ok());
        REQUIRE(duplicate_gap_fill_event.value().outbound_frames.empty());
        REQUIRE(!duplicate_gap_fill_event.value().disconnect);

        const auto snapshot = protocol.session().Snapshot();
        REQUIRE(snapshot.state == fastfix::session::SessionState::kActive);
        REQUIRE(snapshot.next_in_seq == 5U);
        REQUIRE(snapshot.last_inbound_ns == 20U);

        auto recovery = store.LoadRecoveryState(5035U);
        REQUIRE(recovery.ok());
        REQUIRE(recovery.value().next_in_seq == 5U);
        REQUIRE(recovery.value().last_inbound_ns == 20U);
    }

    {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5003U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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

        fastfix::message::MessageBuilder app_builder("D");
        app_builder.set_string(35U, "D").set_string(11U, "ORD-1");
        auto outbound = protocol.SendApplication(std::move(app_builder).build(), 100U);
        REQUIRE(outbound.ok());

        auto original = fastfix::codec::DecodeFixMessage(outbound.value().bytes, dictionary.value());
        REQUIRE(original.ok());
        REQUIRE(!original.value().header.sending_time.empty());

        fastfix::message::MessageBuilder resend_builder("2");
        resend_builder.set_string(35U, "2").set_int(7U, 2).set_int(16U, 2);
        auto inbound = EncodeInboundFrame(
            std::move(resend_builder).build(),
            dictionary.value(),
            "FIX.4.4",
            "BUY",
            "SELL",
            2U,
            false);
        REQUIRE(inbound.ok());

        auto event = protocol.OnInbound(inbound.value(), 200U);
        REQUIRE(event.ok());
        REQUIRE(event.value().outbound_frames.size() == 1U);

        auto replay = fastfix::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
        REQUIRE(replay.ok());
        REQUIRE(replay.value().header.msg_type == "D");
        REQUIRE(replay.value().header.poss_dup);
        REQUIRE(replay.value().header.orig_sending_time == original.value().header.sending_time);
        REQUIRE(replay.value().message.view().get_string(122U).has_value());
        REQUIRE(replay.value().message.view().get_string(122U).value() == original.value().header.sending_time);
    }

    {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5030U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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
        REQUIRE(fastfix::codec::DecodeFixMessage(logout.value().bytes, dictionary.value()).ok());

        const auto before_replay = protocol.session().Snapshot();
        REQUIRE(before_replay.state == fastfix::session::SessionState::kAwaitingLogout);
        REQUIRE(before_replay.next_out_seq == 3U);

        fastfix::message::MessageBuilder resend_builder("2");
        resend_builder.set_string(35U, "2").set_int(7U, 2).set_int(16U, 2);
        auto inbound = EncodeInboundFrame(
            std::move(resend_builder).build(),
            dictionary.value(),
            "FIX.4.4",
            "BUY",
            "SELL",
            2U,
            false);
        REQUIRE(inbound.ok());

        auto event = protocol.OnInbound(inbound.value(), 200U);
        REQUIRE(event.ok());
        REQUIRE(event.value().outbound_frames.size() == 1U);
        REQUIRE(!event.value().disconnect);

        auto replay = fastfix::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
        REQUIRE(replay.ok());
        REQUIRE(replay.value().header.msg_type == "4");
        REQUIRE(replay.value().header.msg_seq_num == 2U);
        REQUIRE(replay.value().message.view().get_boolean(123U).value());
        REQUIRE(replay.value().message.view().get_int(36U).value() == 3);

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
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5031U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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

        fastfix::message::MessageBuilder logout_builder("5");
        logout_builder.set_string(35U, "5");
        auto inbound = EncodeInboundFrame(
            std::move(logout_builder).build(),
            dictionary.value(),
            "FIX.4.4",
            "BUY",
            "SELL",
            2U,
            false);
        REQUIRE(inbound.ok());

        auto event = protocol.OnInbound(inbound.value(), 10U);
        REQUIRE(event.ok());
        REQUIRE(event.value().outbound_frames.size() == 1U);
        REQUIRE(event.value().disconnect);

        auto decoded = fastfix::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
        REQUIRE(decoded.ok());
        REQUIRE(decoded.value().header.msg_type == "5");

        const auto snapshot = protocol.session().Snapshot();
        REQUIRE(snapshot.state == fastfix::session::SessionState::kAwaitingLogout);
        REQUIRE(snapshot.next_in_seq == 3U);
        REQUIRE(snapshot.next_out_seq == 3U);
    }

    {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5032U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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

        fastfix::message::MessageBuilder heartbeat_builder("0");
        heartbeat_builder.set_string(35U, "0");
        auto inbound = EncodeInboundFrame(
            std::move(heartbeat_builder).build(),
            dictionary.value(),
            "FIX.4.4",
            "BUY",
            "SELL",
            4U,
            false);
        REQUIRE(inbound.ok());

        auto event = protocol.OnInbound(inbound.value(), 10U);
        REQUIRE(event.ok());
        REQUIRE(event.value().outbound_frames.size() == 1U);
        REQUIRE(!event.value().disconnect);

        auto resend = fastfix::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
        REQUIRE(resend.ok());
        REQUIRE(resend.value().header.msg_type == "2");
        REQUIRE(resend.value().message.view().get_int(7U).value() == 2);
        REQUIRE(resend.value().message.view().get_int(16U).value() == 3);

        const auto snapshot = protocol.session().Snapshot();
        REQUIRE(snapshot.state == fastfix::session::SessionState::kResendProcessing);
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

        auto heartbeat = fastfix::codec::DecodeFixMessage(
            timer_event.value().outbound_frames.front().bytes,
            dictionary.value());
        REQUIRE(heartbeat.ok());
        REQUIRE(heartbeat.value().header.msg_type == "0");

        timer_event = protocol.OnTimer(60000000011ULL);
        REQUIRE(timer_event.ok());
        REQUIRE(timer_event.value().outbound_frames.size() == 1U);

        auto test_request = fastfix::codec::DecodeFixMessage(
            timer_event.value().outbound_frames.front().bytes,
            dictionary.value());
        REQUIRE(test_request.ok());
        REQUIRE(test_request.value().header.msg_type == "1");
        REQUIRE(test_request.value().message.view().get_string(112U).has_value());
    }

    {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5007U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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

        fastfix::message::MessageBuilder app_builder("Z");
        app_builder.set_string(35U, "Z");
        auto inbound = EncodeInboundFrame(
            std::move(app_builder).build(),
            dictionary.value(),
            "FIX.4.4",
            "BUY",
            "SELL",
            2U,
            false);
        REQUIRE(inbound.ok());

        auto event = protocol.OnInbound(inbound.value(), 10U);
        REQUIRE(event.ok());
        REQUIRE(event.value().outbound_frames.size() == 1U);
        REQUIRE(event.value().application_messages.empty());
        REQUIRE(!event.value().disconnect);

        auto decoded = fastfix::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
        REQUIRE(decoded.ok());
        REQUIRE(decoded.value().header.msg_type == "3");
        REQUIRE(decoded.value().message.view().get_int(371U).value() == 35);
        REQUIRE(decoded.value().message.view().get_string(372U).value() == "Z");
        REQUIRE(decoded.value().message.view().get_int(373U).value() == 11);
    }

    {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5011U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
                    .profile_id = dictionary.value().profile().header().profile_id,
                    .heartbeat_interval_seconds = 30U,
                    .is_initiator = false,
                },
                .begin_string = "FIX.4.4",
                .sender_comp_id = "SELL",
                .target_comp_id = "BUY",
                .heartbeat_interval_seconds = 30U,
                .validation_policy = fastfix::session::ValidationPolicy::Compatible(),
            },
            dictionary.value(),
            &store);

        REQUIRE(protocol.OnTransportConnected(1U).ok());
        REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

        fastfix::message::MessageBuilder app_builder("Z");
        app_builder.set_string(35U, "Z");
        auto inbound = EncodeInboundFrame(
            std::move(app_builder).build(),
            dictionary.value(),
            "FIX.4.4",
            "BUY",
            "SELL",
            2U,
            false);
        REQUIRE(inbound.ok());

        auto event = protocol.OnInbound(inbound.value(), 10U);
        REQUIRE(event.ok());
        REQUIRE(event.value().outbound_frames.empty());
        REQUIRE(event.value().application_messages.size() == 1U);
    }

    {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5012U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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

        const auto inbound = ::fastfix::tests::EncodeFixFrame(
            "35=D|34=2|49=BUY|56=SELL|52=20260402-12:00:00.000|55=AAPL|9999=BAD|");
        auto event = protocol.OnInbound(inbound, 10U);
        REQUIRE(event.ok());
        REQUIRE(event.value().outbound_frames.size() == 1U);
        REQUIRE(event.value().application_messages.empty());

        auto decoded = fastfix::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
        REQUIRE(decoded.ok());
        REQUIRE(decoded.value().header.msg_type == "3");
        REQUIRE(decoded.value().message.view().get_int(371U).value() == 9999);
        REQUIRE(decoded.value().message.view().get_string(372U).value() == "D");
        REQUIRE(decoded.value().message.view().get_int(373U).value() == 3);
    }

    {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5013U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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

        const auto inbound = ::fastfix::tests::EncodeFixFrame(
            "35=D|34=2|49=BUY|56=SELL|52=20260402-12:00:00.000|55=AAPL|448=ORPHAN|");
        auto event = protocol.OnInbound(inbound, 10U);
        REQUIRE(event.ok());
        REQUIRE(event.value().outbound_frames.size() == 1U);
        REQUIRE(event.value().application_messages.empty());

        auto decoded = fastfix::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
        REQUIRE(decoded.ok());
        REQUIRE(decoded.value().header.msg_type == "3");
        REQUIRE(decoded.value().message.view().get_int(371U).value() == 448);
        REQUIRE(decoded.value().message.view().get_string(372U).value() == "D");
        REQUIRE(decoded.value().message.view().get_int(373U).value() == 2);
    }

    {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5014U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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

        const auto inbound = ::fastfix::tests::EncodeFixFrame(
            "35=D|34=2|49=BUY|56=SELL|52=20260402-12:00:00.000|55=AAPL|55=MSFT|");
        auto event = protocol.OnInbound(inbound, 10U);
        REQUIRE(event.ok());
        REQUIRE(event.value().outbound_frames.size() == 1U);
        REQUIRE(event.value().application_messages.empty());

        auto decoded = fastfix::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
        REQUIRE(decoded.ok());
        REQUIRE(decoded.value().header.msg_type == "3");
        REQUIRE(decoded.value().message.view().get_int(371U).value() == 55);
        REQUIRE(decoded.value().message.view().get_string(372U).value() == "D");
        REQUIRE(decoded.value().message.view().get_int(373U).value() == 13);
    }

    {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5015U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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

        const auto inbound = ::fastfix::tests::EncodeFixFrame(
            "35=D|34=2|49=BUY|56=SELL|52=20260402-12:00:00.000|453=1|448=PTY1|447=D|452=7|55=AAPL|");
        auto event = protocol.OnInbound(inbound, 10U);
        REQUIRE(event.ok());
        REQUIRE(event.value().outbound_frames.size() == 1U);
        REQUIRE(event.value().application_messages.empty());

        auto decoded = fastfix::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
        REQUIRE(decoded.ok());
        REQUIRE(decoded.value().header.msg_type == "3");
        REQUIRE(decoded.value().message.view().get_int(371U).value() == 55);
        REQUIRE(decoded.value().message.view().get_string(372U).value() == "D");
        REQUIRE(decoded.value().message.view().get_int(373U).value() == 14);
    }

    {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5019U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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

        const auto inbound = ::fastfix::tests::EncodeFixFrame(
            "35=D|34=2|49=BUY|56=SELL|52=20260402-12:00:00.000|55=AAPL|453=1|");
        auto event = protocol.OnInbound(inbound, 10U);
        REQUIRE(event.ok());
        REQUIRE(event.value().outbound_frames.size() == 1U);
        REQUIRE(event.value().application_messages.empty());

        auto decoded = fastfix::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
        REQUIRE(decoded.ok());
        REQUIRE(decoded.value().header.msg_type == "3");
        REQUIRE(decoded.value().message.view().get_int(371U).value() == 453);
        REQUIRE(decoded.value().message.view().get_string(372U).value() == "D");
        REQUIRE(decoded.value().message.view().get_int(373U).value() == 16);
    }

    {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5016U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
                    .profile_id = dictionary.value().profile().header().profile_id,
                    .heartbeat_interval_seconds = 30U,
                    .is_initiator = false,
                },
                .begin_string = "FIX.4.4",
                .sender_comp_id = "SELL",
                .target_comp_id = "BUY",
                .heartbeat_interval_seconds = 30U,
                .validation_policy = fastfix::session::ValidationPolicy::Compatible(),
            },
            dictionary.value(),
            &store);

        REQUIRE(protocol.OnTransportConnected(1U).ok());
        REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

        const auto inbound = ::fastfix::tests::EncodeFixFrame(
            "35=D|34=2|49=BUY|56=SELL|52=20260402-12:00:00.000|55=AAPL|9999=BAD|");
        auto event = protocol.OnInbound(inbound, 10U);
        REQUIRE(event.ok());
        REQUIRE(event.value().outbound_frames.empty());
        REQUIRE(event.value().application_messages.size() == 1U);
    }

    {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5020U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
                    .profile_id = dictionary.value().profile().header().profile_id,
                    .heartbeat_interval_seconds = 30U,
                    .is_initiator = false,
                },
                .begin_string = "FIX.4.4",
                .sender_comp_id = "SELL",
                .target_comp_id = "BUY",
                .heartbeat_interval_seconds = 30U,
                .validation_policy = fastfix::session::ValidationPolicy::Compatible(),
            },
            dictionary.value(),
            &store);

        REQUIRE(protocol.OnTransportConnected(1U).ok());
        REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

        const auto inbound = ::fastfix::tests::EncodeFixFrame(
            "35=D|34=2|49=BUY|56=SELL|52=20260402-12:00:00.000|55=AAPL|453=1|");
        auto event = protocol.OnInbound(inbound, 10U);
        REQUIRE(event.ok());
        REQUIRE(event.value().outbound_frames.empty());
        REQUIRE(event.value().application_messages.size() == 1U);
    }

    {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5017U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
                    .profile_id = dictionary.value().profile().header().profile_id,
                    .heartbeat_interval_seconds = 30U,
                    .is_initiator = false,
                },
                .begin_string = "FIX.4.4",
                .sender_comp_id = "SELL",
                .target_comp_id = "BUY",
                .heartbeat_interval_seconds = 30U,
                .validation_policy = fastfix::session::ValidationPolicy::Compatible(),
            },
            dictionary.value(),
            &store);

        REQUIRE(protocol.OnTransportConnected(1U).ok());
        REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

        const auto inbound = ::fastfix::tests::EncodeFixFrame(
            "35=D|34=2|49=BUY|56=SELL|52=20260402-12:00:00.000|55=AAPL|55=MSFT|");
        auto event = protocol.OnInbound(inbound, 10U);
        REQUIRE(event.ok());
        REQUIRE(event.value().outbound_frames.empty());
        REQUIRE(event.value().application_messages.size() == 1U);
    }

    {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5018U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
                    .profile_id = dictionary.value().profile().header().profile_id,
                    .heartbeat_interval_seconds = 30U,
                    .is_initiator = false,
                },
                .begin_string = "FIX.4.4",
                .sender_comp_id = "SELL",
                .target_comp_id = "BUY",
                .heartbeat_interval_seconds = 30U,
                .validation_policy = fastfix::session::ValidationPolicy::Compatible(),
            },
            dictionary.value(),
            &store);

        REQUIRE(protocol.OnTransportConnected(1U).ok());
        REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

        const auto inbound = ::fastfix::tests::EncodeFixFrame(
            "35=D|34=2|49=BUY|56=SELL|52=20260402-12:00:00.000|453=1|448=PTY1|447=D|452=7|55=AAPL|");
        auto event = protocol.OnInbound(inbound, 10U);
        REQUIRE(event.ok());
        REQUIRE(event.value().outbound_frames.empty());
        REQUIRE(event.value().application_messages.size() == 1U);
    }

    {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5008U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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

        fastfix::message::MessageBuilder app_builder("D");
        app_builder.set_string(35U, "D");
        auto inbound = EncodeInboundFrame(
            std::move(app_builder).build(),
            dictionary.value(),
            "FIX.4.4",
            "BUY",
            "SELL",
            2U,
            false);
        REQUIRE(inbound.ok());

        auto event = protocol.OnInbound(inbound.value(), 10U);
        REQUIRE(event.ok());
        REQUIRE(event.value().outbound_frames.size() == 1U);
        REQUIRE(event.value().application_messages.empty());

        auto decoded = fastfix::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
        REQUIRE(decoded.ok());
        REQUIRE(decoded.value().header.msg_type == "3");
        REQUIRE(decoded.value().message.view().get_int(371U).value() == 55);
        REQUIRE(decoded.value().message.view().get_string(372U).value() == "D");
        REQUIRE(decoded.value().message.view().get_int(373U).value() == 1);
    }

    {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5009U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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

        fastfix::message::MessageBuilder app_builder("D");
        app_builder.set_string(35U, "D").set_string(55U, "AAPL");
        auto party = app_builder.add_group_entry(453U);
        party.set_string(448U, "PTY1").set_int(452U, 7);
        auto inbound = EncodeInboundFrame(
            std::move(app_builder).build(),
            dictionary.value(),
            "FIX.4.4",
            "BUY",
            "SELL",
            2U,
            false);
        REQUIRE(inbound.ok());

        auto event = protocol.OnInbound(inbound.value(), 10U);
        REQUIRE(event.ok());
        REQUIRE(event.value().outbound_frames.size() == 1U);
        REQUIRE(event.value().application_messages.empty());

        auto decoded = fastfix::codec::DecodeFixMessage(event.value().outbound_frames.front().bytes, dictionary.value());
        REQUIRE(decoded.ok());
        REQUIRE(decoded.value().header.msg_type == "3");
        REQUIRE(decoded.value().message.view().get_int(371U).value() == 447);
        REQUIRE(decoded.value().message.view().get_string(372U).value() == "D");
        REQUIRE(decoded.value().message.view().get_int(373U).value() == 1);
    }

    {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5010U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
                    .profile_id = dictionary.value().profile().header().profile_id,
                    .heartbeat_interval_seconds = 30U,
                    .is_initiator = false,
                },
                .begin_string = "FIX.4.4",
                .sender_comp_id = "SELL",
                .target_comp_id = "BUY",
                .heartbeat_interval_seconds = 30U,
                .validation_policy = fastfix::session::ValidationPolicy::Permissive(),
            },
            dictionary.value(),
            &store);

        REQUIRE(protocol.OnTransportConnected(1U).ok());
        REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

        fastfix::message::MessageBuilder app_builder("D");
        app_builder.set_string(35U, "D");
        auto inbound = EncodeInboundFrame(
            std::move(app_builder).build(),
            dictionary.value(),
            "FIX.4.4",
            "BUY",
            "SELL",
            2U,
            false);
        REQUIRE(inbound.ok());

        auto event = protocol.OnInbound(inbound.value(), 10U);
        REQUIRE(event.ok());
        REQUIRE(event.value().outbound_frames.empty());
        REQUIRE(event.value().application_messages.size() == 1U);
    }

    {
        fastfix::store::MemorySessionStore store;
        fastfix::session::AdminProtocol protocol(
            fastfix::session::AdminProtocolConfig{
                .session = fastfix::session::SessionConfig{
                    .session_id = 5011U,
                    .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
                    .profile_id = dictionary.value().profile().header().profile_id,
                    .heartbeat_interval_seconds = 30U,
                    .is_initiator = false,
                },
                .begin_string = "FIX.4.4",
                .sender_comp_id = "SELL",
                .target_comp_id = "BUY",
                .heartbeat_interval_seconds = 30U,
                .validation_policy = fastfix::session::ValidationPolicy::Permissive(),
            },
            dictionary.value(),
            &store);

        REQUIRE(protocol.OnTransportConnected(1U).ok());
        REQUIRE(ActivateAcceptorSession(&protocol, dictionary.value(), "FIX.4.4").ok());

        fastfix::message::MessageBuilder app_builder("D");
        app_builder.set_string(35U, "D").set_string(55U, "AAPL");
        auto party = app_builder.add_group_entry(453U);
        party.set_string(448U, "PTY1").set_char(447U, 'D').set_int(452U, 1);
        auto inbound = EncodeInboundFrame(
            std::move(app_builder).build(),
            dictionary.value(),
            "FIX.4.4",
            "BUY",
            "SELL",
            2U,
            false);
        REQUIRE(inbound.ok());

        auto inbound_frame = std::move(inbound).value();
        auto inbound_result = protocol.OnInbound(std::move(inbound_frame), 10U);
        REQUIRE(inbound_result.ok());

        auto event = std::move(inbound_result).value();
        REQUIRE(event.outbound_frames.empty());
        REQUIRE(event.application_messages.size() == 1U);

        auto moved_event = std::move(event);
        REQUIRE(moved_event.application_messages.size() == 1U);
        REQUIRE(moved_event.application_messages.front().view().get_string(55U).value() == "AAPL");
        const auto parties = moved_event.application_messages.front().view().group(453U);
        REQUIRE(parties.has_value());
        REQUIRE(parties->size() == 1U);
        REQUIRE((*parties)[0].get_char(447U).value() == 'D');
        REQUIRE((*parties)[0].get_string(448U).value() == "PTY1");
    }

}

TEST_CASE("PossResend flag detected on app message", "[admin-protocol]") {
    auto dictionary = LoadAdminDictionary();
    REQUIRE(dictionary.ok());

    fastfix::store::MemorySessionStore store;
    fastfix::session::AdminProtocol protocol(
        fastfix::session::AdminProtocolConfig{
            .session = fastfix::session::SessionConfig{
                .session_id = 6001U,
                .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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

    fastfix::message::MessageBuilder app_builder("D");
    app_builder.set_string(35U, "D").set_string(55U, "MSFT").set_boolean(97U, true);
    auto inbound = EncodeInboundFrame(
        std::move(app_builder).build(),
        dictionary.value(),
        "FIX.4.4",
        "BUY",
        "SELL",
        2U,
        false);
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().application_messages.size() == 1U);
    REQUIRE(event.value().poss_resend);
}

TEST_CASE("PossResend flag not set on normal message", "[admin-protocol]") {
    auto dictionary = LoadAdminDictionary();
    REQUIRE(dictionary.ok());

    fastfix::store::MemorySessionStore store;
    fastfix::session::AdminProtocol protocol(
        fastfix::session::AdminProtocolConfig{
            .session = fastfix::session::SessionConfig{
                .session_id = 6002U,
                .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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

    fastfix::message::MessageBuilder app_builder("D");
    app_builder.set_string(35U, "D").set_string(55U, "AAPL");
    auto inbound = EncodeInboundFrame(
        std::move(app_builder).build(),
        dictionary.value(),
        "FIX.4.4",
        "BUY",
        "SELL",
        2U,
        false);
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().application_messages.size() == 1U);
    REQUIRE_FALSE(event.value().poss_resend);
}

TEST_CASE("PossResend message still processed by application", "[admin-protocol]") {
    auto dictionary = LoadAdminDictionary();
    REQUIRE(dictionary.ok());

    fastfix::store::MemorySessionStore store;
    fastfix::session::AdminProtocol protocol(
        fastfix::session::AdminProtocolConfig{
            .session = fastfix::session::SessionConfig{
                .session_id = 6003U,
                .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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

    fastfix::message::MessageBuilder app_builder("D");
    app_builder.set_string(35U, "D").set_string(55U, "GOOG").set_boolean(97U, true);
    auto inbound = EncodeInboundFrame(
        std::move(app_builder).build(),
        dictionary.value(),
        "FIX.4.4",
        "BUY",
        "SELL",
        2U,
        false);
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 10U);
    REQUIRE(event.ok());
    // PossResend messages must NOT be silently dropped — they are passed to the application layer.
    REQUIRE(event.value().application_messages.size() == 1U);
    REQUIRE(event.value().poss_resend);
    // The application message content is intact.
    REQUIRE(event.value().application_messages.front().view().get_string(55U).value() == "GOOG");
    REQUIRE(event.value().application_messages.front().view().get_boolean(97U).value());
    // No reject or disconnect — the message is delivered normally.
    REQUIRE(event.value().outbound_frames.empty());
    REQUIRE_FALSE(event.value().disconnect);
}

// ==================== ResendRequest Tests ====================

TEST_CASE("ResendRequest happy path", "[admin-protocol]") {
    auto dictionary = LoadAdminDictionary();
    REQUIRE(dictionary.ok());

    fastfix::store::MemorySessionStore store;
    fastfix::session::AdminProtocol protocol(
        fastfix::session::AdminProtocolConfig{
            .session = fastfix::session::SessionConfig{
                .session_id = 7001U,
                .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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
        fastfix::message::MessageBuilder app_builder("D");
        app_builder.set_string(35U, "D").set_string(55U, "AAPL");
        auto sent = protocol.SendApplication(
            std::move(app_builder).build(), 100U + static_cast<std::uint64_t>(i));
        REQUIRE(sent.ok());
    }
    REQUIRE(protocol.session().Snapshot().next_out_seq == 5U);

    // Counterparty requests resend of seqs 2-4
    fastfix::message::MessageBuilder resend_builder("2");
    resend_builder.set_string(35U, "2").set_int(7U, 2).set_int(16U, 4);
    auto inbound = EncodeInboundFrame(
        std::move(resend_builder).build(),
        dictionary.value(),
        "FIX.4.4",
        "BUY",
        "SELL",
        2U,
        false);
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 200U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.size() == 3U);
    REQUIRE(!event.value().disconnect);

    // All replayed frames should be app messages with PossDup
    for (std::size_t i = 0U; i < 3U; ++i) {
        auto replayed = fastfix::codec::DecodeFixMessage(
            event.value().outbound_frames[i].bytes, dictionary.value());
        REQUIRE(replayed.ok());
        REQUIRE(replayed.value().header.msg_type == "D");
        REQUIRE(replayed.value().header.poss_dup);
    }
}

TEST_CASE("ResendRequest BeginSeqNo=0 rejected", "[admin-protocol]") {
    auto dictionary = LoadAdminDictionary();
    REQUIRE(dictionary.ok());

    fastfix::store::MemorySessionStore store;
    fastfix::session::AdminProtocol protocol(
        fastfix::session::AdminProtocolConfig{
            .session = fastfix::session::SessionConfig{
                .session_id = 7002U,
                .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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
    fastfix::message::MessageBuilder resend_builder("2");
    resend_builder.set_string(35U, "2").set_int(7U, 0).set_int(16U, 5);
    auto inbound = EncodeInboundFrame(
        std::move(resend_builder).build(),
        dictionary.value(),
        "FIX.4.4",
        "BUY",
        "SELL",
        2U,
        false);
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 200U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.size() == 1U);
    REQUIRE(!event.value().disconnect);

    auto decoded = fastfix::codec::DecodeFixMessage(
        event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "3");
    REQUIRE(decoded.value().message.view().get_int(371U).value() == 7);
    REQUIRE(decoded.value().message.view().get_string(372U).value() == "2");
    REQUIRE(decoded.value().message.view().get_int(373U).value() == 5);
}

TEST_CASE("ResendRequest EndSeqNo=0 replays to infinity", "[admin-protocol]") {
    auto dictionary = LoadAdminDictionary();
    REQUIRE(dictionary.ok());

    fastfix::store::MemorySessionStore store;
    fastfix::session::AdminProtocol protocol(
        fastfix::session::AdminProtocolConfig{
            .session = fastfix::session::SessionConfig{
                .session_id = 7003U,
                .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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
        fastfix::message::MessageBuilder app_builder("D");
        app_builder.set_string(35U, "D").set_string(55U, "AAPL");
        auto sent = protocol.SendApplication(
            std::move(app_builder).build(), 100U + static_cast<std::uint64_t>(i));
        REQUIRE(sent.ok());
    }
    REQUIRE(protocol.session().Snapshot().next_out_seq == 4U);

    // EndSeqNo=0 means "all messages from BeginSeqNo to end"
    fastfix::message::MessageBuilder resend_builder("2");
    resend_builder.set_string(35U, "2").set_int(7U, 1).set_int(16U, 0);
    auto inbound = EncodeInboundFrame(
        std::move(resend_builder).build(),
        dictionary.value(),
        "FIX.4.4",
        "BUY",
        "SELL",
        2U,
        false);
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 200U);
    REQUIRE(event.ok());
    REQUIRE(!event.value().disconnect);

    // Seq 1 is logon (admin) → GapFill; seqs 2,3 are app → replay
    REQUIRE(event.value().outbound_frames.size() == 3U);

    auto gap_fill = fastfix::codec::DecodeFixMessage(
        event.value().outbound_frames[0U].bytes, dictionary.value());
    REQUIRE(gap_fill.ok());
    REQUIRE(gap_fill.value().header.msg_type == "4");
    REQUIRE(gap_fill.value().message.view().get_boolean(123U).value());
    REQUIRE(gap_fill.value().message.view().get_int(36U).value() == 2);

    for (std::size_t i = 1U; i < event.value().outbound_frames.size(); ++i) {
        auto replayed = fastfix::codec::DecodeFixMessage(
            event.value().outbound_frames[i].bytes, dictionary.value());
        REQUIRE(replayed.ok());
        REQUIRE(replayed.value().header.msg_type == "D");
        REQUIRE(replayed.value().header.poss_dup);
    }
}

// ==================== TestRequest Tests ====================

TEST_CASE("TestRequest happy path", "[admin-protocol]") {
    auto dictionary = LoadAdminDictionary();
    REQUIRE(dictionary.ok());

    fastfix::store::MemorySessionStore store;
    fastfix::session::AdminProtocol protocol(
        fastfix::session::AdminProtocolConfig{
            .session = fastfix::session::SessionConfig{
                .session_id = 7004U,
                .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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
    fastfix::message::MessageBuilder test_builder("1");
    test_builder.set_string(35U, "1").set_string(112U, "HELLO-123");
    auto inbound = EncodeInboundFrame(
        std::move(test_builder).build(),
        dictionary.value(),
        "FIX.4.4",
        "BUY",
        "SELL",
        2U,
        false);
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.size() == 1U);
    REQUIRE(!event.value().disconnect);

    // Verify Heartbeat response echoes the TestReqID
    auto decoded = fastfix::codec::DecodeFixMessage(
        event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "0");
    REQUIRE(decoded.value().message.view().get_string(112U).value() == "HELLO-123");
}

TEST_CASE("TestRequest timeout triggers disconnect", "[admin-protocol]") {
    auto dictionary = LoadAdminDictionary();
    REQUIRE(dictionary.ok());

    fastfix::store::MemorySessionStore store;
    fastfix::session::AdminProtocol protocol(
        fastfix::session::AdminProtocolConfig{
            .session = fastfix::session::SessionConfig{
                .session_id = 7005U,
                .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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
    // After 2 * heartbeat_interval without inbound → protocol sends TestRequest.
    constexpr std::uint64_t kIntervalNs = 30ULL * 1'000'000'000ULL;
    const std::uint64_t ts_test_request = 2U + 2U * kIntervalNs + 1U;

    auto timer1 = protocol.OnTimer(ts_test_request);
    REQUIRE(timer1.ok());
    REQUIRE(timer1.value().outbound_frames.size() == 1U);
    REQUIRE(!timer1.value().disconnect);

    auto test_request = fastfix::codec::DecodeFixMessage(
        timer1.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(test_request.ok());
    REQUIRE(test_request.value().header.msg_type == "1");
    REQUIRE(test_request.value().message.view().has_field(112U));

    // No Heartbeat response within another heartbeat_interval → disconnect
    auto timer2 = protocol.OnTimer(ts_test_request + kIntervalNs + 1U);
    REQUIRE(timer2.ok());
    REQUIRE(timer2.value().disconnect);
    REQUIRE(timer2.value().outbound_frames.empty());
}

TEST_CASE("TestRequest duplicate handling", "[admin-protocol]") {
    auto dictionary = LoadAdminDictionary();
    REQUIRE(dictionary.ok());

    fastfix::store::MemorySessionStore store;
    fastfix::session::AdminProtocol protocol(
        fastfix::session::AdminProtocolConfig{
            .session = fastfix::session::SessionConfig{
                .session_id = 7006U,
                .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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
    fastfix::message::MessageBuilder test1("1");
    test1.set_string(35U, "1").set_string(112U, "DUP-REQ");
    auto inbound1 = EncodeInboundFrame(
        std::move(test1).build(), dictionary.value(),
        "FIX.4.4", "BUY", "SELL", 2U, false);
    REQUIRE(inbound1.ok());

    auto event1 = protocol.OnInbound(inbound1.value(), 10U);
    REQUIRE(event1.ok());
    REQUIRE(event1.value().outbound_frames.size() == 1U);

    auto hb1 = fastfix::codec::DecodeFixMessage(
        event1.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(hb1.ok());
    REQUIRE(hb1.value().header.msg_type == "0");
    REQUIRE(hb1.value().message.view().get_string(112U).value() == "DUP-REQ");

    // Second TestRequest with same TestReqID at next seq
    fastfix::message::MessageBuilder test2("1");
    test2.set_string(35U, "1").set_string(112U, "DUP-REQ");
    auto inbound2 = EncodeInboundFrame(
        std::move(test2).build(), dictionary.value(),
        "FIX.4.4", "BUY", "SELL", 3U, false);
    REQUIRE(inbound2.ok());

    auto event2 = protocol.OnInbound(inbound2.value(), 20U);
    REQUIRE(event2.ok());
    REQUIRE(event2.value().outbound_frames.size() == 1U);

    auto hb2 = fastfix::codec::DecodeFixMessage(
        event2.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(hb2.ok());
    REQUIRE(hb2.value().header.msg_type == "0");
    REQUIRE(hb2.value().message.view().get_string(112U).value() == "DUP-REQ");
}

// ==================== SequenceReset Tests ====================

TEST_CASE("SequenceReset-Reset happy path", "[admin-protocol]") {
    auto dictionary = LoadAdminDictionary();
    REQUIRE(dictionary.ok());

    fastfix::store::MemorySessionStore store;
    fastfix::session::AdminProtocol protocol(
        fastfix::session::AdminProtocolConfig{
            .session = fastfix::session::SessionConfig{
                .session_id = 7007U,
                .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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
    fastfix::message::MessageBuilder reset_builder("4");
    reset_builder.set_string(35U, "4").set_int(36U, 10);
    auto inbound = EncodeInboundFrame(
        std::move(reset_builder).build(),
        dictionary.value(),
        "FIX.4.4",
        "BUY",
        "SELL",
        2U,
        false);
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.empty());
    REQUIRE(!event.value().disconnect);

    const auto after = protocol.session().Snapshot();
    REQUIRE(after.next_in_seq == 10U);
    REQUIRE(after.state == fastfix::session::SessionState::kActive);
}

TEST_CASE("SequenceReset backward rejected", "[admin-protocol]") {
    auto dictionary = LoadAdminDictionary();
    REQUIRE(dictionary.ok());

    fastfix::store::MemorySessionStore store;
    fastfix::session::AdminProtocol protocol(
        fastfix::session::AdminProtocolConfig{
            .session = fastfix::session::SessionConfig{
                .session_id = 7008U,
                .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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
    fastfix::message::MessageBuilder reset_builder("4");
    reset_builder.set_string(35U, "4").set_int(36U, 1);
    auto inbound = EncodeInboundFrame(
        std::move(reset_builder).build(),
        dictionary.value(),
        "FIX.4.4",
        "BUY",
        "SELL",
        2U,
        false);
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.size() == 1U);
    REQUIRE(!event.value().disconnect);

    auto decoded = fastfix::codec::DecodeFixMessage(
        event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().header.msg_type == "3");
    REQUIRE(decoded.value().message.view().get_int(371U).value() == 36);
    REQUIRE(decoded.value().message.view().get_string(372U).value() == "4");
    REQUIRE(decoded.value().message.view().get_int(373U).value() == 5);
}

TEST_CASE("SequenceReset-GapFill happy path", "[admin-protocol]") {
    auto dictionary = LoadAdminDictionary();
    REQUIRE(dictionary.ok());

    fastfix::store::MemorySessionStore store;
    fastfix::session::AdminProtocol protocol(
        fastfix::session::AdminProtocolConfig{
            .session = fastfix::session::SessionConfig{
                .session_id = 7009U,
                .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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
    fastfix::message::MessageBuilder gap_fill_builder("4");
    gap_fill_builder.set_string(35U, "4").set_boolean(123U, true).set_int(36U, 5);
    auto inbound = EncodeInboundFrame(
        std::move(gap_fill_builder).build(),
        dictionary.value(),
        "FIX.4.4",
        "BUY",
        "SELL",
        2U,
        false);
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.empty());
    REQUIRE(!event.value().disconnect);

    const auto after = protocol.session().Snapshot();
    REQUIRE(after.next_in_seq == 5U);
    REQUIRE(after.state == fastfix::session::SessionState::kActive);
}

TEST_CASE("SequenceReset-GapFill partial fill", "[admin-protocol]") {
    auto dictionary = LoadAdminDictionary();
    REQUIRE(dictionary.ok());

    fastfix::store::MemorySessionStore store;
    fastfix::session::AdminProtocol protocol(
        fastfix::session::AdminProtocolConfig{
            .session = fastfix::session::SessionConfig{
                .session_id = 7010U,
                .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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
    fastfix::message::MessageBuilder heartbeat_builder("0");
    heartbeat_builder.set_string(35U, "0");
    auto gap_inbound = EncodeInboundFrame(
        std::move(heartbeat_builder).build(),
        dictionary.value(),
        "FIX.4.4",
        "BUY",
        "SELL",
        5U,
        false);
    REQUIRE(gap_inbound.ok());

    auto gap_event = protocol.OnInbound(gap_inbound.value(), 10U);
    REQUIRE(gap_event.ok());
    REQUIRE(gap_event.value().outbound_frames.size() == 1U);

    auto resend_req = fastfix::codec::DecodeFixMessage(
        gap_event.value().outbound_frames.front().bytes, dictionary.value());
    REQUIRE(resend_req.ok());
    REQUIRE(resend_req.value().header.msg_type == "2");
    REQUIRE(resend_req.value().message.view().get_int(7U).value() == 2);
    REQUIRE(resend_req.value().message.view().get_int(16U).value() == 4);

    // Partial GapFill: covers seqs 2-3 only (NewSeqNo=4), seq 4 still missing
    fastfix::message::MessageBuilder gap_fill_builder("4");
    gap_fill_builder.set_string(35U, "4").set_boolean(123U, true).set_int(36U, 4);
    auto gap_fill_inbound = EncodeInboundFrame(
        std::move(gap_fill_builder).build(),
        dictionary.value(),
        "FIX.4.4",
        "BUY",
        "SELL",
        2U,
        false);
    REQUIRE(gap_fill_inbound.ok());

    auto fill_event = protocol.OnInbound(gap_fill_inbound.value(), 20U);
    REQUIRE(fill_event.ok());
    REQUIRE(fill_event.value().outbound_frames.empty());
    REQUIRE(!fill_event.value().disconnect);

    // Gap not fully resolved — session stays in ResendProcessing
    const auto after = protocol.session().Snapshot();
    REQUIRE(after.state == fastfix::session::SessionState::kResendProcessing);
    REQUIRE(after.has_pending_resend);
    REQUIRE(after.next_in_seq == 4U);
}

// ==================== Reject Tests ====================

TEST_CASE("Reject received processed silently", "[admin-protocol]") {
    auto dictionary = LoadAdminDictionary();
    REQUIRE(dictionary.ok());

    fastfix::store::MemorySessionStore store;
    fastfix::session::AdminProtocol protocol(
        fastfix::session::AdminProtocolConfig{
            .session = fastfix::session::SessionConfig{
                .session_id = 7011U,
                .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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
    fastfix::message::MessageBuilder reject_builder("3");
    reject_builder.set_string(35U, "3")
        .set_int(45U, 1)
        .set_string(58U, "invalid message");
    auto inbound = EncodeInboundFrame(
        std::move(reject_builder).build(),
        dictionary.value(),
        "FIX.4.4",
        "BUY",
        "SELL",
        2U,
        false);
    REQUIRE(inbound.ok());

    auto event = protocol.OnInbound(inbound.value(), 10U);
    REQUIRE(event.ok());
    REQUIRE(event.value().outbound_frames.empty());
    REQUIRE(!event.value().disconnect);
    REQUIRE(event.value().application_messages.empty());

    const auto after = protocol.session().Snapshot();
    REQUIRE(after.next_in_seq == 3U);
}