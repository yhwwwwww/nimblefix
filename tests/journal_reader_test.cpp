#include "nimblefix/journal.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

namespace {
using namespace nimblefix;

int failures = 0;
#define CHECK(condition) do { \
    if (!(condition)) { std::cerr << __FILE__ << ':' << __LINE__ \
        << ": CHECK failed: " #condition << '\n'; ++failures; } \
} while (false)

struct TempStore {
    std::string directory;
    std::string path;
    TempStore() {
        char pattern[] = "/tmp/nimblefix-reader-XXXXXX";
        if (char* result = ::mkdtemp(pattern)) {
            directory = result;
            path = directory + "/session.nfxj";
        }
    }
    ~TempStore() { if (!directory.empty()) std::filesystem::remove_all(directory); }
    [[nodiscard]] JournalConfig config() const {
        return JournalConfig{
            .path = path,
            .session_key = "FIX.4.4|local=SELL|remote=BUY",
            .capacity_bytes = 3U * 65536U,
            .max_records = 1024,
            .segment_bytes = 65536,
            .max_segments = 3,
            .max_readers = 4,
            .max_reader_lag_records = 512,
        };
    }
};

void test_reader_runs_concurrently_with_append_and_delivery_updates() {
    TempStore store;
    auto opened = Journal::open(store.config());
    CHECK(opened.ok());
    if (!opened.ok()) return;
    auto journal = std::move(opened).value();
    auto reader_result = journal.create_reader();
    CHECK(reader_result.ok());
    if (!reader_result.ok()) return;
    auto reader = std::move(reader_result).value();

    std::atomic<bool> start{};
    std::atomic<bool> done{};
    std::atomic<int> reader_failures{};
    std::atomic<std::size_t> observed{};
    std::thread consumer([reader = std::move(reader), &start, &done,
                          &reader_failures, &observed]() mutable {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        std::uint64_t last_inbound = 0;
        std::uint64_t last_outbound = 0;
        JournalRecord record;
        for (;;) {
            const auto status = reader.next(record);
            if (status.code == ErrorCode::need_more) {
                if (done.load(std::memory_order_acquire) && reader.lag_records() == 0) break;
                std::this_thread::yield();
                continue;
            }
            if (!status.ok()) { reader_failures.fetch_add(1, std::memory_order_relaxed); break; }
            if (record.kind == RecordKind::inbound) {
                if (record.seq != last_inbound + 1 || record.delivered || record.payload != "inbound-payload")
                    reader_failures.fetch_add(1, std::memory_order_relaxed);
                last_inbound = record.seq;
            } else if (record.kind == RecordKind::outbound) {
                if (record.seq != last_outbound + 1 || record.payload != "outbound-payload")
                    reader_failures.fetch_add(1, std::memory_order_relaxed);
                last_outbound = record.seq;
            } else if (record.kind != RecordKind::delivery) {
                reader_failures.fetch_add(1, std::memory_order_relaxed);
            }
            observed.fetch_add(1, std::memory_order_relaxed);
        }
    });

    start.store(true, std::memory_order_release);
    for (std::uint64_t seq = 1; seq <= 100; ++seq) {
        auto inbound = journal.append(RecordKind::inbound, seq, 0, 0, "inbound-payload");
        CHECK(inbound.ok());
        if (!inbound.ok()) break;
        const auto outbound = journal.append(RecordKind::outbound, seq, seq,
            inbound.value().id, "outbound-payload");
        CHECK(outbound.ok());
        if (!outbound.ok()) break;
        const auto delivery = journal.append(RecordKind::delivery, seq, 0,
            inbound.value().id, {});
        CHECK(delivery.ok());
        if (!delivery.ok()) break;
    }
    done.store(true, std::memory_order_release);
    consumer.join();
    CHECK(reader_failures.load() == 0);
    CHECK(observed.load() == 300);
    CHECK(journal.records().size() == 300);
}

void test_record_lease_pins_mapping_for_async_consumers() {
    TempStore store;
    auto config = store.config();
    config.capacity_bytes = 3U * 8192U;
    config.max_records = 128;
    config.segment_bytes = 8192;
    auto opened = Journal::open(config);
    CHECK(opened.ok());
    if (!opened.ok()) return;
    auto& journal = opened.value();
    const std::string payload(3000, 'L');
    RecordId first_id = 0;
    for (std::uint64_t seq = 1; seq <= 3; ++seq) {
        auto appended = journal.append(RecordKind::outbound, seq, seq, 0, payload);
        CHECK(appended.ok());
        if (!appended.ok()) return;
        if (seq == 1) first_id = appended.value().id;
    }
    {
        auto lease_result = journal.pin_record(first_id);
        CHECK(lease_result.ok());
        if (!lease_result.ok()) return;
        auto lease = std::move(lease_result).value();
        CHECK(lease.payload() == payload);
        CHECK(journal.set_retention_floor(4, 0).ok());
        CHECK(journal.find_record(first_id) != nullptr);
        CHECK(lease.payload() == payload);
    }
    CHECK(journal.set_retention_floor(4, 0).ok());
    CHECK(journal.find_record(first_id) == nullptr);
}

void test_reader_and_legacy_pin_keep_payload_views_alive() {
    TempStore segmented_store;
    JournalReader reader;
    JournalRecord read;
    const std::string payload(128, 'V');
    {
        auto opened = Journal::open(segmented_store.config());
        CHECK(opened.ok());
        if (!opened.ok()) return;
        auto appended = opened.value().append(RecordKind::outbound, 1, 1, 0, payload);
        CHECK(appended.ok());
        if (!appended.ok()) return;
        auto created_reader = opened.value().create_reader(appended.value().id);
        CHECK(created_reader.ok());
        if (!created_reader.ok()) return;
        reader = std::move(created_reader).value();
        CHECK(reader.next(read).ok());
        Journal moved = std::move(opened).value();
        CHECK(reader.next(read).code == ErrorCode::need_more);
    }
    CHECK(read.payload == payload);
    CHECK(reader.next(read).code == ErrorCode::reader_expired);

    TempStore legacy_store;
    JournalLease lease;
    {
        auto config = legacy_store.config();
        config.segment_bytes = 0;
        config.max_segments = 0;
        auto opened = Journal::open(config);
        CHECK(opened.ok());
        if (!opened.ok()) return;
        CHECK(opened.value().append(RecordKind::outbound, 1, 1, 0, payload).ok());
        auto pinned = opened.value().pin_record(opened.value().records().back().id);
        CHECK(pinned.ok());
        if (!pinned.ok()) return;
        lease = std::move(pinned).value();
    }
    CHECK(lease.payload() == payload);
}

void test_reader_calls_race_safely_with_journal_teardown() {
    TempStore store;
    for (int iteration = 0; iteration < 32; ++iteration) {
        auto opened = Journal::open(store.config());
        CHECK(opened.ok());
        if (!opened.ok()) return;
        auto journal = std::make_unique<Journal>(std::move(opened).value());
        const auto sequence = static_cast<std::uint64_t>(iteration + 1);
        const auto appended = journal->append(RecordKind::outbound, sequence, sequence, 0, "teardown-frame");
        CHECK(appended.ok());
        if (!appended.ok()) return;
        auto created = journal->create_reader(appended.value().id);
        CHECK(created.ok());
        if (!created.ok()) return;
        std::atomic<bool> start{};
        std::atomic<bool> stop{};
        std::atomic<std::size_t> calls{};
        std::atomic<int> errors{};
        std::thread worker([reader = std::move(created).value(), &start, &stop,
                            &calls, &errors]() mutable {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            JournalRecord record;
            while (!stop.load(std::memory_order_acquire)) {
                const auto status = reader.next(record);
                if (!status.ok() && status.code != ErrorCode::need_more &&
                    status.code != ErrorCode::reader_expired)
                    errors.fetch_add(1, std::memory_order_relaxed);
                (void)reader.lag_records();
                calls.fetch_add(1, std::memory_order_relaxed);
            }
            if (reader.next(record).code != ErrorCode::reader_expired)
                errors.fetch_add(1, std::memory_order_relaxed);
        });
        start.store(true, std::memory_order_release);
        while (calls.load(std::memory_order_acquire) < 32) std::this_thread::yield();
        journal.reset();
        stop.store(true, std::memory_order_release);
        worker.join();
        CHECK(errors.load(std::memory_order_relaxed) == 0);
    }
}

} // namespace

int main() {
    test_reader_runs_concurrently_with_append_and_delivery_updates();
    test_record_lease_pins_mapping_for_async_consumers();
    test_reader_and_legacy_pin_keep_payload_views_alive();
    test_reader_calls_race_safely_with_journal_teardown();
    if (failures != 0) {
        std::cerr << failures << " journal reader test(s) failed\n";
        return 1;
    }
    std::cout << "journal reader concurrency and lease tests passed\n";
    return 0;
}
