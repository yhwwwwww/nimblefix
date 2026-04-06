#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include "fastfix/store/durable_batch_store.h"
#include "fastfix/store/memory_store.h"
#include "fastfix/store/mmap_store.h"

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

auto CountArchivedLogs(const std::filesystem::path& root) -> std::size_t {
    const auto archive_dir = root / "archive";
    if (!std::filesystem::exists(archive_dir)) {
        return 0U;
    }

    std::size_t count = 0U;
    for (const auto& entry : std::filesystem::directory_iterator(archive_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".log") {
            ++count;
        }
    }
    return count;
}

}  // namespace

TEST_CASE("store-recovery", "[store-recovery]") {
    fastfix::store::MemorySessionStore memory_store;
    fastfix::store::MessageRecord outbound{
        .session_id = 77U,
        .seq_num = 1U,
        .timestamp_ns = 1000U,
        .flags = 0U,
        .payload = Payload("8=FIX.4.4|35=D|"),
    };
    REQUIRE(memory_store.SaveOutbound(outbound).ok());
    REQUIRE(memory_store.SaveRecoveryState(
        fastfix::store::SessionRecoveryState{
            .session_id = 77U,
            .next_in_seq = 12U,
            .next_out_seq = 19U,
            .last_inbound_ns = 222U,
            .last_outbound_ns = 333U,
            .active = true,
        }).ok());

    auto loaded_memory = memory_store.LoadOutboundRange(77U, 1U, 1U);
    REQUIRE(loaded_memory.ok());
    REQUIRE(loaded_memory.value().size() == 1U);
    REQUIRE(loaded_memory.value().front().payload.size() == outbound.payload.size());

    auto loaded_memory_views = memory_store.LoadOutboundRangeViews(77U, 1U, 1U);
    REQUIRE(loaded_memory_views.ok());
    REQUIRE(loaded_memory_views.value().records.size() == 1U);
    REQUIRE(loaded_memory_views.value().owned_storage.empty());
    REQUIRE(loaded_memory_views.value().records.front().seq_num == 1U);
    REQUIRE(loaded_memory_views.value().records.front().payload.size() == outbound.payload.size());

    auto memory_state = memory_store.LoadRecoveryState(77U);
    REQUIRE(memory_state.ok());
    REQUIRE(memory_state.value().next_in_seq == 12U);
    REQUIRE(memory_state.value().next_out_seq == 19U);

    const auto path = std::filesystem::temp_directory_path() / "fastfix-mmap-store-test.dat";
    std::filesystem::remove(path);

    {
        fastfix::store::MmapSessionStore mmap_store(path);
        REQUIRE(mmap_store.Open().ok());
        REQUIRE(mmap_store.SaveOutbound(outbound).ok());
        REQUIRE(mmap_store.SaveRecoveryState(
            fastfix::store::SessionRecoveryState{
                .session_id = 77U,
                .next_in_seq = 21U,
                .next_out_seq = 34U,
                .last_inbound_ns = 444U,
                .last_outbound_ns = 555U,
                .active = false,
            }).ok());
    }

    {
        fastfix::store::MmapSessionStore mmap_store(path);
        REQUIRE(mmap_store.Open().ok());
        auto loaded = mmap_store.LoadOutboundRange(77U, 1U, 1U);
        REQUIRE(loaded.ok());
        REQUIRE(loaded.value().size() == 1U);
        REQUIRE(loaded.value().front().seq_num == 1U);
        REQUIRE(loaded.value().front().payload.size() == outbound.payload.size());

        auto loaded_views = mmap_store.LoadOutboundRangeViews(77U, 1U, 1U);
        REQUIRE(loaded_views.ok());
        REQUIRE(loaded_views.value().records.size() == 1U);
        REQUIRE(loaded_views.value().owned_storage.empty());
        REQUIRE(loaded_views.value().records.front().seq_num == 1U);
        REQUIRE(loaded_views.value().records.front().payload.size() == outbound.payload.size());

        auto recovery = mmap_store.LoadRecoveryState(77U);
        REQUIRE(recovery.ok());
        REQUIRE(recovery.value().next_in_seq == 21U);
        REQUIRE(recovery.value().next_out_seq == 34U);
    }

    std::filesystem::remove(path);

    const auto durable_root = std::filesystem::temp_directory_path() / "fastfix-durable-store-test";
    std::filesystem::remove_all(durable_root);

    constexpr std::uint64_t kDay = 86'400ULL * 1'000'000'000ULL;
    {
        fastfix::store::DurableBatchSessionStore durable_store(
            durable_root,
            fastfix::store::DurableBatchStoreOptions{
                .flush_threshold = 2U,
                .rollover_mode = fastfix::store::DurableStoreRolloverMode::kUtcDay,
                .max_archived_segments = 1U,
            });
        REQUIRE(durable_store.Open().ok());
        REQUIRE(durable_store.SaveOutbound(
            fastfix::store::MessageRecord{
                .session_id = 88U,
                .seq_num = 1U,
                .timestamp_ns = 10U,
                .flags = 0U,
                .payload = Payload("8=FIX.4.4|35=D|11=AAA|"),
            }).ok());
        REQUIRE(durable_store.SaveRecoveryState(
            fastfix::store::SessionRecoveryState{
                .session_id = 88U,
                .next_in_seq = 4U,
                .next_out_seq = 9U,
                .last_inbound_ns = 12U,
                .last_outbound_ns = 13U,
                .active = true,
            }).ok());
        REQUIRE(std::filesystem::exists(durable_root / "active.log"));
        REQUIRE(durable_store.SaveOutbound(
            fastfix::store::MessageRecord{
                .session_id = 88U,
                .seq_num = 2U,
                .timestamp_ns = 100U,
                .flags = 0U,
                .payload = Payload("8=FIX.4.4|35=D|11=BBB|"),
            }).ok());
        REQUIRE(durable_store.SaveOutbound(
            fastfix::store::MessageRecord{
                .session_id = 88U,
                .seq_num = 3U,
                .timestamp_ns = kDay + 200U,
                .flags = 0U,
                .payload = Payload("8=FIX.4.4|35=D|11=CCC|"),
            }).ok());
        REQUIRE(durable_store.SaveOutbound(
            fastfix::store::MessageRecord{
                .session_id = 88U,
                .seq_num = 4U,
                .timestamp_ns = (2U * kDay) + 300U,
                .flags = 0U,
                .payload = Payload("8=FIX.4.4|35=D|11=DDD|"),
            }).ok());
        REQUIRE(durable_store.SaveRecoveryState(
            fastfix::store::SessionRecoveryState{
                .session_id = 88U,
                .next_in_seq = 8U,
                .next_out_seq = 15U,
                .last_inbound_ns = (2U * kDay) + 301U,
                .last_outbound_ns = (2U * kDay) + 302U,
                .active = false,
            }).ok());
        REQUIRE(durable_store.Flush().ok());
    }

    {
        fastfix::store::DurableBatchSessionStore durable_store(
            durable_root,
            fastfix::store::DurableBatchStoreOptions{
                .flush_threshold = 2U,
                .rollover_mode = fastfix::store::DurableStoreRolloverMode::kUtcDay,
                .max_archived_segments = 1U,
            });
        REQUIRE(durable_store.Open().ok());
        auto recent = durable_store.LoadOutboundRange(88U, 2U, 4U);
        REQUIRE(recent.ok());
        REQUIRE(recent.value().size() == 2U);
        REQUIRE(recent.value()[0].seq_num == 3U);
        REQUIRE(recent.value()[1].seq_num == 4U);

        auto recent_views = durable_store.LoadOutboundRangeViews(88U, 2U, 4U);
        REQUIRE(recent_views.ok());
        REQUIRE(recent_views.value().records.size() == 2U);
        REQUIRE(recent_views.value().owned_storage.empty());
        REQUIRE(!recent_views.value().payload_storage.empty());
        REQUIRE(recent_views.value().records[0].seq_num == 3U);
        REQUIRE(recent_views.value().records[1].seq_num == 4U);

        auto pruned = durable_store.LoadOutboundRange(88U, 1U, 2U);
        REQUIRE(pruned.ok());
        REQUIRE(pruned.value().empty());

        auto recovery = durable_store.LoadRecoveryState(88U);
        REQUIRE(recovery.ok());
        REQUIRE(recovery.value().next_in_seq == 8U);
        REQUIRE(recovery.value().next_out_seq == 15U);
        REQUIRE(!recovery.value().active);
        REQUIRE(CountArchivedLogs(durable_root) == 1U);
    }

    const auto external_root = std::filesystem::temp_directory_path() / "fastfix-durable-store-external-test";
    std::filesystem::remove_all(external_root);
    {
        fastfix::store::DurableBatchSessionStore durable_store(
            external_root,
            fastfix::store::DurableBatchStoreOptions{
                .flush_threshold = 1U,
                .rollover_mode = fastfix::store::DurableStoreRolloverMode::kExternal,
                .max_archived_segments = 2U,
            });
        REQUIRE(durable_store.Open().ok());
        REQUIRE(durable_store.SaveOutbound(
            fastfix::store::MessageRecord{
                .session_id = 91U,
                .seq_num = 1U,
                .timestamp_ns = 1U,
                .flags = 0U,
                .payload = Payload("8=FIX.4.4|35=D|11=EXT1|"),
            }).ok());
        REQUIRE(durable_store.Rollover().ok());
        REQUIRE(durable_store.SaveOutbound(
            fastfix::store::MessageRecord{
                .session_id = 91U,
                .seq_num = 2U,
                .timestamp_ns = 2U,
                .flags = 0U,
                .payload = Payload("8=FIX.4.4|35=D|11=EXT2|"),
            }).ok());
    }

    {
        fastfix::store::DurableBatchSessionStore durable_store(
            external_root,
            fastfix::store::DurableBatchStoreOptions{
                .flush_threshold = 1U,
                .rollover_mode = fastfix::store::DurableStoreRolloverMode::kExternal,
                .max_archived_segments = 2U,
            });
        REQUIRE(durable_store.Open().ok());
        auto loaded = durable_store.LoadOutboundRange(91U, 1U, 2U);
        REQUIRE(loaded.ok());
        REQUIRE(loaded.value().size() == 2U);

        auto loaded_views = durable_store.LoadOutboundRangeViews(91U, 1U, 2U);
        REQUIRE(loaded_views.ok());
        REQUIRE(loaded_views.value().records.size() == 2U);
        REQUIRE(loaded_views.value().owned_storage.empty());
        REQUIRE(CountArchivedLogs(external_root) == 1U);
    }

    const auto crash_root = std::filesystem::temp_directory_path() / "fastfix-durable-store-crash-test";
    std::filesystem::remove_all(crash_root);
    {
        fastfix::store::DurableBatchSessionStore durable_store(
            crash_root,
            fastfix::store::DurableBatchStoreOptions{
                .flush_threshold = 3U,
                .rollover_mode = fastfix::store::DurableStoreRolloverMode::kExternal,
                .max_archived_segments = 2U,
            });
        REQUIRE(durable_store.Open().ok());
        REQUIRE(durable_store.SaveOutbound(
            fastfix::store::MessageRecord{
                .session_id = 200U,
                .seq_num = 1U,
                .timestamp_ns = 10U,
                .flags = 0U,
                .payload = Payload("8=FIX.4.4|35=D|11=CRASH1|"),
            }).ok());
        REQUIRE(durable_store.SaveOutbound(
            fastfix::store::MessageRecord{
                .session_id = 200U,
                .seq_num = 2U,
                .timestamp_ns = 11U,
                .flags = 0U,
                .payload = Payload("8=FIX.4.4|35=D|11=CRASH2|"),
            }).ok());
        REQUIRE(durable_store.SaveRecoveryState(
            fastfix::store::SessionRecoveryState{
                .session_id = 200U,
                .next_in_seq = 41U,
                .next_out_seq = 77U,
                .last_inbound_ns = 910U,
                .last_outbound_ns = 920U,
                .active = true,
            }).ok());
    }

    {
        fastfix::store::DurableBatchSessionStore durable_store(
            crash_root,
            fastfix::store::DurableBatchStoreOptions{
                .flush_threshold = 3U,
                .rollover_mode = fastfix::store::DurableStoreRolloverMode::kExternal,
                .max_archived_segments = 2U,
            });
        REQUIRE(durable_store.Open().ok());
        auto loaded = durable_store.LoadOutboundRange(200U, 1U, 2U);
        REQUIRE(loaded.ok());
        REQUIRE(loaded.value().size() == 2U);
        REQUIRE(loaded.value()[0].seq_num == 1U);
        REQUIRE(loaded.value()[1].seq_num == 2U);

        auto recovery = durable_store.LoadRecoveryState(200U);
        REQUIRE(recovery.ok());
        REQUIRE(recovery.value().next_in_seq == 41U);
        REQUIRE(recovery.value().next_out_seq == 77U);
        REQUIRE(recovery.value().last_inbound_ns == 910U);
        REQUIRE(recovery.value().last_outbound_ns == 920U);
        REQUIRE(recovery.value().active);
    }

    const auto corrupt_root = std::filesystem::temp_directory_path() / "fastfix-durable-store-corrupt-test";
    std::filesystem::remove_all(corrupt_root);
    {
        fastfix::store::DurableBatchSessionStore durable_store(
            corrupt_root,
            fastfix::store::DurableBatchStoreOptions{
                .flush_threshold = 1U,
                .rollover_mode = fastfix::store::DurableStoreRolloverMode::kExternal,
                .max_archived_segments = 1U,
            });
        REQUIRE(durable_store.Open().ok());
        REQUIRE(durable_store.SaveOutbound(
            fastfix::store::MessageRecord{
                .session_id = 201U,
                .seq_num = 1U,
                .timestamp_ns = 15U,
                .flags = 0U,
                .payload = Payload("8=FIX.4.4|35=D|11=CORRUPT|"),
            }).ok());
        REQUIRE(durable_store.SaveRecoveryState(
            fastfix::store::SessionRecoveryState{
                .session_id = 201U,
                .next_in_seq = 9U,
                .next_out_seq = 12U,
                .last_inbound_ns = 16U,
                .last_outbound_ns = 17U,
                .active = false,
            }).ok());
    }

    const auto recovery_path = corrupt_root / "recovery.log";
    REQUIRE(std::filesystem::exists(recovery_path));
    const auto recovery_size = std::filesystem::file_size(recovery_path);
    REQUIRE(recovery_size > sizeof(std::uint64_t));
    std::filesystem::resize_file(recovery_path, recovery_size - 1U);

    {
        fastfix::store::DurableBatchSessionStore durable_store(
            corrupt_root,
            fastfix::store::DurableBatchStoreOptions{
                .flush_threshold = 1U,
                .rollover_mode = fastfix::store::DurableStoreRolloverMode::kExternal,
                .max_archived_segments = 1U,
            });
        const auto status = durable_store.Open();
        REQUIRE(!status.ok());
        REQUIRE(status.code() == fastfix::base::ErrorCode::kFormatError);
    }

    std::filesystem::remove_all(durable_root);
    std::filesystem::remove_all(external_root);
    std::filesystem::remove_all(crash_root);
    std::filesystem::remove_all(corrupt_root);
}

