#include "nimblefix/journal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <new>
#include <string>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace nimblefix {
namespace {

constexpr std::size_t superblock_size = 4096;
constexpr std::size_t superblock_crc_offset = 32;
constexpr std::size_t superblock_key_offset = 64;
constexpr std::size_t record_header_size = 64;
constexpr std::size_t record_crc_offset = 48;
constexpr std::size_t record_commit_offset = 56;
constexpr std::uint32_t record_commit_marker = 0x4e46434dU; // "NFCM"
constexpr std::uint32_t current_format_version = journal_format_version;
constexpr std::uint32_t current_epoch = journal_initial_epoch;
constexpr std::array<unsigned char, 8> superblock_magic{
    'N', 'I', 'M', 'B', 'L', 'J', 'N', 'L'
};
constexpr std::array<unsigned char, 8> v2_manifest_magic{
    'N', 'I', 'M', 'B', 'L', 'J', '2', 0
};
constexpr std::array<unsigned char, 8> v2_segment_magic{
    'N', 'I', 'M', 'B', 'S', 'E', 'G', '2'
};
constexpr std::array<unsigned char, 8> checkpoint_magic{
    'N', 'F', 'X', 'C', 'K', 'P', '2', 0
};
constexpr std::array<unsigned char, 8> audit_magic{
    'N', 'F', 'X', 'A', 'U', 'D', '2', 0
};
constexpr std::size_t v2_header_crc_offset = 60;
constexpr std::size_t checkpoint_fixed_size = 128;
constexpr std::size_t checkpoint_crc_offset = 12;
constexpr std::uint32_t maximum_segment_index = 0xffffU;
constexpr std::uint64_t record_offset_mask = (std::uint64_t{1} << 48U) - 1U;
constexpr std::size_t audit_file_size = 1024U * 1024U;
constexpr std::size_t audit_entry_count = 1024;
constexpr std::size_t audit_entry_size = (audit_file_size - superblock_size) / audit_entry_count;
constexpr std::size_t audit_commit_offset = audit_entry_size - sizeof(std::uint32_t);
constexpr std::uint32_t audit_commit_marker = 0x4e464155U; // "NFAU"
static_assert(audit_entry_size == 1020 && (audit_entry_size & 3U) == 0);

static_assert(std::atomic_ref<std::uint32_t>::is_always_lock_free,
              "the journal commit marker requires lock-free 32-bit atomics");

std::uint32_t to_little_endian(std::uint32_t value) noexcept {
    const std::uint16_t one = 1;
    if (*reinterpret_cast<const unsigned char*>(&one) == 1) return value;
    return ((value & 0x000000ffU) << 24U) |
           ((value & 0x0000ff00U) << 8U) |
           ((value & 0x00ff0000U) >> 8U) |
           ((value & 0xff000000U) >> 24U);
}

std::uint32_t from_little_endian(std::uint32_t value) noexcept {
    return to_little_endian(value);
}

void put_u32(unsigned char* destination, std::uint32_t value) noexcept {
    value = to_little_endian(value);
    std::memcpy(destination, &value, sizeof(value));
}

void put_u64(unsigned char* destination, std::uint64_t value) noexcept {
    const std::uint16_t one = 1;
    if (*reinterpret_cast<const unsigned char*>(&one) == 1) {
        std::memcpy(destination, &value, sizeof(value));
        return;
    }
    for (std::size_t i = 0; i < sizeof(value); ++i)
        destination[i] = static_cast<unsigned char>(value >> (i * 8U));
}

std::uint32_t get_u32(const unsigned char* source) noexcept {
    std::uint32_t value = 0;
    std::memcpy(&value, source, sizeof(value));
    return from_little_endian(value);
}

std::uint64_t get_u64(const unsigned char* source) noexcept {
    const std::uint16_t one = 1;
    if (*reinterpret_cast<const unsigned char*>(&one) == 1) {
        std::uint64_t value = 0;
        std::memcpy(&value, source, sizeof(value));
        return value;
    }
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < sizeof(value); ++i)
        value |= static_cast<std::uint64_t>(source[i]) << (i * 8U);
    return value;
}

std::uint32_t load_u32_acquire(const unsigned char* source) noexcept {
    auto* word = reinterpret_cast<std::uint32_t*>(const_cast<unsigned char*>(source));
    std::atomic_ref<std::uint32_t> marker(*word);
    return from_little_endian(marker.load(std::memory_order_acquire));
}

std::uint32_t crc32c_byte(std::uint32_t crc, unsigned char value) noexcept {
    crc ^= value;
    for (unsigned bit = 0; bit < 8; ++bit)
        crc = (crc >> 1U) ^ ((crc & 1U) ? 0x82f63b78U : 0U);
    return crc;
}

std::uint32_t superblock_crc(const unsigned char* bytes) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t i = 0; i < superblock_size; ++i) {
        const unsigned char value = (i >= superblock_crc_offset && i < superblock_crc_offset + 4)
            ? 0 : bytes[i];
        crc = crc32c_byte(crc, value);
    }
    return ~crc;
}

std::uint32_t crc32c(const unsigned char* bytes, std::size_t size,
                     std::size_t zero_offset, std::size_t zero_length) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t i = 0; i < size; ++i) {
        const unsigned char value = i >= zero_offset && i - zero_offset < zero_length
            ? 0 : bytes[i];
        crc = crc32c_byte(crc, value);
    }
    return ~crc;
}

std::uint32_t record_crc(const unsigned char* bytes, std::size_t total_size) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t i = 0; i < total_size; ++i) {
        const bool skip_crc = i >= record_crc_offset && i < record_crc_offset + 4;
        const bool skip_commit = i >= record_commit_offset && i < record_commit_offset + 4;
        const unsigned char value = (skip_crc || skip_commit) ? 0 : bytes[i];
        crc = crc32c_byte(crc, value);
    }
    return ~crc;
}

std::size_t align_record(std::size_t value) noexcept {
    return (value + 7U) & ~std::size_t{7U};
}

bool valid_record_kind(std::uint32_t raw_kind) noexcept {
    return raw_kind == static_cast<std::uint32_t>(RecordKind::inbound) ||
           raw_kind == static_cast<std::uint32_t>(RecordKind::outbound) ||
           raw_kind == static_cast<std::uint32_t>(RecordKind::delivery) ||
           raw_kind == static_cast<std::uint32_t>(RecordKind::epoch_reset);
}

