#include "nimblefix/journal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace nimblefix;

#define CHECK(condition) do { \
    if (!(condition)) { std::cerr << __FILE__ << ':' << __LINE__ \
        << ": CHECK failed: " #condition << '\n'; ++failures; } \
} while (false)

int failures = 0;

struct TempStore {
    std::string directory;
    std::string path;
    TempStore() {
        char pattern[] = "/tmp/nimblefix-rotation-XXXXXX";
        if (char* result = ::mkdtemp(pattern)) {
            directory = result;
            path = directory + "/session.nfxj";
        }
    }
    ~TempStore() {
        if (!directory.empty()) std::filesystem::remove_all(directory);
    }
    [[nodiscard]] JournalConfig config(JournalTestHook hook = {}) const {
        return JournalConfig{
            .path = path,
            .session_key = "FIX.4.4|local=SELL|remote=BUY",
            .capacity_bytes = 3U * 8192U,
            .max_records = 128,
            .test_hook = hook,
            .segment_bytes = 8192,
            .max_segments = 3,
            .max_readers = 4,
            .max_reader_lag_records = 64,
        };
    }
    [[nodiscard]] std::size_t segment_files() const {
        std::size_t count = 0;
        for (const auto& entry : std::filesystem::directory_iterator(directory))
            if (entry.path().filename().string().find("session.nfxj.seg") == 0) ++count;
        return count;
    }
};

std::string large_payload(char fill = 'F') { return std::string(3000, fill); }

enum class CrashPoint : std::uint8_t { segment, append, checkpoint, retention, epoch };

struct KillContext { JournalFaultPoint point{}; };
void kill_at(JournalFaultPoint point, void* opaque) noexcept {
    if (point == static_cast<KillContext*>(opaque)->point) {
        (void)::kill(::getpid(), SIGKILL);
        ::_exit(128 + SIGKILL);
    }
}

bool child_was_killed(pid_t child) {
    int status = 0;
    if (::waitpid(child, &status, 0) != child) return false;
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
}

void test_rotation_recovery_and_inspection() {
    TempStore store;
    CHECK(!store.path.empty());
    const auto config = store.config();
    RecordId first_id = 0;
    RecordId second_id = 0;
    {
        auto opened = Journal::open(config);
        CHECK(opened.ok());
        if (!opened.ok()) return;
        auto& journal = opened.value();
        CHECK(store.segment_files() == 3);
        for (std::uint64_t seq = 1; seq <= 3; ++seq) {
            auto appended = journal.append(RecordKind::outbound, seq, seq + 100, 0, large_payload());
            CHECK(appended.ok());
            if (!appended.ok()) return;
            if (seq == 1) first_id = appended.value().id;
            if (seq == 2) second_id = appended.value().id;
        }
        CHECK(first_id == 4096);
        CHECK(second_id == ((RecordId{1} << 48U) | 4096U));
        CHECK(journal.find_outbound(3) != nullptr);
        CHECK(journal.next_outbound_seq() == 4);
        const auto full = journal.append(RecordKind::outbound, 4, 104, 0, large_payload());
        CHECK(!full.ok() && full.status().code == ErrorCode::storage_full);
        CHECK(journal.records().size() == 3);

        const auto before = std::filesystem::last_write_time(store.path);
        const auto checkpoint_before = std::filesystem::last_write_time(store.path + ".ckp");
        const auto segment_before = std::filesystem::last_write_time(store.path + ".seg0");
        const auto inspection = Journal::inspect_snapshot(config);
        CHECK(inspection.diagnostic.ok());
        CHECK(inspection.format_version == 2);
        CHECK(inspection.segment_count == 3);
        CHECK(inspection.committed_records == 3);
        CHECK(inspection.outbound_records == 3);
        CHECK(inspection.next_outbound_seq == 4);
        CHECK(!inspection.checkpoint_stale);
        CHECK(std::filesystem::last_write_time(store.path) == before);
        CHECK(std::filesystem::last_write_time(store.path + ".ckp") == checkpoint_before);
        CHECK(std::filesystem::last_write_time(store.path + ".seg0") == segment_before);
    }
    {
        auto reopened = Journal::open(config);
        CHECK(reopened.ok());
        if (!reopened.ok()) return;
        auto& journal = reopened.value();
        CHECK(journal.records().size() == 3);
        CHECK(journal.next_outbound_seq() == 4);
        CHECK(journal.find_record(first_id) != nullptr);
        CHECK(journal.find_record(second_id) != nullptr);
        CHECK(journal.find_outbound(2)->payload == large_payload());
    }
}