TEST_CASE("durable store kLocalTime rollover", "[store-recovery]") {
    constexpr std::uint64_t kDay = 86'400ULL * 1'000'000'000ULL;
    // UTC+8: local midnight is at UTC 16:00 of the previous day.
    // Day boundary in UTC ns: 16:00 UTC = 57600 seconds = 57600000000000 ns.
    constexpr std::int32_t kUtcPlus8 = 28800;
    constexpr std::uint64_t kUtcMidnightLocal = kDay - static_cast<std::uint64_t>(kUtcPlus8) * 1'000'000'000ULL;

    const auto local_root = std::filesystem::temp_directory_path() / "fastfix-durable-store-localtime-test";
    std::filesystem::remove_all(local_root);

    {
        fastfix::store::DurableBatchSessionStore store(
            local_root,
            fastfix::store::DurableBatchStoreOptions{
                .flush_threshold = 1U,
                .rollover_mode = fastfix::store::DurableStoreRolloverMode::kLocalTime,
                .max_archived_segments = 2U,
                .local_utc_offset_seconds = kUtcPlus8,
                .use_system_timezone = false,
            });
        REQUIRE(store.Open().ok());

        // Message 1: UTC 15:00 of day 0 → local day 0 (15:00 + 8h = 23:00 local day 0)
        REQUIRE(store.SaveOutbound(
            fastfix::store::MessageRecord{
                .session_id = 300U,
                .seq_num = 1U,
                .timestamp_ns = kUtcMidnightLocal - 3'600'000'000'000ULL,  // 15:00 UTC
                .flags = 0U,
                .payload = Payload("8=FIX.4.4|35=D|11=LT1|"),
            }).ok());

        // Message 2: UTC 15:59 of day 0 → local day 0 (23:59 local)
        REQUIRE(store.SaveOutbound(
            fastfix::store::MessageRecord{
                .session_id = 300U,
                .seq_num = 2U,
                .timestamp_ns = kUtcMidnightLocal - 60'000'000'000ULL,  // 15:59 UTC
                .flags = 0U,
                .payload = Payload("8=FIX.4.4|35=D|11=LT2|"),
            }).ok());

        // Message 3: UTC 16:01 of day 0 → local day 1 (00:01 local next day) → triggers rollover
        REQUIRE(store.SaveOutbound(
            fastfix::store::MessageRecord{
                .session_id = 300U,
                .seq_num = 3U,
                .timestamp_ns = kUtcMidnightLocal + 60'000'000'000ULL,  // 16:01 UTC
                .flags = 0U,
                .payload = Payload("8=FIX.4.4|35=D|11=LT3|"),
            }).ok());

        REQUIRE(store.SaveRecoveryState(
            fastfix::store::SessionRecoveryState{
                .session_id = 300U,
                .next_in_seq = 1U,
                .next_out_seq = 4U,
                .last_inbound_ns = 0U,
                .last_outbound_ns = kUtcMidnightLocal + 60'000'000'000ULL,
                .active = true,
            }).ok());
        REQUIRE(store.Flush().ok());
    }

    // Verify: archived segment should contain seq 1 & 2, active segment has seq 3
    {
        fastfix::store::DurableBatchSessionStore store(
            local_root,
            fastfix::store::DurableBatchStoreOptions{
                .flush_threshold = 1U,
                .rollover_mode = fastfix::store::DurableStoreRolloverMode::kLocalTime,
                .max_archived_segments = 2U,
                .local_utc_offset_seconds = kUtcPlus8,
                .use_system_timezone = false,
            });
        REQUIRE(store.Open().ok());

        auto all = store.LoadOutboundRange(300U, 1U, 3U);
        REQUIRE(all.ok());
        REQUIRE(all.value().size() == 3U);
        REQUIRE(all.value()[0].seq_num == 1U);
        REQUIRE(all.value()[1].seq_num == 2U);
        REQUIRE(all.value()[2].seq_num == 3U);

        REQUIRE(CountArchivedLogs(local_root) == 1U);

        auto recovery = store.LoadRecoveryState(300U);
        REQUIRE(recovery.ok());
        REQUIRE(recovery.value().next_out_seq == 4U);
    }

    std::filesystem::remove_all(local_root);
}

