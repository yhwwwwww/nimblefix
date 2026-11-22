#pragma once

#include "crc32c.hpp"
#include "nimblefix/journal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace nimblefix::store {

inline constexpr std::size_t superblock_size = 4096;
inline constexpr std::size_t superblock_crc_offset = 32;
inline constexpr std::size_t superblock_key_offset = 64;
inline constexpr std::size_t record_header_size = 64;
inline constexpr std::size_t record_crc_offset = 48;
inline constexpr std::size_t record_commit_offset = 56;
inline constexpr std::uint32_t record_commit_marker = 0x4e46434dU; // "NFCM"
inline constexpr std::uint32_t current_epoch_initial = journal_initial_epoch;

inline constexpr std::array<unsigned char, 8> v2_manifest_magic{
    'N', 'I', 'M', 'B', 'L', 'J', '2', 0
};
inline constexpr std::array<unsigned char, 8> v2_segment_magic{
    'N', 'I', 'M', 'B', 'S', 'E', 'G', '2'
};
inline constexpr std::array<unsigned char, 8> checkpoint_magic{
    'N', 'F', 'X', 'C', 'K', 'P', '2', 0
};
inline constexpr std::array<unsigned char, 8> audit_magic{
    'N', 'F', 'X', 'A', 'U', 'D', '2', 0
};

inline constexpr std::size_t v2_header_crc_offset = 60;
inline constexpr std::size_t checkpoint_fixed_size = 128;
inline constexpr std::size_t checkpoint_crc_offset = 12;
inline constexpr std::uint32_t maximum_segment_index = 0xffffU;
inline constexpr std::uint64_t record_offset_mask = (std::uint64_t{1} << 48U) - 1U;
inline constexpr std::size_t audit_file_size = 1024U * 1024U;
inline constexpr std::size_t audit_entry_count = 1024;
inline constexpr std::size_t audit_entry_size = (audit_file_size - superblock_size) / audit_entry_count;
inline constexpr std::size_t audit_commit_offset = audit_entry_size - sizeof(std::uint32_t);
inline constexpr std::uint32_t audit_commit_marker = 0x4e464155U; // "NFAU"
static_assert(audit_entry_size == 1020 && (audit_entry_size & 3U) == 0);
static_assert(std::atomic_ref<std::uint32_t>::is_always_lock_free,
              "the journal commit marker requires lock-free 32-bit atomics");

[[nodiscard]] constexpr std::uint32_t to_little_endian(std::uint32_t value) noexcept {
    if constexpr (std::endian::native == std::endian::little) return value;
    return ((value & 0x000000ffU) << 24U) | ((value & 0x0000ff00U) << 8U) |
           ((value & 0x00ff0000U) >> 8U) | ((value & 0xff000000U) >> 24U);
}

[[nodiscard]] constexpr std::uint32_t from_little_endian(std::uint32_t value) noexcept {
    return to_little_endian(value);
}

inline void put_u32(unsigned char* destination, std::uint32_t value) noexcept {
    value = to_little_endian(value);
    std::memcpy(destination, &value, sizeof(value));
}

inline void put_u64(unsigned char* destination, std::uint64_t value) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        std::memcpy(destination, &value, sizeof(value));
        return;
    }
    for (std::size_t i = 0; i < sizeof(value); ++i)
        destination[i] = static_cast<unsigned char>(value >> (i * 8U));
}

[[nodiscard]] inline std::uint32_t get_u32(const unsigned char* source) noexcept {
    std::uint32_t value = 0;
    std::memcpy(&value, source, sizeof(value));
    return from_little_endian(value);
}

[[nodiscard]] inline std::uint64_t get_u64(const unsigned char* source) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        std::uint64_t value = 0;
        std::memcpy(&value, source, sizeof(value));
        return value;
    }
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < sizeof(value); ++i)
        value |= static_cast<std::uint64_t>(source[i]) << (i * 8U);
    return value;
}

[[nodiscard]] inline std::uint32_t load_u32_acquire(const unsigned char* source) noexcept {
    auto* word = reinterpret_cast<std::uint32_t*>(const_cast<unsigned char*>(source));
    std::atomic_ref<std::uint32_t> marker(*word);
    return from_little_endian(marker.load(std::memory_order_acquire));
}