void test_safe_retention_reuses_disk_without_reusing_ids() {
    TempStore store;
    const auto config = store.config();
    std::array<RecordId, 3> ids{};
    {
        auto opened = Journal::open(config);
        CHECK(opened.ok());
        if (!opened.ok()) return;
        auto& journal = opened.value();
        for (std::uint64_t seq = 1; seq <= 3; ++seq) {
            auto record = journal.append(RecordKind::outbound, seq, seq, 0, large_payload());
            CHECK(record.ok());
            if (!record.ok()) return;
            ids[seq - 1] = record.value().id;
        }
        CHECK(journal.set_retention_floor(4, 0).ok());
        CHECK(journal.lowest_retained_outbound_seq() == 4);
        CHECK(journal.records().size() == 1);
        CHECK(journal.records()[0].id == ids[2]);
        CHECK(journal.find_outbound(1) == nullptr);
        CHECK(journal.pin_record(ids[0]).status().code == ErrorCode::history_unavailable);
        CHECK(store.segment_files() == 3);
        auto next = journal.append(RecordKind::outbound, 4, 4, 0, large_payload());
        CHECK(next.ok());
        if (next.ok()) CHECK(next.value().id == ((RecordId{3} << 48U) | 4096U));
        CHECK(next.ok() && next.value().id != ids[0] && next.value().id != ids[1]);
    }
    {
        auto reopened = Journal::open(config);
        CHECK(reopened.ok());
        if (!reopened.ok()) return;
        const auto& journal = reopened.value();
        CHECK(journal.next_outbound_seq() == 5);
        CHECK(journal.lowest_retained_outbound_seq() == 4);
        CHECK(journal.records().size() == 2);
        CHECK(journal.find_outbound(3) != nullptr);
        CHECK(journal.find_outbound(4) != nullptr);
        CHECK(journal.find_outbound(1) == nullptr);
    }
}

void test_reader_lease_blocks_retention_and_lag_is_bounded() {
    TempStore store;
    auto config = store.config();
    auto opened = Journal::open(config);
    CHECK(opened.ok());
    if (!opened.ok()) return;
    auto& journal = opened.value();
    std::array<RecordId, 3> ids{};
    for (std::uint64_t seq = 1; seq <= 3; ++seq) {
        auto record = journal.append(RecordKind::outbound, seq, seq, 0, large_payload());
        CHECK(record.ok());
        if (!record.ok()) return;
        ids[seq - 1] = record.value().id;
    }
    auto reader_result = journal.create_reader(ids[0]);
    CHECK(reader_result.ok());
    if (!reader_result.ok()) return;
    {
        auto reader = std::move(reader_result).value();
        JournalRecord record;
        CHECK(reader.next(record).ok() && record.seq == 1 && !record.delivered);
        CHECK(journal.set_retention_floor(4, 0).ok());
        CHECK(journal.records().size() == 3);
        CHECK(journal.lowest_retained_record_id() == ids[0]);
    }
    CHECK(journal.set_retention_floor(4, 0).ok());
    CHECK(journal.records().size() == 1);

    TempStore lag_store;
    auto lag_config = lag_store.config();
    lag_config.max_reader_lag_records = 1;
    auto lag_opened = Journal::open(lag_config);
    CHECK(lag_opened.ok());
    if (!lag_opened.ok()) return;
    auto& lag_journal = lag_opened.value();
    auto lag_reader_result = lag_journal.create_reader();
    CHECK(lag_reader_result.ok());
    if (!lag_reader_result.ok()) return;
    auto lag_reader = std::move(lag_reader_result).value();
    CHECK(lag_journal.append(RecordKind::outbound, 1, 1, 0, "frame").ok());
    CHECK(lag_journal.append(RecordKind::outbound, 2, 2, 0, "frame").status().code == ErrorCode::queue_full);
    JournalRecord read;
    CHECK(lag_reader.next(read).ok() && read.seq == 1);
    CHECK(lag_journal.append(RecordKind::outbound, 2, 2, 0, "frame").ok());
}

void test_checkpoint_corruption_rebuild_and_payload_corruption_fails_closed() {
    TempStore store;
    const auto config = store.config();
    {
        auto opened = Journal::open(config);
        CHECK(opened.ok());
        if (!opened.ok()) return;
        CHECK(opened.value().append(RecordKind::outbound, 1, 1, 0, "committed").ok());
        CHECK(opened.value().publish_checkpoint(0, 1).ok());
    }
    const auto checkpoint_path = store.path + ".ckp";
    const int ckp = ::open(checkpoint_path.c_str(), O_RDWR | O_CLOEXEC);
    CHECK(ckp >= 0);
    if (ckp >= 0) {
        const unsigned char byte = 0x55;
        CHECK(::pwrite(ckp, &byte, 1, 12) == 1);
        ::close(ckp);
    }
    const auto inspection = Journal::inspect_snapshot(config);
    CHECK(inspection.diagnostic.ok());
    CHECK(inspection.committed_records == 1);
    CHECK(inspection.checkpoint_stale);
    {
        auto rebuilt = Journal::open(config);
        CHECK(rebuilt.ok());
        if (!rebuilt.ok()) return;
        CHECK(rebuilt.value().records().size() == 1);
        CHECK(rebuilt.value().find_outbound(1) != nullptr);
        CHECK(rebuilt.value().checkpoint_stale());
    }

    TempStore corrupt_store;
    const auto corrupt_config = corrupt_store.config();
    RecordId id = 0;
    {
        auto opened = Journal::open(corrupt_config);
        CHECK(opened.ok());
        if (!opened.ok()) return;
        auto record = opened.value().append(RecordKind::outbound, 1, 1, 0, "payload");
        CHECK(record.ok());
        if (!record.ok()) return;
        id = record.value().id;
    }
    const auto data_path = corrupt_store.path + ".seg0";
    const int data_fd = ::open(data_path.c_str(), O_RDWR | O_CLOEXEC);
    CHECK(data_fd >= 0);
    if (data_fd >= 0) {
        const unsigned char changed = 'X';
        CHECK(::pwrite(data_fd, &changed, 1, static_cast<off_t>(id + 64)) == 1);
        ::close(data_fd);
    }
    CHECK(Journal::open(corrupt_config).status().code == ErrorCode::corrupt_store);
    CHECK(Journal::inspect_snapshot(corrupt_config).diagnostic.code == ErrorCode::corrupt_store);
}