TEST_CASE("durable store kLocalTime with system timezone", "[store-recovery]") {
    const auto systz_root = std::filesystem::temp_directory_path() / "fastfix-durable-store-systz-test";
    std::filesystem::remove_all(systz_root);

    {
        fastfix::store::DurableBatchSessionStore store(
            systz_root,
            fastfix::store::DurableBatchStoreOptions{
                .flush_threshold = 2U,
                .rollover_mode = fastfix::store::DurableStoreRolloverMode::kLocalTime,
                .max_archived_segments = 1U,
                .local_utc_offset_seconds = 0,
                .use_system_timezone = true,
            });
        REQUIRE(store.Open().ok());

        REQUIRE(store.SaveOutbound(
            fastfix::store::MessageRecord{
                .session_id = 301U,
                .seq_num = 1U,
                .timestamp_ns = 1'000'000'000ULL,
                .flags = 0U,
                .payload = Payload("8=FIX.4.4|35=D|11=SYS1|"),
            }).ok());
        REQUIRE(store.Flush().ok());
    }

    {
        fastfix::store::DurableBatchSessionStore store(
            systz_root,
            fastfix::store::DurableBatchStoreOptions{
                .flush_threshold = 2U,
                .rollover_mode = fastfix::store::DurableStoreRolloverMode::kLocalTime,
                .max_archived_segments = 1U,
                .local_utc_offset_seconds = 0,
                .use_system_timezone = true,
            });
        REQUIRE(store.Open().ok());
        auto loaded = store.LoadOutboundRange(301U, 1U, 1U);
        REQUIRE(loaded.ok());
        REQUIRE(loaded.value().size() == 1U);
        REQUIRE(loaded.value().front().seq_num == 1U);
    }

    std::filesystem::remove_all(systz_root);
}