bool valid_committed_record_at(const unsigned char* bytes,
                               std::size_t offset,
                               std::size_t map_size) noexcept {
    if (offset > map_size || map_size - offset < record_header_size ||
        get_u32(bytes + offset + record_commit_offset) != record_commit_marker)
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

bool has_committed_record_after(const unsigned char* bytes,
                                std::size_t offset,
                                std::size_t map_size) noexcept {
    // Check the expected next boundary first. If a committed marker was
    // damaged at this record, later valid history must not be mistaken for an
    // expendable uncommitted tail.
    const std::uint32_t current_size = get_u32(bytes + offset);
    if (current_size >= record_header_size && (current_size & 7U) == 0 &&
        current_size <= map_size - offset) {
        const std::size_t next = offset + current_size;
        if (map_size - next >= record_header_size &&
            get_u32(bytes + next + record_commit_offset) != 0)
            return true;
    }

    // If the damaged record header is also incomplete, search aligned
    // positions for a later fully valid committed record. CRC validation
    // avoids mistaking arbitrary payload bytes for a record boundary.
    std::size_t candidate = offset + 8U;
    candidate = (candidate + 7U) & ~std::size_t{7U};
    while (candidate <= map_size && map_size - candidate >= record_header_size) {
        if (valid_committed_record_at(bytes, candidate, map_size)) return true;
        candidate += 8U;
    }
    return false;
}

Status io_status(int error_number, std::size_t offset = 0) noexcept {
    if (error_number == ENOSPC || error_number == EDQUOT)
        return {ErrorCode::storage_full, 0, offset};
    return {ErrorCode::io_error, 0, offset};
}

void write_superblock(unsigned char* bytes,
                      std::size_t capacity,
                      std::string_view session_key) noexcept {
    std::memset(bytes, 0, superblock_size);
    std::memcpy(bytes, superblock_magic.data(), superblock_magic.size());
    put_u32(bytes + 8, current_format_version);
    put_u32(bytes + 12, static_cast<std::uint32_t>(superblock_size));
    put_u64(bytes + 16, static_cast<std::uint64_t>(capacity));
    put_u32(bytes + 24, static_cast<std::uint32_t>(session_key.size()));
    put_u32(bytes + 28, current_epoch);
    std::memcpy(bytes + superblock_key_offset, session_key.data(), session_key.size());
    put_u32(bytes + superblock_crc_offset, superblock_crc(bytes));
}

Status validate_superblock(const unsigned char* bytes,
                           std::size_t file_size,
                           const JournalConfig& config) noexcept {
    if (std::memcmp(bytes, superblock_magic.data(), superblock_magic.size()) != 0)
        return {ErrorCode::corrupt_store, 0, 0};
    if (get_u32(bytes + 8) != current_format_version)
        return {ErrorCode::unsupported_version, 0, 8};
    if (get_u32(bytes + 12) != superblock_size)
        return {ErrorCode::corrupt_store, 0, 12};
    const std::uint64_t stored_capacity = get_u64(bytes + 16);
    if (stored_capacity != file_size)
        return {ErrorCode::corrupt_store, 0, 16};
    if (stored_capacity != config.capacity_bytes)
        return {ErrorCode::invalid_config, 0, 16};
    const std::uint32_t key_size = get_u32(bytes + 24);
    if (key_size == 0 || key_size > journal_max_session_key_bytes)
        return {ErrorCode::corrupt_store, 0, 24};
    if (get_u32(bytes + 28) != current_epoch)
        return {ErrorCode::unsupported_version, 0, 28};
    if (get_u32(bytes + superblock_crc_offset) != superblock_crc(bytes))
        return {ErrorCode::corrupt_store, 0, superblock_crc_offset};
    if (key_size != config.session_key.size() ||
        std::memcmp(bytes + superblock_key_offset,
                    config.session_key.data(),
                    key_size) != 0)
        return {ErrorCode::identity_mismatch, 0, superblock_key_offset};
    return Status::success();
}

Status pread_exact(int fd, unsigned char* bytes, std::size_t size) noexcept {
    std::size_t read_count = 0;
    while (read_count < size) {
        const ssize_t n = ::pread(fd, bytes + read_count, size - read_count,
                                  static_cast<off_t>(read_count));
        if (n > 0) {
            read_count += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n == 0) return {ErrorCode::corrupt_store, 0, read_count};
        return io_status(errno, read_count);
    }
    return Status::success();
}

std::string segment_path(std::string_view base, std::uint32_t index) {
    std::string path(base);
    path += ".seg";
    path += std::to_string(index);
    return path;
}

bool has_segment_files(std::string_view base) noexcept try {
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

RecordId segment_record_id(std::uint32_t index, std::size_t offset) noexcept {
    return (static_cast<RecordId>(index) << 48U) | static_cast<RecordId>(offset);
}

std::uint32_t segment_index_from_id(RecordId id) noexcept {
    return static_cast<std::uint32_t>(id >> 48U);
}

bool has_symlink_directory_component(const std::filesystem::path& directory) noexcept {
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

void write_v2_header(unsigned char* bytes, const std::array<unsigned char, 8>& magic,
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
    put_u32(bytes + 28, current_epoch);
    put_u32(bytes + 32, segment_index);
    put_u32(bytes + 36, static_cast<std::uint32_t>(max_segments));
    put_u64(bytes + 40, static_cast<std::uint64_t>(segment_bytes));
    put_u64(bytes + 48, static_cast<std::uint64_t>(total_bytes));
    put_u32(bytes + 56, static_cast<std::uint32_t>(audit_bytes));
    std::memcpy(bytes + superblock_key_offset, session_key.data(), session_key.size());
    put_u32(bytes + v2_header_crc_offset,
            crc32c(bytes, superblock_size, v2_header_crc_offset, sizeof(std::uint32_t)));
}

Status validate_v2_header(const unsigned char* bytes,
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
    if (get_u32(bytes + 28) != current_epoch) return {ErrorCode::unsupported_version, 0, 28};
    if (get_u32(bytes + 32) != segment_index) return {ErrorCode::corrupt_store, 0, 32};
    if (get_u32(bytes + 36) != max_segments || get_u64(bytes + 40) != segment_bytes ||
        get_u64(bytes + 48) != total_bytes || get_u32(bytes + 56) != audit_bytes)
        return {ErrorCode::invalid_config, 0, 36};
    if (get_u32(bytes + v2_header_crc_offset) !=
        crc32c(bytes, superblock_size, v2_header_crc_offset, sizeof(std::uint32_t)))
        return {ErrorCode::corrupt_store, 0, v2_header_crc_offset};
    return Status::success();
}

Status write_all(int fd, const unsigned char* bytes, std::size_t size) noexcept {
    std::size_t done = 0;
    while (done < size) {
        const ssize_t n = ::write(fd, bytes + done, size - done);
        if (n > 0) { done += static_cast<std::size_t>(n); continue; }
        if (n < 0 && errno == EINTR) continue;
        return io_status(n == 0 ? EIO : errno, done);
    }
    return Status::success();
}

void touch_mapping(void* mapping, std::size_t size) noexcept {
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

void write_audit_header(unsigned char* bytes, std::string_view session_key) noexcept {
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

Status validate_audit_header(const unsigned char* bytes, std::string_view session_key) noexcept {
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

std::uint32_t audit_entry_crc(const unsigned char* bytes) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t i = 0; i < audit_entry_size; ++i) {
        const bool skip_crc = i >= 24 && i < 28;
        const bool skip_commit = i >= audit_commit_offset && i < audit_commit_offset + 4;
        crc = crc32c_byte(crc, skip_crc || skip_commit ? 0 : bytes[i]);
    }
    return ~crc;
}

struct CheckpointRead {
    bool exists{};
    bool valid{};
    JournalCheckpoint checkpoint{};
    std::vector<std::pair<std::uint64_t, RecordId>> outbound_index{};
};

CheckpointRead read_checkpoint_file(std::string_view base, std::string_view session_key,
                                   std::size_t max_records) noexcept {
    CheckpointRead result;
    std::string path;
    try { path.assign(base); path += ".ckp"; }
    catch (...) { return result; }
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return result;
    result.exists = true;
    struct stat info {};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
        static_cast<std::uint64_t>(info.st_size) > checkpoint_fixed_size +
            journal_max_session_key_bytes + max_records * sizeof(std::uint64_t) * 2U) {
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
            crc32c(bytes.data(), bytes.size(), checkpoint_crc_offset, sizeof(std::uint32_t)))
        return result;
    const auto key_size = get_u32(bytes.data() + 88);
    const auto index_count = get_u32(bytes.data() + 84);
    if (key_size != session_key.size() || key_size > journal_max_session_key_bytes ||
        std::memcmp(bytes.data() + checkpoint_fixed_size, session_key.data(), key_size) != 0 ||
        index_count > max_records ||
        bytes.size() != checkpoint_fixed_size + key_size +
            static_cast<std::size_t>(index_count) * sizeof(std::uint64_t) * 2U)
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

} // namespace

struct Journal::Segment {
    int fd{-1};
    void* mapping{};
    std::size_t mapping_size{};
    std::size_t write_offset{superblock_size};
    std::uint32_t index{};
    std::string path{};

    ~Segment() {
        if (mapping != nullptr && mapping_size != 0) ::munmap(mapping, mapping_size);
        if (fd >= 0) ::close(fd);
    }
};

struct JournalReader::Registry {
    struct Slot {
        std::atomic<Journal*> owner{};
        std::atomic<State*> state{};
        std::atomic<std::size_t> position{};
        std::atomic<bool> claimed{};
        std::atomic<bool> active{};
    };
    explicit Registry(std::size_t count) : slots(std::make_unique<Slot[]>(count)), capacity(count) {}
    std::unique_ptr<Slot[]> slots{};
    std::size_t capacity{};
    std::atomic<std::size_t> active_count{};
    std::atomic<std::size_t> active_calls{};
};

struct JournalReader::State {
    std::shared_ptr<Registry> registry{};
    std::vector<std::shared_ptr<void>> mapping_leases{};
    std::size_t slot{};
    std::atomic<RecordId> cursor{};

    ~State() {
        if (!registry || slot >= registry->capacity) return;
        auto& entry = registry->slots[slot];
        State* expected = this;
        if (entry.state.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel)) {
            entry.active.store(false, std::memory_order_release);
            entry.claimed.store(false, std::memory_order_release);
            registry->active_count.fetch_sub(1, std::memory_order_acq_rel);
        }
    }
};

JournalReader::JournalReader(JournalReader&& other) noexcept
    : state_(std::move(other.state_)) {}

JournalReader& JournalReader::operator=(JournalReader&& other) noexcept {
    if (this == &other) return *this;
    state_ = std::move(other.state_);
    return *this;
}

JournalReader::~JournalReader() = default;

Status JournalReader::next(JournalRecord& record) noexcept {
    if (!state_ || !state_->registry || state_->slot >= state_->registry->capacity)
        return {ErrorCode::reader_expired, 0, 0};
    auto registry = state_->registry;
    registry->active_calls.fetch_add(1, std::memory_order_seq_cst);
    struct ActiveCall {
        std::shared_ptr<Registry> registry;
        ~ActiveCall() { registry->active_calls.fetch_sub(1, std::memory_order_seq_cst); }
    } active_call{registry};
    auto* owner = registry->slots[state_->slot].owner.load(std::memory_order_seq_cst);
    if (owner == nullptr) return {ErrorCode::reader_expired, 0, 0};
    return owner->reader_next(*state_, record);
}

RecordId JournalReader::cursor() const noexcept {
    return state_ ? state_->cursor.load(std::memory_order_acquire) : 0;
}

std::uint64_t JournalReader::lag_records() const noexcept {
    if (!state_ || !state_->registry || state_->slot >= state_->registry->capacity) return 0;
    auto registry = state_->registry;
    registry->active_calls.fetch_add(1, std::memory_order_seq_cst);
    struct ActiveCall {
        std::shared_ptr<Registry> registry;
        ~ActiveCall() { registry->active_calls.fetch_sub(1, std::memory_order_seq_cst); }
    } active_call{registry};
    auto* owner = registry->slots[state_->slot].owner.load(std::memory_order_seq_cst);
    if (owner == nullptr) return 0;
    const auto count = owner->published_record_count_.load(std::memory_order_acquire);
    const auto position = registry->slots[state_->slot].position.load(std::memory_order_acquire);
    return count > position ? count - position : 0;
}

Journal::Journal(Journal&& other) noexcept { *this = std::move(other); }

Journal& Journal::operator=(Journal&& other) noexcept {
    if (this == &other) return *this;
    reset();
    other.quiesce_readers();
    fd_ = std::exchange(other.fd_, -1);
    mapping_ = std::exchange(other.mapping_, nullptr);
    mapping_size_ = std::exchange(other.mapping_size_, 0);
    capacity_bytes_ = std::exchange(other.capacity_bytes_, 0);
    write_offset_ = std::exchange(other.write_offset_, 0);
    next_inbound_seq_ = std::exchange(other.next_inbound_seq_, 1);
    next_outbound_seq_ = std::exchange(other.next_outbound_seq_, 1);
    max_records_ = std::exchange(other.max_records_, 0);
    test_hook_ = std::exchange(other.test_hook_, {});
    records_ = std::move(other.records_);
    outbound_index_ = std::move(other.outbound_index_);
    records_data_ = records_.data();
    segments_ = std::move(other.segments_);
    reader_registry_ = std::move(other.reader_registry_);
    published_record_count_.store(
        other.published_record_count_.exchange(0, std::memory_order_acq_rel), std::memory_order_release);
    active_segment_ = std::exchange(other.active_segment_, 0);
    segment_capacity_bytes_ = std::exchange(other.segment_capacity_bytes_, 0);
    max_segments_ = std::exchange(other.max_segments_, 0);
    next_segment_index_ = std::exchange(other.next_segment_index_, 0);
    first_segment_index_ = std::exchange(other.first_segment_index_, 0);
    format_version_ = std::exchange(other.format_version_, 1);
    used_total_bytes_ = std::exchange(other.used_total_bytes_, 0);
    max_readers_ = std::exchange(other.max_readers_, 0);
    max_reader_lag_records_ = std::exchange(other.max_reader_lag_records_, 0);
    lowest_retained_outbound_seq_ = std::exchange(other.lowest_retained_outbound_seq_, 1);
    lowest_retained_record_id_ = std::exchange(other.lowest_retained_record_id_, 4096);
    completed_inbound_seq_ = std::exchange(other.completed_inbound_seq_, 0);
    completed_inbound_record_id_ = std::exchange(other.completed_inbound_record_id_, 0);
    completed_scan_index_ = std::exchange(other.completed_scan_index_, 0);
    checkpoint_ = std::exchange(other.checkpoint_, {});
    checkpoint_stale_ = std::exchange(other.checkpoint_stale_, false);
    current_epoch_ = std::exchange(other.current_epoch_, journal_initial_epoch);
    epoch_reset_armed_ = std::exchange(other.epoch_reset_armed_, false);
    base_path_ = std::move(other.base_path_);
    session_key_ = std::move(other.session_key_);
    audit_fd_ = std::exchange(other.audit_fd_, -1);
    audit_mapping_ = std::exchange(other.audit_mapping_, nullptr);
    audit_file_size_ = std::exchange(other.audit_file_size_, 0);
    audit_bytes_ = std::exchange(other.audit_bytes_, 0);
    audit_count_ = std::exchange(other.audit_count_, 0);
    epoch_audit_records_ = std::move(other.epoch_audit_records_);
    if (reader_registry_) {
        for (std::size_t i = 0; i < reader_registry_->capacity; ++i)
            if (reader_registry_->slots[i].claimed.load(std::memory_order_acquire))
                reader_registry_->slots[i].owner.store(this, std::memory_order_seq_cst);
    }
    return *this;
}

Journal::~Journal() { reset(); }

void Journal::reset() noexcept {
    quiesce_readers();
    records_.clear();
    outbound_index_.clear();
    records_data_ = nullptr;
    reader_registry_.reset();
    published_record_count_.store(0, std::memory_order_release);
    segments_.clear();
    epoch_audit_records_.clear();
    if (audit_mapping_ != nullptr && audit_file_size_ != 0)
        ::munmap(audit_mapping_, audit_file_size_);
    audit_mapping_ = nullptr;
    audit_file_size_ = 0;
    if (audit_fd_ >= 0) ::close(audit_fd_);
    audit_fd_ = -1;
    audit_bytes_ = 0;
    audit_count_ = 0;
    if (mapping_ != nullptr && mapping_size_ != 0)
        ::munmap(mapping_, mapping_size_);
    mapping_ = nullptr;
    mapping_size_ = 0;
    if (fd_ >= 0) {
        ::flock(fd_, LOCK_UN);
        ::close(fd_);
    }
    fd_ = -1;
    capacity_bytes_ = 0;
    write_offset_ = 0;
    next_inbound_seq_ = 1;
    next_outbound_seq_ = 1;
    max_records_ = 0;
    test_hook_ = {};
    active_segment_ = 0;
    segment_capacity_bytes_ = 0;
    max_segments_ = 0;
    next_segment_index_ = 0;
    first_segment_index_ = 0;
    format_version_ = 1;
    used_total_bytes_ = 0;
    max_readers_ = 0;
    max_reader_lag_records_ = 0;
    lowest_retained_outbound_seq_ = 1;
    lowest_retained_record_id_ = superblock_size;
    completed_inbound_seq_ = 0;
    completed_inbound_record_id_ = 0;
    completed_scan_index_ = 0;
    checkpoint_ = {};
    checkpoint_stale_ = false;
    current_epoch_ = journal_initial_epoch;
    epoch_reset_armed_ = false;
    base_path_.clear();
    session_key_.clear();
}

void Journal::quiesce_readers() noexcept {
    if (!reader_registry_) return;
    // Reader calls increment active_calls before loading owner. Sequentially
    // consistent ordering closes the load-before-increment teardown race:
    // either the owner load sees null, or reset observes the active call.
    for (std::size_t i = 0; i < reader_registry_->capacity; ++i)
        reader_registry_->slots[i].owner.store(nullptr, std::memory_order_seq_cst);
    while (reader_registry_->active_calls.load(std::memory_order_seq_cst) != 0)
        std::this_thread::yield();
}

Result<Journal> Journal::open_segmented(JournalConfig config) noexcept try {
    if (config.path.empty() || config.session_key.empty() ||
        config.path.find('\0') != std::string_view::npos || config.path.size() > 4096 ||
        config.session_key.size() > journal_max_session_key_bytes ||
        config.segment_bytes < superblock_size + record_header_size ||
        (config.segment_bytes & 7U) != 0 ||
        config.segment_bytes > record_offset_mask || config.segment_bytes > config.capacity_bytes ||
        config.capacity_bytes > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
        config.max_records == 0 || config.max_readers == 0 ||
        config.max_reader_lag_records == 0)
        return Status{ErrorCode::invalid_config, 0, 0};
    const std::size_t audit_budget = config.capacity_bytes >= audit_file_size + config.segment_bytes
        ? audit_file_size : 0;
    const std::size_t segment_budget = config.capacity_bytes - audit_budget;
    const std::size_t budget_segments = segment_budget / config.segment_bytes;
    if (budget_segments == 0 || budget_segments > static_cast<std::size_t>(maximum_segment_index) + 1U)
        return Status{ErrorCode::invalid_config, 0, 0};
    const std::size_t max_segments = config.max_segments == 0 ? budget_segments : config.max_segments;
    if (max_segments == 0 || max_segments > budget_segments ||
        max_segments > static_cast<std::size_t>(maximum_segment_index) + 1U)
        return Status{ErrorCode::invalid_config, 0, 0};

    Journal journal;
    std::string base;
    try {
        base.assign(config.path.data(), config.path.size());
        journal.base_path_ = base;
        journal.session_key_.assign(config.session_key.data(), config.session_key.size());
        journal.records_.reserve(config.max_records);
        journal.outbound_index_.reserve(config.max_records);
        journal.records_data_ = journal.records_.data();
        journal.segments_.reserve(max_segments);
        if (audit_budget != 0) journal.epoch_audit_records_.reserve(audit_entry_count);
        journal.reader_registry_ = std::make_shared<JournalReader::Registry>(config.max_readers);
    } catch (...) {
        return Status{ErrorCode::capacity_exceeded, 0, 0};
    }
    try {
        const auto parent = std::filesystem::path(base).parent_path();
        if (!parent.empty()) {
            if (has_symlink_directory_component(parent))
                return Status{ErrorCode::invalid_config, 0, 0};
            std::error_code error;
            (void)std::filesystem::create_directories(parent, error);
            if (error) return Status{ErrorCode::io_error, 0, 0};
            if (has_symlink_directory_component(parent))
                return Status{ErrorCode::invalid_config, 0, 0};
        }
    } catch (...) {
        return Status{ErrorCode::io_error, 0, 0};
    }

    bool manifest_created = false;
    int manifest_fd = ::open(base.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (manifest_fd >= 0) manifest_created = true;
    else if (errno == EEXIST) manifest_fd = ::open(base.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (manifest_fd < 0) return io_status(errno);

    if (::flock(manifest_fd, LOCK_EX | LOCK_NB) != 0) {
        const int saved_errno = errno;
        ::close(manifest_fd);
        if (saved_errno == EWOULDBLOCK || saved_errno == EAGAIN)
            return Status{ErrorCode::store_locked, 0, 0};
        return io_status(saved_errno);
    }
    journal.fd_ = manifest_fd;
    journal.capacity_bytes_ = config.capacity_bytes;
    journal.max_records_ = config.max_records;
    journal.test_hook_ = config.test_hook;
    journal.format_version_ = 2;
    journal.segment_capacity_bytes_ = config.segment_bytes;
    journal.audit_bytes_ = audit_budget;
    journal.max_segments_ = max_segments;
    journal.max_readers_ = config.max_readers;
    journal.max_reader_lag_records_ = config.max_reader_lag_records;

    auto fail = [&](Status status) -> Result<Journal> {
        if (manifest_created) {
            (void)::unlink(base.c_str());
            for (std::size_t i = 0; i < max_segments; ++i) {
                const auto path = segment_path(base, static_cast<std::uint32_t>(i));
                (void)::unlink(path.c_str());
            }
            const auto checkpoint_path = base + ".ckp";
            (void)::unlink(checkpoint_path.c_str());
            const auto audit_path = base + ".audit";
            (void)::unlink(audit_path.c_str());
        }
        journal.reset();
        return status;
    };

    struct stat manifest_stat {};
    if (::fstat(manifest_fd, &manifest_stat) != 0) return fail(io_status(errno));
    if (!S_ISREG(manifest_stat.st_mode) || manifest_stat.st_uid != ::geteuid() ||
        (manifest_stat.st_mode & 0077) != 0)
        return fail(Status{ErrorCode::invalid_config, 0, 0});
    if (manifest_created) {
        if (::ftruncate(manifest_fd, static_cast<off_t>(superblock_size)) != 0)
            return fail(io_status(errno));
        const int allocation_error = ::posix_fallocate(manifest_fd, 0, static_cast<off_t>(superblock_size));
        if (allocation_error != 0) return fail(io_status(allocation_error));
        std::array<unsigned char, superblock_size> header{};
        write_v2_header(header.data(), v2_manifest_magic, superblock_size, config.session_key,
                        config.segment_bytes, config.capacity_bytes, max_segments, 0, audit_budget);
        if (::pwrite(manifest_fd, header.data(), header.size(), 0) !=
            static_cast<ssize_t>(header.size()))
            return fail(io_status(errno == 0 ? EIO : errno));
    } else {
        if (manifest_stat.st_size < static_cast<off_t>(superblock_size))
            return fail(Status{ErrorCode::corrupt_store, 0, 16});
        std::array<unsigned char, superblock_size> header{};
        const auto read_status = pread_exact(manifest_fd, header.data(), header.size());
        if (!read_status.ok()) return fail(read_status);
        if (std::memcmp(header.data(), superblock_magic.data(), superblock_magic.size()) == 0 &&
            get_u32(header.data() + 8) == journal_format_version) {
            // A pre-segmentation store remains readable and writable as v1.
            journal.reset();
            config.segment_bytes = 0;
            config.max_segments = 0;
            return Journal::open(config);
        }
        if (manifest_stat.st_size != static_cast<off_t>(superblock_size))
            return fail(Status{ErrorCode::corrupt_store, 0, 16});
        const auto header_status = validate_v2_header(header.data(), v2_manifest_magic,
            superblock_size, config.session_key, config.segment_bytes, config.capacity_bytes,
            max_segments, 0, audit_budget);
        if (!header_status.ok()) return fail(header_status);
    }

    if (audit_budget != 0) {
        const std::string audit_path = base + ".audit";
        bool audit_created = false;
        int audit_fd = ::open(audit_path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (audit_fd >= 0) audit_created = true;
        else if (errno == EEXIST) audit_fd = ::open(audit_path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        if (audit_fd < 0) return fail(io_status(errno));
        journal.audit_fd_ = audit_fd;
        struct stat audit_stat {};
        if (::fstat(audit_fd, &audit_stat) != 0) return fail(io_status(errno));
        if (!S_ISREG(audit_stat.st_mode) || audit_stat.st_uid != ::geteuid() ||
            (audit_stat.st_mode & 0077) != 0)
            return fail(Status{ErrorCode::invalid_config, 0, 0});
        if (audit_created || audit_stat.st_size == 0) {
            struct stat checkpoint_stat {};
            const bool checkpoint_exists = ::lstat((base + ".ckp").c_str(), &checkpoint_stat) == 0;
            if (!manifest_created && (checkpoint_exists || has_segment_files(base))) {
                if (audit_created) (void)::unlink(audit_path.c_str());
                return fail(Status{ErrorCode::corrupt_store, 0, 0});
            }
            if (::ftruncate(audit_fd, static_cast<off_t>(audit_file_size)) != 0)
                return fail(io_status(errno));
            const int allocation_error = ::posix_fallocate(audit_fd, 0, static_cast<off_t>(audit_file_size));
            if (allocation_error != 0) return fail(io_status(allocation_error));
        } else if (audit_stat.st_size != static_cast<off_t>(audit_file_size)) {
            return fail(Status{ErrorCode::corrupt_store, 0, 16});
        }
        void* audit_mapping = ::mmap(nullptr, audit_file_size, PROT_READ | PROT_WRITE,
                                     MAP_SHARED, audit_fd, 0);
        if (audit_mapping == MAP_FAILED) return fail(io_status(errno));
        journal.audit_mapping_ = audit_mapping;
        journal.audit_file_size_ = audit_file_size;
        auto* audit_data = static_cast<unsigned char*>(audit_mapping);
        if (audit_created || audit_stat.st_size == 0) {
            write_audit_header(audit_data, config.session_key);
            touch_mapping(audit_mapping, audit_file_size);
        } else {
            const auto header_status = validate_audit_header(audit_data, config.session_key);
            if (!header_status.ok()) return fail(header_status);
        }
        for (std::size_t i = 0; i < audit_entry_count; ++i) {
            auto* entry = audit_data + superblock_size + i * audit_entry_size;
            auto* marker_word = reinterpret_cast<std::uint32_t*>(entry + audit_commit_offset);
            std::atomic_ref<std::uint32_t> marker(*marker_word);
            const auto commit = from_little_endian(marker.load(std::memory_order_acquire));
            if (commit == 0) {
                bool later_commit = false;
                for (std::size_t later = i + 1; later < audit_entry_count; ++later) {
                    const auto* candidate = audit_data + superblock_size + later * audit_entry_size;
                    if (get_u32(candidate + audit_commit_offset) != 0) { later_commit = true; break; }
                }
                const auto entry_offset = superblock_size + i * audit_entry_size;
                if (later_commit) return fail(Status{ErrorCode::corrupt_store, 0, entry_offset});
                if (get_u32(entry) != 0) {
                    std::memset(entry + sizeof(std::uint32_t), 0,
                        audit_file_size - entry_offset - sizeof(std::uint32_t));
                    std::atomic_thread_fence(std::memory_order_release);
                    put_u32(entry, 0);
                }
                break;
            }
            const auto entry_offset = superblock_size + i * audit_entry_size;
            if (commit != audit_commit_marker || get_u32(entry) != audit_entry_size ||
                get_u32(entry + 24) != audit_entry_crc(entry))
                return fail(Status{ErrorCode::corrupt_store, 0, entry_offset});
            const auto previous_epoch = get_u32(entry + 4);
            const auto next_epoch = get_u32(entry + 8);
            const auto reason_size = get_u32(entry + 12);
            const auto record_id = get_u64(entry + 16);
            if (previous_epoch == 0 || next_epoch != previous_epoch + 1U || reason_size == 0 ||
                reason_size > 256 || record_id == 0 ||
                std::memchr(entry + 64, 0, reason_size) != nullptr ||
                (!journal.epoch_audit_records_.empty() &&
                 (journal.epoch_audit_records_.back().next_epoch != previous_epoch ||
                  journal.epoch_audit_records_.back().reset_record_id >= record_id)) ||
                (journal.epoch_audit_records_.empty() && previous_epoch != 1))
                return fail(Status{ErrorCode::corrupt_store, 0, entry_offset});
            journal.epoch_audit_records_.push_back(EpochAuditRecord{
                previous_epoch, next_epoch, record_id,
                std::string_view(reinterpret_cast<const char*>(entry + 64), reason_size)});
            ++journal.audit_count_;
        }
    } else {
        struct stat audit_stat {};
        const auto audit_path = base + ".audit";
        if (::lstat(audit_path.c_str(), &audit_stat) == 0)
            return fail(Status{ErrorCode::invalid_config, 0, 0});
        if (errno != ENOENT) return fail(io_status(errno));
    }

    const auto checkpoint_read = read_checkpoint_file(base, config.session_key, config.max_records);
    const bool checkpoint_usable = checkpoint_read.exists && checkpoint_read.valid;
    journal.checkpoint_stale_ = !checkpoint_usable;
    if (checkpoint_usable) {
        journal.checkpoint_ = checkpoint_read.checkpoint;
        journal.first_segment_index_ = checkpoint_read.checkpoint.first_segment_index;
        journal.lowest_retained_outbound_seq_ = checkpoint_read.checkpoint.lowest_retained_outbound_seq;
        journal.lowest_retained_record_id_ = checkpoint_read.checkpoint.lowest_retained_record_id;
    }

    const auto parent_path = std::filesystem::path(base).parent_path();
    const auto directory = parent_path.empty() ? std::filesystem::path(".") : parent_path;
    const std::string prefix = std::filesystem::path(base).filename().string() + ".seg";
    std::vector<std::uint32_t> found_indices;
    bool segment_index_overflow = false;
    try {
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            const auto name = entry.path().filename().string();
            if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0) continue;
            std::uint64_t index = 0;
            const auto* first = name.data() + prefix.size();
            const auto* last = name.data() + name.size();
            const auto parsed = std::from_chars(first, last, index);
            if (parsed.ec == std::errc{} && parsed.ptr == last) {
                if (index > maximum_segment_index) segment_index_overflow = true;
                else found_indices.push_back(static_cast<std::uint32_t>(index));
            }
        }
        std::sort(found_indices.begin(), found_indices.end());
        found_indices.erase(std::unique(found_indices.begin(), found_indices.end()), found_indices.end());
    } catch (...) {
        return fail(Status{ErrorCode::io_error, 0, 0});
    }
    if (segment_index_overflow) return fail(Status{ErrorCode::storage_full, 0, 0});

    if (!checkpoint_usable) {
        // The checkpoint is only an index. After a committed retention
        // publication the surviving segment prefix may start above zero, so
        // reconstruct its retained boundary from monotonic segment IDs.
        journal.first_segment_index_ = found_indices.empty() ? 0U : found_indices.front();
        journal.lowest_retained_outbound_seq_ = 1;
        journal.lowest_retained_record_id_ = segment_record_id(journal.first_segment_index_, superblock_size);
        if (found_indices.empty() && checkpoint_read.exists)
            return fail(Status{ErrorCode::corrupt_store, 0, found_indices.size()});
    } else if (!found_indices.empty() && found_indices.front() > journal.first_segment_index_) {
        return fail(Status{ErrorCode::corrupt_store, 0, found_indices.front()});
    }

    const std::uint32_t first_present = found_indices.empty()
        ? journal.first_segment_index_ : std::max(found_indices.front(), journal.first_segment_index_);
    if (journal.first_segment_index_ != 0 &&
        (found_indices.empty() || found_indices.back() < journal.first_segment_index_))
        return fail(Status{ErrorCode::corrupt_store, 0, journal.first_segment_index_});
    if (journal.first_segment_index_ != 0) {
        for (const auto index : found_indices) {
            if (index >= journal.first_segment_index_) break;
            const auto retired_path = segment_path(base, index);
            if (::unlink(retired_path.c_str()) != 0 && errno != ENOENT)
                return fail(io_status(errno));
        }
    }
    std::vector<std::uint32_t> retained_indices;
    for (auto index : found_indices) if (index >= first_present) retained_indices.push_back(index);
    if (!retained_indices.empty()) {
        for (std::size_t i = 1; i < retained_indices.size(); ++i) {
            if (retained_indices[i] != retained_indices[i - 1] + 1U)
                return fail(Status{ErrorCode::corrupt_store, 0, retained_indices[i]});
        }
    }
    if (retained_indices.size() > max_segments)
        return fail(Status{ErrorCode::corrupt_store, 0, retained_indices.size()});
    if (found_indices.empty() && checkpoint_usable &&
        (checkpoint_read.checkpoint.last_record_id != 0 ||
         checkpoint_read.checkpoint.next_inbound_seq != 1 ||
         checkpoint_read.checkpoint.next_outbound_seq != 1 ||
         checkpoint_read.checkpoint.first_segment_index != 0))
        return fail(Status{ErrorCode::corrupt_store, 0, 0});

    auto open_data_segment = [&](std::uint32_t index, bool allow_create)
        -> Result<std::shared_ptr<Segment>> {
        const auto data_path = segment_path(base, index);
        bool created = false;
        int fd = ::open(data_path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd >= 0) created = true;
        else if (errno == EEXIST) fd = ::open(data_path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        else if (!allow_create) return io_status(errno);
        if (fd < 0) return io_status(errno);
        struct stat info {};
        if (::fstat(fd, &info) != 0) { const int saved = errno; ::close(fd); return io_status(saved); }
        if (!S_ISREG(info.st_mode) || info.st_uid != ::geteuid() || (info.st_mode & 0077) != 0) {
            ::close(fd);
            return Status{ErrorCode::invalid_config, 0, 0};
        }
        if (created) {
            if (::ftruncate(fd, static_cast<off_t>(config.segment_bytes)) != 0) {
                const int saved = errno; ::close(fd); (void)::unlink(data_path.c_str()); return io_status(saved);
            }
            const int allocation_error = ::posix_fallocate(fd, 0, static_cast<off_t>(config.segment_bytes));
            if (allocation_error != 0) {
                ::close(fd); (void)::unlink(data_path.c_str()); return io_status(allocation_error);
            }
        } else if (info.st_size != static_cast<off_t>(config.segment_bytes)) {
            ::close(fd);
            return Status{ErrorCode::corrupt_store, 0, 16};
        }
        int flags = MAP_SHARED;
#if defined(MAP_POPULATE)
        flags |= MAP_POPULATE;
#endif
        void* mapping = ::mmap(nullptr, config.segment_bytes, PROT_READ | PROT_WRITE, flags, fd, 0);
        if (mapping == MAP_FAILED) { const int saved = errno; ::close(fd); return io_status(saved); }
        auto segment = std::make_shared<Segment>();
        segment->fd = fd;
        segment->mapping = mapping;
        segment->mapping_size = config.segment_bytes;
        segment->index = index;
        segment->path = data_path;
        (void)::madvise(mapping, config.segment_bytes, MADV_WILLNEED);
        if (created) {
            write_v2_header(static_cast<unsigned char*>(mapping), v2_segment_magic,
                config.segment_bytes, config.session_key, config.segment_bytes,
                config.capacity_bytes, max_segments, index, audit_budget);
            touch_mapping(mapping, config.segment_bytes);
            if (config.test_hook.callback != nullptr)
                config.test_hook.callback(JournalFaultPoint::segment_created, config.test_hook.context);
        } else {
            const auto status = validate_v2_header(static_cast<const unsigned char*>(mapping),
                v2_segment_magic, config.segment_bytes, config.session_key,
                config.segment_bytes, config.capacity_bytes, max_segments, index, audit_budget);
            if (!status.ok()) return status;
        }
        return segment;
    };

    if (retained_indices.empty()) {
        const auto created = open_data_segment(first_present, true);
        if (!created.ok()) return fail(created.status());
        journal.segments_.push_back(created.value());
        journal.first_segment_index_ = first_present;
        journal.next_segment_index_ = first_present + 1U;
    } else {
        for (auto index : retained_indices) {
            const auto opened = open_data_segment(index, false);
            if (!opened.ok()) return fail(opened.status());
            journal.segments_.push_back(opened.value());
            if (index == maximum_segment_index) journal.next_segment_index_ = maximum_segment_index + 1U;
            else journal.next_segment_index_ = std::max(journal.next_segment_index_, index + 1U);
        }
    }

    // Recover all committed records from mapped segments. Side indexes are only hints.
    std::uint32_t last_data_segment = journal.segments_.front()->index;
    bool saw_any_record = false;
    bool saw_inbound = false;
    bool saw_outbound = false;
    bool saw_epoch_reset = false;
    bool epoch_known = false;
    bool require_one_in = journal.first_segment_index_ == 0;
    bool require_one_out = journal.first_segment_index_ == 0;
    journal.current_epoch_ = checkpoint_usable ? checkpoint_read.checkpoint.epoch : journal_initial_epoch;
    std::uint64_t expected_inbound = 0;
    std::uint64_t expected_outbound = 0;
    for (std::size_t segment_slot = 0; segment_slot < journal.segments_.size(); ++segment_slot) {
        auto& segment = *journal.segments_[segment_slot];
        auto* data = static_cast<unsigned char*>(segment.mapping);
        std::size_t offset = superblock_size;
        bool segment_has_record = false;
        while (offset <= segment.mapping_size && segment.mapping_size - offset >= record_header_size) {
            auto* record = data + offset;
            auto* marker_word = reinterpret_cast<std::uint32_t*>(record + record_commit_offset);
            std::atomic_ref<std::uint32_t> marker(*marker_word);
            const auto commit = from_little_endian(marker.load(std::memory_order_acquire));
            if (commit == 0) {
                if (has_committed_record_after(data, offset, segment.mapping_size))
                    return fail(Status{ErrorCode::corrupt_store, 0,
                        static_cast<std::size_t>(segment_record_id(segment.index, offset + record_commit_offset))});
                if (get_u32(record) != 0) {
                    std::memset(record + sizeof(std::uint32_t), 0,
                                segment.mapping_size - offset - sizeof(std::uint32_t));
                    std::atomic_thread_fence(std::memory_order_release);
                    put_u32(record, 0);
                }
                break;
            }
            if (commit != record_commit_marker)
                return fail(Status{ErrorCode::corrupt_store, 0,
                    static_cast<std::size_t>(segment_record_id(segment.index, offset + record_commit_offset))});
            const std::uint32_t total_size = get_u32(record);
            const std::uint32_t payload_size = get_u32(record + 4);
            const std::uint32_t raw_kind = get_u32(record + 8);
            const auto seq = get_u64(record + 16);
            const auto receipt = get_u64(record + 24);
            const auto dependency = get_u64(record + 32);
            const auto next_seq = get_u64(record + 40);
            const auto record_epoch = get_u32(record + 12);
            if (total_size < record_header_size || (total_size & 7U) != 0 ||
                total_size > segment.mapping_size - offset ||
                payload_size > total_size - record_header_size ||
                align_record(record_header_size + static_cast<std::size_t>(payload_size)) != total_size ||
                !valid_record_kind(raw_kind) || record_epoch == 0)
                return fail(Status{ErrorCode::corrupt_store, 0,
                    static_cast<std::size_t>(segment_record_id(segment.index, offset))});
            if (get_u32(record + record_crc_offset) != record_crc(record, total_size))
                return fail(Status{ErrorCode::corrupt_store, 0,
                    static_cast<std::size_t>(segment_record_id(segment.index, offset + record_crc_offset))});
            if (journal.records_.size() >= journal.max_records_)
                return fail(Status{ErrorCode::invalid_config, 0,
                    static_cast<std::size_t>(segment_record_id(segment.index, offset))});
            const auto kind = static_cast<RecordKind>(raw_kind);
            const auto id = segment_record_id(segment.index, offset);
            const std::string_view payload(reinterpret_cast<const char*>(record + record_header_size), payload_size);
            JournalRecord recovered{id, kind, seq, receipt, dependency, next_seq, payload, false};
            recovered.epoch = record_epoch;
            if (!epoch_known) {
                journal.current_epoch_ = record_epoch;
                epoch_known = true;
            } else if (record_epoch != journal.current_epoch_) {
                return fail(Status{ErrorCode::corrupt_store, 0, static_cast<std::size_t>(id + 12)});
            }
            if (kind == RecordKind::inbound) {
                if (seq == 0 || seq == std::numeric_limits<std::uint64_t>::max() ||
                    (saw_inbound && seq != expected_inbound) ||
                    (!saw_inbound && require_one_in && seq != 1) || next_seq <= seq ||
                    receipt != 0 || dependency != 0 || payload.empty())
                    return fail(Status{ErrorCode::corrupt_store, 0, static_cast<std::size_t>(id)});
                expected_inbound = next_seq;
                saw_inbound = true;
                require_one_in = false;
            } else if (kind == RecordKind::outbound) {
                if (seq == 0 || seq == std::numeric_limits<std::uint64_t>::max() ||
                    (saw_outbound && seq != expected_outbound) ||
                    (!saw_outbound && require_one_out && seq != 1) || next_seq != 0 || payload.empty())
                    return fail(Status{ErrorCode::corrupt_store, 0, static_cast<std::size_t>(id)});
                expected_outbound = seq + 1;
                saw_outbound = true;
                require_one_out = false;
                if (dependency != 0) {
                    const auto* dep = journal.find_record(dependency);
                    if ((dep == nullptr && dependency >= journal.lowest_retained_record_id_) ||
                        (dep != nullptr && dep->kind != RecordKind::inbound))
                        return fail(Status{ErrorCode::corrupt_store, 0, static_cast<std::size_t>(id + 32)});
                }
            } else if (kind == RecordKind::delivery) {
                if (seq == 0 || receipt != 0 || next_seq != 0 || !payload.empty())
                    return fail(Status{ErrorCode::corrupt_store, 0, static_cast<std::size_t>(id)});
                const auto* inbound = journal.find_record(dependency);
                if (inbound == nullptr) {
                    if (dependency >= journal.lowest_retained_record_id_)
                        return fail(Status{ErrorCode::corrupt_store, 0, static_cast<std::size_t>(id + 32)});
                } else if (inbound->kind != RecordKind::inbound || inbound->seq != seq || inbound->delivered) {
                    return fail(Status{ErrorCode::corrupt_store, 0, static_cast<std::size_t>(id + 32)});
                }
            } else {
                if (journal.current_epoch_ == std::numeric_limits<std::uint32_t>::max() ||
                    seq != journal.current_epoch_ || next_seq != journal.current_epoch_ + 1U ||
                    receipt != 0 || dependency != 0 || payload.empty() || payload.size() > 256 ||
                    payload.find('\0') != std::string_view::npos)
                    return fail(Status{ErrorCode::corrupt_store, 0, static_cast<std::size_t>(id)});
                for (const auto& item : journal.records_) {
                    if (item.kind == RecordKind::inbound && item.epoch == journal.current_epoch_ && !item.delivered)
                        return fail(Status{ErrorCode::corrupt_store, 0, static_cast<std::size_t>(id)});
                }
                journal.current_epoch_++;
                journal.next_inbound_seq_ = 1;
                journal.next_outbound_seq_ = 1;
                journal.lowest_retained_outbound_seq_ = 1;
                journal.checkpoint_.replay_cursor = 1;
                saw_epoch_reset = true;
                expected_inbound = 1;
                expected_outbound = 1;
                saw_inbound = false;
                saw_outbound = false;
                require_one_in = true;
                require_one_out = true;
                journal.outbound_index_.clear();
            }
            if (kind == RecordKind::inbound && next_seq > seq + 1) recovered.delivered = true;
            const std::size_t record_index = journal.records_.size();
            journal.records_.push_back(recovered);
            if (kind == RecordKind::outbound) journal.outbound_index_.push_back(record_index);
            if (kind == RecordKind::delivery) {
                auto inbound = std::lower_bound(journal.records_.begin(), journal.records_.end(), dependency,
                    [](const JournalRecord& item, RecordId value) { return item.id < value; });
                if (inbound != journal.records_.end() && inbound->id == dependency) inbound->delivered = true;
            }
            offset += total_size;
            segment_has_record = true;
            saw_any_record = true;
            last_data_segment = segment.index;
        }
        segment.write_offset = offset;
        if (segment_has_record) {
            journal.active_segment_ = segment_slot;
        } else if (!saw_any_record && segment_slot == 0) {
            journal.active_segment_ = 0;
        }
    }
    if (!epoch_known && !journal.epoch_audit_records_.empty())
        journal.current_epoch_ = journal.epoch_audit_records_.back().next_epoch;
    else if (!epoch_known && checkpoint_usable)
        journal.current_epoch_ = checkpoint_read.checkpoint.epoch;

    if (journal.audit_bytes_ == 0) {
        if (!journal.epoch_audit_records_.empty() || std::any_of(
            journal.records_.begin(), journal.records_.end(), [](const auto& record) {
                return record.kind == RecordKind::epoch_reset;
            }))
            return fail(Status{ErrorCode::corrupt_store, 0, 0});
    } else {
        for (const auto& record : journal.records_) {
            if (record.kind != RecordKind::epoch_reset) continue;
            const auto audit = std::find_if(journal.epoch_audit_records_.begin(),
                journal.epoch_audit_records_.end(), [&record](const auto& item) {
                    return item.reset_record_id == record.id;
                });
            if (audit != journal.epoch_audit_records_.end()) {
                if (audit->previous_epoch != record.epoch || audit->next_epoch != record.next_seq ||
                    audit->reason != record.payload)
                    return fail(Status{ErrorCode::corrupt_store, 0, static_cast<std::size_t>(record.id)});
                continue;
            }
            const auto expected_previous = journal.epoch_audit_records_.empty()
                ? 1U : journal.epoch_audit_records_.back().next_epoch;
            if (record.epoch != expected_previous || journal.audit_count_ >= audit_entry_count)
                return fail(Status{ErrorCode::corrupt_store, 0, static_cast<std::size_t>(record.id)});
            const auto audit_status = journal.append_epoch_audit(record.epoch,
                static_cast<std::uint32_t>(record.next_seq), record.id, record.payload);
            if (!audit_status.ok()) return fail(audit_status);
        }
        for (const auto& audit : journal.epoch_audit_records_) {
            const auto* event = journal.find_record(audit.reset_record_id);
            if (event != nullptr) {
                if (event->kind != RecordKind::epoch_reset || event->epoch != audit.previous_epoch ||
                    event->next_seq != audit.next_epoch || event->payload != audit.reason)
                    return fail(Status{ErrorCode::corrupt_store, 0,
                        static_cast<std::size_t>(audit.reset_record_id)});
            } else if (audit.reset_record_id >= journal.lowest_retained_record_id_) {
                return fail(Status{ErrorCode::corrupt_store, 0,
                    static_cast<std::size_t>(audit.reset_record_id)});
            }
        }
        const std::uint32_t audit_epoch = journal.epoch_audit_records_.empty()
            ? journal_initial_epoch : journal.epoch_audit_records_.back().next_epoch;
        if (audit_epoch != journal.current_epoch_ ||
            journal.epoch_audit_records_.size() != journal.current_epoch_ - 1U)
            return fail(Status{ErrorCode::corrupt_store, 0, journal.current_epoch_});
        journal.lowest_retained_outbound_seq_ = journal.current_epoch_ == journal_initial_epoch
            ? journal.lowest_retained_outbound_seq_ : std::max<std::uint64_t>(1,
                journal.lowest_retained_outbound_seq_);
    }
    if (!saw_inbound && checkpoint_usable && checkpoint_read.checkpoint.epoch == journal.current_epoch_)
        journal.next_inbound_seq_ = checkpoint_read.checkpoint.next_inbound_seq;
    else if (saw_inbound) journal.next_inbound_seq_ = expected_inbound;
    else if (!saw_inbound) journal.next_inbound_seq_ = 1;
    if (!saw_outbound && checkpoint_usable && checkpoint_read.checkpoint.epoch == journal.current_epoch_)
        journal.next_outbound_seq_ = checkpoint_read.checkpoint.next_outbound_seq;
    else if (saw_outbound) journal.next_outbound_seq_ = expected_outbound;
    else if (!saw_outbound) journal.next_outbound_seq_ = 1;
    if (journal.next_inbound_seq_ == 0 || journal.next_outbound_seq_ == 0)
        return fail(Status{ErrorCode::corrupt_store, 0, 0});

    // A nonempty segment followed by another nonempty segment is valid only
    // when the earlier segment ended at a clean record boundary.
    for (std::size_t i = 0; i + 1 < journal.segments_.size(); ++i) {
        const auto& segment = *journal.segments_[i];
        if (segment.write_offset < segment.mapping_size &&
            get_u32(static_cast<const unsigned char*>(segment.mapping) + segment.write_offset) != 0) {
            // A dirty tail is recoverable only in the final data segment.
            bool later_has_records = false;
            for (std::size_t j = i + 1; j < journal.segments_.size(); ++j) {
                if (journal.segments_[j]->write_offset > superblock_size) later_has_records = true;
            }
            if (later_has_records) return fail(Status{ErrorCode::corrupt_store, 0, segment.write_offset});
        }
    }

    journal.used_total_bytes_ = 0;
    for (const auto& segment : journal.segments_) journal.used_total_bytes_ += segment->write_offset;
    journal.used_total_bytes_ += journal.audit_file_size_;
    if (journal.records_.empty()) {
        journal.lowest_retained_record_id_ = segment_record_id(
            journal.segments_.front()->index, superblock_size);
    } else if (!checkpoint_usable || journal.first_segment_index_ == 0) {
        journal.lowest_retained_record_id_ = journal.records_.front().id;
    }
    if (!checkpoint_usable) {
        const auto first_retained_outbound = std::find_if(journal.records_.begin(), journal.records_.end(),
            [&journal](const auto& record) {
                return record.kind == RecordKind::outbound && record.epoch == journal.current_epoch_;
            });
        journal.lowest_retained_outbound_seq_ = first_retained_outbound == journal.records_.end()
            ? journal.next_outbound_seq_ : first_retained_outbound->seq;
    } else if (saw_epoch_reset && checkpoint_read.checkpoint.epoch != journal.current_epoch_)
        journal.lowest_retained_outbound_seq_ = 1;
    else if (journal.lowest_retained_outbound_seq_ > journal.next_outbound_seq_)
        return fail(Status{ErrorCode::corrupt_store, 0, 0});

    journal.next_segment_index_ = std::max(journal.next_segment_index_, last_data_segment + 1U);
    if (journal.next_segment_index_ > maximum_segment_index + 1U)
        return fail(Status{ErrorCode::storage_full, 0, 0});

    // Restore tail preallocation up to the configured active-segment budget.
    while (journal.segments_.size() < journal.max_segments_ &&
           journal.next_segment_index_ <= maximum_segment_index) {
        const auto created = open_data_segment(journal.next_segment_index_, true);
        if (!created.ok()) return fail(created.status());
        journal.segments_.push_back(created.value());
        ++journal.next_segment_index_;
    }

    journal.checkpoint_ = checkpoint_usable ? checkpoint_read.checkpoint : JournalCheckpoint{};
    if (saw_epoch_reset &&
        (!checkpoint_usable || checkpoint_read.checkpoint.epoch != journal.current_epoch_))
        journal.checkpoint_.replay_cursor = 1;
    const RecordId last_record_id = journal.records_.empty() ? 0 : journal.records_.back().id;
    if (checkpoint_usable && checkpoint_read.checkpoint.last_record_id != 0 &&
        last_record_id < checkpoint_read.checkpoint.last_record_id)
        return fail(Status{ErrorCode::corrupt_store, 0,
            static_cast<std::size_t>(checkpoint_read.checkpoint.last_record_id)});
    RecordId rebuilt_delivery_cursor = 0;
    bool delivered_prefix = true;
    for (const auto& record : journal.records_) {
        if (record.kind != RecordKind::inbound) continue;
        if (!record.delivered) {
            delivered_prefix = false;
            continue;
        }
        if (delivered_prefix) rebuilt_delivery_cursor = record.id;
    }
    const RecordId stored_delivery_cursor = journal.checkpoint_.delivery_cursor;
    bool delivery_cursor_valid = true;
    if (stored_delivery_cursor != 0 && stored_delivery_cursor >= journal.lowest_retained_record_id_) {
        const auto* cursor_record = journal.find_record(stored_delivery_cursor);
        if (cursor_record == nullptr || cursor_record->kind != RecordKind::inbound || !cursor_record->delivered)
            delivery_cursor_valid = false;
    }
    if (delivery_cursor_valid) {
        for (const auto& record : journal.records_) {
            if (record.kind == RecordKind::inbound && record.id <= stored_delivery_cursor && !record.delivered) {
                delivery_cursor_valid = false;
                break;
            }
        }
    }
    const RecordId recovered_delivery_cursor = delivery_cursor_valid
        ? std::max(stored_delivery_cursor, rebuilt_delivery_cursor) : rebuilt_delivery_cursor;
    const bool checkpoint_epoch_matches = checkpoint_usable &&
        checkpoint_read.checkpoint.epoch == journal.current_epoch_;
    bool completed_checkpoint_valid = checkpoint_epoch_matches;
    std::uint64_t recovered_completed_seq = checkpoint_epoch_matches
        ? checkpoint_read.checkpoint.completed_inbound_seq : 0;
    if (completed_checkpoint_valid) {
        for (const auto& record : journal.records_) {
            if (record.kind == RecordKind::inbound && record.epoch == journal.current_epoch_ &&
                record.seq <= recovered_completed_seq && !record.delivered) {
                completed_checkpoint_valid = false;
                break;
            }
        }
    }
    if (!completed_checkpoint_valid) recovered_completed_seq = 0;
    const auto first_current_inbound = std::find_if(journal.records_.begin(), journal.records_.end(),
        [&journal](const auto& record) {
            return record.kind == RecordKind::inbound && record.epoch == journal.current_epoch_;
        });
    if (journal.first_segment_index_ != 0) {
        const auto retained_base = first_current_inbound != journal.records_.end()
            ? first_current_inbound->seq - 1U
            : journal.next_inbound_seq_ - 1U;
        if (retained_base > recovered_completed_seq) {
            recovered_completed_seq = retained_base;
            completed_checkpoint_valid = false;
        }
    }
    for (const auto& record : journal.records_) {
        if (record.kind == RecordKind::inbound && record.epoch == journal.current_epoch_ &&
            record.seq <= recovered_completed_seq && !record.delivered) {
            recovered_completed_seq = record.seq - 1U;
            completed_checkpoint_valid = false;
            break;
        }
    }
    journal.completed_inbound_seq_ = recovered_completed_seq;
    journal.completed_inbound_record_id_ = recovered_delivery_cursor;
    journal.completed_scan_index_ = 0;
    journal.checkpoint_.completed_inbound_seq = recovered_completed_seq;
    journal.checkpoint_.delivery_cursor = recovered_delivery_cursor;
    journal.advance_completed_inbound_cursor();
    const bool completed_cursor_stale = !checkpoint_epoch_matches || !completed_checkpoint_valid ||
        journal.completed_inbound_seq_ != (checkpoint_epoch_matches
            ? checkpoint_read.checkpoint.completed_inbound_seq : 0) ||
        !delivery_cursor_valid || journal.completed_inbound_record_id_ != stored_delivery_cursor;
    const bool index_matches = checkpoint_usable &&
        checkpoint_read.outbound_index.size() == journal.outbound_index_.size() &&
        std::equal(checkpoint_read.outbound_index.begin(), checkpoint_read.outbound_index.end(),
            journal.outbound_index_.begin(), [&journal](const auto& entry, std::size_t index) {
                const auto& record = journal.records_[index];
                return entry.first == record.seq && entry.second == record.id;
            });
    journal.checkpoint_stale_ = !checkpoint_usable ||
        journal.checkpoint_.epoch != journal.current_epoch_ ||
        journal.checkpoint_.last_record_id != last_record_id ||
        journal.checkpoint_.next_inbound_seq != journal.next_inbound_seq_ ||
        journal.checkpoint_.next_outbound_seq != journal.next_outbound_seq_ || !index_matches ||
        completed_cursor_stale;
    journal.checkpoint_.next_inbound_seq = journal.next_inbound_seq_;
    journal.checkpoint_.next_outbound_seq = journal.next_outbound_seq_;
    journal.checkpoint_.completed_inbound_seq = journal.completed_inbound_seq_;
    journal.checkpoint_.epoch = journal.current_epoch_;
    journal.checkpoint_.last_record_id = last_record_id;
    journal.checkpoint_.first_segment_index = journal.first_segment_index_;
    journal.checkpoint_.lowest_retained_outbound_seq = journal.lowest_retained_outbound_seq_;
    journal.checkpoint_.lowest_retained_record_id = journal.lowest_retained_record_id_;
    journal.checkpoint_.delivery_cursor = journal.completed_inbound_record_id_;
    if (!checkpoint_usable) {
        journal.checkpoint_.replay_cursor = journal.lowest_retained_outbound_seq_;
    }
    if (!checkpoint_usable || journal.checkpoint_stale_) {
        const auto checkpoint_status = journal.write_checkpoint_file(journal.checkpoint_);
        if (!checkpoint_status.ok()) return fail(checkpoint_status);
    }
    journal.published_record_count_.store(journal.records_.size(), std::memory_order_release);
    return journal;
} catch (const std::bad_alloc&) {
    return Status{ErrorCode::capacity_exceeded, 0, 0};
} catch (...) {
    return Status{ErrorCode::io_error, 0, 0};
}

Result<Journal> Journal::open(JournalConfig config) noexcept {
    if (config.path.empty() || config.session_key.empty() ||
        config.path.find('\0') != std::string_view::npos ||
        config.path.size() > 4096 || config.session_key.size() > journal_max_session_key_bytes ||
        config.capacity_bytes < superblock_size + record_header_size ||
        config.capacity_bytes > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
        config.max_records == 0 ||
        config.max_records > (config.capacity_bytes - superblock_size) / record_header_size)
        return Status{ErrorCode::invalid_config, 0, 0};

    if (config.segment_bytes != 0) return open_segmented(config);

    Journal journal;
    std::string path;
    try {
        path.assign(config.path.data(), config.path.size());
        journal.records_.reserve(config.max_records);
        journal.outbound_index_.reserve(config.max_records);
        journal.records_data_ = journal.records_.data();
        journal.max_readers_ = config.max_readers;
        journal.max_reader_lag_records_ = config.max_reader_lag_records;
        if (config.max_readers != 0)
            journal.reader_registry_ = std::make_shared<JournalReader::Registry>(config.max_readers);
    } catch (...) {
        return Status{ErrorCode::capacity_exceeded, 0, 0};
    }

    try {
        const auto parent = std::filesystem::path(path).parent_path();
        if (!parent.empty()) {
            if (has_symlink_directory_component(parent))
                return Status{ErrorCode::invalid_config, 0, 0};
            std::error_code error;
            (void)std::filesystem::create_directories(parent, error);
            if (error) return Status{ErrorCode::io_error, 0, 0};
            if (has_symlink_directory_component(parent))
                return Status{ErrorCode::invalid_config, 0, 0};
        }
    } catch (...) {
        return Status{ErrorCode::capacity_exceeded, 0, 0};
    }

    bool created = false;
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd >= 0) {
        created = true;
    } else if (errno == EEXIST) {
        fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    }
    if (fd < 0) return io_status(errno);

    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int saved_errno = errno;
        ::close(fd);
        if (saved_errno == EWOULDBLOCK || saved_errno == EAGAIN)
            return Status{ErrorCode::store_locked, 0, 0};
        return io_status(saved_errno);
    }
    journal.fd_ = fd;
    journal.capacity_bytes_ = config.capacity_bytes;
    journal.max_records_ = config.max_records;
    journal.test_hook_ = config.test_hook;

    auto fail = [&](Status status) -> Result<Journal> {
        if (created) ::unlink(path.c_str());
        journal.reset();
        return status;
    };

    struct stat file_stat {};
    if (::fstat(fd, &file_stat) != 0) return fail(io_status(errno));
    if (!S_ISREG(file_stat.st_mode) || file_stat.st_uid != ::geteuid() ||
        (file_stat.st_mode & 0077) != 0)
        return fail(Status{ErrorCode::invalid_config, 0, 0});
    if (file_stat.st_size < 0) return fail(Status{ErrorCode::corrupt_store, 0, 0});

    if (created) {
        if (file_stat.st_size != 0) return fail(Status{ErrorCode::corrupt_store, 0, 0});
        if (::ftruncate(fd, static_cast<off_t>(config.capacity_bytes)) != 0)
            return fail(io_status(errno));
        const int allocation_error = ::posix_fallocate(fd, 0, static_cast<off_t>(config.capacity_bytes));
        if (allocation_error != 0) return fail(io_status(allocation_error));
    } else {
        if (static_cast<std::uint64_t>(file_stat.st_size) < superblock_size)
            return fail(Status{ErrorCode::corrupt_store, 0, 0});
        if (static_cast<std::uint64_t>(file_stat.st_size) >
            static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
            return fail(Status{ErrorCode::too_large, 0, 0});
        std::array<unsigned char, superblock_size> header{};
        const auto read_status = pread_exact(fd, header.data(), header.size());
        if (!read_status.ok()) return fail(read_status);
        const auto header_status = validate_superblock(
            header.data(), static_cast<std::size_t>(file_stat.st_size), config);
        if (!header_status.ok()) return fail(header_status);
    }

    const std::size_t map_size = created
        ? config.capacity_bytes
        : static_cast<std::size_t>(file_stat.st_size);
    int map_flags = MAP_SHARED;
#if defined(MAP_POPULATE)
    map_flags |= MAP_POPULATE;
#endif
    void* mapping = ::mmap(nullptr, map_size, PROT_READ | PROT_WRITE, map_flags, fd, 0);
    if (mapping == MAP_FAILED) {
        journal.mapping_ = nullptr;
        return fail(io_status(errno));
    }
    journal.mapping_ = mapping;
    journal.mapping_size_ = map_size;
    journal.capacity_bytes_ = map_size;
    (void)::madvise(mapping, map_size, MADV_WILLNEED);

    auto* bytes = static_cast<unsigned char*>(mapping);
    if (created) {
        write_superblock(bytes, map_size, config.session_key);
        journal.write_offset_ = superblock_size;
        return journal;
    }

    const auto* stored_header = bytes;
    const auto header_status = validate_superblock(stored_header, map_size, config);
    if (!header_status.ok()) return fail(header_status);

    std::size_t offset = superblock_size;
    while (offset <= map_size && map_size - offset >= record_header_size) {
        auto* record = bytes + offset;
        auto* marker_word = reinterpret_cast<std::uint32_t*>(record + record_commit_offset);
        std::atomic_ref<std::uint32_t> marker(*marker_word);
        const std::uint32_t commit = from_little_endian(marker.load(std::memory_order_acquire));
        if (commit == 0) {
            if (has_committed_record_after(bytes, offset, map_size))
                return fail(Status{ErrorCode::corrupt_store, 0, offset + record_commit_offset});
            // A crashed append may have dirtied an arbitrarily long tail. Clear
            // it before reusing this offset so a shorter future record cannot
            // expose stale bytes as a phantom commit marker. Keep total_size
            // nonzero until the clear is complete, making an interrupted clear
            // recognizable and repeatable on the next recovery.
            if (get_u32(record) != 0) {
                std::memset(record + sizeof(std::uint32_t), 0,
                            map_size - offset - sizeof(std::uint32_t));
                std::atomic_thread_fence(std::memory_order_release);
                put_u32(record, 0);
            }
            journal.write_offset_ = offset;
            journal.completed_scan_index_ = 0;
            journal.advance_completed_inbound_cursor();
            journal.published_record_count_.store(journal.records_.size(), std::memory_order_release);
            return journal;
        }
        if (commit != record_commit_marker)
            return fail(Status{ErrorCode::corrupt_store, 0, offset + record_commit_offset});

        const std::uint32_t total_size = get_u32(record);
        const std::uint32_t payload_size = get_u32(record + 4);
        const std::uint32_t raw_kind = get_u32(record + 8);
        const std::uint32_t epoch = get_u32(record + 12);
        const std::uint64_t seq = get_u64(record + 16);
        const std::uint64_t receipt = get_u64(record + 24);
        const RecordId dependency = get_u64(record + 32);
        const std::uint64_t next_seq = get_u64(record + 40);

        if (total_size < record_header_size || (total_size & 7U) != 0 ||
            total_size > map_size - offset ||
            payload_size > total_size - record_header_size ||
            align_record(record_header_size + static_cast<std::size_t>(payload_size)) != total_size ||
            raw_kind < static_cast<std::uint32_t>(RecordKind::inbound) ||
            raw_kind > static_cast<std::uint32_t>(RecordKind::delivery) || epoch != current_epoch)
            return fail(Status{ErrorCode::corrupt_store, 0, offset});
        if (get_u32(record + record_crc_offset) != record_crc(record, total_size))
            return fail(Status{ErrorCode::corrupt_store, 0, offset + record_crc_offset});
        if (journal.records_.size() >= journal.max_records_)
            return fail(Status{ErrorCode::invalid_config, 0, offset});

        const auto kind = static_cast<RecordKind>(raw_kind);
        const std::string_view payload(reinterpret_cast<const char*>(record + record_header_size),
                                       payload_size);
        JournalRecord recovered{static_cast<RecordId>(offset), kind, seq, receipt,
                                dependency, next_seq, payload, false};

        if (kind == RecordKind::inbound) {
            if (seq != journal.next_inbound_seq_ || seq == std::numeric_limits<std::uint64_t>::max() ||
                next_seq <= seq || receipt != 0 || dependency != 0 || payload.empty())
                return fail(Status{ErrorCode::corrupt_store, 0, offset});
            journal.next_inbound_seq_ = next_seq;
        } else if (kind == RecordKind::outbound) {
            if (seq != journal.next_outbound_seq_ || seq == std::numeric_limits<std::uint64_t>::max() ||
                next_seq != 0 || payload.empty())
                return fail(Status{ErrorCode::corrupt_store, 0, offset});
            if (dependency != 0) {
                const auto* dependency_record = journal.find_record(dependency);
                if (dependency_record == nullptr || dependency_record->kind != RecordKind::inbound)
                    return fail(Status{ErrorCode::corrupt_store, 0, offset + 32});
            }
            journal.next_outbound_seq_ = seq + 1;
        } else {
            if (seq == 0 || receipt != 0 || next_seq != 0 || !payload.empty())
                return fail(Status{ErrorCode::corrupt_store, 0, offset});
            const auto* inbound_record = journal.find_record(dependency);
            if (inbound_record == nullptr || inbound_record->kind != RecordKind::inbound ||
                inbound_record->seq != seq || journal.has_delivery(dependency))
                return fail(Status{ErrorCode::corrupt_store, 0, offset + 32});
        }
        if (kind == RecordKind::inbound && next_seq > seq + 1)
            recovered.delivered = true;
        const std::size_t record_index = journal.records_.size();
        journal.records_.push_back(recovered);
        if (kind == RecordKind::outbound) journal.outbound_index_.push_back(record_index);
        if (kind == RecordKind::delivery) {
            auto inbound = std::lower_bound(journal.records_.begin(), journal.records_.end(), dependency,
                [](const JournalRecord& item, RecordId id) { return item.id < id; });
            if (inbound != journal.records_.end() && inbound->id == dependency)
                inbound->delivered = true;
        }
        offset += total_size;
    }
    journal.write_offset_ = offset;
    journal.completed_scan_index_ = 0;
    journal.advance_completed_inbound_cursor();
    journal.published_record_count_.store(journal.records_.size(), std::memory_order_release);
    return journal;
}

JournalInspection Journal::inspect_snapshot(JournalConfig config) noexcept try {
    JournalInspection snapshot;
    if (config.path.empty() || config.session_key.empty() || config.max_records == 0 ||
        config.path.find('\0') != std::string_view::npos ||
        config.path.size() > 4096 || config.session_key.size() > journal_max_session_key_bytes) {
        snapshot.diagnostic = {ErrorCode::invalid_config, 0, 0};
        return snapshot;
    }
    std::string base(config.path);
    const auto inspect_parent = std::filesystem::path(base).parent_path();
    if (!inspect_parent.empty() && has_symlink_directory_component(inspect_parent)) {
        snapshot.diagnostic = {ErrorCode::invalid_config, 0, 0};
        return snapshot;
    }
    const int fd = ::open(base.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) { snapshot.diagnostic = io_status(errno); return snapshot; }
    struct stat info {};
    if (::fstat(fd, &info) != 0) {
        snapshot.diagnostic = io_status(errno);
        ::close(fd);
        return snapshot;
    }
    if (!S_ISREG(info.st_mode) || info.st_uid != ::geteuid() || (info.st_mode & 0077) != 0 ||
        info.st_size < static_cast<off_t>(superblock_size)) {
        snapshot.diagnostic = {ErrorCode::corrupt_store, 0, 0};
        ::close(fd);
        return snapshot;
    }
    std::array<unsigned char, superblock_size> header{};
    const auto read_status = pread_exact(fd, header.data(), header.size());
    if (!read_status.ok()) {
        snapshot.diagnostic = read_status;
        ::close(fd);
        return snapshot;
    }
    const auto version = get_u32(header.data() + 8);
    if (version == journal_format_version &&
        std::memcmp(header.data(), superblock_magic.data(), superblock_magic.size()) == 0) {
        JournalConfig validation = config;
        validation.capacity_bytes = static_cast<std::size_t>(info.st_size);
        snapshot.diagnostic = validate_superblock(header.data(),
            static_cast<std::size_t>(info.st_size), validation);
        if (!snapshot.diagnostic.ok()) { ::close(fd); return snapshot; }
        snapshot.format_version = journal_format_version;
        const auto map_size = static_cast<std::size_t>(info.st_size);
        void* mapping = ::mmap(nullptr, map_size, PROT_READ, MAP_SHARED, fd, 0);
        ::close(fd);
        if (mapping == MAP_FAILED) { snapshot.diagnostic = io_status(errno); return snapshot; }
        const auto* bytes = static_cast<const unsigned char*>(mapping);
        std::size_t offset = superblock_size;
        std::uint64_t next_in = 1;
        std::uint64_t next_out = 1;
        while (offset <= map_size && map_size - offset >= record_header_size) {
            const auto* record = bytes + offset;
            const auto commit = get_u32(record + record_commit_offset);
            if (commit == 0) {
                if (has_committed_record_after(bytes, offset, map_size))
                    snapshot.diagnostic = {ErrorCode::corrupt_store, 0, offset + record_commit_offset};
                break;
            }
            if (commit != record_commit_marker || !valid_committed_record_at(bytes, offset, map_size) ||
                get_u32(record + 8) > static_cast<std::uint32_t>(RecordKind::delivery)) {
                snapshot.diagnostic = {ErrorCode::corrupt_store, 0, offset};
                break;
            }
            const auto total = get_u32(record);
            const auto payload = get_u32(record + 4);
            const auto kind = static_cast<RecordKind>(get_u32(record + 8));
            const auto seq = get_u64(record + 16);
            const auto receipt = get_u64(record + 24);
            const auto dependency = get_u64(record + 32);
            const auto next = get_u64(record + 40);
            if ((kind == RecordKind::inbound &&
                 (seq != next_in || next <= seq || receipt != 0 || dependency != 0 || payload == 0)) ||
                (kind == RecordKind::outbound &&
                 (seq != next_out || next != 0 || payload == 0)) ||
                (kind == RecordKind::delivery &&
                 (seq == 0 || receipt != 0 || next != 0 || payload != 0 || dependency < superblock_size))) {
                snapshot.diagnostic = {ErrorCode::corrupt_store, 0, offset};
                break;
            }
            if (kind == RecordKind::inbound) { ++snapshot.inbound_records; next_in = next; }
            else if (kind == RecordKind::outbound) { ++snapshot.outbound_records; ++next_out; }
            else ++snapshot.delivery_records;
            ++snapshot.committed_records;
            offset += total;
        }
        snapshot.used_bytes = offset;
        snapshot.next_inbound_seq = next_in;
        snapshot.next_outbound_seq = next_out;
        snapshot.lowest_retained_record_id = superblock_size;
        snapshot.lowest_retained_outbound_seq = 1;
        (void)::munmap(mapping, map_size);
        return snapshot;
    }
    if (version != 2 || std::memcmp(header.data(), v2_manifest_magic.data(), v2_manifest_magic.size()) != 0) {
        snapshot.diagnostic = version > 2 ? Status{ErrorCode::unsupported_version, 0, 8}
                                         : Status{ErrorCode::corrupt_store, 0, 0};
        ::close(fd);
        return snapshot;
    }
    if (info.st_size != static_cast<off_t>(superblock_size) ||
        get_u64(header.data() + 16) != superblock_size ||
        get_u32(header.data() + 12) != superblock_size ||
        get_u32(header.data() + 24) != config.session_key.size() ||
        std::memcmp(header.data() + superblock_key_offset, config.session_key.data(), config.session_key.size()) != 0 ||
        get_u32(header.data() + 28) != current_epoch || get_u32(header.data() + 32) != 0 ||
        get_u32(header.data() + v2_header_crc_offset) !=
            crc32c(header.data(), header.size(), v2_header_crc_offset, sizeof(std::uint32_t))) {
        snapshot.diagnostic = {ErrorCode::corrupt_store, 0, 0};
        ::close(fd);
        return snapshot;
    }
    const auto segment_bytes = static_cast<std::size_t>(get_u64(header.data() + 40));
    const auto total_bytes = static_cast<std::size_t>(get_u64(header.data() + 48));
    const auto max_segments = get_u32(header.data() + 36);
    const auto audit_bytes = static_cast<std::size_t>(get_u32(header.data() + 56));
    if (config.segment_bytes != 0 && config.segment_bytes != segment_bytes) {
        snapshot.diagnostic = {ErrorCode::invalid_config, 0, 40};
        ::close(fd);
        return snapshot;
    }
    if (segment_bytes < superblock_size + record_header_size || max_segments == 0 ||
        (audit_bytes != 0 && audit_bytes != audit_file_size) || total_bytes < audit_bytes + segment_bytes ||
        total_bytes > std::numeric_limits<std::size_t>::max() ||
        max_segments > (total_bytes - audit_bytes) / segment_bytes) {
        snapshot.diagnostic = {ErrorCode::corrupt_store, 0, 36};
        ::close(fd);
        return snapshot;
    }
    ::close(fd);
    snapshot.format_version = 2;
    const auto cp = read_checkpoint_file(base, config.session_key, config.max_records);
    snapshot.checkpoint_stale = !cp.valid;
    auto first_segment = cp.valid ? cp.checkpoint.first_segment_index : 0U;
    snapshot.lowest_retained_outbound_seq = cp.valid ? cp.checkpoint.lowest_retained_outbound_seq : 1U;
    snapshot.lowest_retained_record_id = cp.valid ? cp.checkpoint.lowest_retained_record_id : 4096U;

    const auto parent = std::filesystem::path(base).parent_path();
    const auto directory = parent.empty() ? std::filesystem::path(".") : parent;
    const std::string prefix = std::filesystem::path(base).filename().string() + ".seg";
    std::vector<std::uint32_t> indices;
    bool segment_index_overflow = false;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        const auto name = entry.path().filename().string();
        if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0) continue;
        std::uint64_t index = 0;
        const auto parsed = std::from_chars(name.data() + prefix.size(), name.data() + name.size(), index);
        if (parsed.ec == std::errc{} && parsed.ptr == name.data() + name.size()) {
            if (index > maximum_segment_index) segment_index_overflow = true;
            else indices.push_back(static_cast<std::uint32_t>(index));
        }
    }
    if (segment_index_overflow) {
        snapshot.diagnostic = {ErrorCode::storage_full, 0, 0};
        return snapshot;
    }
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    if (!cp.valid) {
        if (indices.empty()) {
            snapshot.diagnostic = {ErrorCode::corrupt_store, 0, 0};
            return snapshot;
        }
        first_segment = indices.front();
        snapshot.lowest_retained_record_id = segment_record_id(first_segment, superblock_size);
    }
    if (cp.valid && !indices.empty() && indices.front() > first_segment) {
        snapshot.diagnostic = {ErrorCode::corrupt_store, 0, indices.front()};
        return snapshot;
    }
    std::vector<std::uint32_t> retained;
    for (const auto index : indices) if (index >= first_segment) retained.push_back(index);
    if (first_segment != 0 && (retained.empty() || retained.front() != first_segment)) {
        snapshot.diagnostic = {ErrorCode::corrupt_store, 0, first_segment};
        return snapshot;
    }
    if (retained.empty()) {
        snapshot.diagnostic = {ErrorCode::corrupt_store, 0, 0};
        return snapshot;
    }
    for (std::size_t i = 1; i < retained.size(); ++i) {
        if (retained[i] != retained[i - 1] + 1U) {
            snapshot.diagnostic = {ErrorCode::corrupt_store, 0, retained[i]};
            return snapshot;
        }
    }
    snapshot.segment_count = retained.size();
    std::uint64_t next_in = cp.valid ? cp.checkpoint.next_inbound_seq : 1;
    std::uint64_t next_out = cp.valid ? cp.checkpoint.next_outbound_seq : 1;
    bool saw_in = false;
    bool saw_out = false;
    bool epoch_known = false;
    bool require_one_in = first_segment == 0;
    bool require_one_out = first_segment == 0;
    std::uint32_t active_epoch = cp.valid ? cp.checkpoint.epoch : journal_initial_epoch;
    RecordId last_record = 0;
    std::vector<std::pair<std::uint64_t, RecordId>> outbound_index;
    struct ResetSummary {
        std::uint32_t previous_epoch{};
        std::uint32_t next_epoch{};
        RecordId id{};
        std::string reason{};
    };
    struct RecordSummary {
        RecordId id{};
        RecordKind kind{};
        std::uint64_t seq{};
        RecordId dependency{};
        std::uint32_t epoch{};
        bool delivered{};
    };
    std::vector<ResetSummary> reset_events;
    std::vector<RecordSummary> records;
    std::vector<RecordId> delivery_dependencies;
    records.reserve(config.max_records);
    delivery_dependencies.reserve(config.max_records);
    auto find_summary = [&records](RecordId id) -> RecordSummary* {
        const auto found = std::lower_bound(records.begin(), records.end(), id,
            [](const RecordSummary& item, RecordId value) { return item.id < value; });
        return found != records.end() && found->id == id ? &*found : nullptr;
    };
    for (const auto index : retained) {
        const auto path = segment_path(base, index);
        const int seg_fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (seg_fd < 0) { snapshot.diagnostic = io_status(errno); return snapshot; }
        struct stat seg_stat {};
        if (::fstat(seg_fd, &seg_stat) != 0 || !S_ISREG(seg_stat.st_mode) ||
            seg_stat.st_uid != ::geteuid() || (seg_stat.st_mode & 0077) != 0 ||
            seg_stat.st_size != static_cast<off_t>(segment_bytes)) {
            snapshot.diagnostic = {ErrorCode::corrupt_store, 0, 0};
            ::close(seg_fd);
            return snapshot;
        }
        void* mapping = ::mmap(nullptr, segment_bytes, PROT_READ, MAP_SHARED, seg_fd, 0);
        ::close(seg_fd);
        if (mapping == MAP_FAILED) { snapshot.diagnostic = io_status(errno); return snapshot; }
        const auto* bytes = static_cast<const unsigned char*>(mapping);
        const auto segment_header = validate_v2_header(bytes, v2_segment_magic, segment_bytes,
            config.session_key, segment_bytes, total_bytes, max_segments, index, audit_bytes);
        if (!segment_header.ok()) {
            snapshot.diagnostic = segment_header;
            (void)::munmap(mapping, segment_bytes);
            return snapshot;
        }
        std::size_t offset = superblock_size;
        while (offset <= segment_bytes && segment_bytes - offset >= record_header_size) {
            const auto* record = bytes + offset;
            const auto commit = load_u32_acquire(record + record_commit_offset);
            if (commit == 0) {
                if (has_committed_record_after(bytes, offset, segment_bytes))
                    snapshot.diagnostic = {ErrorCode::corrupt_store, 0, offset + record_commit_offset};
                break;
            }
            if (commit != record_commit_marker || !valid_committed_record_at(bytes, offset, segment_bytes)) {
                snapshot.diagnostic = {ErrorCode::corrupt_store, 0, offset};
                break;
            }
            const auto total = get_u32(record);
            const auto payload = get_u32(record + 4);
            const auto raw_kind = get_u32(record + 8);
            const auto record_epoch = get_u32(record + 12);
            const auto seq = get_u64(record + 16);
            const auto receipt = get_u64(record + 24);
            const auto dependency = get_u64(record + 32);
            const auto next = get_u64(record + 40);
            const auto id = segment_record_id(index, offset);
            if (snapshot.committed_records >= config.max_records || id <= last_record) {
                snapshot.diagnostic = {ErrorCode::corrupt_store, 0, static_cast<std::size_t>(id)};
                break;
            }
            if (record_epoch == 0 || (epoch_known && record_epoch != active_epoch)) {
                snapshot.diagnostic = {ErrorCode::corrupt_store, 0, static_cast<std::size_t>(id + 12)};
                break;
            }
            if (!epoch_known) { active_epoch = record_epoch; epoch_known = true; }
            if (raw_kind == static_cast<std::uint32_t>(RecordKind::inbound)) {
                if ((saw_in && seq != next_in) || (!saw_in && require_one_in && seq != 1) ||
                    next <= seq || payload == 0 || receipt != 0 || dependency != 0) {
                    snapshot.diagnostic = {ErrorCode::corrupt_store, 0, static_cast<std::size_t>(id)};
                    break;
                }
                next_in = next; saw_in = true; require_one_in = false; ++snapshot.inbound_records;
            } else if (raw_kind == static_cast<std::uint32_t>(RecordKind::outbound)) {
                if ((saw_out && seq != next_out) || (!saw_out && require_one_out && seq != 1) ||
                    seq == std::numeric_limits<std::uint64_t>::max() || next != 0 || payload == 0) {
                    snapshot.diagnostic = {ErrorCode::corrupt_store, 0, static_cast<std::size_t>(id)};
                    break;
                }
                if (dependency != 0) {
                    const auto* inbound = find_summary(dependency);
                    if ((inbound == nullptr && dependency >= snapshot.lowest_retained_record_id) ||
                        (inbound != nullptr && inbound->kind != RecordKind::inbound)) {
                        snapshot.diagnostic = {ErrorCode::corrupt_store, 0,
                            static_cast<std::size_t>(id + 32)};
                        break;
                    }
                }
                next_out = seq + 1; saw_out = true; require_one_out = false; ++snapshot.outbound_records;
                outbound_index.emplace_back(seq, id);
            } else if (raw_kind == static_cast<std::uint32_t>(RecordKind::delivery)) {
                if (seq == 0 || receipt != 0 || next != 0 || payload != 0 || dependency == 0 || dependency >= id) {
                    snapshot.diagnostic = {ErrorCode::corrupt_store, 0, static_cast<std::size_t>(id)};
                    break;
                }
                auto* inbound = find_summary(dependency);
                const bool duplicate = std::find(delivery_dependencies.begin(), delivery_dependencies.end(),
                    dependency) != delivery_dependencies.end();
                if (duplicate || (inbound == nullptr && dependency >= snapshot.lowest_retained_record_id) ||
                    (inbound != nullptr && (inbound->kind != RecordKind::inbound ||
                        inbound->seq != seq || inbound->delivered))) {
                    snapshot.diagnostic = {ErrorCode::corrupt_store, 0,
                        static_cast<std::size_t>(id + 32)};
                    break;
                }
                delivery_dependencies.push_back(dependency);
                if (inbound != nullptr) inbound->delivered = true;
                ++snapshot.delivery_records;
            } else if (raw_kind == static_cast<std::uint32_t>(RecordKind::epoch_reset)) {
                if (active_epoch == std::numeric_limits<std::uint32_t>::max() || seq != active_epoch ||
                    next != active_epoch + 1U || payload == 0 || payload > 256 ||
                    receipt != 0 || dependency != 0 ||
                    std::memchr(record + record_header_size, 0, payload) != nullptr) {
                    snapshot.diagnostic = {ErrorCode::corrupt_store, 0, static_cast<std::size_t>(id)};
                    break;
                }
                for (const auto& previous : records) {
                    if (previous.kind == RecordKind::inbound && previous.epoch == active_epoch &&
                        !previous.delivered) {
                        snapshot.diagnostic = {ErrorCode::corrupt_store, 0,
                            static_cast<std::size_t>(id)};
                        break;
                    }
                }
                if (!snapshot.diagnostic.ok()) break;
                ++active_epoch;
                next_in = 1;
                next_out = 1;
                saw_in = false;
                saw_out = false;
                require_one_in = true;
                require_one_out = true;
                outbound_index.clear();
                ++snapshot.epoch_reset_records;
                reset_events.push_back(ResetSummary{
                    record_epoch, record_epoch + 1U, id,
                    std::string(reinterpret_cast<const char*>(record + record_header_size), payload)});
            } else {
                snapshot.diagnostic = {ErrorCode::corrupt_store, 0, static_cast<std::size_t>(id + 8)};
                break;
            }
            records.push_back(RecordSummary{id, static_cast<RecordKind>(raw_kind), seq,
                dependency, record_epoch,
                raw_kind == static_cast<std::uint32_t>(RecordKind::inbound) && next > seq + 1U});
            ++snapshot.committed_records;
            last_record = id;
            offset += total;
        }
        snapshot.used_bytes += offset;
        (void)::munmap(mapping, segment_bytes);
        if (!snapshot.diagnostic.ok()) return snapshot;
    }
    if (!epoch_known && cp.valid) active_epoch = cp.checkpoint.epoch;
    if (!saw_in && cp.valid && cp.checkpoint.epoch == active_epoch) next_in = cp.checkpoint.next_inbound_seq;
    else if (!saw_in) next_in = 1;
    if (!saw_out && cp.valid && cp.checkpoint.epoch == active_epoch) next_out = cp.checkpoint.next_outbound_seq;
    else if (!saw_out) next_out = 1;
    if (cp.valid && cp.checkpoint.last_record_id != 0 && last_record < cp.checkpoint.last_record_id) {
        snapshot.diagnostic = {ErrorCode::corrupt_store, 0,
            static_cast<std::size_t>(cp.checkpoint.last_record_id)};
        return snapshot;
    }
    snapshot.next_inbound_seq = next_in;
    snapshot.next_outbound_seq = next_out;
    if (!cp.valid) {
        snapshot.lowest_retained_outbound_seq = outbound_index.empty()
            ? next_out : outbound_index.front().first;
    }
    if (cp.valid) {
        snapshot.checkpoint_stale = cp.checkpoint.last_record_id != last_record ||
            cp.checkpoint.epoch != active_epoch ||
            cp.checkpoint.next_inbound_seq != next_in || cp.checkpoint.next_outbound_seq != next_out ||
            cp.outbound_index != outbound_index;
    }

    if (audit_bytes != 0) {
        const std::string audit_path = base + ".audit";
        const int audit_fd = ::open(audit_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (audit_fd < 0) {
            snapshot.diagnostic = io_status(errno);
            return snapshot;
        }
        struct stat audit_stat {};
        if (::fstat(audit_fd, &audit_stat) != 0 || !S_ISREG(audit_stat.st_mode) ||
            audit_stat.st_uid != ::geteuid() || (audit_stat.st_mode & 0077) != 0 ||
            audit_stat.st_size != static_cast<off_t>(audit_file_size)) {
            snapshot.diagnostic = {ErrorCode::corrupt_store, 0, 0};
            ::close(audit_fd);
            return snapshot;
        }
        void* audit_mapping = ::mmap(nullptr, audit_file_size, PROT_READ, MAP_SHARED, audit_fd, 0);
        const int map_error = errno;
        ::close(audit_fd);
        if (audit_mapping == MAP_FAILED) {
            snapshot.diagnostic = io_status(map_error);
            return snapshot;
        }
        const auto* audit_data = static_cast<const unsigned char*>(audit_mapping);
        const auto audit_header_status = validate_audit_header(audit_data, config.session_key);
        if (!audit_header_status.ok()) {
            snapshot.diagnostic = audit_header_status;
            (void)::munmap(audit_mapping, audit_file_size);
            return snapshot;
        }
        std::vector<ResetSummary> audit_records;
        audit_records.reserve(audit_entry_count);
        bool pending_audit_tail = false;
        for (std::size_t i = 0; i < audit_entry_count; ++i) {
            const auto* entry = audit_data + superblock_size + i * audit_entry_size;
            const auto commit = load_u32_acquire(entry + audit_commit_offset);
            if (commit == 0) {
                bool later_commit = false;
                for (std::size_t later = i + 1; later < audit_entry_count; ++later) {
                    const auto* candidate = audit_data + superblock_size + later * audit_entry_size;
                    if (load_u32_acquire(candidate + audit_commit_offset) != 0) {
                        later_commit = true;
                        break;
                    }
                }
                if (later_commit) {
                    snapshot.diagnostic = {ErrorCode::corrupt_store, 0,
                        superblock_size + i * audit_entry_size};
                    break;
                }
                for (std::size_t byte = 0; byte < audit_commit_offset; ++byte) {
                    if (entry[byte] != 0) { pending_audit_tail = true; break; }
                }
                break;
            }
            const auto entry_offset = superblock_size + i * audit_entry_size;
            if (commit != audit_commit_marker || get_u32(entry) != audit_entry_size ||
                get_u32(entry + 24) != audit_entry_crc(entry)) {
                snapshot.diagnostic = {ErrorCode::corrupt_store, 0, entry_offset};
                break;
            }
            const auto previous_epoch = get_u32(entry + 4);
            const auto next_epoch = get_u32(entry + 8);
            const auto reason_size = get_u32(entry + 12);
            const auto id = get_u64(entry + 16);
            if (previous_epoch == 0 || next_epoch != previous_epoch + 1U || reason_size == 0 ||
                reason_size > 256 || id == 0 || std::memchr(entry + 64, 0, reason_size) != nullptr ||
                (!audit_records.empty() &&
                 (audit_records.back().next_epoch != previous_epoch || audit_records.back().id >= id)) ||
                (audit_records.empty() && previous_epoch != journal_initial_epoch)) {
                snapshot.diagnostic = {ErrorCode::corrupt_store, 0, entry_offset};
                break;
            }
            audit_records.push_back(ResetSummary{
                previous_epoch, next_epoch, id,
                std::string(reinterpret_cast<const char*>(entry + 64), reason_size)});
        }
        if (snapshot.diagnostic.ok()) {
            if (pending_audit_tail) snapshot.checkpoint_stale = true;
            snapshot.audit_records = audit_records.size();
            snapshot.used_bytes += audit_file_size;
            const std::uint32_t audit_epoch = audit_records.empty()
                ? journal_initial_epoch : audit_records.back().next_epoch;
            if (audit_epoch > active_epoch || audit_records.size() > active_epoch - journal_initial_epoch) {
                snapshot.diagnostic = {ErrorCode::corrupt_store, 0, active_epoch};
            }
            for (const auto& audit : audit_records) {
                if (!snapshot.diagnostic.ok()) break;
                const auto event = std::find_if(reset_events.begin(), reset_events.end(),
                    [&audit](const auto& item) { return item.id == audit.id; });
                if (event != reset_events.end()) {
                    if (event->previous_epoch != audit.previous_epoch ||
                        event->next_epoch != audit.next_epoch || event->reason != audit.reason)
                        snapshot.diagnostic = {ErrorCode::corrupt_store, 0,
                            static_cast<std::size_t>(audit.id)};
                } else if (audit.id >= snapshot.lowest_retained_record_id) {
                    snapshot.diagnostic = {ErrorCode::corrupt_store, 0,
                        static_cast<std::size_t>(audit.id)};
                }
            }
            // An uncommitted audit tail is repairable only while every missing
            // epoch event is still present in the committed journal.
            if (snapshot.diagnostic.ok() && audit_epoch < active_epoch) {
                std::uint32_t expected = audit_epoch;
                for (const auto& event : reset_events) {
                    if (event.previous_epoch < expected) continue;
                    if (event.previous_epoch != expected || event.next_epoch != expected + 1U) {
                        snapshot.diagnostic = {ErrorCode::corrupt_store, 0,
                            static_cast<std::size_t>(event.id)};
                        break;
                    }
                    ++expected;
                    snapshot.checkpoint_stale = true;
                }
                if (snapshot.diagnostic.ok() && expected != active_epoch)
                    snapshot.diagnostic = {ErrorCode::corrupt_store, 0, active_epoch};
                if (pending_audit_tail) snapshot.checkpoint_stale = true;
            } else if (snapshot.diagnostic.ok() && audit_epoch != active_epoch) {
                snapshot.diagnostic = {ErrorCode::corrupt_store, 0, active_epoch};
            }
        }
        (void)::munmap(audit_mapping, audit_file_size);
        if (!snapshot.diagnostic.ok()) return snapshot;
    } else if (snapshot.epoch_reset_records != 0 || active_epoch != journal_initial_epoch) {
        snapshot.diagnostic = {ErrorCode::corrupt_store, 0, 0};
        return snapshot;
    }
    return snapshot;
} catch (const std::bad_alloc&) {
    JournalInspection snapshot;
    snapshot.diagnostic = {ErrorCode::capacity_exceeded, 0, 0};
    return snapshot;
} catch (...) {
    JournalInspection snapshot;
    snapshot.diagnostic = {ErrorCode::io_error, 0, 0};
    return snapshot;
}

Result<JournalRecord> Journal::append(RecordKind kind,
                                     std::uint64_t seq,
                                     std::uint64_t receipt,
                                     RecordId dependency,
                                     std::string_view payload,
                                     std::uint64_t next_inbound) noexcept {
    if (kind == RecordKind::epoch_reset && !epoch_reset_armed_)
        return Status{ErrorCode::invalid_config, 0, 0};
    if (format_version_ >= 2)
        return append_segmented(kind, seq, receipt, dependency, payload, next_inbound);
    if (fd_ < 0 || mapping_ == nullptr)
        return Status{ErrorCode::invalid_config, 0, write_offset_};
    if (records_.size() >= max_records_)
        return Status{ErrorCode::storage_full, 0, write_offset_};
    if (seq == 0 || payload.size() > std::numeric_limits<std::uint32_t>::max())
        return Status{ErrorCode::invalid_value, 0, write_offset_};

    std::uint64_t persisted_next = 0;
    if (kind == RecordKind::inbound) {
        if (seq != next_inbound_seq_)
            return Status{ErrorCode::sequence_gap, 0, write_offset_};
        if (seq == std::numeric_limits<std::uint64_t>::max())
            return Status{ErrorCode::invalid_value, 0, write_offset_};
        persisted_next = next_inbound == 0 ? seq + 1 : next_inbound;
        if (persisted_next <= seq)
            return Status{ErrorCode::invalid_value, 0, write_offset_};
        if (receipt != 0 || dependency != 0 || payload.empty())
            return Status{ErrorCode::invalid_config, 0, write_offset_};
    } else if (kind == RecordKind::outbound) {
        if (seq != next_outbound_seq_)
            return Status{ErrorCode::sequence_gap, 0, write_offset_};
        if (seq == std::numeric_limits<std::uint64_t>::max())
            return Status{ErrorCode::invalid_value, 0, write_offset_};
        if (next_inbound != 0 || payload.empty())
            return Status{ErrorCode::invalid_config, 0, write_offset_};
        if (dependency != 0) {
            const auto* inbound_record = find_record(dependency);
            if (inbound_record == nullptr || inbound_record->kind != RecordKind::inbound)
                return Status{ErrorCode::missing_field, 0, write_offset_};
        }
    } else if (kind == RecordKind::delivery) {
        if (next_inbound != 0 || receipt != 0 || !payload.empty())
            return Status{ErrorCode::invalid_config, 0, write_offset_};
        const auto* inbound_record = find_record(dependency);
        if (inbound_record == nullptr || inbound_record->kind != RecordKind::inbound ||
            inbound_record->seq != seq || has_delivery(dependency))
            return Status{ErrorCode::invalid_config, 0, write_offset_};
    } else {
        return Status{ErrorCode::invalid_config, 0, write_offset_};
    }

    if (payload.size() > std::numeric_limits<std::size_t>::max() - record_header_size - 7U)
        return Status{ErrorCode::too_large, 0, write_offset_};
    const std::size_t total_size = align_record(record_header_size + payload.size());
    if (total_size > std::numeric_limits<std::uint32_t>::max())
        return Status{ErrorCode::too_large, 0, write_offset_};
    if (total_size > capacity_bytes_ - write_offset_)
        return Status{ErrorCode::storage_full, 0, write_offset_};

    auto* bytes = static_cast<unsigned char*>(mapping_);
    auto* record = bytes + write_offset_;
    std::memset(record, 0, total_size);
    put_u32(record, static_cast<std::uint32_t>(total_size));
    put_u32(record + 4, static_cast<std::uint32_t>(payload.size()));
    put_u32(record + 8, static_cast<std::uint32_t>(kind));
    put_u32(record + 12, current_epoch_);
    put_u64(record + 16, seq);
    put_u64(record + 24, receipt);
    put_u64(record + 32, dependency);
    put_u64(record + 40, persisted_next);
    auto* marker_word = reinterpret_cast<std::uint32_t*>(record + record_commit_offset);
    std::atomic_ref<std::uint32_t> marker(*marker_word);
    marker.store(to_little_endian(0), std::memory_order_relaxed);
    if (test_hook_.callback != nullptr)
        test_hook_.callback(JournalFaultPoint::header, test_hook_.context);

    if (!payload.empty())
        std::memcpy(record + record_header_size, payload.data(), payload.size());
    if (test_hook_.callback != nullptr)
        test_hook_.callback(JournalFaultPoint::payload, test_hook_.context);

    put_u32(record + record_crc_offset, record_crc(record, total_size));
    if (test_hook_.callback != nullptr)
        test_hook_.callback(JournalFaultPoint::precommit, test_hook_.context);

    marker.store(to_little_endian(record_commit_marker), std::memory_order_release);
    if (test_hook_.callback != nullptr)
        test_hook_.callback(JournalFaultPoint::postcommit, test_hook_.context);

    JournalRecord appended{static_cast<RecordId>(write_offset_), kind, seq, receipt,
                           dependency, persisted_next,
                           std::string_view(reinterpret_cast<const char*>(record + record_header_size),
                                            payload.size()),
                           kind == RecordKind::inbound && persisted_next > seq + 1};
    appended.epoch = current_epoch_;
    const std::size_t record_index = records_.size();
    records_.push_back(appended);
    if (kind == RecordKind::outbound) outbound_index_.push_back(record_index);
    if (kind == RecordKind::delivery) {
        auto inbound = std::lower_bound(records_.begin(), records_.end(), dependency,
            [](const JournalRecord& item, RecordId id) { return item.id < id; });
        if (inbound != records_.end() && inbound->id == dependency)
            inbound->delivered = true;
    }
    if (kind == RecordKind::inbound || kind == RecordKind::delivery)
        advance_completed_inbound_cursor();
    write_offset_ += total_size;
    if (kind == RecordKind::inbound) next_inbound_seq_ = persisted_next;
    if (kind == RecordKind::outbound) next_outbound_seq_ = seq + 1;
    published_record_count_.store(records_.size(), std::memory_order_release);
    return appended;
}

Result<JournalRecord> Journal::append_segmented(RecordKind kind,
                                                std::uint64_t seq,
                                                std::uint64_t receipt,
                                                RecordId dependency,
                                                std::string_view payload,
                                                std::uint64_t next_inbound) noexcept try {
    if (fd_ < 0 || segments_.empty()) return Status{ErrorCode::invalid_config, 0, 0};
    if (records_.size() >= max_records_) return Status{ErrorCode::storage_full, 0, used_total_bytes_};
    const auto published_count = published_record_count_.load(std::memory_order_relaxed);
    if (reader_registry_) {
        for (std::size_t i = 0; i < reader_registry_->capacity; ++i) {
            auto& slot = reader_registry_->slots[i];
            if (!slot.active.load(std::memory_order_acquire)) continue;
            const auto position = slot.position.load(std::memory_order_acquire);
            if (published_count >= position && published_count - position >= max_reader_lag_records_)
                return Status{ErrorCode::queue_full, 0, used_total_bytes_};
        }
    }
    if (seq == 0 || payload.size() > std::numeric_limits<std::uint32_t>::max())
        return Status{ErrorCode::invalid_value, 0, used_total_bytes_};

    std::uint64_t persisted_next = 0;
    if (kind == RecordKind::inbound) {
        if (seq != next_inbound_seq_) return Status{ErrorCode::sequence_gap, 0, used_total_bytes_};
        if (seq == std::numeric_limits<std::uint64_t>::max()) return Status{ErrorCode::invalid_value, 0, used_total_bytes_};
        persisted_next = next_inbound == 0 ? seq + 1 : next_inbound;
        if (persisted_next <= seq || receipt != 0 || dependency != 0 || payload.empty())
            return Status{ErrorCode::invalid_config, 0, used_total_bytes_};
    } else if (kind == RecordKind::outbound) {
        if (seq != next_outbound_seq_) return Status{ErrorCode::sequence_gap, 0, used_total_bytes_};
        if (seq == std::numeric_limits<std::uint64_t>::max() || next_inbound != 0 || payload.empty())
            return Status{ErrorCode::invalid_config, 0, used_total_bytes_};
        if (dependency != 0) {
            const auto* inbound = find_record(dependency);
            if (inbound == nullptr || inbound->kind != RecordKind::inbound)
                return Status{ErrorCode::missing_field, 0, used_total_bytes_};
        }
    } else if (kind == RecordKind::delivery) {
        if (next_inbound != 0 || receipt != 0 || !payload.empty())
            return Status{ErrorCode::invalid_config, 0, used_total_bytes_};
        const auto* inbound = find_record(dependency);
        if (inbound == nullptr) return Status{ErrorCode::history_unavailable, 0, used_total_bytes_};
        if (inbound->kind != RecordKind::inbound || inbound->seq != seq || has_delivery(dependency))
            return Status{ErrorCode::invalid_config, 0, used_total_bytes_};
    } else if (kind == RecordKind::epoch_reset) {
        if (!epoch_reset_armed_ || current_epoch_ == std::numeric_limits<std::uint32_t>::max() ||
            seq != current_epoch_ || next_inbound != 0 || receipt != 0 || dependency != 0 ||
            payload.empty() || payload.size() > 256 || payload.find('\0') != std::string_view::npos)
            return Status{ErrorCode::invalid_config, 0, used_total_bytes_};
        persisted_next = static_cast<std::uint64_t>(current_epoch_) + 1U;
    } else {
        return Status{ErrorCode::invalid_config, 0, used_total_bytes_};
    }

    if (payload.size() > std::numeric_limits<std::size_t>::max() - record_header_size - 7U)
        return Status{ErrorCode::too_large, 0, used_total_bytes_};
    const std::size_t total_size = align_record(record_header_size + payload.size());
    if (total_size > std::numeric_limits<std::uint32_t>::max() ||
        total_size > segment_capacity_bytes_ - superblock_size)
        return Status{ErrorCode::storage_full, 0, used_total_bytes_};

    bool rotated = false;
    auto* segment = segments_[active_segment_].get();
    if (total_size > segment->mapping_size - segment->write_offset) {
        const std::size_t next_slot = active_segment_ + 1;
        if (next_slot < segments_.size()) {
            active_segment_ = next_slot;
            segment = segments_[active_segment_].get();
        } else {
            if (segments_.size() >= max_segments_ || next_segment_index_ > maximum_segment_index)
                return Status{ErrorCode::storage_full, 0, used_total_bytes_};
            const auto index = next_segment_index_;
            const auto path = segment_path(base_path_, index);
            const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
            if (fd < 0) return io_status(errno, used_total_bytes_);
            auto cleanup = [&] { (void)::close(fd); (void)::unlink(path.c_str()); };
            if (::ftruncate(fd, static_cast<off_t>(segment_capacity_bytes_)) != 0) {
                const int saved = errno; cleanup(); return io_status(saved, used_total_bytes_);
            }
            const int allocation_error = ::posix_fallocate(fd, 0, static_cast<off_t>(segment_capacity_bytes_));
            if (allocation_error != 0) { cleanup(); return io_status(allocation_error, used_total_bytes_); }
            int map_flags = MAP_SHARED;
#if defined(MAP_POPULATE)
            map_flags |= MAP_POPULATE;
#endif
            void* mapping = ::mmap(nullptr, segment_capacity_bytes_, PROT_READ | PROT_WRITE,
                                   map_flags, fd, 0);
            if (mapping == MAP_FAILED) { const int saved = errno; cleanup(); return io_status(saved, used_total_bytes_); }
            auto created = std::make_shared<Segment>();
            created->fd = fd;
            created->mapping = mapping;
            created->mapping_size = segment_capacity_bytes_;
            created->index = index;
            created->path = path;
            write_v2_header(static_cast<unsigned char*>(mapping), v2_segment_magic,
                segment_capacity_bytes_, session_key_, segment_capacity_bytes_, capacity_bytes_,
                max_segments_, index, audit_bytes_);
            touch_mapping(mapping, segment_capacity_bytes_);
            (void)::madvise(mapping, segment_capacity_bytes_, MADV_WILLNEED);
            if (test_hook_.callback != nullptr)
                test_hook_.callback(JournalFaultPoint::segment_created, test_hook_.context);
            segments_.push_back(std::move(created));
            used_total_bytes_ += superblock_size;
            ++next_segment_index_;
            active_segment_ = segments_.size() - 1;
            segment = segments_[active_segment_].get();
        }
        rotated = true;
        if (total_size > segment->mapping_size - segment->write_offset)
            return Status{ErrorCode::storage_full, 0, used_total_bytes_};
    }

    auto* record = static_cast<unsigned char*>(segment->mapping) + segment->write_offset;
    std::memset(record, 0, total_size);
    put_u32(record, static_cast<std::uint32_t>(total_size));
    put_u32(record + 4, static_cast<std::uint32_t>(payload.size()));
    put_u32(record + 8, static_cast<std::uint32_t>(kind));
    put_u32(record + 12, current_epoch_);
    put_u64(record + 16, seq);
    put_u64(record + 24, receipt);
    put_u64(record + 32, dependency);
    put_u64(record + 40, persisted_next);
    auto* marker_word = reinterpret_cast<std::uint32_t*>(record + record_commit_offset);
    std::atomic_ref<std::uint32_t> marker(*marker_word);
    marker.store(to_little_endian(0), std::memory_order_relaxed);
    if (test_hook_.callback != nullptr) test_hook_.callback(JournalFaultPoint::header, test_hook_.context);
    if (!payload.empty()) std::memcpy(record + record_header_size, payload.data(), payload.size());
    if (test_hook_.callback != nullptr) test_hook_.callback(JournalFaultPoint::payload, test_hook_.context);
    put_u32(record + record_crc_offset, record_crc(record, total_size));
    if (test_hook_.callback != nullptr) test_hook_.callback(JournalFaultPoint::precommit, test_hook_.context);
    marker.store(to_little_endian(record_commit_marker), std::memory_order_release);
    if (test_hook_.callback != nullptr) test_hook_.callback(JournalFaultPoint::postcommit, test_hook_.context);

    const auto id = segment_record_id(segment->index, segment->write_offset);
    JournalRecord appended{id, kind, seq, receipt, dependency, persisted_next,
        std::string_view(reinterpret_cast<const char*>(record + record_header_size), payload.size()),
        kind == RecordKind::inbound && persisted_next > seq + 1};
    appended.epoch = current_epoch_;
    const std::size_t record_index = records_.size();
    records_.push_back(appended);
    if (kind == RecordKind::outbound) outbound_index_.push_back(record_index);
    if (kind == RecordKind::delivery) {
        auto inbound = std::lower_bound(records_.begin(), records_.end(), dependency,
            [](const JournalRecord& item, RecordId value) { return item.id < value; });
        if (inbound != records_.end() && inbound->id == dependency) inbound->delivered = true;
    }
    published_record_count_.store(records_.size(), std::memory_order_release);
    segment->write_offset += total_size;
    used_total_bytes_ += total_size;
    if (kind == RecordKind::inbound) next_inbound_seq_ = persisted_next;
    if (kind == RecordKind::outbound) next_outbound_seq_ = seq + 1;
    if (kind == RecordKind::epoch_reset) {
        current_epoch_ = static_cast<std::uint32_t>(persisted_next);
        next_inbound_seq_ = 1;
        next_outbound_seq_ = 1;
        completed_inbound_seq_ = 0;
        completed_scan_index_ = records_.size();
        lowest_retained_outbound_seq_ = 1;
        outbound_index_.clear();
    }
    if (kind == RecordKind::inbound || kind == RecordKind::delivery)
        advance_completed_inbound_cursor();

    checkpoint_.epoch = current_epoch_;
    checkpoint_.next_inbound_seq = next_inbound_seq_;
    checkpoint_.next_outbound_seq = next_outbound_seq_;
    checkpoint_.completed_inbound_seq = completed_inbound_seq_;
    checkpoint_.delivery_cursor = completed_inbound_record_id_;
    checkpoint_.last_record_id = id;
    checkpoint_.first_segment_index = first_segment_index_;
    checkpoint_.lowest_retained_outbound_seq = lowest_retained_outbound_seq_;
    checkpoint_.lowest_retained_record_id = lowest_retained_record_id_;
    if (rotated) {
        ++checkpoint_.generation;
        if (!write_checkpoint_file(checkpoint_).ok()) checkpoint_stale_ = true;
        else checkpoint_stale_ = false;
    } else {
        checkpoint_stale_ = true;
    }
    return appended;
} catch (const std::bad_alloc&) {
    return Status{ErrorCode::capacity_exceeded, 0, 0};
} catch (...) {
    return Status{ErrorCode::io_error, 0, 0};
}

Status Journal::write_checkpoint_file(const JournalCheckpoint& checkpoint) noexcept {
    if (format_version_ < 2 || base_path_.empty() || session_key_.empty())
        return {ErrorCode::unsupported, 0, 0};
    if (outbound_index_.size() > std::numeric_limits<std::uint32_t>::max())
        return {ErrorCode::too_large, 0, 0};
    std::vector<unsigned char> bytes;
    try {
        bytes.resize(checkpoint_fixed_size + session_key_.size() + outbound_index_.size() * 16U);
    } catch (...) {
        return {ErrorCode::capacity_exceeded, 0, 0};
    }
    std::memcpy(bytes.data(), checkpoint_magic.data(), checkpoint_magic.size());
    put_u32(bytes.data() + 8, 2);
    put_u64(bytes.data() + 16, checkpoint.generation);
    put_u64(bytes.data() + 24, checkpoint.next_inbound_seq);
    put_u64(bytes.data() + 32, checkpoint.next_outbound_seq);
    put_u64(bytes.data() + 40, checkpoint.delivery_cursor);
    put_u64(bytes.data() + 48, checkpoint.replay_cursor);
    put_u64(bytes.data() + 56, checkpoint.last_record_id);
    put_u32(bytes.data() + 64, checkpoint.first_segment_index);
    put_u64(bytes.data() + 68, checkpoint.lowest_retained_outbound_seq);
    put_u64(bytes.data() + 76, checkpoint.lowest_retained_record_id);
    put_u32(bytes.data() + 84, static_cast<std::uint32_t>(outbound_index_.size()));
    put_u32(bytes.data() + 88, static_cast<std::uint32_t>(session_key_.size()));
    put_u32(bytes.data() + 92, checkpoint.epoch);
    put_u64(bytes.data() + 96, checkpoint.completed_inbound_seq);
    std::memcpy(bytes.data() + checkpoint_fixed_size, session_key_.data(), session_key_.size());
    std::size_t offset = checkpoint_fixed_size + session_key_.size();
    for (const auto index : outbound_index_) {
        put_u64(bytes.data() + offset, records_[index].seq);
        put_u64(bytes.data() + offset + 8, records_[index].id);
        offset += 16;
    }
    put_u32(bytes.data() + checkpoint_crc_offset,
        crc32c(bytes.data(), bytes.size(), checkpoint_crc_offset, sizeof(std::uint32_t)));

    static std::atomic<std::uint64_t> temporary_counter{1};
    std::string target;
    std::string temporary;
    try {
        target = base_path_ + ".ckp";
        temporary = target + ".tmp." + std::to_string(::getpid()) + "." +
            std::to_string(temporary_counter.fetch_add(1, std::memory_order_relaxed));
    } catch (...) {
        return {ErrorCode::capacity_exceeded, 0, 0};
    }
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return io_status(errno);
    const auto write_status = write_all(fd, bytes.data(), bytes.size());
    const int close_result = ::close(fd);
    if (!write_status.ok() || close_result != 0) {
        const auto status = write_status.ok() ? io_status(errno) : write_status;
        (void)::unlink(temporary.c_str());
        return status;
    }
    if (::rename(temporary.c_str(), target.c_str()) != 0) {
        const int saved = errno;
        (void)::unlink(temporary.c_str());
        return io_status(saved);
    }
    if (test_hook_.callback != nullptr)
        test_hook_.callback(JournalFaultPoint::checkpoint_published, test_hook_.context);
    return Status::success();
}

Status Journal::append_epoch_audit(std::uint32_t previous_epoch,
                                   std::uint32_t next_epoch,
                                   RecordId record_id,
                                   std::string_view reason) noexcept {
    if (audit_mapping_ == nullptr || audit_count_ >= audit_entry_count)
        return {ErrorCode::storage_full, 0, audit_count_};
    if (previous_epoch == 0 || next_epoch != previous_epoch + 1U || record_id == 0 ||
        reason.empty() || reason.size() > 256 || reason.find('\0') != std::string_view::npos)
        return {ErrorCode::invalid_value, 0, 0};
    auto* entry = static_cast<unsigned char*>(audit_mapping_) + superblock_size + audit_count_ * audit_entry_size;
    std::memset(entry, 0, audit_entry_size);
    put_u32(entry, static_cast<std::uint32_t>(audit_entry_size));
    put_u32(entry + 4, previous_epoch);
    put_u32(entry + 8, next_epoch);
    put_u32(entry + 12, static_cast<std::uint32_t>(reason.size()));
    put_u64(entry + 16, record_id);
    std::memcpy(entry + 64, reason.data(), reason.size());
    put_u32(entry + 24, audit_entry_crc(entry));
    auto* marker_word = reinterpret_cast<std::uint32_t*>(entry + audit_commit_offset);
    std::atomic_ref<std::uint32_t> marker(*marker_word);
    marker.store(to_little_endian(0), std::memory_order_relaxed);
    marker.store(to_little_endian(audit_commit_marker), std::memory_order_release);
    epoch_audit_records_.push_back(EpochAuditRecord{previous_epoch, next_epoch, record_id,
        std::string_view(reinterpret_cast<const char*>(entry + 64), reason.size())});
    ++audit_count_;
    if (test_hook_.callback != nullptr)
        test_hook_.callback(JournalFaultPoint::audit_published, test_hook_.context);
    return Status::success();
}

JournalCheckpoint Journal::checkpoint() const noexcept {
    auto result = checkpoint_;
    result.epoch = current_epoch_;
    result.next_inbound_seq = next_inbound_seq_;
    result.next_outbound_seq = next_outbound_seq_;
    result.completed_inbound_seq = completed_inbound_seq_;
    result.delivery_cursor = completed_inbound_record_id_;
    result.last_record_id = records_.empty() ? 0 : records_.back().id;
    result.first_segment_index = first_segment_index_;
    result.lowest_retained_outbound_seq = lowest_retained_outbound_seq_;
    result.lowest_retained_record_id = lowest_retained_record_id_;
    return result;
}

Status Journal::publish_checkpoint(RecordId delivery_cursor,
                                   std::uint64_t replay_cursor) noexcept {
    if (format_version_ < 2) return {ErrorCode::unsupported, 0, 0};
    if (replay_cursor < lowest_retained_outbound_seq_ || replay_cursor > next_outbound_seq_ ||
        replay_cursor < checkpoint_.replay_cursor || delivery_cursor < checkpoint_.delivery_cursor)
        return {ErrorCode::invalid_value, 0, 0};
    const bool persisted_pruned_cursor = delivery_cursor != 0 &&
        delivery_cursor == checkpoint_.delivery_cursor &&
        delivery_cursor < lowest_retained_record_id_;
    if (delivery_cursor != 0 && !persisted_pruned_cursor) {
        const auto* record = find_record(delivery_cursor);
        if (record == nullptr || record->kind != RecordKind::inbound || !record->delivered)
            return {ErrorCode::invalid_value, 0, static_cast<std::size_t>(delivery_cursor)};
        for (const auto& item : records_) {
            if (item.kind == RecordKind::inbound && item.id <= delivery_cursor && !item.delivered)
                return {ErrorCode::invalid_value, 0, static_cast<std::size_t>(item.id)};
        }
    }
    auto next = checkpoint();
    next.delivery_cursor = delivery_cursor;
    next.replay_cursor = replay_cursor;
    ++next.generation;
    const auto status = write_checkpoint_file(next);
    if (!status.ok()) {
        checkpoint_stale_ = true;
        return status;
    }
    checkpoint_ = next;
    checkpoint_stale_ = false;
    return Status::success();
}

Status Journal::begin_epoch(std::string_view reason) noexcept {
    if (format_version_ < 2) return {ErrorCode::unsupported, 0, 0};
    if (audit_mapping_ == nullptr || audit_bytes_ != audit_file_size)
        return {ErrorCode::storage_full, 0, 0};
    if (reason.empty() || reason.size() > 256 || reason.find('\0') != std::string_view::npos ||
        current_epoch_ == std::numeric_limits<std::uint32_t>::max())
        return {ErrorCode::invalid_value, 0, 0};
    if (audit_count_ >= audit_entry_count) return {ErrorCode::storage_full, 0, audit_count_};
    if ((reader_registry_ && reader_registry_->active_count.load(std::memory_order_acquire) != 0) ||
        std::any_of(segments_.begin(), segments_.end(), [](const auto& segment) {
            return segment.use_count() != 1;
        }))
        return {ErrorCode::sequence_reset_denied, 0, 0};
    RecordId delivery_cursor = checkpoint_.delivery_cursor;
    for (const auto& record : records_) {
        if (record.kind != RecordKind::inbound) continue;
        if (!record.delivered) return {ErrorCode::sequence_reset_denied, 0, static_cast<std::size_t>(record.id)};
        delivery_cursor = record.id;
    }
    epoch_reset_armed_ = true;
    const auto appended = append_segmented(RecordKind::epoch_reset, current_epoch_, 0, 0, reason, 0);
    epoch_reset_armed_ = false;
    if (!appended.ok()) return appended.status();
    const auto audit_status = append_epoch_audit(appended.value().epoch,
        appended.value().epoch + 1U, appended.value().id, reason);
    if (!audit_status.ok()) return audit_status;

    auto next = checkpoint();
    next.epoch = current_epoch_;
    next.delivery_cursor = delivery_cursor;
    next.replay_cursor = 1;
    ++next.generation;
    if (write_checkpoint_file(next).ok()) {
        checkpoint_ = next;
        checkpoint_stale_ = false;
    } else {
        checkpoint_ = next;
        checkpoint_stale_ = true;
    }
    return Status::success();
}

Status Journal::set_retention_floor(std::uint64_t lowest_outbound_seq,
                                    RecordId delivery_cursor) noexcept try {
    if (format_version_ < 2) return {ErrorCode::unsupported, 0, 0};
    if (lowest_outbound_seq < lowest_retained_outbound_seq_ ||
        lowest_outbound_seq > next_outbound_seq_ ||
        lowest_outbound_seq < checkpoint_.replay_cursor ||
        delivery_cursor < checkpoint_.delivery_cursor)
        return {ErrorCode::invalid_value, 0, 0};
    const bool persisted_pruned_cursor = delivery_cursor != 0 &&
        delivery_cursor == checkpoint_.delivery_cursor &&
        delivery_cursor < lowest_retained_record_id_;
    if (delivery_cursor != 0 && !persisted_pruned_cursor) {
        const auto* record = find_record(delivery_cursor);
        if (record == nullptr || record->kind != RecordKind::inbound || !record->delivered)
            return {ErrorCode::invalid_value, 0, static_cast<std::size_t>(delivery_cursor)};
        for (const auto& item : records_) {
            if (item.kind == RecordKind::inbound && item.id <= delivery_cursor && !item.delivered)
                return {ErrorCode::invalid_value, 0, static_cast<std::size_t>(item.id)};
        }
    }

    RecordId latest_current_inbound = 0;
    RecordId latest_current_outbound = 0;
    for (const auto& record : records_) {
        if (record.epoch != current_epoch_) continue;
        if (record.kind == RecordKind::inbound) latest_current_inbound = record.id;
        else if (record.kind == RecordKind::outbound) latest_current_outbound = record.id;
    }
    std::size_t safe_segments = 0;
    for (std::size_t slot = 0; slot < active_segment_ && slot < segments_.size(); ++slot) {
        const auto& segment = segments_[slot];
        if (segment.use_count() != 1) break;
        bool safe = true;
        for (const auto& record : records_) {
            if (segment_index_from_id(record.id) != segment->index) continue;
            // Preserve one committed sequence anchor per direction. If the
            // checkpoint sidecar is damaged after retention, the next FIX
            // sequence can still be rebuilt from primary journal records.
            if (record.id == latest_current_inbound || record.id == latest_current_outbound) {
                safe = false;
                break;
            }
            if (record.kind == RecordKind::epoch_reset) {
                const auto audited = std::any_of(epoch_audit_records_.begin(), epoch_audit_records_.end(),
                    [&record](const auto& item) { return item.reset_record_id == record.id; });
                if (!audited) { safe = false; break; }
            }
            if (record.kind == RecordKind::outbound && record.seq >= lowest_outbound_seq) {
                if (record.epoch >= current_epoch_) {
                    safe = false;
                    break;
                }
            }
            if (record.kind == RecordKind::inbound &&
                (record.id > delivery_cursor || !record.delivered)) {
                safe = false;
                break;
            }
            if (record.kind == RecordKind::delivery && record.dependency > delivery_cursor) {
                safe = false;
                break;
            }
        }
        if (!safe) break;
        ++safe_segments;
    }
    if (reader_registry_ && reader_registry_->active_count.load(std::memory_order_acquire) != 0)
        safe_segments = 0;

    std::uint32_t next_first_segment = first_segment_index_;
    if (safe_segments != 0) next_first_segment = segments_[safe_segments]->index;
    auto next_checkpoint = checkpoint();
    next_checkpoint.delivery_cursor = delivery_cursor;
    next_checkpoint.replay_cursor = lowest_outbound_seq;
    next_checkpoint.first_segment_index = next_first_segment;
    next_checkpoint.lowest_retained_outbound_seq = lowest_outbound_seq;
    next_checkpoint.last_record_id = (!records_.empty() &&
        segment_index_from_id(records_.back().id) >= next_first_segment)
        ? records_.back().id : 0;
    RecordId next_lowest_record_id = lowest_retained_record_id_;
    if (safe_segments != 0) {
        const auto first_kept = std::find_if(records_.begin(), records_.end(), [next_first_segment](const auto& record) {
            return segment_index_from_id(record.id) >= next_first_segment;
        });
        next_lowest_record_id = first_kept == records_.end()
            ? segment_record_id(next_first_segment, superblock_size) : first_kept->id;
    }
    next_checkpoint.lowest_retained_record_id = next_lowest_record_id;
    ++next_checkpoint.generation;
    const auto checkpoint_status = write_checkpoint_file(next_checkpoint);
    if (!checkpoint_status.ok()) return checkpoint_status;

    checkpoint_ = next_checkpoint;
    checkpoint_stale_ = false;
    lowest_retained_outbound_seq_ = lowest_outbound_seq;
    first_segment_index_ = next_first_segment;
    lowest_retained_record_id_ = next_lowest_record_id;
    if (test_hook_.callback != nullptr)
        test_hook_.callback(JournalFaultPoint::retention_published, test_hook_.context);

    if (safe_segments != 0) {
        for (std::size_t i = 0; i < safe_segments; ++i) {
            const auto& segment = segments_[i];
            used_total_bytes_ -= std::min(used_total_bytes_, segment->write_offset);
            if (::unlink(segment->path.c_str()) != 0 && errno != ENOENT)
                return io_status(errno, used_total_bytes_);
        }
        active_segment_ -= safe_segments;
        segments_.erase(segments_.begin(), segments_.begin() + static_cast<std::ptrdiff_t>(safe_segments));
        records_.erase(std::remove_if(records_.begin(), records_.end(), [next_first_segment](const auto& record) {
            return segment_index_from_id(record.id) < next_first_segment;
        }), records_.end());
        records_data_ = records_.data();
        completed_scan_index_ = 0;
        advance_completed_inbound_cursor();
        outbound_index_.clear();
        for (std::size_t i = 0; i < records_.size(); ++i)
            if (records_[i].kind == RecordKind::outbound && records_[i].epoch == current_epoch_)
                outbound_index_.push_back(i);
        published_record_count_.store(records_.size(), std::memory_order_release);
        checkpoint_.lowest_retained_record_id = lowest_retained_record_id_;
        checkpoint_.last_record_id = records_.empty() ? 0 : records_.back().id;
        if (!write_checkpoint_file(checkpoint_).ok()) checkpoint_stale_ = true;
    }

    // Refill the freed file slots before the active segment reaches its end.
    while (segments_.size() < max_segments_ && next_segment_index_ <= maximum_segment_index) {
        const auto path = segment_path(base_path_, next_segment_index_);
        const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) return io_status(errno, used_total_bytes_);
        if (::ftruncate(fd, static_cast<off_t>(segment_capacity_bytes_)) != 0) {
            const int saved = errno; ::close(fd); (void)::unlink(path.c_str()); return io_status(saved, used_total_bytes_);
        }
        const int allocation_error = ::posix_fallocate(fd, 0, static_cast<off_t>(segment_capacity_bytes_));
        if (allocation_error != 0) { ::close(fd); (void)::unlink(path.c_str()); return io_status(allocation_error, used_total_bytes_); }
        int map_flags = MAP_SHARED;
#if defined(MAP_POPULATE)
        map_flags |= MAP_POPULATE;
#endif
        void* mapping = ::mmap(nullptr, segment_capacity_bytes_, PROT_READ | PROT_WRITE,
                               map_flags, fd, 0);
        if (mapping == MAP_FAILED) { const int saved = errno; ::close(fd); (void)::unlink(path.c_str()); return io_status(saved, used_total_bytes_); }
        try {
            auto segment = std::make_shared<Segment>();
            segment->fd = fd;
            segment->mapping = mapping;
            segment->mapping_size = segment_capacity_bytes_;
            segment->index = next_segment_index_;
            segment->path = path;
            write_v2_header(static_cast<unsigned char*>(mapping), v2_segment_magic,
                segment_capacity_bytes_, session_key_, segment_capacity_bytes_, capacity_bytes_,
                max_segments_, next_segment_index_, audit_bytes_);
            touch_mapping(mapping, segment_capacity_bytes_);
            segments_.push_back(std::move(segment));
            used_total_bytes_ += superblock_size;
        } catch (...) {
            (void)::munmap(mapping, segment_capacity_bytes_);
            (void)::close(fd);
            (void)::unlink(path.c_str());
            return {ErrorCode::capacity_exceeded, 0, used_total_bytes_};
        }
        ++next_segment_index_;
        (void)::madvise(mapping, segment_capacity_bytes_, MADV_WILLNEED);
        if (test_hook_.callback != nullptr)
            test_hook_.callback(JournalFaultPoint::segment_created, test_hook_.context);
    }
    return Status::success();
} catch (const std::bad_alloc&) {
    return {ErrorCode::capacity_exceeded, 0, 0};
} catch (...) {
    return {ErrorCode::io_error, 0, 0};
}

Result<JournalReader> Journal::create_reader(RecordId start_id) noexcept {
    if (!reader_registry_ || reader_registry_->capacity == 0)
        return Status{ErrorCode::invalid_config, 0, 0};
    const auto published = published_record_count_.load(std::memory_order_acquire);
    std::size_t position = published;
    if (start_id != 0) {
        if (format_version_ >= 2 && start_id < lowest_retained_record_id_)
            return Status{ErrorCode::history_unavailable, 0, static_cast<std::size_t>(start_id)};
        std::size_t lower = 0;
        std::size_t upper = published;
        while (lower < upper) {
            const auto middle = lower + (upper - lower) / 2;
            if (records_data_[middle].id < start_id) lower = middle + 1;
            else upper = middle;
        }
        position = lower;
    }
    if (format_version_ >= 2 && published - position > max_reader_lag_records_)
        return Status{ErrorCode::reader_expired, 0, static_cast<std::size_t>(start_id)};
    try {
        auto state = std::make_shared<JournalReader::State>();
        state->registry = reader_registry_;
        state->mapping_leases.reserve(segments_.size());
        for (const auto& segment : segments_) state->mapping_leases.push_back(segment);
        bool acquired = false;
        for (std::size_t i = 0; i < reader_registry_->capacity; ++i) {
            auto& slot = reader_registry_->slots[i];
            bool expected = false;
            if (!slot.claimed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) continue;
            state->slot = i;
            slot.position.store(position, std::memory_order_relaxed);
            slot.state.store(state.get(), std::memory_order_relaxed);
            slot.owner.store(this, std::memory_order_relaxed);
            reader_registry_->active_count.fetch_add(1, std::memory_order_acq_rel);
            slot.active.store(true, std::memory_order_release);
            acquired = true;
            break;
        }
        if (!acquired) return Status{ErrorCode::queue_full, 0, 0};
        state->cursor.store(start_id == 0
            ? (published == 0 ? 0 : records_data_[published - 1].id) : start_id,
            std::memory_order_release);
        return JournalReader{std::move(state)};
    } catch (...) {
        return Status{ErrorCode::capacity_exceeded, 0, 0};
    }
}

Status Journal::reader_next(JournalReader::State& state, JournalRecord& record) noexcept {
    if (state.registry != reader_registry_ || state.slot >= state.registry->capacity)
        return {ErrorCode::reader_expired, 0, 0};
    auto& slot = state.registry->slots[state.slot];
    if (slot.owner.load(std::memory_order_acquire) != this ||
        !slot.active.load(std::memory_order_acquire))
        return {ErrorCode::reader_expired, 0, 0};
    const auto cursor = state.cursor.load(std::memory_order_acquire);
    if (format_version_ >= 2 && cursor != 0 && cursor < lowest_retained_record_id_)
        return {ErrorCode::reader_expired, 0, static_cast<std::size_t>(cursor)};
    const auto position = slot.position.load(std::memory_order_relaxed);
    const auto published = published_record_count_.load(std::memory_order_acquire);
    if (position > published) return {ErrorCode::reader_expired, 0, 0};
    if (format_version_ >= 2 && published - position > max_reader_lag_records_)
        return {ErrorCode::reader_expired, 0, static_cast<std::size_t>(cursor)};
    if (position >= published) return {ErrorCode::need_more, 0, 0};
    const auto& source = records_data_[position];
    record.id = source.id;
    record.kind = source.kind;
    record.seq = source.seq;
    record.receipt = source.receipt;
    record.dependency = source.dependency;
    record.next_seq = source.next_seq;
    record.payload = source.payload;
    record.delivered = false;
    record.epoch = source.epoch;
    slot.position.store(position + 1, std::memory_order_release);
    state.cursor.store(record.id, std::memory_order_release);
    return Status::success();
}

void Journal::advance_completed_inbound_cursor() noexcept {
    if (completed_scan_index_ > records_.size()) completed_scan_index_ = records_.size();
    const auto old_seq = completed_inbound_seq_;
    const auto old_id = completed_inbound_record_id_;
    std::size_t i = completed_scan_index_;
    for (; i < records_.size(); ++i) {
        const auto& record = records_[i];
        if (record.kind != RecordKind::inbound || record.epoch != current_epoch_) {
            completed_scan_index_ = i + 1;
            continue;
        }
        if (record.seq <= completed_inbound_seq_) {
            if (!record.delivered) {
                completed_scan_index_ = i;
                break;
            }
            completed_scan_index_ = i + 1;
            continue;
        }
        if (completed_inbound_seq_ == std::numeric_limits<std::uint64_t>::max() ||
            record.seq != completed_inbound_seq_ + 1 || !record.delivered) {
            completed_scan_index_ = i;
            break;
        }
        completed_inbound_seq_ = record.next_seq > record.seq + 1
            ? record.next_seq - 1 : record.seq;
        completed_inbound_record_id_ = record.id;
        completed_scan_index_ = i + 1;
    }
    if (i == records_.size()) completed_scan_index_ = records_.size();
    if (completed_inbound_seq_ != old_seq || completed_inbound_record_id_ != old_id) {
        checkpoint_.completed_inbound_seq = completed_inbound_seq_;
        checkpoint_.delivery_cursor = completed_inbound_record_id_;
        checkpoint_stale_ = true;
    }
}

const JournalRecord* Journal::find_record(RecordId id) const noexcept {
    const auto found = std::lower_bound(records_.begin(), records_.end(), id,
        [](const JournalRecord& record, RecordId value) { return record.id < value; });
    return found != records_.end() && found->id == id ? &*found : nullptr;
}

Result<JournalLease> Journal::pin_record(RecordId id) const noexcept {
    const auto* record = find_record(id);
    if (record == nullptr) return Status{format_version_ >= 2 && id < lowest_retained_record_id_
        ? ErrorCode::history_unavailable : ErrorCode::missing_field, 0, static_cast<std::size_t>(id)};
    JournalLease lease;
    lease.record_ = *record;
    if (format_version_ >= 2) {
        const auto index = segment_index_from_id(id);
        for (const auto& segment : segments_) {
            if (segment->index == index) {
                lease.mapping_lease_ = segment;
                return lease;
            }
        }
        return Status{ErrorCode::history_unavailable, 0, static_cast<std::size_t>(id)};
    }
    try {
        auto payload_copy = std::make_shared<std::string>(lease.record_.payload);
        lease.mapping_lease_ = payload_copy;
        lease.record_.payload = *payload_copy;
    } catch (...) {
        return Status{ErrorCode::capacity_exceeded, 0, static_cast<std::size_t>(id)};
    }
    return lease;
}

const JournalRecord* Journal::find_outbound(std::uint64_t seq) const noexcept {
    const auto found = std::lower_bound(outbound_index_.begin(), outbound_index_.end(), seq,
        [this](std::size_t index, std::uint64_t value) { return records_[index].seq < value; });
    if (found == outbound_index_.end() || records_[*found].seq != seq) return nullptr;
    return &records_[*found];
}

bool Journal::has_delivery(RecordId inbound_id) const noexcept {
    const auto* inbound = find_record(inbound_id);
    if (inbound == nullptr || inbound->kind != RecordKind::inbound) return false;
    return inbound->delivered;
}

} // namespace nimblefix