void test_checkpoint_rebuild_after_retention() {
    TempStore store;
    const auto config = store.config();
    RecordId oldest_kept = 0;
    {
        auto opened = Journal::open(config);
        CHECK(opened.ok());
        if (!opened.ok()) return;
        auto& journal = opened.value();
        for (std::uint64_t seq = 1; seq <= 3; ++seq) {
            auto record = journal.append(RecordKind::outbound, seq, seq, 0, large_payload());
            CHECK(record.ok());
            if (!record.ok()) return;
            if (seq == 3) oldest_kept = record.value().id;
        }
        CHECK(journal.set_retention_floor(4, 0).ok());
        CHECK(journal.lowest_retained_outbound_seq() == 4);
        CHECK(journal.lowest_retained_record_id() == oldest_kept);
    }
    const auto checkpoint_path = store.path + ".ckp";
    const int fd = ::open(checkpoint_path.c_str(), O_RDWR | O_CLOEXEC);
    CHECK(fd >= 0);
    if (fd >= 0) {
        const unsigned char byte = 0xa5;
        CHECK(::pwrite(fd, &byte, 1, 12) == 1);
        ::close(fd);
    }
    const auto inspection = Journal::inspect_snapshot(config);
    CHECK(inspection.diagnostic.ok());
    CHECK(inspection.checkpoint_stale);
    CHECK(inspection.lowest_retained_record_id == oldest_kept);
    CHECK(inspection.lowest_retained_outbound_seq == 3);
    auto rebuilt = Journal::open(config);
    CHECK(rebuilt.ok());
    if (rebuilt.ok()) {
        CHECK(rebuilt.value().checkpoint_stale());
        CHECK(rebuilt.value().lowest_retained_record_id() == oldest_kept);
        CHECK(rebuilt.value().lowest_retained_outbound_seq() == 3);
        CHECK(rebuilt.value().next_outbound_seq() == 4);
        CHECK(rebuilt.value().find_outbound(1) == nullptr);
        CHECK(rebuilt.value().find_outbound(3) != nullptr);
    }
}

void test_retention_keeps_sequence_anchors_for_checkpoint_rebuild() {
    TempStore store;
    const auto config = store.config();
    RecordId pending_id = 0;
    {
        auto opened = Journal::open(config);
        CHECK(opened.ok());
        if (!opened.ok()) return;
        auto& journal = opened.value();
        auto first = journal.append(RecordKind::inbound, 1, 0, 0, "completed-input");
        CHECK(first.ok());
        if (!first.ok()) return;
        CHECK(journal.append(RecordKind::delivery, 1, 0, first.value().id, {}).ok());
        CHECK(journal.append(RecordKind::outbound, 1, 11, first.value().id, large_payload()).ok());
        CHECK(journal.append(RecordKind::outbound, 2, 12, first.value().id, large_payload()).ok());
        auto pending = journal.append(RecordKind::inbound, 2, 0, 0, "pending-input");
        CHECK(pending.ok());
        if (!pending.ok()) return;
        pending_id = pending.value().id;
        CHECK(journal.set_retention_floor(3, first.value().id).ok());
        CHECK(journal.find_record(first.value().id) == nullptr);
        CHECK(journal.find_record(pending_id) != nullptr);
    }
    const auto checkpoint_path = store.path + ".ckp";
    const int fd = ::open(checkpoint_path.c_str(), O_RDWR | O_CLOEXEC);
    CHECK(fd >= 0);
    if (fd >= 0) {
        const unsigned char byte = 0x73;
        CHECK(::pwrite(fd, &byte, 1, 12) == 1);
        ::close(fd);
    }
    const auto inspection = Journal::inspect_snapshot(config);
    CHECK(inspection.diagnostic.ok());
    CHECK(inspection.checkpoint_stale);
    CHECK(inspection.next_inbound_seq == 3);
    CHECK(inspection.next_outbound_seq == 3);
    auto rebuilt = Journal::open(config);
    CHECK(rebuilt.ok());
    if (!rebuilt.ok()) return;
    CHECK(rebuilt.value().next_inbound_seq() == 3);
    CHECK(rebuilt.value().next_outbound_seq() == 3);
    CHECK(rebuilt.value().completed_inbound_seq() == 1);
    CHECK(rebuilt.value().records().front().kind == RecordKind::outbound);
    CHECK(rebuilt.value().find_record(pending_id) != nullptr);
    auto next = rebuilt.value().append(RecordKind::inbound, 3, 0, 0, "next-input");
    CHECK(next.ok());
    CHECK(rebuilt.value().next_inbound_seq() == 4);
}