[[nodiscard]] inline std::uint32_t superblock_crc(const unsigned char* bytes) noexcept {
    return crc32c_with_hole(bytes, superblock_size, superblock_crc_offset, sizeof(std::uint32_t));
}

[[nodiscard]] inline std::uint32_t record_crc(const unsigned char* bytes,
                                              std::size_t total_size) noexcept {
    return crc32c_with_two_holes(bytes, total_size,
                                 record_crc_offset, sizeof(std::uint32_t),
                                 record_commit_offset, sizeof(std::uint32_t));
}

[[nodiscard]] inline std::uint32_t audit_entry_crc(const unsigned char* bytes) noexcept {
    return crc32c_with_two_holes(bytes, audit_entry_size,
                                 24, sizeof(std::uint32_t),
                                 audit_commit_offset, sizeof(std::uint32_t));
}

[[nodiscard]] constexpr std::size_t align_record(std::size_t value) noexcept {
    return (value + 7U) & ~std::size_t{7U};
}

[[nodiscard]] constexpr bool valid_record_kind(std::uint32_t raw_kind) noexcept {
    return raw_kind == static_cast<std::uint32_t>(RecordKind::inbound) ||
           raw_kind == static_cast<std::uint32_t>(RecordKind::outbound) ||
           raw_kind == static_cast<std::uint32_t>(RecordKind::delivery) ||
           raw_kind == static_cast<std::uint32_t>(RecordKind::epoch_reset);
}

[[nodiscard]] constexpr RecordId segment_record_id(std::uint32_t index, std::size_t offset) noexcept {
    return (static_cast<RecordId>(index) << 48U) | static_cast<RecordId>(offset);
}

[[nodiscard]] constexpr std::uint32_t segment_index_from_id(RecordId id) noexcept {
    return static_cast<std::uint32_t>(id >> 48U);
}

[[nodiscard]] inline Status io_status(int error_number, std::size_t offset = 0) noexcept {
    if (error_number == ENOSPC || error_number == EDQUOT)
        return {ErrorCode::storage_full, 0, offset};
    return {ErrorCode::io_error, 0, offset};
}

[[nodiscard]] inline std::string segment_path(std::string_view base, std::uint32_t index) {
    std::string path(base);
    path += ".seg";
    path += std::to_string(index);
    return path;
}

[[nodiscard]] inline std::string make_temp_path(std::string_view target) {
    static std::atomic<std::uint64_t> counter{1};
    std::string temp(target);
    temp += ".tmp.";
    temp += std::to_string(::getpid());
    temp += '.';
    temp += std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
    return temp;
}

inline void touch_mapping(void* mapping, std::size_t size) noexcept {
    auto* bytes = static_cast<volatile unsigned char*>(mapping);
    constexpr std::size_t page = 4096;
    for (std::size_t offset = 0; offset < size; offset += page) {
        const auto value = bytes[offset];
        bytes[offset] = value;
    }
    if (size != 0) {
        const auto value = bytes[size - 1];
        bytes[size - 1] = value;
    }
}

[[nodiscard]] inline Status pread_exact(int fd, unsigned char* bytes, std::size_t size) noexcept {
    std::size_t read_count = 0;
    while (read_count < size) {
        const ssize_t n = ::pread(fd, bytes + read_count, size - read_count,
                                  static_cast<off_t>(read_count));
        if (n > 0) { read_count += static_cast<std::size_t>(n); continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n == 0) return {ErrorCode::corrupt_store, 0, read_count};
        return io_status(errno, read_count);
    }
    return Status::success();
}

[[nodiscard]] inline Status write_all(int fd, const unsigned char* bytes, std::size_t size) noexcept {
    std::size_t done = 0;
    while (done < size) {
        const ssize_t n = ::write(fd, bytes + done, size - done);
        if (n > 0) { done += static_cast<std::size_t>(n); continue; }
        if (n < 0 && errno == EINTR) continue;
        return io_status(n == 0 ? EIO : errno, done);
    }
    return Status::success();
}