TEST_CASE("inbound message recovery", "[store-recovery]") {
    const auto root = std::filesystem::temp_directory_path() / "fastfix-durable-inbound-recovery-test";
    std::filesystem::remove_all(root);

    {
        fastfix::store::DurableBatchSessionStore store(
            root,
            fastfix::store::DurableBatchStoreOptions{
                .flush_threshold = 1U,
                .rollover_mode = fastfix::store::DurableStoreRolloverMode::kExternal,
                .max_archived_segments = 2U,
            });
        REQUIRE(store.Open().ok());

        // Save outbound messages
        REQUIRE(store.SaveOutbound(
            fastfix::store::MessageRecord{
                .session_id = 500U,
                .seq_num = 1U,
                .timestamp_ns = 100U,
                .flags = 0U,
                .payload = Payload("8=FIX.4.4|35=D|11=OUT1|"),
            }).ok());
        REQUIRE(store.SaveOutbound(
            fastfix::store::MessageRecord{
                .session_id = 500U,
                .seq_num = 2U,
                .timestamp_ns = 200U,
                .flags = 0U,
                .payload = Payload("8=FIX.4.4|35=D|11=OUT2|"),
            }).ok());

        // Save inbound messages
        REQUIRE(store.SaveInbound(
            fastfix::store::MessageRecord{
                .session_id = 500U,
                .seq_num = 1U,
                .timestamp_ns = 150U,
                .flags = 0U,
                .payload = Payload("8=FIX.4.4|35=8|11=IN1|"),
            }).ok());
        REQUIRE(store.SaveInbound(
            fastfix::store::MessageRecord{
                .session_id = 500U,
                .seq_num = 2U,
                .timestamp_ns = 250U,
                .flags = 0U,
                .payload = Payload("8=FIX.4.4|35=8|11=IN2|"),
            }).ok());
        REQUIRE(store.SaveInbound(
            fastfix::store::MessageRecord{
                .session_id = 500U,
                .seq_num = 3U,
                .timestamp_ns = 350U,
                .flags = 0U,
                .payload = Payload("8=FIX.4.4|35=8|11=IN3|"),
            }).ok());

        // Recovery state reflects 3 received inbound messages → next expected = 4
        REQUIRE(store.SaveRecoveryState(
            fastfix::store::SessionRecoveryState{
                .session_id = 500U,
                .next_in_seq = 4U,
                .next_out_seq = 3U,
                .last_inbound_ns = 350U,
                .last_outbound_ns = 200U,
                .active = true,
            }).ok());
        REQUIRE(store.Flush().ok());
    }

    // Reopen and verify both outbound data and inbound recovery state survived
    {
        fastfix::store::DurableBatchSessionStore store(
            root,
            fastfix::store::DurableBatchStoreOptions{
                .flush_threshold = 1U,
                .rollover_mode = fastfix::store::DurableStoreRolloverMode::kExternal,
                .max_archived_segments = 2U,
            });
        REQUIRE(store.Open().ok());

        // Outbound messages are loadable
        auto outbound = store.LoadOutboundRange(500U, 1U, 2U);
        REQUIRE(outbound.ok());
        REQUIRE(outbound.value().size() == 2U);
        REQUIRE(outbound.value()[0].seq_num == 1U);
        REQUIRE(outbound.value()[1].seq_num == 2U);

        // Recovery state correctly reflects inbound tracking
        auto recovery = store.LoadRecoveryState(500U);
        REQUIRE(recovery.ok());
        REQUIRE(recovery.value().next_in_seq == 4U);
        REQUIRE(recovery.value().next_out_seq == 3U);
        REQUIRE(recovery.value().last_inbound_ns == 350U);
        REQUIRE(recovery.value().last_outbound_ns == 200U);
        REQUIRE(recovery.value().active);
    }

    std::filesystem::remove_all(root);
}