void test_delivery_cursor_rebuild_and_pruned_cursor_continuity() {
    TempStore store;
    auto config = store.config();
    config.capacity_bytes = 1024U * 1024U + 3U * config.segment_bytes;
    RecordId inbound_id = 0;
    {
        auto opened = Journal::open(config);
        CHECK(opened.ok());
        if (!opened.ok()) return;
        auto& journal = opened.value();
        auto inbound = journal.append(RecordKind::inbound, 1, 0, 0, "inbound-before-checkpoint");
        CHECK(inbound.ok());
        if (!inbound.ok()) return;
        inbound_id = inbound.value().id;
        CHECK(journal.publish_checkpoint(0, 1).ok());
        CHECK(journal.append(RecordKind::delivery, 1, 0, inbound_id, {}).ok());
    }
    {
        auto reopened = Journal::open(config);
        CHECK(reopened.ok());
        if (!reopened.ok()) return;
        CHECK(reopened.value().checkpoint_stale());
        CHECK(reopened.value().checkpoint().delivery_cursor == inbound_id);
        CHECK(reopened.value().records().front().delivered);
        CHECK(reopened.value().append(RecordKind::outbound, 1, 11, inbound_id, large_payload()).ok());
        CHECK(reopened.value().append(RecordKind::outbound, 2, 12, inbound_id, large_payload()).ok());
        CHECK(reopened.value().set_retention_floor(3, inbound_id).ok());
        CHECK(reopened.value().lowest_retained_record_id() <= inbound_id);
        CHECK(reopened.value().find_record(inbound_id) != nullptr);
        CHECK(reopened.value().set_retention_floor(3, inbound_id).ok());
    }
    {
        auto reopened = Journal::open(config);
        CHECK(reopened.ok());
        if (!reopened.ok()) return;
        CHECK(reopened.value().checkpoint().delivery_cursor == inbound_id);
        CHECK(reopened.value().begin_epoch("preserve pruned delivery cursor").ok());
        CHECK(reopened.value().checkpoint().delivery_cursor == inbound_id);
        CHECK(reopened.value().set_retention_floor(1, inbound_id).ok());
        CHECK(reopened.value().find_record(inbound_id) == nullptr);
        CHECK(reopened.value().lowest_retained_record_id() > inbound_id);
    }
    auto final_reopen = Journal::open(config);
    CHECK(final_reopen.ok());
    if (final_reopen.ok()) {
        CHECK(final_reopen.value().checkpoint().epoch == 2);
        CHECK(final_reopen.value().checkpoint().delivery_cursor == inbound_id);
        CHECK(final_reopen.value().epoch_audit().size() == 1);
    }
}

void test_completed_inbound_watermark_is_contiguous_and_gapfill_aware() {
    TempStore store;
    const auto config = store.config();
    RecordId last_completed = 0;
    {
        auto opened = Journal::open(config);
        CHECK(opened.ok());
        if (!opened.ok()) return;
        auto& journal = opened.value();
        auto one = journal.append(RecordKind::inbound, 1, 0, 0, "inbound-1");
        auto two = journal.append(RecordKind::inbound, 2, 0, 0, "inbound-2");
        CHECK(one.ok() && two.ok());
        if (!one.ok() || !two.ok()) return;
        CHECK(journal.append(RecordKind::delivery, 2, 0, two.value().id, {}).ok());
        CHECK(journal.completed_inbound_seq() == 0);
        CHECK(journal.append(RecordKind::delivery, 1, 0, one.value().id, {}).ok());
        CHECK(journal.completed_inbound_seq() == 2);
        auto gapfill = journal.append(RecordKind::inbound, 3, 0, 0, "gapfill", 8);
        CHECK(gapfill.ok());
        if (!gapfill.ok()) return;
        last_completed = gapfill.value().id;
        CHECK(journal.completed_inbound_seq() == 7);
    }
    auto reopened = Journal::open(config);
    CHECK(reopened.ok());
    if (reopened.ok()) {
        CHECK(reopened.value().completed_inbound_seq() == 7);
        CHECK(reopened.value().completed_inbound_record_id() == last_completed);
        CHECK(reopened.value().checkpoint().completed_inbound_seq == 7);
    }
}

void test_committed_delivery_completion_survives_process_kill() {
    TempStore store;
    const auto config = store.config();
    RecordId inbound_id = 0;
    {
        auto opened = Journal::open(config);
        CHECK(opened.ok());
        if (!opened.ok()) return;
        auto inbound = opened.value().append(RecordKind::inbound, 1, 0, 0, "callback-pending");
        CHECK(inbound.ok());
        if (!inbound.ok()) return;
        inbound_id = inbound.value().id;
        CHECK(opened.value().publish_checkpoint(0, 1).ok());
    }
    const auto child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        KillContext context{JournalFaultPoint::postcommit};
        auto hooked = config;
        hooked.test_hook = {kill_at, &context};
        auto opened = Journal::open(hooked);
        if (!opened.ok()) ::_exit(17);
        (void)opened.value().append(RecordKind::delivery, 1, 0, inbound_id, {});
        ::_exit(18);
    }
    if (child >= 0) CHECK(child_was_killed(child));
    auto recovered = Journal::open(config);
    CHECK(recovered.ok());
    if (recovered.ok()) {
        CHECK(recovered.value().completed_inbound_seq() == 1);
        CHECK(recovered.value().completed_inbound_record_id() == inbound_id);
        CHECK(recovered.value().has_delivery(inbound_id));
    }
}

