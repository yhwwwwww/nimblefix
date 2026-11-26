#include "nimblefix/journal.hpp"
#include "nimblefix/replay.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

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

void test_reader_calls_race_safely_with_retention_and_teardown() {
    TempStore store;
    auto config = store.config();
    config.capacity_bytes = 8U * 8192U;
    config.max_records = 256;
    config.segment_bytes = 8192;
    config.max_segments = 8;
    auto opened = Journal::open(config);
    CHECK(opened.ok());
    if (!opened.ok()) return;
    auto journal = std::move(opened).value();
    auto reader_result = journal.create_reader();
    CHECK(reader_result.ok());
    if (!reader_result.ok()) return;
    auto reader = std::move(reader_result).value();

    std::atomic<bool> done{};
    std::atomic<int> reader_errors{};
    std::atomic<std::size_t> seen{};
    const std::string payload(1500, 'R');
    std::thread consumer([reader = std::move(reader), &done, &reader_errors, &seen, &payload]() mutable {
        JournalRecord rec;
        for (;;) {
            const auto st = reader.next(rec);
            if (st.code == ErrorCode::need_more) {
                if (done.load(std::memory_order_acquire)) break;
                std::this_thread::yield();
                continue;
            }
            if (!st.ok()) {
                reader_errors.fetch_add(1, std::memory_order_relaxed);
                break;
            }
            if (rec.payload != payload) reader_errors.fetch_add(1, std::memory_order_relaxed);
            seen.fetch_add(1, std::memory_order_relaxed);
        }
    });

    for (std::uint64_t seq = 1; seq <= 12; ++seq) {
        CHECK(journal.append(RecordKind::outbound, seq, seq, 0, payload).ok());
        if ((seq % 2) == 0) CHECK(journal.set_retention_floor(seq, 0).ok());
    }
    done.store(true, std::memory_order_release);
    consumer.join();
    // Once the reader is destroyed, its segment leases drop and retention reclaims old segments.
    CHECK(journal.set_retention_floor(13, 0).ok());
    CHECK(reader_errors.load() == 0);
    CHECK(seen.load() == 12);
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

std::string make_replay_fix_frame(std::string_view msg_type, std::uint64_t seq,
                                  std::string_view sending_time, std::string_view extra = {}) {
    std::string body = "35=" + std::string{msg_type} + "\x01"
                       "49=BUY\x01"
                       "56=SELL\x01"
                       "34=" + std::to_string(seq) + "\x01"
                       "52=" + std::string{sending_time} + "\x01";
    body.append(extra);
    std::string raw = "8=FIX.4.4\x01" "9=" + std::to_string(body.size()) + "\x01" + body;
    unsigned sum = 0;
    for (unsigned char c : raw) sum += c;
    sum %= 256U;
    char csum[8];
    csum[0] = '1'; csum[1] = '0'; csum[2] = '=';
    csum[3] = static_cast<char>('0' + (sum / 100U));
    csum[4] = static_cast<char>('0' + ((sum / 10U) % 10U));
    csum[5] = static_cast<char>('0' + (sum % 10U));
    csum[6] = '\x01';
    raw.append(csum, 7);
    return raw;
}

void test_deterministic_journal_replayer_offline_timing_and_tcp() {
    // 1. Verify UTC timestamp nanosecond parser
    std::uint64_t t1_ns = 0, t2_ns = 0;
    CHECK(detail::parse_utc_timestamp_ns("20261125-12:00:00.000", t1_ns));
    CHECK(detail::parse_utc_timestamp_ns("20261125-12:00:00.050000123", t2_ns));
    CHECK(t2_ns > t1_ns && (t2_ns - t1_ns) == 50'000'123ULL);

    // 2. Populate a real segmented mmap journal with inbound + outbound admin & business frames
    TempStore store;
    const std::string f_logon = make_replay_fix_frame("A", 1, "20261125-12:00:00.000", "98=0\x01" "108=30\x01");
    const std::string f_ord1  = make_replay_fix_frame("D", 2, "20261125-12:00:00.040", "11=O1\x01" "55=AAPL\x01");
    const std::string f_ord2  = make_replay_fix_frame("D", 3, "20261125-12:00:00.080", "11=O2\x01" "55=MSFT\x01");
    const std::string f_in1   = make_replay_fix_frame("8", 1, "20261125-12:00:00.045", "37=E1\x01");
    {
        auto opened = Journal::open(store.config());
        CHECK(opened.ok());
        if (!opened.ok()) return;
        auto& j = opened.value();
        CHECK(j.append(RecordKind::outbound, 1, 1, 0, f_logon).ok());
        CHECK(j.append(RecordKind::outbound, 2, 2, 0, f_ord1).ok());
        auto in_rec = j.append(RecordKind::inbound, 1, 0, 0, f_in1);
        CHECK(in_rec.ok());
        CHECK(j.append(RecordKind::delivery, 1, 0, in_rec.value().id, {}).ok());
        CHECK(j.append(RecordKind::outbound, 3, 3, 0, f_ord2).ok());

        // Live-journal concurrent replay with skip_admin + seq range
        ReplayOptions live_opts{};
        live_opts.direction = ReplayDirection::outbound;
        live_opts.skip_admin = true;
        live_opts.seq_from = 1;
        live_opts.seq_to = 3;
        std::vector<std::string> live_frames;
        auto live_res = JournalReplayer::replay(j, live_opts,
            [&](const JournalRecord& rec, const FlatMessageView* parsed) noexcept -> Status {
                CHECK(parsed != nullptr);
                live_frames.emplace_back(rec.payload);
                return Status::success();
            });
        CHECK(live_res.ok());
        if (live_res.ok()) {
            CHECK(live_res.value().replayed_frames == 2);
            CHECK(live_res.value().first_seq == 2 && live_res.value().last_seq == 3);
        }
        CHECK(live_frames.size() == 2 && live_frames[0] == f_ord1 && live_frames[1] == f_ord2);
    }

    // 3. Disk reopen with auto-detected session_key + scaled timing (4x faster => 80ms / 4 = 20ms sleep)
    {
        ReplayOptions opts{};
        opts.journal_path = store.path;
        // Intentionally leave session_key empty to verify auto-detection from v2 manifest!
        opts.direction = ReplayDirection::outbound;
        opts.timing = ReplayTiming::scaled;
        opts.speed_multiplier = 4.0;
        std::vector<std::string> replayed;
        auto res = JournalReplayer::replay(opts,
            [&](const JournalRecord& rec, const FlatMessageView*) noexcept -> Status {
                replayed.emplace_back(rec.payload);
                return Status::success();
            });
        CHECK(res.ok());
        if (res.ok()) {
            CHECK(res.value().replayed_frames == 3);
            CHECK(res.value().total_sleep_ns == 20'000'000ULL);
            CHECK(res.value().elapsed_ns >= 15'000'000ULL);
        }
        CHECK(replayed.size() == 3 && replayed[0] == f_logon && replayed[2] == f_ord2);
    }

    // 4. Real loopback TCP replay verifying exact byte-for-byte wire arrival
    {
        const int srv = ::socket(AF_INET, SOCK_STREAM, 0);
        CHECK(srv >= 0);
        int opt = 1;
        ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        CHECK(::bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        socklen_t len = sizeof(addr);
        CHECK(::getsockname(srv, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
        const std::uint16_t port = ::ntohs(addr.sin_port);
        CHECK(::listen(srv, 1) == 0);

        std::string received_wire;
        std::thread receiver([&] {
            const int cli = ::accept(srv, nullptr, nullptr);
            if (cli >= 0) {
                char buf[4096];
                for (;;) {
                    const ssize_t n = ::recv(cli, buf, sizeof(buf), 0);
                    if (n <= 0) break;
                    received_wire.append(buf, static_cast<std::size_t>(n));
                }
                ::close(cli);
            }
            ::close(srv);
        });

        ReplayOptions tcp_opts{};
        tcp_opts.journal_path = store.path;
        tcp_opts.session_key = "FIX.4.4|local=SELL|remote=BUY";
        tcp_opts.direction = ReplayDirection::outbound;
        tcp_opts.skip_admin = true;
        auto tcp_res = JournalReplayer::replay_to_tcp(tcp_opts, TcpEndpoint{"127.0.0.1", port});
        receiver.join();

        CHECK(tcp_res.ok());
        if (tcp_res.ok()) {
            CHECK(tcp_res.value().replayed_frames == 2);
            CHECK(tcp_res.value().total_bytes == f_ord1.size() + f_ord2.size());
        }
        CHECK(received_wire == f_ord1 + f_ord2);
    }
}

// Regression test for F07 (ReplayOptions::wire_hints for custom LENGTH/DATA with embedded SOH)
// and F08 (cross-process SPMC JournalTailer: live path replay, multi-process fork() tailing
// across segment rotations, aborted reserve skip, start=end/from_id, and pruned expiration).
void test_replay_wire_hints_and_cross_process_spmc_tailer() {
    // 1. F07 + F08: Custom LENGTH/DATA (5000/5001) with embedded SOH + live path replay while writer holds LOCK_EX
    {
        TempStore store;
        const std::string raw_custom = make_replay_fix_frame(
            "Z", 1, "20261126-00:00:00.000", std::string_view{"5000=3\x01" "5001=A\x01" "B\x01"});
        const std::string raw_admin = make_replay_fix_frame(
            "0", 2, "20261126-00:00:00.010");
        const std::string raw_custom2 = make_replay_fix_frame(
            "Z", 3, "20261126-00:00:00.020", std::string_view{"5000=3\x01" "5001=X\x01" "Y\x01"});

        const std::array<LengthDataPair, 1> pairs{{{5000, 5001}}};
        const WireHints custom_hints{pairs};

        auto opened = Journal::open(store.config());
        CHECK(opened.ok());
        if (!opened.ok()) return;
        auto& writer = opened.value();
        CHECK(writer.append(RecordKind::outbound, 1, 1, 0, raw_custom).ok());
        CHECK(writer.append(RecordKind::outbound, 2, 2, 0, raw_admin).ok());
        CHECK(writer.append(RecordKind::outbound, 3, 3, 0, raw_custom2).ok());

        // Path-based replay while writer STILL holds LOCK_EX + custom WireHints + skip_admin + scaled timing!
        ReplayOptions opts{};
        opts.journal_path = store.path;
        opts.direction = ReplayDirection::outbound;
        opts.wire_hints = custom_hints;
        opts.skip_admin = true;
        opts.verify_frames = true;
        opts.timing = ReplayTiming::scaled;
        opts.speed_multiplier = 20.0; // 20ms / 20 = 1ms
        std::vector<std::string> got_values;
        const auto live_res = JournalReplayer::replay(opts,
            [&](const JournalRecord&, const FlatMessageView* parsed) noexcept -> Status {
                if (parsed == nullptr) return Status{ErrorCode::invalid_frame};
                const auto f = parsed->find_unique(5001);
                if (!f.ok()) return f.status();
                got_values.emplace_back(f.value().value);
                return Status::success();
            });
        CHECK(live_res.ok());
        if (live_res.ok()) {
            CHECK(live_res.value().replayed_frames == 2);
            CHECK(live_res.value().invalid_frames == 0);
            CHECK(live_res.value().total_sleep_ns == 1'000'000ULL);
        }
        CHECK(got_values.size() == 2 &&
              got_values[0] == std::string_view("A\x01" "B", 3) &&
              got_values[1] == std::string_view("X\x01" "Y", 3));
    }

    // 2. True multi-process SPMC (fork()): 1 writer process rotating across multiple segments
    //    (including an aborted reserve before rotation), 2 independent consumer processes tailing live!
    {
        TempStore store;
        JournalConfig cfg = store.config();
        cfg.capacity_bytes = 256U * 1024U; // 16 segments of 16 KiB (plus no audit) -> fits 160 records
        cfg.segment_bytes = 16U * 1024U;
        cfg.max_segments = 16;
        cfg.max_records = 4096;

        auto opened = Journal::open(cfg);
        CHECK(opened.ok());
        if (!opened.ok()) return;
        auto writer = std::move(opened).value();

        // Pre-seed 5 records before spawning the "start=end" tailer.
        constexpr std::uint64_t pre_count = 5;
        constexpr std::uint64_t total_count = 140; // ~200B/rec => ~28 KiB => crosses multiple 16 KiB segments!
        const std::string pad(140, 'P');
        for (std::uint64_t s = 1; s <= pre_count; ++s) {
            const std::string f = make_replay_fix_frame("D", s, "20261126-00:00:00.000",
                "11=O" + std::to_string(s) + "\x01" "58=" + pad + "\x01");
            CHECK(writer.append(RecordKind::outbound, s, s, 0, f).ok());
        }

        // Open both tailers in separate child processes via fork() — genuine cross-process SPMC!
        int ready_pipe[2]{-1, -1};
        CHECK(::pipe(ready_pipe) == 0);

        auto spawn_tailer = [&](JournalTailStart start_mode, std::uint64_t expected_first,
                                std::uint64_t expected_last) -> pid_t {
            const pid_t pid = ::fork();
            if (pid == 0) {
                ::close(ready_pipe[0]);
                auto t = JournalTailer::open(JournalTailerConfig{
                    store.path, "FIX.4.4|local=SELL|remote=BUY", start_mode, 0, true});
                if (!t.ok()) ::_exit(10);
                // Signal parent that tailer is open and positioned before parent appends more records.
                const char b = 'R';
                if (::write(ready_pipe[1], &b, 1) != 1) ::_exit(11);
                ::close(ready_pipe[1]);

                auto& tailer = t.value();
                std::uint64_t expect = expected_first;
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                JournalRecord rec{};
                while (expect <= expected_last) {
                    const auto st = tailer.next(rec);
                    if (st.code == ErrorCode::need_more) {
                        if (std::chrono::steady_clock::now() > deadline) ::_exit(12);
                        std::this_thread::yield();
                        continue;
                    }
                    if (!st.ok()) ::_exit(13);
                    if (rec.kind != RecordKind::outbound || rec.seq != expect) ::_exit(14);
                    const std::string needle = "11=O" + std::to_string(expect) + "\x01";
                    if (rec.payload.find(needle) == std::string_view::npos) ::_exit(15);
                    ++expect;
                }
                // Confirm tailer crossed at least one segment boundary!
                if (tailer.current_segment_index() == 0) ::_exit(16);
                ::_exit(0);
            }
            return pid;
        };

        const pid_t child_begin = spawn_tailer(JournalTailStart::beginning, 1, total_count);
        const pid_t child_end   = spawn_tailer(JournalTailStart::end, pre_count + 1, total_count);
        ::close(ready_pipe[1]);

        // Wait for both child tailers to open and position before appending remaining records.
        for (int i = 0; i < 2; ++i) {
            char b{};
            CHECK(::read(ready_pipe[0], &b, 1) == 1 && b == 'R');
        }
        ::close(ready_pipe[0]);

        for (std::uint64_t s = pre_count + 1; s <= total_count; ++s) {
            // Exercise an aborted/uncommitted reserve() before a real commit to verify tailer skips cleanly.
            if (s == 50) {
                auto discarded = writer.reserve(RecordKind::outbound, s, s, 0, 512);
                if (discarded.ok()) {
                    std::memset(discarded.value().payload.data(), 'Z', 64);
                }
            }
            const std::string f = make_replay_fix_frame("D", s, "20261126-00:00:00.001",
                "11=O" + std::to_string(s) + "\x01" "58=" + pad + "\x01");
            CHECK(writer.append(RecordKind::outbound, s, s, 0, f).ok());
            if (writer.needs_maintenance()) CHECK(writer.poll_maintenance().ok());
        }

        int st1 = 0, st2 = 0;
        CHECK(::waitpid(child_begin, &st1, 0) == child_begin && WIFEXITED(st1) && WEXITSTATUS(st1) == 0);
        CHECK(::waitpid(child_end, &st2, 0) == child_end && WIFEXITED(st2) && WEXITSTATUS(st2) == 0);

        // Verify from_id starting at a mid-journal RecordId!
        const RecordId mid_id = writer.records()[75].id;
        auto from_mid = JournalTailer::open(JournalTailerConfig{
            store.path, {}, JournalTailStart::from_id, mid_id, true});
        CHECK(from_mid.ok());
        if (from_mid.ok()) {
            JournalRecord rec{};
            CHECK(from_mid.value().next(rec).ok());
            CHECK(rec.id == mid_id && rec.seq == writer.records()[75].seq);
        }
    }

    // 3. Pruned segment expiration: writer prunes old segments while external tailer lags behind
    {
        TempStore store;
        JournalConfig cfg = store.config();
        cfg.capacity_bytes = 64U * 1024U; // 8 segments of 8 KiB
        cfg.segment_bytes = 8U * 1024U;
        cfg.max_segments = 8;
        cfg.max_records = 2048;
        cfg.max_readers = 0; // External tailers are independent of in-process reader leases

        auto opened = Journal::open(cfg);
        CHECK(opened.ok());
        if (!opened.ok()) return;
        auto writer = std::move(opened).value();
        const std::string big(1000, 'B');

        // Fill seg 0, open tailer on seg 0 (reads only 1 record), then write across seg 1..3 and prune seg 0..1!
        CHECK(writer.append(RecordKind::outbound, 1, 1, 0, big).ok());
        const RecordId first_id = writer.records().front().id;
        auto tailer_res = JournalTailer::open(JournalTailerConfig{
            store.path, {}, JournalTailStart::beginning, 0, true});
        CHECK(tailer_res.ok());
        if (!tailer_res.ok()) return;
        auto& tailer = tailer_res.value();
        JournalRecord rec{};
        CHECK(tailer.next(rec).ok() && rec.seq == 1);

        for (std::uint64_t s = 2; s <= 15; ++s) {
            CHECK(writer.append(RecordKind::outbound, s, s, 0, big).ok());
            if (writer.needs_maintenance()) CHECK(writer.poll_maintenance().ok());
        }
        // Advance retention floor past seg 0 and seg 1 so seg 0 and seg 1 are unlinked!
        CHECK(writer.set_retention_floor(14, 0).ok());

        // Drain whatever remained in already-mapped seg 0; once tailer tries to enter pruned seg 1 -> reader_expired!
        Status final_st{};
        for (int i = 0; i < 32; ++i) {
            final_st = tailer.next(rec);
            if (!final_st.ok()) break;
        }
        CHECK(final_st.code == ErrorCode::reader_expired);

        // Opening a new tailer with from_id pointing into pruned seg 0 -> history_unavailable!
        const auto stale_open = JournalTailer::open(JournalTailerConfig{
            store.path, {}, JournalTailStart::from_id, first_id, true});
        CHECK(!stale_open.ok() && stale_open.status().code == ErrorCode::history_unavailable);
    }
}

} // namespace

int main() {
    test_reader_runs_concurrently_with_append_and_delivery_updates();
    test_record_lease_pins_mapping_for_async_consumers();
    test_reader_and_legacy_pin_keep_payload_views_alive();
    test_reader_calls_race_safely_with_retention_and_teardown();
    test_reader_calls_race_safely_with_journal_teardown();
    test_deterministic_journal_replayer_offline_timing_and_tcp();
    test_replay_wire_hints_and_cross_process_spmc_tailer();
    if (failures != 0) {
        std::cerr << failures << " journal reader test(s) failed\n";
        return 1;
    }
    std::cout << "journal reader concurrency and lease tests passed\n";
    return 0;
}