TEST_CASE("inbound recovery sequence tracking", "[store-recovery]") {
    const auto root = std::filesystem::temp_directory_path() / "fastfix-durable-inbound-seq-tracking-test";
    std::filesystem::remove_all(root);

    // Phase 1: initial session with some inbound/outbound
    {
        fastfix::store::DurableBatchSessionStore store(
            root,
            fastfix::store::DurableBatchStoreOptions{
                .flush_threshold = 2U,
                .rollover_mode = fastfix::store::DurableStoreRolloverMode::kExternal,
                .max_archived_segments = 2U,
            });
        REQUIRE(store.Open().ok());

        REQUIRE(store.SaveInbound(
            fastfix::store::MessageRecord{
                .session_id = 501U,
                .seq_num = 1U,
                .timestamp_ns = 10U,
                .flags = 0U,
                .payload = Payload("8=FIX.4.4|35=0|"),
            }).ok());
        REQUIRE(store.SaveRecoveryState(
            fastfix::store::SessionRecoveryState{
                .session_id = 501U,
                .next_in_seq = 2U,
                .next_out_seq = 1U,
                .last_inbound_ns = 10U,
                .last_outbound_ns = 0U,
                .active = true,
            }).ok());
        REQUIRE(store.Flush().ok());
    }

    // Phase 2: reopen, receive more inbound, update recovery state
    {
        fastfix::store::DurableBatchSessionStore store(
            root,
            fastfix::store::DurableBatchStoreOptions{
                .flush_threshold = 2U,
                .rollover_mode = fastfix::store::DurableStoreRolloverMode::kExternal,
                .max_archived_segments = 2U,
            });
        REQUIRE(store.Open().ok());

        // Verify phase 1 state survived
        auto recovery1 = store.LoadRecoveryState(501U);
        REQUIRE(recovery1.ok());
        REQUIRE(recovery1.value().next_in_seq == 2U);

        // More inbound messages arrive
        REQUIRE(store.SaveInbound(
            fastfix::store::MessageRecord{
                .session_id = 501U,
                .seq_num = 2U,
                .timestamp_ns = 20U,
                .flags = 0U,
                .payload = Payload("8=FIX.4.4|35=D|"),
            }).ok());
        REQUIRE(store.SaveInbound(
            fastfix::store::MessageRecord{
                .session_id = 501U,
                .seq_num = 3U,
                .timestamp_ns = 30U,
                .flags = 0U,
                .payload = Payload("8=FIX.4.4|35=8|"),
            }).ok());
        REQUIRE(store.SaveRecoveryState(
            fastfix::store::SessionRecoveryState{
                .session_id = 501U,
                .next_in_seq = 4U,
                .next_out_seq = 1U,
                .last_inbound_ns = 30U,
                .last_outbound_ns = 0U,
                .active = true,
            }).ok());
        REQUIRE(store.Flush().ok());
    }

    // Phase 3: reopen again, verify cumulative inbound tracking
    {
        fastfix::store::DurableBatchSessionStore store(
            root,
            fastfix::store::DurableBatchStoreOptions{
                .flush_threshold = 2U,
                .rollover_mode = fastfix::store::DurableStoreRolloverMode::kExternal,
                .max_archived_segments = 2U,
            });
        REQUIRE(store.Open().ok());

        auto recovery = store.LoadRecoveryState(501U);
        REQUIRE(recovery.ok());
        REQUIRE(recovery.value().next_in_seq == 4U);
        REQUIRE(recovery.value().next_out_seq == 1U);
        REQUIRE(recovery.value().last_inbound_ns == 30U);
        REQUIRE(recovery.value().active);
    }

    std::filesystem::remove_all(root);
}