void test_format_compatibility_and_path_safety() {
    TempStore legacy;
    auto v1_config = legacy.config();
    v1_config.segment_bytes = 0;
    v1_config.max_segments = 0;
    {
        auto created = Journal::open(v1_config);
        CHECK(created.ok());
        if (!created.ok()) return;
        CHECK(created.value().append(RecordKind::outbound, 1, 1, 0, "v1-frame").ok());
    }
    auto segmented_config = legacy.config();
    {
        auto compatible = Journal::open(segmented_config);
        if (!compatible.ok()) std::cerr << "v1 compatibility: " << compatible.status().message()
            << " code=" << static_cast<int>(compatible.status().code)
            << " offset=" << compatible.status().offset << '\n';
        CHECK(compatible.ok());
        if (!compatible.ok()) return;
        CHECK(compatible.value().records().size() == 1);
        CHECK(compatible.value().append(RecordKind::outbound, 2, 2, 0, "v1-next").ok());
    }

    TempStore unsupported;
    const auto config = unsupported.config();
    { auto opened = Journal::open(config); CHECK(opened.ok()); }
    const int fd = ::open(unsupported.path.c_str(), O_RDWR | O_CLOEXEC);
    CHECK(fd >= 0);
    if (fd >= 0) {
        const std::array<unsigned char, 4> version{99, 0, 0, 0};
        CHECK(::pwrite(fd, version.data(), version.size(), 8) == static_cast<ssize_t>(version.size()));
        ::close(fd);
    }
    CHECK(Journal::open(config).status().code == ErrorCode::unsupported_version);

    TempStore symlink_store;
    {
        auto valid = Journal::open(symlink_store.config());
        CHECK(valid.ok());
        if (!valid.ok()) return;
    }
    const auto alias = symlink_store.directory + "/alias.nfxj";
    CHECK(::symlink(symlink_store.path.c_str(), alias.c_str()) == 0);
    auto alias_config = symlink_store.config();
    alias_config.path = alias;
    CHECK(Journal::open(alias_config).status().code == ErrorCode::io_error);

    const auto linked_directory = symlink_store.directory + "/linked";
    CHECK(::symlink(symlink_store.directory.c_str(), linked_directory.c_str()) == 0);
    auto parent_config = symlink_store.config();
    const auto nested = linked_directory + "/outside.nfxj";
    parent_config.path = nested;
    CHECK(Journal::open(parent_config).status().code == ErrorCode::invalid_config);
    CHECK(!std::filesystem::exists(nested));

    CHECK(::chmod(symlink_store.path.c_str(), 0644) == 0);
    CHECK(Journal::open(symlink_store.config()).status().code == ErrorCode::invalid_config);
}