[[nodiscard]] inline bool valid_committed_record_at(const unsigned char* bytes,
                                                    std::size_t offset,
                                                    std::size_t map_size) noexcept {
    if (offset > map_size || map_size - offset < record_header_size ||
        load_u32_acquire(bytes + offset + record_commit_offset) != record_commit_marker)
        return false;
    const auto* record = bytes + offset;
    const std::uint32_t total_size = get_u32(record);
    const std::uint32_t payload_size = get_u32(record + 4);
    const std::uint32_t raw_kind = get_u32(record + 8);
    if (total_size < record_header_size || (total_size & 7U) != 0 ||
        total_size > map_size - offset || payload_size > total_size - record_header_size ||
        align_record(record_header_size + static_cast<std::size_t>(payload_size)) != total_size ||
        !valid_record_kind(raw_kind) || get_u32(record + 12) == 0)
        return false;
    return get_u32(record + record_crc_offset) == record_crc(record, total_size);
}

[[nodiscard]] inline bool has_nonzero_tail(const unsigned char* bytes,
                                           std::size_t offset,
                                           std::size_t map_size) noexcept {
    std::size_t cursor = offset;
    while (cursor + 8 <= map_size) {
        std::uint64_t word = 0;
        std::memcpy(&word, bytes + cursor, sizeof(word));
        if (word != 0) return true;
        cursor += 8;
    }
    while (cursor < map_size) {
        if (bytes[cursor++] != 0) return true;
    }
    return false;
}

[[nodiscard]] inline bool has_committed_record_after(const unsigned char* bytes,
                                                     std::size_t offset,
                                                     std::size_t map_size) noexcept {
    const std::uint32_t current_size = get_u32(bytes + offset);
    if (current_size >= record_header_size && (current_size & 7U) == 0 &&
        current_size <= map_size - offset) {
        const std::size_t next = offset + current_size;
        if (map_size - next >= record_header_size &&
            load_u32_acquire(bytes + next + record_commit_offset) != 0)
            return true;
    }
    std::size_t candidate = (offset + 8U + 7U) & ~std::size_t{7U};
    while (candidate <= map_size && map_size - candidate >= record_header_size) {
        if (valid_committed_record_at(bytes, candidate, map_size)) return true;
        candidate += 8U;
    }
    return false;
}

inline void write_v2_header(unsigned char* bytes, const std::array<unsigned char, 8>& magic,
                            std::size_t file_size, std::string_view session_key,
                            std::size_t segment_bytes, std::size_t total_bytes,
                            std::size_t max_segments, std::uint32_t segment_index,
                            std::size_t audit_bytes) noexcept {
    std::memset(bytes, 0, superblock_size);
    std::memcpy(bytes, magic.data(), magic.size());
    put_u32(bytes + 8, 2);
    put_u32(bytes + 12, static_cast<std::uint32_t>(superblock_size));
    put_u64(bytes + 16, static_cast<std::uint64_t>(file_size));
    put_u32(bytes + 24, static_cast<std::uint32_t>(session_key.size()));
    put_u32(bytes + 28, current_epoch_initial);
    put_u32(bytes + 32, segment_index);
    put_u32(bytes + 36, static_cast<std::uint32_t>(max_segments));
    put_u64(bytes + 40, static_cast<std::uint64_t>(segment_bytes));
    put_u64(bytes + 48, static_cast<std::uint64_t>(total_bytes));
    put_u32(bytes + 56, static_cast<std::uint32_t>(audit_bytes));
    std::memcpy(bytes + superblock_key_offset, session_key.data(), session_key.size());
    put_u32(bytes + v2_header_crc_offset,
            crc32c_with_hole(bytes, superblock_size, v2_header_crc_offset, sizeof(std::uint32_t)));
}

[[nodiscard]] inline Status validate_v2_header(const unsigned char* bytes,
                                               const std::array<unsigned char, 8>& expected_magic,
                                               std::size_t file_size, std::string_view session_key,
                                               std::size_t segment_bytes, std::size_t total_bytes,
                                               std::size_t max_segments, std::uint32_t segment_index,
                                               std::size_t audit_bytes) noexcept {
    if (std::memcmp(bytes, expected_magic.data(), expected_magic.size()) != 0)
        return {ErrorCode::corrupt_store, 0, 0};
    if (get_u32(bytes + 8) != 2) return {ErrorCode::unsupported_version, 0, 8};
    if (get_u32(bytes + 12) != superblock_size) return {ErrorCode::corrupt_store, 0, 12};
    if (get_u64(bytes + 16) != file_size) return {ErrorCode::corrupt_store, 0, 16};
    if (get_u32(bytes + 24) != session_key.size() ||
        std::memcmp(bytes + superblock_key_offset, session_key.data(), session_key.size()) != 0)
        return {ErrorCode::identity_mismatch, 0, superblock_key_offset};
    if (get_u32(bytes + 28) != current_epoch_initial) return {ErrorCode::unsupported_version, 0, 28};
    if (get_u32(bytes + 32) != segment_index) return {ErrorCode::corrupt_store, 0, 32};
    if (get_u32(bytes + 36) != max_segments || get_u64(bytes + 40) != segment_bytes ||
        get_u64(bytes + 48) != total_bytes || get_u32(bytes + 56) != audit_bytes)
        return {ErrorCode::invalid_config, 0, 36};
    if (get_u32(bytes + v2_header_crc_offset) !=
        crc32c_with_hole(bytes, superblock_size, v2_header_crc_offset, sizeof(std::uint32_t)))
        return {ErrorCode::corrupt_store, 0, v2_header_crc_offset};
    return Status::success();
}