TEST_CASE("multi session concurrent recovery", "[store-recovery]") {
    const auto root_a = std::filesystem::temp_directory_path() / "fastfix-multi-session-recovery-a";
    const auto root_b = std::filesystem::temp_directory_path() / "fastfix-multi-session-recovery-b";
    std::filesystem::remove_all(root_a);
    std::filesystem::remove_all(root_b);

    // Create two stores simultaneously and populate them
    {
        fastfix::store::DurableBatchSessionStore store_a(
            root_a,
            fastfix::store::DurableBatchStoreOptions{
                .flush_threshold = 1U,
                .rollover_mode = fastfix::store::DurableStoreRolloverMode::kExternal,
                .max_archived_segments = 2U,
            });
        fastfix::store::DurableBatchSessionStore store_b(
            root_b,
            fastfix::store::DurableBatchStoreOptions{
                .flush_threshold = 1U,
                .rollover_mode = fastfix::store::DurableStoreRolloverMode::kExternal,
                .max_archived_segments = 2U,
            });

        REQUIRE(store_a.Open().ok());
        REQUIRE(store_b.Open().ok());

        // Session A: outbound seq 1-2, inbound seq 1
        REQUIRE(store_a.SaveOutbound(
            fastfix::store::MessageRecord{
                .session_id = 600U,
                .seq_num = 1U,
                .timestamp_ns = 100U,
                .flags = 0U,
                .payload = Payload("8=FIX.4.4|35=D|11=A1|"),
            }).ok());
        REQUIRE(store_a.SaveOutbound(
            fastfix::store::MessageRecord{
                .session_id = 600U,
                .seq_num = 2U,
                .timestamp_ns = 200U,
                .flags = 0U,
                .payload = Payload("8=FIX.4.4|35=D|11=A2|"),
            }).ok());
        REQUIRE(store_a.SaveInbound(
            fastfix::store::MessageRecord{
                .session_id = 600U,
                .seq_num = 1U,
                .timestamp_ns = 150U,
                .flags = 0U,
                .payload = Payload("8=FIX.4.4|35=8|11=A-IN1|"),
            }).ok());
        REQUIRE(store_a.SaveRecoveryState(
            fastfix::store::SessionRecoveryState{
                .session_id = 600U,
                .next_in_seq = 2U,
                .next_out_seq = 3U,
                .last_inbound_ns = 150U,
                .last_outbound_ns = 200U,
                .active = true,
            }).ok());

        // Session B: outbound seq 1-3, inbound seq 1-2
        REQUIRE(store_b.SaveOutbound(
            fastfix::store::MessageRecord{
                .session_id = 700U,
                .seq_num = 1U,
                .timestamp_ns = 1000U,
                .flags = 0U,
                .payload = Payload("8=FIX.4.4|35=D|11=B1|"),
            }).ok());
        REQUIRE(store_b.SaveOutbound(
            fastfix::store::MessageRecord{
                .session_id = 700U,
                .seq_num = 2U,
                .timestamp_ns = 2000U,
                .flags = 0U,
                .payload = Payload("8=FIX.4.4|35=D|11=B2|"),
            }).ok());
        REQUIRE(store_b.SaveOutbound(
            fastfix::store::MessageRecord{
                .session_id = 700U,
                .seq_num = 3U,
                .timestamp_ns = 3000U,
                .flags = 0U,
                .payload = Payload("8=FIX.4.4|35=D|11=B3|"),
            }).ok());
        REQUIRE(store_b.SaveInbound(
            fastfix::store::MessageRecord{
                .session_id = 700U,
                .seq_num = 1U,
                .timestamp_ns = 1500U,
                .flags = 0U,
                .payload = Payload("8=FIX.4.4|35=8|11=B-IN1|"),
            }).ok());
        REQUIRE(store_b.SaveInbound(
            fastfix::store::MessageRecord{
                .session_id = 700U,
                .seq_num = 2U,
                .timestamp_ns = 2500U,
                .flags = 0U,
                .payload = Payload("8=FIX.4.4|35=8|11=B-IN2|"),
            }).ok());
        REQUIRE(store_b.SaveRecoveryState(
            fastfix::store::SessionRecoveryState{
                .session_id = 700U,
                .next_in_seq = 3U,
                .next_out_seq = 4U,
                .last_inbound_ns = 2500U,
                .last_outbound_ns = 3000U,
                .active = false,
            }).ok());

        REQUIRE(store_a.Flush().ok());
        REQUIRE(store_b.Flush().ok());
    }

    // Reopen both concurrently and verify independent recovery
    {
        fastfix::store::DurableBatchSessionStore store_a(
            root_a,
            fastfix::store::DurableBatchStoreOptions{
                .flush_threshold = 1U,
                .rollover_mode = fastfix::store::DurableStoreRolloverMode::kExternal,
                .max_archived_segments = 2U,
            });
        fastfix::store::DurableBatchSessionStore store_b(
            root_b,
            fastfix::store::DurableBatchStoreOptions{
                .flush_threshold = 1U,
                .rollover_mode = fastfix::store::DurableStoreRolloverMode::kExternal,
                .max_archived_segments = 2U,
            });

        REQUIRE(store_a.Open().ok());
        REQUIRE(store_b.Open().ok());

        // Verify session A
        auto out_a = store_a.LoadOutboundRange(600U, 1U, 2U);
        REQUIRE(out_a.ok());
        REQUIRE(out_a.value().size() == 2U);
        REQUIRE(out_a.value()[0].seq_num == 1U);
        REQUIRE(out_a.value()[1].seq_num == 2U);

        auto rec_a = store_a.LoadRecoveryState(600U);
        REQUIRE(rec_a.ok());
        REQUIRE(rec_a.value().next_in_seq == 2U);
        REQUIRE(rec_a.value().next_out_seq == 3U);
        REQUIRE(rec_a.value().active);

        // Verify session B
        auto out_b = store_b.LoadOutboundRange(700U, 1U, 3U);
        REQUIRE(out_b.ok());
        REQUIRE(out_b.value().size() == 3U);
        REQUIRE(out_b.value()[0].seq_num == 1U);
        REQUIRE(out_b.value()[1].seq_num == 2U);
        REQUIRE(out_b.value()[2].seq_num == 3U);

        auto rec_b = store_b.LoadRecoveryState(700U);
        REQUIRE(rec_b.ok());
        REQUIRE(rec_b.value().next_in_seq == 3U);
        REQUIRE(rec_b.value().next_out_seq == 4U);
        REQUIRE(!rec_b.value().active);

        // Session A's store should NOT have session B's data
        auto cross_a = store_a.LoadOutboundRange(700U, 1U, 3U);
        REQUIRE(cross_a.ok());
        REQUIRE(cross_a.value().empty());
        auto cross_rec_a = store_a.LoadRecoveryState(700U);
        REQUIRE(!cross_rec_a.ok());
        REQUIRE(cross_rec_a.status().code() == fastfix::base::ErrorCode::kNotFound);

        // Session B's store should NOT have session A's data
        auto cross_b = store_b.LoadOutboundRange(600U, 1U, 2U);
        REQUIRE(cross_b.ok());
        REQUIRE(cross_b.value().empty());
        auto cross_rec_b = store_b.LoadRecoveryState(600U);
        REQUIRE(!cross_rec_b.ok());
        REQUIRE(cross_rec_b.status().code() == fastfix::base::ErrorCode::kNotFound);
    }

    std::filesystem::remove_all(root_a);
    std::filesystem::remove_all(root_b);
}

