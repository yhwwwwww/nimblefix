#include "nimblefix/journal.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using nimblefix::ErrorCode;
using nimblefix::Journal;
using nimblefix::JournalConfig;
using nimblefix::JournalFaultPoint;
using nimblefix::JournalRecord;
using nimblefix::JournalTestHook;
using nimblefix::RecordId;
using nimblefix::RecordKind;

int failures = 0;

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #condition << '\n'; \
            ++failures;                                                                     \
        }                                                                                   \
    } while (false)

struct TempJournal {
    std::string directory;
    std::string path;

    TempJournal() {
        char pattern[] = "/tmp/nimblefix-journal-XXXXXX";
        char* result = ::mkdtemp(pattern);
        if (result != nullptr) {
            directory = result;
            path = directory + "/session.journal";
        }
    }

    ~TempJournal() {
        if (!path.empty()) ::unlink(path.c_str());
        if (!directory.empty()) ::rmdir(directory.c_str());
    }

    [[nodiscard]] JournalConfig config(std::size_t capacity = 64U * 1024U,
                                       std::size_t max_records = 256,
                                       JournalTestHook hook = {}) const {
        return {path, "FIX.4.4/local/remote", capacity, max_records, hook};
    }
};

void test_append_recovery_lock_and_dependencies() {
    TempJournal temp;
    CHECK(!temp.path.empty());
    const auto config = temp.config();
    {
        auto opened = Journal::open(config);
        CHECK(opened.ok());
        if (!opened.ok()) return;
        auto& journal = opened.value();
        struct stat info {};
        CHECK(::stat(temp.path.c_str(), &info) == 0);
        CHECK(static_cast<std::size_t>(info.st_size) == config.capacity_bytes);

        auto inbound = journal.append(RecordKind::inbound, 1, 0, 0, "raw-inbound-1");
        CHECK(inbound.ok());
        if (!inbound.ok()) return;
        CHECK(inbound.value().id == 4096);
        CHECK(inbound.value().next_seq == 2);
        CHECK(inbound.value().payload == "raw-inbound-1");
        CHECK(journal.next_inbound_seq() == 2);
        CHECK(!journal.has_delivery(inbound.value().id));

        auto outbound = journal.append(RecordKind::outbound, 1, 7001,
                                      inbound.value().id, "raw-outbound-1");
        CHECK(outbound.ok());
        if (!outbound.ok()) return;
        CHECK(outbound.value().dependency == inbound.value().id);
        CHECK(journal.next_outbound_seq() == 2);
        CHECK(journal.find_outbound(1) != nullptr);
        CHECK(journal.find_outbound(1)->receipt == 7001);

        auto delivery = journal.append(RecordKind::delivery, 1, 0,
                                       inbound.value().id, {});
        CHECK(delivery.ok());
        CHECK(journal.has_delivery(inbound.value().id));
        CHECK(journal.records().size() == 3);

        auto locked = Journal::open(config);
        CHECK(!locked.ok());
        CHECK(locked.status().code == ErrorCode::store_locked);

        CHECK(journal.append(RecordKind::inbound, 4, 0, 0, "gap").status().code ==
              ErrorCode::sequence_gap);
        CHECK(journal.append(RecordKind::outbound, 3, 0, 0, "gap").status().code ==
              ErrorCode::sequence_gap);
        CHECK(journal.records().size() == 3);
    }

    auto recovered = Journal::open(config);
    CHECK(recovered.ok());
    if (!recovered.ok()) return;
    const auto& journal = recovered.value();
    CHECK(journal.records().size() == 3);
    CHECK(journal.next_inbound_seq() == 2);
    CHECK(journal.next_outbound_seq() == 2);
    CHECK(journal.has_delivery(journal.records()[0].id));
    CHECK(journal.find_outbound(1) != nullptr);
    CHECK(journal.find_outbound(1)->payload == "raw-outbound-1");
    CHECK(journal.records()[0].delivered);
}

void test_parent_directory_creation_and_indexes() {
    TempJournal temp;
    const std::string nested_path = temp.directory + "/new/subdir/session.journal";
    auto config = temp.config(1024U * 1024U, 128);
    config.path = nested_path;
    {
        auto opened = Journal::open(config);
        CHECK(opened.ok());
        if (!opened.ok()) return;
        auto& journal = opened.value();
        for (std::uint64_t seq = 1; seq <= 32; ++seq) {
            auto appended = journal.append(RecordKind::outbound, seq, seq * 10, 0, "out");
            CHECK(appended.ok());
            if (!appended.ok()) return;
        }
        for (std::uint64_t seq = 1; seq <= 32; ++seq) {
            const auto* outbound = journal.find_outbound(seq);
            CHECK(outbound != nullptr);
            if (outbound != nullptr) CHECK(outbound->receipt == seq * 10);
        }
        CHECK(journal.find_outbound(33) == nullptr);
        CHECK(journal.find_record(4096) != nullptr);
    }
    struct stat info {};
    CHECK(::stat(nested_path.c_str(), &info) == 0);
    CHECK(::unlink(nested_path.c_str()) == 0);
    CHECK(::rmdir((temp.directory + "/new/subdir").c_str()) == 0);
    CHECK(::rmdir((temp.directory + "/new").c_str()) == 0);
}

