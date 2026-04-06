#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include "fastfix/session/resend_recovery.h"

#include "fastfix/store/durable_batch_store.h"
#include "fastfix/store/memory_store.h"

#include "test_support.h"

namespace {

auto Payload(std::string_view text) -> std::vector<std::byte> {
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (const auto ch : text) {
        bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
    }
    return bytes;
}

}  // namespace

TEST_CASE("resend-recovery", "[resend-recovery]") {
    fastfix::store::MemorySessionStore store;
    REQUIRE(store.SaveOutbound(
        fastfix::store::MessageRecord{
            .session_id = 900U,
            .seq_num = 1U,
            .timestamp_ns = 100U,
            .flags = 0U,
            .payload = Payload("app-1"),
        }).ok());
    REQUIRE(store.SaveOutbound(
        fastfix::store::MessageRecord{
            .session_id = 900U,
            .seq_num = 2U,
            .timestamp_ns = 200U,
            .flags = static_cast<std::uint16_t>(fastfix::store::MessageRecordFlags::kAdmin),
            .payload = Payload("admin-2"),
        }).ok());
    REQUIRE(store.SaveOutbound(
        fastfix::store::MessageRecord{
            .session_id = 900U,
            .seq_num = 4U,
            .timestamp_ns = 400U,
            .flags = 0U,
            .payload = Payload("app-4"),
        }).ok());

    auto plan = fastfix::session::BuildReplayPlan(store, 900U, 1U, 4U);
    REQUIRE(plan.ok());
    REQUIRE(plan.value().chunks.size() == 3U);
    REQUIRE(plan.value().chunks[0].kind == fastfix::session::ReplayActionKind::kReplay);
    REQUIRE(plan.value().chunks[0].begin_seq == 1U);
    REQUIRE(plan.value().chunks[0].messages.size() == 1U);
    REQUIRE(plan.value().chunks[1].kind == fastfix::session::ReplayActionKind::kGapFill);
    REQUIRE(plan.value().chunks[1].begin_seq == 2U);
    REQUIRE(plan.value().chunks[1].end_seq == 3U);
    REQUIRE(plan.value().chunks[2].kind == fastfix::session::ReplayActionKind::kReplay);
    REQUIRE(plan.value().chunks[2].begin_seq == 4U);
    REQUIRE(plan.value().chunks[2].messages.size() == 1U);

    fastfix::session::SessionConfig config;
    config.session_id = 900U;
    config.profile_id = 1001U;
    config.key.begin_string = "FIX.4.4";
    config.key.sender_comp_id = "BUY";
    config.key.target_comp_id = "SELL";

    fastfix::session::SessionCore session(config);
    REQUIRE(store.SaveRecoveryState(
        fastfix::store::SessionRecoveryState{
            .session_id = 900U,
            .next_in_seq = 7U,
            .next_out_seq = 11U,
            .last_inbound_ns = 555U,
            .last_outbound_ns = 777U,
            .active = false,
        }).ok());
    REQUIRE(fastfix::session::RecoverSession(
        session, store, fastfix::session::RecoveryMode::kWarmRestart).ok());

    auto snapshot = session.Snapshot();
    REQUIRE(snapshot.state == fastfix::session::SessionState::kDisconnected);
    REQUIRE(snapshot.next_in_seq == 7U);
    REQUIRE(snapshot.next_out_seq == 11U);
    REQUIRE(snapshot.last_inbound_ns == 555U);
    REQUIRE(snapshot.last_outbound_ns == 777U);

    REQUIRE(session.OnTransportConnected().ok());
    REQUIRE(session.OnLogonAccepted().ok());
    REQUIRE(!session.ObserveInboundSeq(9U).ok());

    snapshot = session.Snapshot();
    REQUIRE(snapshot.state == fastfix::session::SessionState::kResendProcessing);
    REQUIRE(snapshot.has_pending_resend);
    REQUIRE(snapshot.pending_resend.begin_seq == 7U);
    REQUIRE(snapshot.pending_resend.end_seq == 8U);

    REQUIRE(session.CompleteResend().ok());
    REQUIRE(session.ObserveInboundSeq(7U).ok());

    const auto durable_root = std::filesystem::temp_directory_path() / "fastfix-durable-resend-recovery-test";
    std::filesystem::remove_all(durable_root);

    {
        fastfix::store::DurableBatchSessionStore durable_store(
            durable_root,
            fastfix::store::DurableBatchStoreOptions{
                .flush_threshold = 1U,
                .rollover_mode = fastfix::store::DurableStoreRolloverMode::kExternal,
                .max_archived_segments = 4U,
            });
        REQUIRE(durable_store.Open().ok());
        REQUIRE(durable_store.SaveOutbound(
            fastfix::store::MessageRecord{
                .session_id = 901U,
                .seq_num = 1U,
                .timestamp_ns = 100U,
                .flags = 0U,
                .payload = Payload("archived-app-1"),
            }).ok());
        REQUIRE(durable_store.SaveOutbound(
            fastfix::store::MessageRecord{
                .session_id = 901U,
                .seq_num = 2U,
                .timestamp_ns = 200U,
                .flags = static_cast<std::uint16_t>(fastfix::store::MessageRecordFlags::kAdmin),
                .payload = Payload("archived-admin-2"),
            }).ok());
        REQUIRE(durable_store.Rollover().ok());
        REQUIRE(durable_store.SaveOutbound(
            fastfix::store::MessageRecord{
                .session_id = 901U,
                .seq_num = 4U,
                .timestamp_ns = 400U,
                .flags = 0U,
                .payload = Payload("active-app-4"),
            }).ok());
        REQUIRE(durable_store.SaveRecoveryState(
            fastfix::store::SessionRecoveryState{
                .session_id = 901U,
                .next_in_seq = 17U,
                .next_out_seq = 23U,
                .last_inbound_ns = 555U,
                .last_outbound_ns = 777U,
                .active = false,
            }).ok());
    }

    {
        fastfix::store::DurableBatchSessionStore durable_store(
            durable_root,
            fastfix::store::DurableBatchStoreOptions{
                .flush_threshold = 1U,
                .rollover_mode = fastfix::store::DurableStoreRolloverMode::kExternal,
                .max_archived_segments = 4U,
            });
        REQUIRE(durable_store.Open().ok());

        auto durable_plan = fastfix::session::BuildReplayPlan(durable_store, 901U, 1U, 4U);
        REQUIRE(durable_plan.ok());
        REQUIRE(durable_plan.value().chunks.size() == 3U);
        REQUIRE(durable_plan.value().chunks[0].kind == fastfix::session::ReplayActionKind::kReplay);
        REQUIRE(durable_plan.value().chunks[0].begin_seq == 1U);
        REQUIRE(durable_plan.value().chunks[0].end_seq == 1U);
        REQUIRE(durable_plan.value().chunks[0].messages.size() == 1U);
        REQUIRE(durable_plan.value().chunks[1].kind == fastfix::session::ReplayActionKind::kGapFill);
        REQUIRE(durable_plan.value().chunks[1].begin_seq == 2U);
        REQUIRE(durable_plan.value().chunks[1].end_seq == 3U);
        REQUIRE(durable_plan.value().chunks[2].kind == fastfix::session::ReplayActionKind::kReplay);
        REQUIRE(durable_plan.value().chunks[2].begin_seq == 4U);
        REQUIRE(durable_plan.value().chunks[2].end_seq == 4U);
        REQUIRE(durable_plan.value().chunks[2].messages.size() == 1U);

        auto durable_config = config;
        durable_config.session_id = 901U;
        fastfix::session::SessionCore durable_session(durable_config);
        REQUIRE(fastfix::session::RecoverSession(
            durable_session, durable_store, fastfix::session::RecoveryMode::kWarmRestart).ok());

        auto durable_snapshot = durable_session.Snapshot();
        REQUIRE(durable_snapshot.next_in_seq == 17U);
        REQUIRE(durable_snapshot.next_out_seq == 23U);
        REQUIRE(durable_snapshot.last_inbound_ns == 555U);
        REQUIRE(durable_snapshot.last_outbound_ns == 777U);

        REQUIRE(durable_session.OnTransportConnected().ok());
        REQUIRE(durable_session.OnLogonAccepted().ok());
        REQUIRE(!durable_session.ObserveInboundSeq(19U).ok());

        durable_snapshot = durable_session.Snapshot();
        REQUIRE(durable_snapshot.state == fastfix::session::SessionState::kResendProcessing);
        REQUIRE(durable_snapshot.has_pending_resend);
        REQUIRE(durable_snapshot.pending_resend.begin_seq == 17U);
        REQUIRE(durable_snapshot.pending_resend.end_seq == 18U);
    }

    std::filesystem::remove_all(durable_root);
}