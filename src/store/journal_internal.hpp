#pragma once

#include "journal_format.hpp"
#include "nimblefix/journal.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

namespace nimblefix {

namespace store {

struct Segment {
    int fd{-1};
    void* mapping{};
    std::size_t mapping_size{};
    std::size_t write_offset{store::superblock_size};
    std::uint32_t index{};
    std::string path{};
    // Explicit acquire/release lease counter replaces unsynchronized shared_ptr::use_count().
    std::atomic<std::size_t> active_leases{0};

    ~Segment() {
        if (mapping != nullptr && mapping_size != 0) ::munmap(mapping, mapping_size);
        if (fd >= 0) ::close(fd);
    }
};

} // namespace store

struct JournalReader::Registry {
    // Cache-line padded per-slot state eliminates false sharing and global active_calls bouncing.
    struct alignas(64) Slot {
        std::atomic<Journal*> owner{};
        std::atomic<State*> state{};
        std::atomic<std::size_t> position{};
        std::atomic<std::size_t> active_calls{};
        std::atomic<bool> claimed{};
        std::atomic<bool> active{};
    };
    explicit Registry(std::size_t count) : slots(std::make_unique<Slot[]>(count)), capacity(count) {}
    std::unique_ptr<Slot[]> slots{};
    std::size_t capacity{};
    alignas(64) std::atomic<std::size_t> active_count{};
};

struct JournalReader::State {
    std::shared_ptr<Registry> registry{};
    std::vector<std::shared_ptr<void>> mapping_leases{};
    std::size_t slot{static_cast<std::size_t>(-1)};
    std::atomic<RecordId> cursor{};

    ~State() {
        if (!registry || slot >= registry->capacity) return;
        auto& entry = registry->slots[slot];
        State* expected = this;
        if (entry.state.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel)) {
            entry.active.store(false, std::memory_order_release);
            entry.owner.store(nullptr, std::memory_order_release);
            registry->active_count.fetch_sub(1, std::memory_order_acq_rel);
            entry.claimed.store(false, std::memory_order_release);
        }
    }
};

namespace store {

// Atomically creates a new preallocated, pre-faulted, header-initialized data segment
// via temporary file + rename() so SIGKILL during creation never leaves a corrupt .segN.
[[nodiscard]] Result<std::shared_ptr<Segment>> create_segment_atomic(
    std::string_view base_path,
    std::uint32_t index,
    std::size_t segment_bytes,
    std::string_view session_key,
    std::size_t capacity_bytes,
    std::size_t max_segments,
    std::size_t audit_bytes,
    const JournalTestHook& test_hook) noexcept;

// Opens an existing data segment file and verifies its v2 superblock header.
[[nodiscard]] Result<std::shared_ptr<Segment>> open_existing_segment(
    std::string_view base_path,
    std::uint32_t index,
    std::size_t segment_bytes,
    std::string_view session_key,
    std::size_t capacity_bytes,
    std::size_t max_segments,
    std::size_t audit_bytes) noexcept;

// Creates a shared_ptr<void> lease that pins the segment mapping and atomically
// tracks Segment::active_leases with acquire/release synchronization.
[[nodiscard]] std::shared_ptr<void> acquire_segment_lease(
    std::shared_ptr<Segment> segment) noexcept;

} // namespace store

} // namespace nimblefix