inline void write_audit_header(unsigned char* bytes, std::string_view session_key) noexcept {
    std::memset(bytes, 0, superblock_size);
    std::memcpy(bytes, audit_magic.data(), audit_magic.size());
    put_u32(bytes + 8, 2);
    put_u32(bytes + 12, static_cast<std::uint32_t>(superblock_size));
    put_u64(bytes + 16, audit_file_size);
    put_u32(bytes + 24, static_cast<std::uint32_t>(session_key.size()));
    put_u32(bytes + 28, static_cast<std::uint32_t>(audit_entry_count));
    std::memcpy(bytes + superblock_key_offset, session_key.data(), session_key.size());
    put_u32(bytes + superblock_crc_offset, superblock_crc(bytes));
}

[[nodiscard]] inline Status validate_audit_header(const unsigned char* bytes,
                                                  std::string_view session_key) noexcept {
    if (std::memcmp(bytes, audit_magic.data(), audit_magic.size()) != 0 ||
        get_u32(bytes + 8) != 2 || get_u32(bytes + 12) != superblock_size ||
        get_u64(bytes + 16) != audit_file_size ||
        get_u32(bytes + 28) != audit_entry_count)
        return {ErrorCode::corrupt_store, 0, 0};
    if (get_u32(bytes + 24) != session_key.size() ||
        std::memcmp(bytes + superblock_key_offset, session_key.data(), session_key.size()) != 0)
        return {ErrorCode::identity_mismatch, 0, superblock_key_offset};
    if (get_u32(bytes + superblock_crc_offset) != superblock_crc(bytes))
        return {ErrorCode::corrupt_store, 0, superblock_crc_offset};
    return Status::success();
}

[[nodiscard]] inline bool has_symlink_directory_component(const std::filesystem::path& directory) noexcept {
    try {
        const auto absolute = std::filesystem::absolute(directory).lexically_normal();
        std::filesystem::path current;
        for (const auto& component : absolute) {
            current /= component;
            struct stat info {};
            if (::lstat(current.c_str(), &info) != 0) {
                if (errno == ENOENT) continue;
                return true;
            }
            if (S_ISLNK(info.st_mode) || !S_ISDIR(info.st_mode)) return true;
        }
        return false;
    } catch (...) {
        return true;
    }
}

inline void cleanup_stale_temp_files(std::string_view base) noexcept {
    try {
        const auto path = std::filesystem::path(base);
        const auto parent = path.parent_path();
        const auto directory = parent.empty() ? std::filesystem::path(".") : parent;
        const std::string stem = path.filename().string();
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            const auto name = entry.path().filename().string();
            if (name.rfind(stem, 0) == 0 && name.find(".tmp.") != std::string::npos) {
                (void)::unlink(entry.path().c_str());
            }
        }
    } catch (...) {}
}

[[nodiscard]] inline bool has_segment_files(std::string_view base) noexcept try {
    const auto path = std::filesystem::path(base);
    const auto parent = path.parent_path();
    const auto directory = parent.empty() ? std::filesystem::path(".") : parent;
    const std::string prefix = path.filename().string() + ".seg";
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        const auto name = entry.path().filename().string();
        if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0) continue;
        std::uint64_t index = 0;
        const auto parsed = std::from_chars(name.data() + prefix.size(), name.data() + name.size(), index);
        if (parsed.ec == std::errc{} && parsed.ptr == name.data() + name.size()) return true;
    }
    return false;
} catch (...) {
    return true;
}