TEST_CASE("multi session recovery isolation", "[store-recovery]") {
    // Two sessions in the SAME store root but with different session_ids
    const auto root = std::filesystem::temp_directory_path() / "fastfix-multi-session-isolation-test";
    std::filesystem::remove_all(root);

    {
        fastfix::store::DurableBatchSessionStore store(
            root,
            fastfix::store::DurableBatchStoreOptions{
                .flush_threshold = 1U,
                .rollover_mode = fastfix::store::DurableStoreRolloverMode::kExternal,
                .max_archived_segments = 2U,
            });
        REQUIRE(store.Open().ok());

        // Session 800: outbound seq 1-2
        REQUIRE(store.SaveOutbound(
            fastfix::store::MessageRecord{
                .session_id = 800U,
                .seq_num = 1U,
                .timestamp_ns = 10U,
                .flags = 0U,
                .payload = Payload("8=FIX.4.4|35=D|11=S800-1|"),
            }).ok());
        REQUIRE(store.SaveOutbound(
            fastfix::store::MessageRecord{
                .session_id = 800U,
                .seq_num = 2U,
                .timestamp_ns = 20U,
                .flags = 0U,
                .payload = Payload("8=FIX.4.4|35=D|11=S800-2|"),
            }).ok());
        REQUIRE(store.SaveRecoveryState(
            fastfix::store::SessionRecoveryState{
                .session_id = 800U,
                .next_in_seq = 5U,
                .next_out_seq = 3U,
                .last_inbound_ns = 15U,
                .last_outbound_ns = 20U,
                .active = true,
            }).ok());

        // Session 801: outbound seq 1
        REQUIRE(store.SaveOutbound(
            fastfix::store::MessageRecord{
                .session_id = 801U,
                .seq_num = 1U,
                .timestamp_ns = 30U,
                .flags = 0U,
                .payload = Payload("8=FIX.4.4|35=D|11=S801-1|"),
            }).ok());
        REQUIRE(store.SaveRecoveryState(
            fastfix::store::SessionRecoveryState{
                .session_id = 801U,
                .next_in_seq = 10U,
                .next_out_seq = 2U,
                .last_inbound_ns = 25U,
                .last_outbound_ns = 30U,
                .active = false,
            }).ok());
        REQUIRE(store.Flush().ok());
    }

    {
        fastfix::store::DurableBatchSessionStore store(
            root,
            fastfix::store::DurableBatchStoreOptions{
                .flush_threshold = 1U,
                .rollover_mode = fastfix::store::DurableStoreRolloverMode::kExternal,
                .max_archived_segments = 2U,
            });
        REQUIRE(store.Open().ok());

        // Session 800: recovery is independent
        auto rec_800 = store.LoadRecoveryState(800U);
        REQUIRE(rec_800.ok());
        REQUIRE(rec_800.value().next_in_seq == 5U);
        REQUIRE(rec_800.value().next_out_seq == 3U);
        REQUIRE(rec_800.value().active);

        auto out_800 = store.LoadOutboundRange(800U, 1U, 2U);
        REQUIRE(out_800.ok());
        REQUIRE(out_800.value().size() == 2U);

        // Session 801: recovery is independent
        auto rec_801 = store.LoadRecoveryState(801U);
        REQUIRE(rec_801.ok());
        REQUIRE(rec_801.value().next_in_seq == 10U);
        REQUIRE(rec_801.value().next_out_seq == 2U);
        REQUIRE(!rec_801.value().active);

        auto out_801 = store.LoadOutboundRange(801U, 1U, 1U);
        REQUIRE(out_801.ok());
        REQUIRE(out_801.value().size() == 1U);

        // Cross-session: session 800 does not see session 801's outbound range
        auto cross = store.LoadOutboundRange(800U, 1U, 10U);
        REQUIRE(cross.ok());
        REQUIRE(cross.value().size() == 2U);  // only 800's messages
    }

    std::filesystem::remove_all(root);
}

