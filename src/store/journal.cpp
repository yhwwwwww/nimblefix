#include "nimblefix/journal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <new>
#include <string>
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
           raw_kind == static_cast<std::uint32_t>(RecordKind::delivery);
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
        !valid_record_kind(raw_kind) || get_u32(record + 12) != current_epoch)
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

} // namespace

Journal::Journal(Journal&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)),
      mapping_(std::exchange(other.mapping_, nullptr)),
      mapping_size_(std::exchange(other.mapping_size_, 0)),
      capacity_bytes_(std::exchange(other.capacity_bytes_, 0)),
      write_offset_(std::exchange(other.write_offset_, 0)),
      next_inbound_seq_(std::exchange(other.next_inbound_seq_, 1)),
      next_outbound_seq_(std::exchange(other.next_outbound_seq_, 1)),
      max_records_(std::exchange(other.max_records_, 0)),
      test_hook_(std::exchange(other.test_hook_, {})),
      records_(std::move(other.records_)),
      outbound_index_(std::move(other.outbound_index_)) {}

Journal& Journal::operator=(Journal&& other) noexcept {
    if (this == &other) return *this;
    reset();
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
    return *this;
}

Journal::~Journal() { reset(); }

void Journal::reset() noexcept {
    records_.clear();
    outbound_index_.clear();
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

    Journal journal;
    std::string path;
    try {
        path.assign(config.path.data(), config.path.size());
        journal.records_.reserve(config.max_records);
        journal.outbound_index_.reserve(config.max_records);
    } catch (...) {
        return Status{ErrorCode::capacity_exceeded, 0, 0};
    }

    try {
        const auto parent = std::filesystem::path(path).parent_path();
        if (!parent.empty()) {
            std::error_code error;
            (void)std::filesystem::create_directories(parent, error);
            if (error) return Status{ErrorCode::io_error, 0, 0};
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
    if (!S_ISREG(file_stat.st_mode)) return fail(Status{ErrorCode::invalid_config, 0, 0});
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
            !valid_record_kind(raw_kind) || epoch != current_epoch)
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
    return journal;
}

Result<JournalRecord> Journal::append(RecordKind kind,
                                     std::uint64_t seq,
                                     std::uint64_t receipt,
                                     RecordId dependency,
                                     std::string_view payload,
                                     std::uint64_t next_inbound) noexcept {
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
    put_u32(record + 12, current_epoch);
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
    const std::size_t record_index = records_.size();
    records_.push_back(appended);
    if (kind == RecordKind::outbound) outbound_index_.push_back(record_index);
    if (kind == RecordKind::delivery) {
        auto inbound = std::lower_bound(records_.begin(), records_.end(), dependency,
            [](const JournalRecord& item, RecordId id) { return item.id < id; });
        if (inbound != records_.end() && inbound->id == dependency)
            inbound->delivered = true;
    }
    write_offset_ += total_size;
    if (kind == RecordKind::inbound) next_inbound_seq_ = persisted_next;
    if (kind == RecordKind::outbound) next_outbound_seq_ = seq + 1;
    return appended;
}

const JournalRecord* Journal::find_record(RecordId id) const noexcept {
    const auto found = std::lower_bound(records_.begin(), records_.end(), id,
        [](const JournalRecord& record, RecordId value) { return record.id < value; });
    return found != records_.end() && found->id == id ? &*found : nullptr;
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
