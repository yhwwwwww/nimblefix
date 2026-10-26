#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "nimblefix/status.hpp"

namespace nimblefix {

using RecordId = std::uint64_t;

enum class RecordKind : std::uint8_t {
    inbound = 1,
    outbound = 2,
    delivery = 3,
};

enum class JournalFaultPoint : std::uint8_t {
    header,
    payload,
    precommit,
    postcommit,
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
};

struct JournalRecord {
    RecordId id{};                 // Stable file offset of this record.
    RecordKind kind{};
    std::uint64_t seq{};            // FIX sequence for inbound/outbound/delivery.
    std::uint64_t receipt{};        // Application receipt id for outbound, zero otherwise.
    RecordId dependency{};          // Inbound record id for its outbound/delivery dependents.
    std::uint64_t next_seq{};       // Persisted next-in sequence decision for inbound, zero otherwise.
    std::string_view payload{};     // Raw FIX frame for inbound/outbound; empty for delivery.
    bool delivered{};               // Rebuilt completion cursor; true for delivery or auto-complete GapFill.
};

class Journal {
public:
    Journal(const Journal&) = delete;
    Journal& operator=(const Journal&) = delete;
    Journal(Journal&& other) noexcept;
    Journal& operator=(Journal&& other) noexcept;
    ~Journal();

    [[nodiscard]] static Result<Journal> open(JournalConfig config) noexcept;

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
    [[nodiscard]] const JournalRecord* find_outbound(std::uint64_t seq) const noexcept;
    [[nodiscard]] bool has_delivery(RecordId inbound_id) const noexcept;
    [[nodiscard]] std::uint64_t next_inbound_seq() const noexcept { return next_inbound_seq_; }
    [[nodiscard]] std::uint64_t next_outbound_seq() const noexcept { return next_outbound_seq_; }
    [[nodiscard]] std::size_t capacity_bytes() const noexcept { return capacity_bytes_; }
    [[nodiscard]] std::size_t used_bytes() const noexcept { return write_offset_; }

private:
    Journal() noexcept = default;
    void reset() noexcept;

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
};

inline constexpr std::uint32_t journal_format_version = 1;
inline constexpr std::uint32_t journal_initial_epoch = 1;
inline constexpr std::size_t journal_max_session_key_bytes = 256;

} // namespace nimblefix