void test_gapfill_sequence_decision_is_inbound_commit() {
    TempJournal temp;
    const auto config = temp.config();
    {
        auto opened = Journal::open(config);
        CHECK(opened.ok());
        if (!opened.ok()) return;
        auto reset = opened.value().append(RecordKind::inbound, 1, 0, 0,
                                           "gapfill-frame", 7);
        CHECK(reset.ok());
        if (!reset.ok()) return;
        CHECK(reset.value().next_seq == 7);
        CHECK(opened.value().next_inbound_seq() == 7);
        CHECK(opened.value().has_delivery(reset.value().id));
    }

    auto recovered = Journal::open(config);
    CHECK(recovered.ok());
    if (!recovered.ok()) return;
    CHECK(recovered.value().next_inbound_seq() == 7);
    auto next = recovered.value().append(RecordKind::inbound, 7, 0, 0, "next-frame");
    CHECK(next.ok());
    CHECK(next.value().next_seq == 8);
    CHECK(recovered.value().next_inbound_seq() == 8);
}

void test_bounded_capacity_and_record_count() {
    TempJournal temp;
    const auto config = temp.config(4096 + 64 + 8 + 64 + 8, 2);
    auto opened = Journal::open(config);
    CHECK(opened.ok());
    if (!opened.ok()) return;
    auto& journal = opened.value();
    auto first = journal.append(RecordKind::outbound, 1, 1, 0, "12345678");
    CHECK(first.ok());
    CHECK(journal.used_bytes() == 4096 + 72);
    const auto full = journal.append(RecordKind::outbound, 2, 2, 0, "123456789012345678901234567890123456");
    CHECK(!full.ok());
    CHECK(full.status().code == ErrorCode::storage_full);
    CHECK(journal.records().size() == 1);

    TempJournal count_temp;
    const auto count_config = count_temp.config(4096 + 64 * 4, 1);
    auto count_open = Journal::open(count_config);
    CHECK(count_open.ok());
    if (!count_open.ok()) return;
    CHECK(count_open.value().append(RecordKind::outbound, 1, 1, 0, "frame").ok());
    const auto count_full = count_open.value().append(RecordKind::outbound, 2, 2, 0, "frame");
    CHECK(count_full.status().code == ErrorCode::storage_full);
}

struct KillContext {
    JournalFaultPoint target{};
};

void kill_at_fault(JournalFaultPoint point, void* opaque) noexcept {
    auto* context = static_cast<KillContext*>(opaque);
    if (point == context->target) {
        (void)::kill(::getpid(), SIGKILL);
        ::_exit(128 + SIGKILL);
    }
}

void test_process_death_at_commit_boundaries() {
    constexpr std::array fault_points{
        JournalFaultPoint::header,
        JournalFaultPoint::payload,
        JournalFaultPoint::precommit,
        JournalFaultPoint::postcommit,
    };

    for (const auto target : fault_points) {
        TempJournal temp;
        const auto base_config = temp.config();
        {
            auto initialized = Journal::open(base_config);
            CHECK(initialized.ok());
            if (!initialized.ok()) continue;
        }

        const pid_t child = ::fork();
        CHECK(child >= 0);
        if (child == 0) {
            KillContext context{target};
            auto child_config = temp.config(64U * 1024U, 256,
                                            JournalTestHook{kill_at_fault, &context});
            auto opened = Journal::open(child_config);
            if (!opened.ok()) ::_exit(20);
            const std::string payload(512, 'P');
            auto appended = opened.value().append(RecordKind::inbound, 1, 0, 0,
                                                  payload, 42);
            ::_exit(appended.ok() ? 0 : 21);
        }
        if (child < 0) continue;

        int wait_status = 0;
        CHECK(::waitpid(child, &wait_status, 0) == child);
        CHECK(WIFSIGNALED(wait_status));
        if (WIFSIGNALED(wait_status)) CHECK(WTERMSIG(wait_status) == SIGKILL);

        const bool committed = target == JournalFaultPoint::postcommit;
        {
            auto recovered = Journal::open(base_config);
            CHECK(recovered.ok());
            if (!recovered.ok()) continue;
            CHECK(recovered.value().records().size() == (committed ? 1U : 0U));
            CHECK(recovered.value().next_inbound_seq() == (committed ? 42U : 1U));
            if (committed) {
                CHECK(recovered.value().records()[0].seq == 1);
                CHECK(recovered.value().records()[0].next_seq == 42);
                CHECK(recovered.value().records()[0].payload.size() == 512);
            } else {
                // Reuse the discarded tail for a shorter record, then reopen;
                // stale uncommitted bytes must not look like a commit marker.
                CHECK(recovered.value().append(RecordKind::inbound, 1, 0, 0, "x").ok());
            }
        }
        if (!committed) {
            auto recovered_again = Journal::open(base_config);
            CHECK(recovered_again.ok());
            if (recovered_again.ok()) {
                CHECK(recovered_again.value().records().size() == 1);
                CHECK(recovered_again.value().records()[0].payload == "x");
                CHECK(recovered_again.value().next_inbound_seq() == 2);
            }
        }
    }
}

