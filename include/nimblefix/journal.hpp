#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nimblefix/status.hpp"

namespace nimblefix {

namespace store {
struct Segment;
}

using RecordId = std::uint64_t;

enum class RecordKind : std::uint8_t {
    inbound = 1,
    outbound = 2,
    delivery = 3,
    epoch_reset = 4,
};

enum class JournalFaultPoint : std::uint8_t {
    header,
    payload,
    precommit,
    postcommit,
    segment_created,
    checkpoint_published,
    retention_published,
    audit_published,
};

using JournalFaultHook = void (*)(JournalFaultPoint point, void* context) noexcept;

struct JournalTestHook {
    JournalFaultHook callback{};
    void* context{};
};

struct JournalConfig {
    // Full base path to one session's segmented journal. Parent directories are created.
    std::string_view path{};
    // Stable identity, for example FIX.4.4|local|remote.
    std::string_view session_key{};
    std::size_t capacity_bytes{16U * 1024U * 1024U};
    std::size_t max_records{65'536};
    // Test-only crash injection. Production configurations leave this empty.
    JournalTestHook test_hook{};
    // Segment size in bytes. Zero automatically selects min(capacity_bytes, 1 MiB)
    // aligned to 8 bytes.
    std::size_t segment_bytes{};
    std::size_t max_segments{};
    // Zero disables the concurrent JournalReader registry (zero hot-path reader overhead).
    std::size_t max_readers{16};
    std::uint64_t max_reader_lag_records{65'536};
};

struct JournalRecord {
    RecordId id{};                 // High 16 bits: segment index; low 48 bits: byte offset.
    RecordKind kind{};
    std::uint64_t seq{};           // FIX sequence for inbound/outbound/delivery.
    std::uint64_t receipt{};       // Application receipt id for outbound, zero otherwise.
    RecordId dependency{};         // Inbound record id for its outbound/delivery dependents.
    std::uint64_t next_seq{};      // Persisted next-in sequence decision for inbound, zero otherwise.
    std::string_view payload{};    // Raw FIX frame for inbound/outbound; empty for delivery.
    bool delivered{};              // Rebuilt completion cursor; true for delivery or auto-complete GapFill.
    std::uint32_t epoch{1};
};

// Handle returned by Journal::reserve() for zero-copy in-place encoding directly
// into the pre-faulted mmap segment. Must be committed via Journal::commit() before
// any other mutation on the Journal, or discarded without writing a commit marker.
struct ReservedRecord {
    std::span<char> payload{};
    RecordKind kind{};
    std::uint64_t seq{};
    std::uint64_t receipt{};
    RecordId dependency{};
    std::uint64_t persisted_next{};
    std::size_t segment_slot{};
    std::size_t offset{};
    std::size_t max_payload_bytes{};
    bool rotated{};
};

struct JournalCheckpoint {
    std::uint32_t epoch{1};
    std::uint64_t next_inbound_seq{1};
    std::uint64_t next_outbound_seq{1};
    RecordId delivery_cursor{};        // Highest contiguous completed inbound record.
    std::uint64_t replay_cursor{1};    // Lowest outbound sequence that may still be requested.
    RecordId last_record_id{};
    std::uint64_t generation{};
    std::uint32_t first_segment_index{};
    std::uint64_t lowest_retained_outbound_seq{1};
    RecordId lowest_retained_record_id{4096};
    std::uint64_t completed_inbound_seq{};
};

struct EpochAuditRecord {
    std::uint32_t previous_epoch{};
    std::uint32_t next_epoch{};
    RecordId reset_record_id{};
    std::string_view reason{};
};

struct JournalInspection {
    Status diagnostic{};
    std::uint32_t format_version{};
    std::size_t segment_count{};
    std::size_t committed_records{};
    std::size_t inbound_records{};
    std::size_t outbound_records{};
    std::size_t delivery_records{};
    std::size_t epoch_reset_records{};
    std::size_t audit_records{};
    std::size_t used_bytes{};
    std::uint64_t next_inbound_seq{1};
    std::uint64_t next_outbound_seq{1};
    RecordId lowest_retained_record_id{};
    std::uint64_t lowest_retained_outbound_seq{1};
    bool checkpoint_stale{};
};

class JournalLease {
public:
    JournalLease() noexcept = default;
    [[nodiscard]] const JournalRecord& record() const noexcept { return record_; }
    [[nodiscard]] std::string_view payload() const noexcept { return record_.payload; }
    [[nodiscard]] RecordId id() const noexcept { return record_.id; }

private:
    JournalRecord record_{};
    std::shared_ptr<void> mapping_lease_{};
    friend class Journal;
};

class Journal;

class JournalReader {
public:
    JournalReader() noexcept = default;
    JournalReader(const JournalReader&) = delete;
    JournalReader& operator=(const JournalReader&) = delete;
    JournalReader(JournalReader&&) noexcept;
    JournalReader& operator=(JournalReader&&) noexcept;
    ~JournalReader();

    [[nodiscard]] Status next(JournalRecord& record) noexcept;
    [[nodiscard]] RecordId cursor() const noexcept;
    [[nodiscard]] std::uint64_t lag_records() const noexcept;

private:
    struct State;
    struct Registry;
    explicit JournalReader(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}
    std::shared_ptr<State> state_{};
    friend class Journal;
};

class Journal {
public:
    Journal(const Journal&) = delete;
    Journal& operator=(const Journal&) = delete;
    Journal(Journal&& other) noexcept;
    Journal& operator=(Journal&& other) noexcept;
    ~Journal();

    [[nodiscard]] static Result<Journal> open(JournalConfig config) noexcept;
    // Read-only inspection does not acquire the writer lock and never repairs files.
    [[nodiscard]] static JournalInspection inspect_snapshot(JournalConfig config) noexcept;

    // Zero-copy in-place reservation: returns a writable payload span directly inside
    // the pre-faulted mmap segment. No heap allocation or synchronous disk flush occurs.
    [[nodiscard]] Result<ReservedRecord> reserve(RecordKind kind,
                                                 std::uint64_t seq,
                                                 std::uint64_t receipt,
                                                 RecordId dependency,
                                                 std::size_t max_payload_bytes,
                                                 std::uint64_t next_inbound = 0) noexcept;

    // Finalizes a reserved record with hardware CRC32C and release commit marker.
    [[nodiscard]] Result<JournalRecord> commit(const ReservedRecord& reserved,
                                               std::size_t actual_payload_bytes) noexcept;

    // Convenience copy-append implemented via reserve() + memcpy + commit().
    [[nodiscard]] Result<JournalRecord> append(RecordKind kind,
                                               std::uint64_t seq,
                                               std::uint64_t receipt,
                                               RecordId dependency,
                                               std::string_view payload,
                                               std::uint64_t next_inbound = 0) noexcept;

    // Off-hot-path maintenance: flushes pending rotation checkpoint and refills
    // preallocated spare segments. Returns success when idle.
    [[nodiscard]] Status poll_maintenance() noexcept;
    [[nodiscard]] bool needs_maintenance() const noexcept { return maintenance_needed_; }

    [[nodiscard]] std::span<const JournalRecord> records() const noexcept { return records_; }
    [[nodiscard]] const JournalRecord* find_record(RecordId id) const noexcept;
    [[nodiscard]] Result<JournalLease> pin_record(RecordId id) const noexcept;
    [[nodiscard]] const JournalRecord* find_outbound(std::uint64_t seq) const noexcept;
    [[nodiscard]] bool has_delivery(RecordId inbound_id) const noexcept;
    [[nodiscard]] Result<JournalReader> create_reader(RecordId start_id = 0) noexcept;
    [[nodiscard]] Status publish_checkpoint(RecordId delivery_cursor,
                                            std::uint64_t replay_cursor) noexcept;
    [[nodiscard]] Status begin_epoch(std::string_view reason) noexcept;
    [[nodiscard]] JournalCheckpoint checkpoint() const noexcept;
    [[nodiscard]] std::span<const EpochAuditRecord> epoch_audit() const noexcept {
        return epoch_audit_records_;
    }
    [[nodiscard]] bool checkpoint_stale() const noexcept { return checkpoint_stale_; }
    // Floors are monotonic. Only whole sealed segments below both floors are reclaimed.
    [[nodiscard]] Status set_retention_floor(std::uint64_t lowest_outbound_seq,
                                             RecordId delivery_cursor) noexcept;
    [[nodiscard]] RecordId lowest_retained_record_id() const noexcept {
        return lowest_retained_record_id_.load(std::memory_order_acquire);
    }
    [[nodiscard]] std::uint64_t lowest_retained_outbound_seq() const noexcept {
        return lowest_retained_outbound_seq_;
    }
    [[nodiscard]] std::uint64_t next_inbound_seq() const noexcept { return next_inbound_seq_; }
    [[nodiscard]] std::uint64_t next_outbound_seq() const noexcept { return next_outbound_seq_; }
    [[nodiscard]] std::uint64_t completed_inbound_seq() const noexcept {
        return completed_inbound_seq_;
    }
    [[nodiscard]] RecordId completed_inbound_record_id() const noexcept {
        return completed_inbound_record_id_;
    }
    [[nodiscard]] std::size_t capacity_bytes() const noexcept { return capacity_bytes_; }
    [[nodiscard]] std::size_t used_bytes() const noexcept { return used_total_bytes_; }

private:
    friend class JournalReader;
    Journal() noexcept = default;
    void reset() noexcept;
    void quiesce_readers() noexcept;
    using Segment = store::Segment;
    [[nodiscard]] Status reader_next(JournalReader::State& state, JournalRecord& record) noexcept;
    void advance_completed_inbound_cursor() noexcept;
    [[nodiscard]] Status write_checkpoint_file(const JournalCheckpoint& checkpoint) noexcept;
    [[nodiscard]] Status refill_spare_segments() noexcept;
    [[nodiscard]] Status append_epoch_audit(std::uint32_t previous_epoch,
                                            std::uint32_t next_epoch,
                                            RecordId record_id,
                                            std::string_view reason) noexcept;

    int fd_{-1};
    std::size_t capacity_bytes_{};
    std::uint64_t next_inbound_seq_{1};
    std::uint64_t next_outbound_seq_{1};
    std::size_t max_records_{};
    JournalTestHook test_hook_{};
    std::vector<JournalRecord> records_{};
    std::vector<std::size_t> outbound_index_{};
    const JournalRecord* records_data_{};
    std::vector<std::shared_ptr<Segment>> segments_{};
    std::shared_ptr<JournalReader::Registry> reader_registry_{};
    std::atomic<std::size_t> published_record_count_{};
    std::size_t active_segment_{};
    std::size_t segment_capacity_bytes_{};
    std::size_t max_segments_{};
    std::uint32_t next_segment_index_{};
    std::uint32_t first_segment_index_{};
    std::size_t used_total_bytes_{};
    std::size_t max_readers_{};
    std::uint64_t max_reader_lag_records_{};
    std::uint64_t lowest_retained_outbound_seq_{1};
    std::atomic<RecordId> lowest_retained_record_id_{4096};
    std::uint64_t completed_inbound_seq_{};
    RecordId completed_inbound_record_id_{};
    std::size_t completed_scan_index_{};
    JournalCheckpoint checkpoint_{};
    bool checkpoint_stale_{};
    bool maintenance_needed_{};
    std::uint32_t current_epoch_{1};
    bool epoch_reset_armed_{};
    std::string base_path_{};
    std::string session_key_{};
    int audit_fd_{-1};
    void* audit_mapping_{};
    std::size_t audit_file_size_{};
    std::size_t audit_bytes_{};
    std::size_t audit_count_{};
    std::vector<EpochAuditRecord> epoch_audit_records_{};
};

inline constexpr std::uint32_t journal_format_version = 2;
inline constexpr std::uint32_t journal_initial_epoch = 1;
inline constexpr std::size_t journal_max_session_key_bytes = 256;

} // namespace nimblefix