void test_epoch_reset_is_audited_and_crash_recoverable() {
    TempStore store;
    auto config = store.config();
    config.capacity_bytes = 1024U * 1024U + 3U * config.segment_bytes;
    {
        auto opened = Journal::open(config);
        CHECK(opened.ok());
        if (!opened.ok()) return;
        auto& journal = opened.value();
        auto inbound = journal.append(RecordKind::inbound, 1, 0, 0, "in-1");
        CHECK(inbound.ok());
        if (!inbound.ok()) return;
        CHECK(journal.append(RecordKind::outbound, 1, 11, inbound.value().id, "out-1").ok());
        CHECK(journal.append(RecordKind::delivery, 1, 0, inbound.value().id, {}).ok());
        {
            auto lease = journal.pin_record(inbound.value().id);
            CHECK(lease.ok());
            CHECK(journal.begin_epoch("authenticated Logon reset").code == ErrorCode::sequence_reset_denied);
        }
        CHECK(journal.begin_epoch("authenticated Logon reset").ok());
        CHECK(journal.next_inbound_seq() == 1);
        CHECK(journal.next_outbound_seq() == 1);
        CHECK(journal.checkpoint().epoch == 2);
        CHECK(journal.epoch_audit().size() == 1);
        CHECK(journal.epoch_audit()[0].reason == "authenticated Logon reset");
        bool saw_audit = false;
        for (const auto& record : journal.records()) {
            if (record.kind == RecordKind::epoch_reset) {
                CHECK(record.epoch == 1);
                CHECK(record.payload == "authenticated Logon reset");
                saw_audit = true;
            }
        }
        CHECK(saw_audit);
        auto current = journal.append(RecordKind::outbound, 1, 22, 0, "new-epoch-out");
        CHECK(current.ok());
        CHECK(journal.find_outbound(1) != nullptr);
        CHECK(journal.find_outbound(1)->epoch == 2);
        CHECK(journal.append(RecordKind::outbound, 2, 23, 0, large_payload()).ok());
        CHECK(journal.append(RecordKind::outbound, 3, 24, 0, large_payload()).ok());
        CHECK(journal.set_retention_floor(4, inbound.value().id).ok());
        CHECK(journal.epoch_audit().size() == 1);
        CHECK(std::none_of(journal.records().begin(), journal.records().end(), [](const auto& record) {
            return record.kind == RecordKind::epoch_reset;
        }));
    }
    {
        auto reopened = Journal::open(config);
        CHECK(reopened.ok());
        if (!reopened.ok()) return;
        CHECK(reopened.value().checkpoint().epoch == 2);
        CHECK(reopened.value().next_inbound_seq() == 1);
        CHECK(reopened.value().next_outbound_seq() == 4);
        CHECK(reopened.value().find_outbound(3)->epoch == 2);
        CHECK(reopened.value().epoch_audit().size() == 1);
        const auto inspection = Journal::inspect_snapshot(config);
        CHECK(inspection.diagnostic.ok());
        CHECK(inspection.epoch_reset_records == 0);
        CHECK(inspection.audit_records == 1);
    }

    const auto audit_path = store.path + ".audit";
    const int audit_fd = ::open(audit_path.c_str(), O_RDWR | O_CLOEXEC);
    CHECK(audit_fd >= 0);
    if (audit_fd >= 0) {
        unsigned char byte = 0;
        CHECK(::pread(audit_fd, &byte, 1, 4096 + 64) == 1);
        byte ^= 0x01;
        CHECK(::pwrite(audit_fd, &byte, 1, 4096 + 64) == 1);
        ::close(audit_fd);
    }
    CHECK(Journal::open(config).status().code == ErrorCode::corrupt_store);
    CHECK(Journal::inspect_snapshot(config).diagnostic.code == ErrorCode::corrupt_store);

    TempStore legacy;
    auto legacy_config = legacy.config();
    legacy_config.segment_bytes = 0;
    legacy_config.max_segments = 0;
    auto old = Journal::open(legacy_config);
    CHECK(old.ok());
    if (old.ok()) CHECK(old.value().begin_epoch("no v1 reset").code == ErrorCode::unsupported);

    TempStore crash_store;
    auto base = crash_store.config();
    base.capacity_bytes = 1024U * 1024U + 3U * base.segment_bytes;
    {
        auto opened = Journal::open(base);
        CHECK(opened.ok());
        if (!opened.ok()) return;
        auto inbound = opened.value().append(RecordKind::inbound, 1, 0, 0, "before-reset");
        CHECK(inbound.ok());
        if (!inbound.ok()) return;
        CHECK(opened.value().append(RecordKind::outbound, 1, 1, inbound.value().id, "old-out").ok());
        CHECK(opened.value().append(RecordKind::delivery, 1, 0, inbound.value().id, {}).ok());
        CHECK(opened.value().set_retention_floor(2, inbound.value().id).ok());
    }
    const auto child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        KillContext context{JournalFaultPoint::postcommit};
        auto hooked = base;
        hooked.test_hook = {kill_at, &context};
        auto opened = Journal::open(hooked);
        if (!opened.ok()) ::_exit(9);
        (void)opened.value().begin_epoch("authenticated reset after crash");
        ::_exit(10);
    }
    if (child >= 0) CHECK(child_was_killed(child));
    auto recovered = Journal::open(base);
    CHECK(recovered.ok());
    if (recovered.ok()) {
        CHECK(recovered.value().checkpoint().epoch == 2);
        CHECK(recovered.value().next_inbound_seq() == 1);
        CHECK(recovered.value().next_outbound_seq() == 1);
        CHECK(recovered.value().lowest_retained_outbound_seq() == 1);
        CHECK(recovered.value().records().back().kind == RecordKind::epoch_reset);
        CHECK(recovered.value().epoch_audit().size() == 1);
    }

    // A kill after the audit commit but before its checkpoint must preserve
    // both the epoch transition and its side-log copy.
    TempStore audit_crash_store;
    auto audit_base = audit_crash_store.config();
    audit_base.capacity_bytes = 1024U * 1024U + 3U * audit_base.segment_bytes;
    {
        auto opened_audit = Journal::open(audit_base);
        CHECK(opened_audit.ok());
        if (!opened_audit.ok()) return;
        auto inbound = opened_audit.value().append(RecordKind::inbound, 1, 0, 0, "before-audit-reset");
        CHECK(inbound.ok());
        if (!inbound.ok()) return;
        CHECK(opened_audit.value().append(RecordKind::delivery, 1, 0, inbound.value().id, {}).ok());
    }
    const auto audit_child = ::fork();
    CHECK(audit_child >= 0);
    if (audit_child == 0) {
        KillContext context{JournalFaultPoint::audit_published};
        auto hooked = audit_base;
        hooked.test_hook = {kill_at, &context};
        auto opened_audit = Journal::open(hooked);
        if (!opened_audit.ok()) ::_exit(11);
        (void)opened_audit.value().begin_epoch("audit commit boundary");
        ::_exit(12);
    }
    if (audit_child >= 0) CHECK(child_was_killed(audit_child));
    auto audit_recovered = Journal::open(audit_base);
    CHECK(audit_recovered.ok());
    if (audit_recovered.ok()) {
        CHECK(audit_recovered.value().checkpoint().epoch == 2);
        CHECK(audit_recovered.value().epoch_audit().size() == 1);
    }
}