[[nodiscard]] inline std::size_t resolve_segment_bytes(const JournalConfig& config) noexcept {
    if (config.segment_bytes != 0) return config.segment_bytes;
    constexpr std::size_t default_segment = 1024U * 1024U;
    const std::size_t chosen = config.capacity_bytes < default_segment
        ? config.capacity_bytes : default_segment;
    return chosen & ~std::size_t{7U};
}

struct CheckpointRead {
    bool exists{};
    bool valid{};
    JournalCheckpoint checkpoint{};
    std::vector<std::pair<std::uint64_t, RecordId>> outbound_index{};
};

[[nodiscard]] inline CheckpointRead read_checkpoint_file(std::string_view base,
                                                         std::string_view session_key,
                                                         std::size_t max_records) noexcept {
    CheckpointRead result;
    std::string path;
    try { path.assign(base); path += ".ckp"; }
    catch (...) { return result; }
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return result;
    result.exists = true;
    struct stat info {};
    const std::uint64_t bounded_records = std::min<std::uint64_t>(max_records, 16'777'216ULL);
    const std::uint64_t max_checkpoint_size = checkpoint_fixed_size +
        journal_max_session_key_bytes + bounded_records * 16ULL;
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
        static_cast<std::uint64_t>(info.st_size) > max_checkpoint_size) {
        ::close(fd);
        return result;
    }
    std::vector<unsigned char> bytes;
    try { bytes.resize(static_cast<std::size_t>(info.st_size)); }
    catch (...) { ::close(fd); return result; }
    std::size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t n = ::read(fd, bytes.data() + done, bytes.size() - done);
        if (n > 0) { done += static_cast<std::size_t>(n); continue; }
        if (n < 0 && errno == EINTR) continue;
        ::close(fd);
        return result;
    }
    ::close(fd);
    if (bytes.size() < checkpoint_fixed_size ||
        std::memcmp(bytes.data(), checkpoint_magic.data(), checkpoint_magic.size()) != 0 ||
        get_u32(bytes.data() + 8) != 2 ||
        get_u32(bytes.data() + checkpoint_crc_offset) !=
            crc32c_with_hole(bytes.data(), bytes.size(), checkpoint_crc_offset, sizeof(std::uint32_t)))
        return result;
    const auto key_size = get_u32(bytes.data() + 88);
    const auto index_count = get_u32(bytes.data() + 84);
    if (key_size != session_key.size() || key_size > journal_max_session_key_bytes ||
        std::memcmp(bytes.data() + checkpoint_fixed_size, session_key.data(), key_size) != 0 ||
        index_count > max_records ||
        bytes.size() != checkpoint_fixed_size + key_size +
            static_cast<std::size_t>(index_count) * 16U)
        return result;
    auto& cp = result.checkpoint;
    cp.generation = get_u64(bytes.data() + 16);
    cp.next_inbound_seq = get_u64(bytes.data() + 24);
    cp.next_outbound_seq = get_u64(bytes.data() + 32);
    cp.delivery_cursor = get_u64(bytes.data() + 40);
    cp.replay_cursor = get_u64(bytes.data() + 48);
    cp.last_record_id = get_u64(bytes.data() + 56);
    cp.first_segment_index = get_u32(bytes.data() + 64);
    cp.lowest_retained_outbound_seq = get_u64(bytes.data() + 68);
    cp.lowest_retained_record_id = get_u64(bytes.data() + 76);
    cp.epoch = get_u32(bytes.data() + 92);
    cp.completed_inbound_seq = get_u64(bytes.data() + 96);
    if (cp.next_inbound_seq == 0 || cp.next_outbound_seq == 0 || cp.replay_cursor == 0 ||
        cp.lowest_retained_outbound_seq == 0 || cp.first_segment_index > maximum_segment_index ||
        cp.epoch == 0 || cp.completed_inbound_seq >= cp.next_inbound_seq)
        return result;
    try { result.outbound_index.reserve(index_count); }
    catch (...) { return result; }
    std::size_t offset = checkpoint_fixed_size + key_size;
    for (std::uint32_t i = 0; i < index_count; ++i) {
        const auto seq = get_u64(bytes.data() + offset);
        const auto id = get_u64(bytes.data() + offset + 8);
        if (seq == 0 || id == 0) return CheckpointRead{};
        result.outbound_index.emplace_back(seq, id);
        offset += 16;
    }
    result.valid = true;
    return result;
}

} // namespace nimblefix::store
