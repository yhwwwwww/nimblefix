#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nimblefix/status.hpp"

namespace nimblefix {

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
    // Full path to one session's journal. The parent directory must exist.
    std::string_view path{};
    // Stable identity, for example FIX.4.4/localCompId/remoteCompId.
    std::string_view session_key{};
    std::size_t capacity_bytes{16U * 1024U * 1024U};
    std::size_t max_records{65'536};
    // Test-only crash injection. Production configurations leave this empty.
    JournalTestHook test_hook{};
    // Nonzero opts into the v2 segmented queue. capacity_bytes is the total
    // preallocated storage budget, including the fixed epoch-audit file when
    // the budget can hold it and at least one segment. max_segments is capped
    // by that budget. Zero preserves the v1 single-file format and behavior.
    std::size_t segment_bytes{};
    std::size_t max_segments{};
    std::size_t max_readers{16};
    std::uint64_t max_reader_lag_records{65'536};
};

struct JournalRecord {
    RecordId id{};                 // Stable offset; v2 uses segment index in the high 16 bits.
    RecordKind kind{};
    std::uint64_t seq{};            // FIX sequence for inbound/outbound/delivery.
    std::uint64_t receipt{};        // Application receipt id for outbound, zero otherwise.
    RecordId dependency{};          // Inbound record id for its outbound/delivery dependents.
    std::uint64_t next_seq{};       // Persisted next-in sequence decision for inbound, zero otherwise.
    std::string_view payload{};     // Raw FIX frame for inbound/outbound; empty for delivery.
    bool delivered{};               // Rebuilt completion cursor; true for delivery or auto-complete GapFill.
    std::uint32_t epoch{1};
};

struct JournalCheckpoint {
    std::uint32_t epoch{1};
    std::uint64_t next_inbound_seq{1};
    std::uint64_t next_outbound_seq{1};
    RecordId delivery_cursor{};        // Highest contiguous completed inbound record.
    std::uint64_t replay_cursor{1};   // Lowest outbound sequence that may still be requested.
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

    // In v2, this reader pins the segment mappings it can read and pauses
    // retention until released. Journal must remain alive while calling next.
    // The returned payload is borrowed: copy it before the next next() call,
    // reader expiry/destruction, or releasing the reader. pin_record() creates
    // an explicit owner-side lease when a payload must live independently.
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

    // Mutations, reader creation, and moving the owner are owner-thread work.
    // Existing JournalReader::next/lag_records calls may run on workers;
    // destruction disarms their owner pointer and waits for in-flight calls.

    // Appends one record and publishes it with a release commit marker. The
    // payload is copied into the preallocated mapping; no append-time allocation
    // or synchronous disk flush is performed. For inbound records, next_inbound
    // is committed atomically with the raw frame; zero means seq + 1, and a
    // SequenceReset GapFill supplies its NewSeqNo. Other record kinds require 0.
    [[nodiscard]] Result<JournalRecord> append(RecordKind kind,
                                               std::uint64_t seq,
                                               std::uint64_t receipt,
                                               RecordId dependency,
                                               std::string_view payload,
                                               std::uint64_t next_inbound = 0) noexcept;

    [[nodiscard]] std::span<const JournalRecord> records() const noexcept {
        return records_;
    }
    [[nodiscard]] const JournalRecord* find_record(RecordId id) const noexcept;
    [[nodiscard]] Result<JournalLease> pin_record(RecordId id) const noexcept;
    [[nodiscard]] const JournalRecord* find_outbound(std::uint64_t seq) const noexcept;
    [[nodiscard]] bool has_delivery(RecordId inbound_id) const noexcept;
    [[nodiscard]] Result<JournalReader> create_reader(RecordId start_id = 0) noexcept;
    // Publish cursor metadata after its referenced records are committed.
    [[nodiscard]] Status publish_checkpoint(RecordId delivery_cursor,
                                            std::uint64_t replay_cursor) noexcept;
    [[nodiscard]] Status begin_epoch(std::string_view reason) noexcept;
    [[nodiscard]] JournalCheckpoint checkpoint() const noexcept;
    [[nodiscard]] std::span<const EpochAuditRecord> epoch_audit() const noexcept {
        return epoch_audit_records_;
    }
    [[nodiscard]] bool checkpoint_stale() const noexcept { return checkpoint_stale_; }
    // Floors are monotonic. Only whole sealed segments below both floors are reclaimed.
    // Retention is disabled until the caller explicitly publishes a floor.
    [[nodiscard]] Status set_retention_floor(std::uint64_t lowest_outbound_seq,
                                             RecordId delivery_cursor) noexcept;
    [[nodiscard]] RecordId lowest_retained_record_id() const noexcept {
        return lowest_retained_record_id_;
    }
    [[nodiscard]] std::uint64_t lowest_retained_outbound_seq() const noexcept {
        return lowest_retained_outbound_seq_;
    }
    [[nodiscard]] std::uint64_t next_inbound_seq() const noexcept { return next_inbound_seq_; }
    [[nodiscard]] std::uint64_t next_outbound_seq() const noexcept { return next_outbound_seq_; }
    // Highest contiguous inbound FIX sequence completed by a delivery marker
    // or an automatic GapFill decision in the current epoch.
    [[nodiscard]] std::uint64_t completed_inbound_seq() const noexcept {
        return completed_inbound_seq_;
    }
    [[nodiscard]] RecordId completed_inbound_record_id() const noexcept {
        return completed_inbound_record_id_;
    }
    [[nodiscard]] std::size_t capacity_bytes() const noexcept { return capacity_bytes_; }
    [[nodiscard]] std::size_t used_bytes() const noexcept {
        return format_version_ >= 2 ? used_total_bytes_ : write_offset_;
    }

private:
    friend class JournalReader;
    Journal() noexcept = default;
    void reset() noexcept;
    void quiesce_readers() noexcept;
    struct Segment;
    [[nodiscard]] static Result<Journal> open_segmented(JournalConfig config) noexcept;
    [[nodiscard]] Result<JournalRecord> append_segmented(RecordKind kind,
                                                         std::uint64_t seq,
                                                         std::uint64_t receipt,
                                                         RecordId dependency,
                                                         std::string_view payload,
                                                         std::uint64_t next_inbound) noexcept;
    [[nodiscard]] Status reader_next(JournalReader::State& state, JournalRecord& record) noexcept;
    void advance_completed_inbound_cursor() noexcept;
    [[nodiscard]] Status write_checkpoint_file(const JournalCheckpoint& checkpoint) noexcept;
    [[nodiscard]] Status append_epoch_audit(std::uint32_t previous_epoch,
                                            std::uint32_t next_epoch,
                                            RecordId record_id,
                                            std::string_view reason) noexcept;

    int fd_{-1};
    void* mapping_{};
    std::size_t mapping_size_{};
    std::size_t capacity_bytes_{};
    std::size_t write_offset_{};
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
    std::uint32_t format_version_{1};
    std::size_t used_total_bytes_{};
    std::size_t max_readers_{};
    std::uint64_t max_reader_lag_records_{};
    std::uint64_t lowest_retained_outbound_seq_{1};
    RecordId lowest_retained_record_id_{4096};
    std::uint64_t completed_inbound_seq_{};
    RecordId completed_inbound_record_id_{};
    std::size_t completed_scan_index_{};
    JournalCheckpoint checkpoint_{};
    bool checkpoint_stale_{};
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

inline constexpr std::uint32_t journal_format_version = 1;
inline constexpr std::uint32_t journal_initial_epoch = 1;
inline constexpr std::size_t journal_max_session_key_bytes = 256;

} // namespace nimblefix