TEST_CASE("write failure on read-only directory", "[store-recovery]") {
    const auto root = std::filesystem::temp_directory_path() / "fastfix-durable-write-failure-test";
    std::filesystem::remove_all(root);

    // Create and populate a store
    {
        fastfix::store::DurableBatchSessionStore store(
            root,
            fastfix::store::DurableBatchStoreOptions{
                .flush_threshold = 1U,
                .rollover_mode = fastfix::store::DurableStoreRolloverMode::kExternal,
                .max_archived_segments = 2U,
            });
        REQUIRE(store.Open().ok());
        REQUIRE(store.SaveOutbound(
            fastfix::store::MessageRecord{
                .session_id = 900U,
                .seq_num = 1U,
                .timestamp_ns = 10U,
                .flags = 0U,
                .payload = Payload("8=FIX.4.4|35=D|11=PREFAIL|"),
            }).ok());
        REQUIRE(store.SaveRecoveryState(
            fastfix::store::SessionRecoveryState{
                .session_id = 900U,
                .next_in_seq = 1U,
                .next_out_seq = 2U,
                .last_inbound_ns = 0U,
                .last_outbound_ns = 10U,
                .active = true,
            }).ok());
        REQUIRE(store.Flush().ok());
    }

    // Make all store files read-only so that reopening with O_RDWR fails
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            std::filesystem::permissions(
                entry.path(),
                std::filesystem::perms::owner_read,
                std::filesystem::perm_options::replace);
        }
    }

    // Attempting to open a new store should fail (files can't be opened for writing)
    {
        fastfix::store::DurableBatchSessionStore store(
            root,
            fastfix::store::DurableBatchStoreOptions{
                .flush_threshold = 1U,
                .rollover_mode = fastfix::store::DurableStoreRolloverMode::kExternal,
                .max_archived_segments = 2U,
            });
        const auto status = store.Open();
        REQUIRE(!status.ok());
        REQUIRE(status.code() == fastfix::base::ErrorCode::kIoError);
    }

    // Restore permissions for cleanup and further testing
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            std::filesystem::permissions(
                entry.path(),
                std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                std::filesystem::perm_options::replace);
        }
    }

    std::filesystem::remove_all(root);
}

TEST_CASE("write failure recovery", "[store-recovery]") {
    const auto root = std::filesystem::temp_directory_path() / "fastfix-durable-write-recovery-test";
    std::filesystem::remove_all(root);

    // Phase 1: create store with known-good data
    {
        fastfix::store::DurableBatchSessionStore store(
            root,
            fastfix::store::DurableBatchStoreOptions{
                .flush_threshold = 1U,
                .rollover_mode = fastfix::store::DurableStoreRolloverMode::kExternal,
                .max_archived_segments = 2U,
            });
        REQUIRE(store.Open().ok());
        REQUIRE(store.SaveOutbound(
            fastfix::store::MessageRecord{
                .session_id = 901U,
                .seq_num = 1U,
                .timestamp_ns = 10U,
                .flags = 0U,
                .payload = Payload("8=FIX.4.4|35=D|11=GOOD1|"),
            }).ok());
        REQUIRE(store.SaveOutbound(
            fastfix::store::MessageRecord{
                .session_id = 901U,
                .seq_num = 2U,
                .timestamp_ns = 20U,
                .flags = 0U,
                .payload = Payload("8=FIX.4.4|35=D|11=GOOD2|"),
            }).ok());
        REQUIRE(store.SaveRecoveryState(
            fastfix::store::SessionRecoveryState{
                .session_id = 901U,
                .next_in_seq = 3U,
                .next_out_seq = 3U,
                .last_inbound_ns = 15U,
                .last_outbound_ns = 20U,
                .active = true,
            }).ok());
        REQUIRE(store.Flush().ok());
    }

    // Phase 2: make files read-only, verify failure
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            std::filesystem::permissions(
                entry.path(),
                std::filesystem::perms::owner_read,
                std::filesystem::perm_options::replace);
        }
    }

    {
        fastfix::store::DurableBatchSessionStore store(
            root,
            fastfix::store::DurableBatchStoreOptions{
                .flush_threshold = 1U,
                .rollover_mode = fastfix::store::DurableStoreRolloverMode::kExternal,
                .max_archived_segments = 2U,
            });
        REQUIRE(!store.Open().ok());
    }

    // Phase 3: restore permissions, reopen and verify original data is intact
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file()) {
            std::filesystem::permissions(
                entry.path(),
                std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                std::filesystem::perm_options::replace);
        }
    }

    {
        fastfix::store::DurableBatchSessionStore store(
            root,
            fastfix::store::DurableBatchStoreOptions{
                .flush_threshold = 1U,
                .rollover_mode = fastfix::store::DurableStoreRolloverMode::kExternal,
                .max_archived_segments = 2U,
            });
        REQUIRE(store.Open().ok());

        auto loaded = store.LoadOutboundRange(901U, 1U, 2U);
        REQUIRE(loaded.ok());
        REQUIRE(loaded.value().size() == 2U);
        REQUIRE(loaded.value()[0].seq_num == 1U);
        REQUIRE(loaded.value()[1].seq_num == 2U);

        auto recovery = store.LoadRecoveryState(901U);
        REQUIRE(recovery.ok());
        REQUIRE(recovery.value().next_in_seq == 3U);
        REQUIRE(recovery.value().next_out_seq == 3U);
        REQUIRE(recovery.value().last_inbound_ns == 15U);
        REQUIRE(recovery.value().last_outbound_ns == 20U);
        REQUIRE(recovery.value().active);
    }

    std::filesystem::remove_all(root);
}