void test_epoch_audit_budget_is_finite() {
    TempStore no_audit_store;
    {
        auto opened = Journal::open(no_audit_store.config());
        CHECK(opened.ok());
        if (!opened.ok()) return;
        CHECK(opened.value().begin_epoch("audit storage is not reserved").code == ErrorCode::storage_full);
        CHECK(opened.value().records().empty());
        CHECK(opened.value().checkpoint().epoch == 1);
    }

    TempStore bounded_store;
    auto config = bounded_store.config();
    config.capacity_bytes = 1024U * 1024U + 3U * config.segment_bytes;
    {
        auto opened = Journal::open(config);
        CHECK(opened.ok());
        if (!opened.ok()) return;
        auto& journal = opened.value();
        const auto frame = large_payload('A');
        for (std::uint32_t epoch = 1; epoch <= 1024; ++epoch) {
            const auto reset = journal.begin_epoch("bounded audit epoch");
            CHECK(reset.ok());
            if (!reset.ok()) return;
            CHECK(journal.append(RecordKind::outbound, 1, epoch, 0, frame).ok());
            CHECK(journal.append(RecordKind::outbound, 2, epoch, 0, frame).ok());
            CHECK(journal.set_retention_floor(3, 0).ok());
        }
        CHECK(journal.epoch_audit().size() == 1024);
        const auto full = journal.begin_epoch("audit budget exhausted");
        CHECK(full.code == ErrorCode::storage_full);
        CHECK(journal.checkpoint().epoch == 1025);
        CHECK(journal.epoch_audit().size() == 1024);
    }
    auto recovered = Journal::open(config);
    CHECK(recovered.ok());
    if (recovered.ok()) {
        CHECK(recovered.value().checkpoint().epoch == 1025);
        CHECK(recovered.value().epoch_audit().size() == 1024);
        CHECK(recovered.value().begin_epoch("still full after restart").code == ErrorCode::storage_full);
    }
}

void test_missing_audit_after_event_retention_fails_closed() {
    TempStore store;
    auto config = store.config();
    config.capacity_bytes = 1024U * 1024U + 3U * config.segment_bytes;
    {
        auto opened = Journal::open(config);
        CHECK(opened.ok());
        if (!opened.ok()) return;
        auto& journal = opened.value();
        CHECK(journal.begin_epoch("audited before retention").ok());
        const auto frame = large_payload();
        CHECK(journal.append(RecordKind::outbound, 1, 1, 0, frame).ok());
        CHECK(journal.append(RecordKind::outbound, 2, 2, 0, frame).ok());
        CHECK(journal.set_retention_floor(3, 0).ok());
        CHECK(std::none_of(journal.records().begin(), journal.records().end(), [](const auto& record) {
            return record.kind == RecordKind::epoch_reset;
        }));
    }
    const auto audit_path = store.path + ".audit";
    CHECK(::unlink(audit_path.c_str()) == 0);
    CHECK(Journal::open(config).status().code == ErrorCode::corrupt_store);
    CHECK(!std::filesystem::exists(audit_path));
}