void put_le32(unsigned char* output, std::uint32_t value) {
    output[0] = static_cast<unsigned char>(value);
    output[1] = static_cast<unsigned char>(value >> 8U);
    output[2] = static_cast<unsigned char>(value >> 16U);
    output[3] = static_cast<unsigned char>(value >> 24U);
}

void create_one_inbound(const JournalConfig& config) {
    auto opened = Journal::open(config);
    CHECK(opened.ok());
    if (!opened.ok()) return;
    CHECK(opened.value().append(RecordKind::inbound, 1, 0, 0, "committed-frame").ok());
}

void test_identity_truncation_version_and_committed_corruption() {
    TempJournal identity_temp;
    const auto identity_config = identity_temp.config();
    create_one_inbound(identity_config);
    auto wrong_identity = identity_config;
    wrong_identity.session_key = "FIX.4.4/other/remote";
    auto identity_result = Journal::open(wrong_identity);
    CHECK(identity_result.status().code == ErrorCode::identity_mismatch);

    TempJournal truncation_temp;
    const auto truncation_config = truncation_temp.config();
    create_one_inbound(truncation_config);
    CHECK(::truncate(truncation_temp.path.c_str(), 4096) == 0);
    auto truncated = Journal::open(truncation_config);
    CHECK(truncated.status().code == ErrorCode::corrupt_store);

    TempJournal version_temp;
    const auto version_config = version_temp.config();
    create_one_inbound(version_config);
    int version_fd = ::open(version_temp.path.c_str(), O_RDWR | O_CLOEXEC);
    CHECK(version_fd >= 0);
    if (version_fd >= 0) {
        std::array<unsigned char, 4> unsupported{};
        put_le32(unsupported.data(), 99);
        CHECK(::pwrite(version_fd, unsupported.data(), unsupported.size(), 8) ==
              static_cast<ssize_t>(unsupported.size()));
        ::close(version_fd);
    }
    auto version_result = Journal::open(version_config);
    CHECK(version_result.status().code == ErrorCode::unsupported_version);

    TempJournal corruption_temp;
    const auto corruption_config = corruption_temp.config();
    RecordId id = 0;
    {
        auto opened = Journal::open(corruption_config);
        CHECK(opened.ok());
        if (!opened.ok()) return;
        auto record = opened.value().append(RecordKind::inbound, 1, 0, 0, "committed-frame");
        CHECK(record.ok());
        if (!record.ok()) return;
        id = record.value().id;
    }
    int corruption_fd = ::open(corruption_temp.path.c_str(), O_RDWR | O_CLOEXEC);
    CHECK(corruption_fd >= 0);
    if (corruption_fd >= 0) {
        const unsigned char corrupted = 'X';
        CHECK(::pwrite(corruption_fd, &corrupted, 1, static_cast<off_t>(id + 64)) == 1);
        ::close(corruption_fd);
    }
    auto corrupt = Journal::open(corruption_config);
    CHECK(corrupt.status().code == ErrorCode::corrupt_store);

    TempJournal marker_temp;
    const auto marker_config = marker_temp.config();
    RecordId first_id = 0;
    {
        auto opened = Journal::open(marker_config);
        CHECK(opened.ok());
        if (!opened.ok()) return;
        auto first = opened.value().append(RecordKind::inbound, 1, 0, 0, "first");
        auto second = opened.value().append(RecordKind::inbound, 2, 0, 0, "second");
        CHECK(first.ok() && second.ok());
        if (!first.ok() || !second.ok()) return;
        first_id = first.value().id;
    }
    int marker_fd = ::open(marker_temp.path.c_str(), O_RDWR | O_CLOEXEC);
    CHECK(marker_fd >= 0);
    if (marker_fd >= 0) {
        const std::array<unsigned char, 4> zero{};
        CHECK(::pwrite(marker_fd, zero.data(), zero.size(),
                       static_cast<off_t>(first_id + 56)) == static_cast<ssize_t>(zero.size()));
        ::close(marker_fd);
    }
    auto missing_middle_commit = Journal::open(marker_config);
    CHECK(missing_middle_commit.status().code == ErrorCode::corrupt_store);
}

} // namespace

int main() {
    test_append_recovery_lock_and_dependencies();
    test_parent_directory_creation_and_indexes();
    test_gapfill_sequence_decision_is_inbound_commit();
    test_bounded_capacity_and_record_count();
    test_process_death_at_commit_boundaries();
    test_identity_truncation_version_and_committed_corruption();
    if (failures != 0) {
        std::cerr << failures << " journal test(s) failed\n";
        return 1;
    }
    std::cout << "journal tests passed\n";
    return 0;
}