void test_kill_boundaries_for_v2_metadata_and_rotation() {
    {
        TempStore store;
        const auto child = ::fork();
        CHECK(child >= 0);
        if (child == 0) {
            KillContext context{JournalFaultPoint::segment_created};
            auto config = store.config({kill_at, &context});
            (void)Journal::open(config);
            ::_exit(2);
        }
        if (child >= 0) CHECK(child_was_killed(child));
        auto recovered = Journal::open(store.config());
        CHECK(recovered.ok());
        CHECK(store.segment_files() == 3);
    }
    {
        TempStore store;
        const auto base = store.config();
        {
            auto opened = Journal::open(base);
            CHECK(opened.ok());
            if (!opened.ok()) return;
            CHECK(opened.value().append(RecordKind::outbound, 1, 1, 0, large_payload()).ok());
            CHECK(opened.value().publish_checkpoint(0, 1).ok());
        }
        const auto child = ::fork();
        CHECK(child >= 0);
        if (child == 0) {
            KillContext context{JournalFaultPoint::postcommit};
            auto opened = Journal::open(store.config({kill_at, &context}));
            if (!opened.ok()) ::_exit(3);
            (void)opened.value().append(RecordKind::outbound, 2, 2, 0, large_payload());
            ::_exit(4);
        }
        if (child >= 0) CHECK(child_was_killed(child));
        auto recovered = Journal::open(base);
        CHECK(recovered.ok());
        if (recovered.ok()) {
            CHECK(recovered.value().records().size() == 2);
            CHECK(recovered.value().find_outbound(2) != nullptr);
        }
    }
    {
        TempStore store;
        const auto base = store.config();
        {
            auto opened = Journal::open(base);
            CHECK(opened.ok());
            if (!opened.ok()) return;
            CHECK(opened.value().append(RecordKind::outbound, 1, 1, 0, large_payload()).ok());
            CHECK(opened.value().publish_checkpoint(0, 1).ok());
        }
        const auto child = ::fork();
        CHECK(child >= 0);
        if (child == 0) {
            KillContext context{JournalFaultPoint::checkpoint_published};
            auto opened = Journal::open(store.config({kill_at, &context}));
            if (!opened.ok()) ::_exit(13);
            (void)opened.value().append(RecordKind::outbound, 2, 2, 0, large_payload());
            ::_exit(14);
        }
        if (child >= 0) CHECK(child_was_killed(child));
        auto recovered = Journal::open(base);
        CHECK(recovered.ok());
        if (recovered.ok()) {
            CHECK(recovered.value().find_outbound(2) != nullptr);
            CHECK(recovered.value().next_outbound_seq() == 3);
        }
    }
    {
        TempStore store;
        const auto base = store.config();
        {
            auto opened = Journal::open(base);
            CHECK(opened.ok());
        }
        const auto child = ::fork();
        CHECK(child >= 0);
        if (child == 0) {
            KillContext context{JournalFaultPoint::checkpoint_published};
            auto opened = Journal::open(store.config({kill_at, &context}));
            if (!opened.ok()) ::_exit(5);
            (void)opened.value().publish_checkpoint(0, 1);
            ::_exit(6);
        }
        if (child >= 0) CHECK(child_was_killed(child));
        auto recovered = Journal::open(base);
        CHECK(recovered.ok());
        if (recovered.ok()) CHECK(recovered.value().next_outbound_seq() == 1);
    }
    {
        TempStore store;
        const auto base = store.config();
        RecordId first = 0;
        {
            auto opened = Journal::open(base);
            CHECK(opened.ok());
            if (!opened.ok()) return;
            for (std::uint64_t seq = 1; seq <= 3; ++seq) {
                auto record = opened.value().append(RecordKind::outbound, seq, seq, 0, large_payload());
                CHECK(record.ok());
                if (!record.ok()) return;
                if (seq == 1) first = record.value().id;
            }
        }
        const auto child = ::fork();
        CHECK(child >= 0);
        if (child == 0) {
            KillContext context{JournalFaultPoint::retention_published};
            auto opened = Journal::open(store.config({kill_at, &context}));
            if (!opened.ok()) ::_exit(7);
            (void)opened.value().set_retention_floor(4, 0);
            ::_exit(8);
        }
        if (child >= 0) CHECK(child_was_killed(child));
        auto recovered = Journal::open(base);
        CHECK(recovered.ok());
        if (recovered.ok()) {
            CHECK(recovered.value().lowest_retained_outbound_seq() == 4);
            CHECK(recovered.value().find_record(first) == nullptr);
            CHECK(recovered.value().records().size() == 1);
        }
    }
    {
        TempStore store;
        const auto base = store.config();
        {
            auto opened = Journal::open(base);
            CHECK(opened.ok());
            if (!opened.ok()) return;
            for (std::uint64_t seq = 1; seq <= 3; ++seq)
                CHECK(opened.value().append(RecordKind::outbound, seq, seq, 0, large_payload()).ok());
        }
        const auto child = ::fork();
        CHECK(child >= 0);
        if (child == 0) {
            KillContext context{JournalFaultPoint::segment_created};
            auto opened = Journal::open(store.config({kill_at, &context}));
            if (!opened.ok()) ::_exit(15);
            (void)opened.value().set_retention_floor(4, 0);
            ::_exit(16);
        }
        if (child >= 0) CHECK(child_was_killed(child));
        auto recovered = Journal::open(base);
        CHECK(recovered.ok());
        if (recovered.ok()) {
            CHECK(recovered.value().lowest_retained_outbound_seq() == 4);
            CHECK(recovered.value().records().size() == 1);
            CHECK(recovered.value().next_outbound_seq() == 4);
        }
    }
}

} // namespace

int main() {
    test_rotation_recovery_and_inspection();
    test_safe_retention_reuses_disk_without_reusing_ids();
    test_reader_lease_blocks_retention_and_lag_is_bounded();
    test_checkpoint_corruption_rebuild_and_payload_corruption_fails_closed();
    test_checkpoint_rebuild_after_retention();
    test_retention_keeps_sequence_anchors_for_checkpoint_rebuild();
    test_delivery_cursor_rebuild_and_pruned_cursor_continuity();
    test_completed_inbound_watermark_is_contiguous_and_gapfill_aware();
    test_committed_delivery_completion_survives_process_kill();
    test_format_compatibility_and_path_safety();
    test_epoch_reset_is_audited_and_crash_recoverable();
    test_epoch_audit_budget_is_finite();
    test_missing_audit_after_event_retention_fails_closed();
    test_kill_boundaries_for_v2_metadata_and_rotation();
    if (failures != 0) {
        std::cerr << failures << " journal rotation test(s) failed\n";
        return 1;
    }
    std::cout << "journal rotation, recovery, retention, checkpoint and epoch tests passed\n";
    return 0;
}
