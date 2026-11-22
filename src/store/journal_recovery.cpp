#include "journal_internal.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <limits>
#include <new>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace nimblefix {

using namespace store;

namespace {

[[nodiscard]] Status create_manifest_atomic(const std::string& base,
                                            std::string_view session_key,
                                            std::size_t segment_bytes,
                                            std::size_t capacity_bytes,
                                            std::size_t max_segments,
                                            std::size_t audit_budget,
                                            int& out_fd,
                                            bool& out_created) noexcept {
    out_fd = -1;
    out_created = false;

    // Fast path: try opening existing manifest first.
    int fd = ::open(base.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd >= 0) {
        if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
            const int saved = errno;
            ::close(fd);
            return (saved == EWOULDBLOCK || saved == EAGAIN)
                ? Status{ErrorCode::store_locked, 0, 0} : io_status(saved);
        }
        out_fd = fd;
        out_created = false;
        return Status::success();
    }
    if (errno != ENOENT) return io_status(errno);

    // Create initialized manifest atomically via temp file + hard link so SIGKILL
    // during creation never leaves a zero-header manifest at `base`.
    const auto temp = make_temp_path(base);
    fd = ::open(temp.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return io_status(errno);
    auto cleanup = [&]() noexcept { (void)::close(fd); (void)::unlink(temp.c_str()); };

    if (::ftruncate(fd, static_cast<off_t>(superblock_size)) != 0) {
        const int saved = errno; cleanup(); return io_status(saved);
    }
    if (const int err = ::posix_fallocate(fd, 0, static_cast<off_t>(superblock_size)); err != 0) {
        cleanup(); return io_status(err);
    }
    std::array<unsigned char, superblock_size> header{};
    write_v2_header(header.data(), v2_manifest_magic, superblock_size, session_key,
                    segment_bytes, capacity_bytes, max_segments, 0, audit_budget);
    if (::pwrite(fd, header.data(), header.size(), 0) != static_cast<ssize_t>(header.size())) {
        const int saved = errno == 0 ? EIO : errno;
        cleanup();
        return io_status(saved);
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int saved = errno; cleanup(); return io_status(saved);
    }
    if (::link(temp.c_str(), base.c_str()) != 0) {
        const int saved = errno;
        cleanup();
        if (saved == EEXIST) {
            // Concurrent creator won; reopen existing manifest.
            int existing = ::open(base.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
            if (existing < 0) return io_status(errno);
            if (::flock(existing, LOCK_EX | LOCK_NB) != 0) {
                const int lock_err = errno;
                ::close(existing);
                return (lock_err == EWOULDBLOCK || lock_err == EAGAIN)
                    ? Status{ErrorCode::store_locked, 0, 0} : io_status(lock_err);
            }
            out_fd = existing;
            out_created = false;
            return Status::success();
        }
        return io_status(saved);
    }
    (void)::unlink(temp.c_str());
    out_fd = fd;
    out_created = true;
    return Status::success();
}

[[nodiscard]] Status open_or_create_audit(const std::string& base,
                                          std::string_view session_key,
                                          bool manifest_created,
                                          int& out_fd,
                                          void*& out_mapping) noexcept {
    out_fd = -1;
    out_mapping = nullptr;
    const std::string audit_path = base + ".audit";

    int fd = ::open(audit_path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    bool created = false;
    if (fd < 0) {
        if (errno != ENOENT) return io_status(errno);
        struct stat checkpoint_stat {};
        const bool checkpoint_exists = ::lstat((base + ".ckp").c_str(), &checkpoint_stat) == 0;
        if (!manifest_created && (checkpoint_exists || has_segment_files(base)))
            return Status{ErrorCode::corrupt_store, 0, 0};

        // Create audit file atomically via temp file + rename.
        const auto temp = make_temp_path(audit_path);
        fd = ::open(temp.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) return io_status(errno);
        auto cleanup = [&]() noexcept { (void)::close(fd); (void)::unlink(temp.c_str()); };
        if (::ftruncate(fd, static_cast<off_t>(audit_file_size)) != 0) {
            const int saved = errno; cleanup(); return io_status(saved);
        }
        if (const int err = ::posix_fallocate(fd, 0, static_cast<off_t>(audit_file_size)); err != 0) {
            cleanup(); return io_status(err);
        }
        void* map = ::mmap(nullptr, audit_file_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (map == MAP_FAILED) { const int saved = errno; cleanup(); return io_status(saved); }
        write_audit_header(static_cast<unsigned char*>(map), session_key);
        touch_mapping(map, audit_file_size);
        if (::rename(temp.c_str(), audit_path.c_str()) != 0) {
            const int saved = errno;
            (void)::munmap(map, audit_file_size);
            cleanup();
            return io_status(saved);
        }
        out_fd = fd;
        out_mapping = map;
        return Status::success();
    }

    struct stat audit_stat {};
    if (::fstat(fd, &audit_stat) != 0) { const int saved = errno; ::close(fd); return io_status(saved); }
    if (!S_ISREG(audit_stat.st_mode) || audit_stat.st_uid != ::geteuid() ||
        (audit_stat.st_mode & 0077) != 0) {
        ::close(fd);
        return Status{ErrorCode::invalid_config, 0, 0};
    }
    if (audit_stat.st_size != static_cast<off_t>(audit_file_size)) {
        ::close(fd);
        return Status{ErrorCode::corrupt_store, 0, 16};
    }
    void* map = ::mmap(nullptr, audit_file_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { const int saved = errno; ::close(fd); return io_status(saved); }
    if (!created) {
        const auto status = validate_audit_header(static_cast<const unsigned char*>(map), session_key);
        if (!status.ok()) {
            (void)::munmap(map, audit_file_size);
            ::close(fd);
            return status;
        }
    }
    out_fd = fd;
    out_mapping = map;
    return Status::success();
}

[[nodiscard]] Status collect_segment_indices(const std::string& base,
                                             std::vector<std::uint32_t>& out_indices) noexcept {
    try {
        const auto parent_path = std::filesystem::path(base).parent_path();
        const auto directory = parent_path.empty() ? std::filesystem::path(".") : parent_path;
        const std::string prefix = std::filesystem::path(base).filename().string() + ".seg";
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            const auto name = entry.path().filename().string();
            if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0) continue;
            std::uint64_t index = 0;
            const auto* first = name.data() + prefix.size();
            const auto* last = name.data() + name.size();
            const auto parsed = std::from_chars(first, last, index);
            if (parsed.ec == std::errc{} && parsed.ptr == last) {
                if (index > maximum_segment_index) return Status{ErrorCode::storage_full, 0, 0};
                out_indices.push_back(static_cast<std::uint32_t>(index));
            }
        }
        std::sort(out_indices.begin(), out_indices.end());
        out_indices.erase(std::unique(out_indices.begin(), out_indices.end()), out_indices.end());
        return Status::success();
    } catch (...) {
        return Status{ErrorCode::io_error, 0, 0};
    }
}

} // namespace

Result<Journal> Journal::open(JournalConfig config) noexcept try {
    const std::size_t segment_bytes = resolve_segment_bytes(config);
    if (config.path.empty() || config.session_key.empty() ||
        config.path.find('\0') != std::string_view::npos || config.path.size() > 4096 ||
        config.session_key.size() > journal_max_session_key_bytes ||
        segment_bytes < superblock_size + record_header_size ||
        (segment_bytes & 7U) != 0 ||
        segment_bytes > record_offset_mask || segment_bytes > config.capacity_bytes ||
        config.capacity_bytes > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) ||
        config.max_records == 0 ||
        (config.max_readers != 0 && config.max_reader_lag_records == 0))
        return Status{ErrorCode::invalid_config, 0, 0};

    const std::size_t audit_budget = config.capacity_bytes >= audit_file_size + segment_bytes
        ? audit_file_size : 0;
    const std::size_t segment_budget = config.capacity_bytes - audit_budget;
    const std::size_t budget_segments = segment_budget / segment_bytes;
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
        if (config.max_readers != 0)
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

    int manifest_fd = -1;
    bool manifest_created = false;
    if (const auto st = create_manifest_atomic(base, config.session_key, segment_bytes,
            config.capacity_bytes, max_segments, audit_budget, manifest_fd, manifest_created);
        !st.ok()) {
        return st;
    }

    journal.fd_ = manifest_fd;
    journal.capacity_bytes_ = config.capacity_bytes;
    journal.max_records_ = config.max_records;
    journal.test_hook_ = config.test_hook;
    journal.segment_capacity_bytes_ = segment_bytes;
    journal.audit_bytes_ = audit_budget;
    journal.max_segments_ = max_segments;
    journal.max_readers_ = config.max_readers;
    journal.max_reader_lag_records_ = config.max_reader_lag_records;

    auto fail = [&](Status status) -> Result<Journal> {
        if (manifest_created) {
            (void)::unlink(base.c_str());
            for (std::size_t i = 0; i < max_segments; ++i)
                (void)::unlink(segment_path(base, static_cast<std::uint32_t>(i)).c_str());
            (void)::unlink((base + ".ckp").c_str());
            (void)::unlink((base + ".audit").c_str());
        }
        journal.reset();
        return status;
    };

    // Clean any orphaned .tmp files left by a prior SIGKILL during file creation.
    cleanup_stale_temp_files(base);

    struct stat manifest_stat {};
    if (::fstat(manifest_fd, &manifest_stat) != 0) return fail(io_status(errno));
    if (!S_ISREG(manifest_stat.st_mode) || manifest_stat.st_uid != ::geteuid() ||
        (manifest_stat.st_mode & 0077) != 0)
        return fail(Status{ErrorCode::invalid_config, 0, 0});
    if (!manifest_created) {
        if (manifest_stat.st_size != static_cast<off_t>(superblock_size))
            return fail(Status{ErrorCode::corrupt_store, 0, 16});
        std::array<unsigned char, superblock_size> header{};
        if (const auto st = pread_exact(manifest_fd, header.data(), header.size()); !st.ok())
            return fail(st);
        if (const auto st = validate_v2_header(header.data(), v2_manifest_magic, superblock_size,
                config.session_key, segment_bytes, config.capacity_bytes, max_segments, 0, audit_budget);
            !st.ok())
            return fail(st);
    }

    if (audit_budget != 0) {
        if (const auto st = open_or_create_audit(base, config.session_key, manifest_created,
                journal.audit_fd_, journal.audit_mapping_); !st.ok())
            return fail(st);
        journal.audit_file_size_ = audit_file_size;
        auto* audit_data = static_cast<unsigned char*>(journal.audit_mapping_);
        for (std::size_t i = 0; i < audit_entry_count; ++i) {
            auto* entry = audit_data + superblock_size + i * audit_entry_size;
            const auto commit = load_u32_acquire(entry + audit_commit_offset);
            const auto entry_offset = superblock_size + i * audit_entry_size;
            if (commit == 0) {
                for (std::size_t later = i + 1; later < audit_entry_count; ++later) {
                    const auto* cand = audit_data + superblock_size + later * audit_entry_size;
                    if (load_u32_acquire(cand + audit_commit_offset) != 0)
                        return fail(Status{ErrorCode::corrupt_store, 0, entry_offset});
                }
                if (has_nonzero_tail(audit_data, entry_offset, audit_file_size)) {
                    std::memset(entry + sizeof(std::uint32_t), 0,
                                audit_file_size - entry_offset - sizeof(std::uint32_t));
                    std::atomic_thread_fence(std::memory_order_release);
                    put_u32(entry, 0);
                }
                break;
            }
            if (commit != audit_commit_marker || get_u32(entry) != audit_entry_size ||
                get_u32(entry + 24) != audit_entry_crc(entry))
                return fail(Status{ErrorCode::corrupt_store, 0, entry_offset});
            const auto prev_ep = get_u32(entry + 4);
            const auto next_ep = get_u32(entry + 8);
            const auto reason_size = get_u32(entry + 12);
            const auto record_id = get_u64(entry + 16);
            if (prev_ep == 0 || next_ep != prev_ep + 1U || reason_size == 0 || reason_size > 256 ||
                record_id == 0 || std::memchr(entry + 64, 0, reason_size) != nullptr ||
                (!journal.epoch_audit_records_.empty() &&
                 (journal.epoch_audit_records_.back().next_epoch != prev_ep ||
                  journal.epoch_audit_records_.back().reset_record_id >= record_id)) ||
                (journal.epoch_audit_records_.empty() && prev_ep != 1))
                return fail(Status{ErrorCode::corrupt_store, 0, entry_offset});
            journal.epoch_audit_records_.push_back(EpochAuditRecord{
                prev_ep, next_ep, record_id,
                std::string_view(reinterpret_cast<const char*>(entry + 64), reason_size)});
            ++journal.audit_count_;
        }
    } else {
        struct stat audit_stat {};
        if (::lstat((base + ".audit").c_str(), &audit_stat) == 0)
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
        journal.lowest_retained_record_id_.store(checkpoint_read.checkpoint.lowest_retained_record_id,
                                                 std::memory_order_relaxed);
    }

    std::vector<std::uint32_t> found_indices;
    if (const auto st = collect_segment_indices(base, found_indices); !st.ok()) return fail(st);

    if (!checkpoint_usable) {
        journal.first_segment_index_ = found_indices.empty() ? 0U : found_indices.front();
        journal.lowest_retained_outbound_seq_ = 1;
        journal.lowest_retained_record_id_.store(
            segment_record_id(journal.first_segment_index_, superblock_size), std::memory_order_relaxed);
        if (found_indices.empty() && checkpoint_read.exists)
            return fail(Status{ErrorCode::corrupt_store, 0, 0});
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
            if (::unlink(segment_path(base, index).c_str()) != 0 && errno != ENOENT)
                return fail(io_status(errno));
        }
    }

    std::vector<std::uint32_t> retained_indices;
    for (auto index : found_indices) if (index >= first_present) retained_indices.push_back(index);
    for (std::size_t i = 1; i < retained_indices.size(); ++i) {
        if (retained_indices[i] != retained_indices[i - 1] + 1U)
            return fail(Status{ErrorCode::corrupt_store, 0, retained_indices[i]});
    }
    if (retained_indices.size() > max_segments)
        return fail(Status{ErrorCode::corrupt_store, 0, retained_indices.size()});
    if (found_indices.empty() && checkpoint_usable &&
        (checkpoint_read.checkpoint.last_record_id != 0 ||
         checkpoint_read.checkpoint.next_inbound_seq != 1 ||
         checkpoint_read.checkpoint.next_outbound_seq != 1 ||
         checkpoint_read.checkpoint.first_segment_index != 0))
        return fail(Status{ErrorCode::corrupt_store, 0, 0});

    if (retained_indices.empty()) {
        auto created = create_segment_atomic(base, first_present, segment_bytes, config.session_key,
            config.capacity_bytes, max_segments, audit_budget, config.test_hook);
        if (!created.ok()) return fail(created.status());
        journal.segments_.push_back(std::move(created).value());
        journal.first_segment_index_ = first_present;
        journal.next_segment_index_ = first_present + 1U;
    } else {
        for (auto index : retained_indices) {
            auto opened = open_existing_segment(base, index, segment_bytes, config.session_key,
                config.capacity_bytes, max_segments, audit_budget);
            if (!opened.ok()) return fail(opened.status());
            journal.segments_.push_back(std::move(opened).value());
            if (index == maximum_segment_index) journal.next_segment_index_ = maximum_segment_index + 1U;
            else journal.next_segment_index_ = std::max(journal.next_segment_index_, index + 1U);
        }
    }

    // Recover all committed records. Track dirty tails BEFORE zeroing so non-final
    // segments with uncommitted tails are strictly rejected as corrupt_store.
    std::vector<bool> segment_dirty_tail(journal.segments_.size(), false);
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
    const RecordId initial_lowest_id = journal.lowest_retained_record_id_.load(std::memory_order_relaxed);

    for (std::size_t slot = 0; slot < journal.segments_.size(); ++slot) {
        auto& segment = *journal.segments_[slot];
        auto* data = static_cast<unsigned char*>(segment.mapping);
        std::size_t offset = superblock_size;
        bool segment_has_record = false;
        while (offset <= segment.mapping_size && segment.mapping_size - offset >= record_header_size) {
            auto* record = data + offset;
            const auto commit = load_u32_acquire(record + record_commit_offset);
            if (commit == 0) {
                if (has_committed_record_after(data, offset, segment.mapping_size))
                    return fail(Status{ErrorCode::corrupt_store, 0,
                        static_cast<std::size_t>(segment_record_id(segment.index, offset + record_commit_offset))});
                if (has_nonzero_tail(data, offset, segment.mapping_size))
                    segment_dirty_tail[slot] = true;
                break;
            }
            if (commit != record_commit_marker)
                return fail(Status{ErrorCode::corrupt_store, 0,
                    static_cast<std::size_t>(segment_record_id(segment.index, offset + record_commit_offset))});
            const std::uint32_t total_size = get_u32(record);
            const std::uint32_t payload_size = get_u32(record + 4);
            const std::uint32_t raw_kind = get_u32(record + 8);
            const auto record_epoch = get_u32(record + 12);
            const auto seq = get_u64(record + 16);
            const auto receipt = get_u64(record + 24);
            const auto dependency = get_u64(record + 32);
            const auto next_seq = get_u64(record + 40);
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
            JournalRecord recovered{id, kind, seq, receipt, dependency, next_seq, payload, false, record_epoch};
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
                    if ((dep == nullptr && dependency >= initial_lowest_id) ||
                        (dep != nullptr && dep->kind != RecordKind::inbound))
                        return fail(Status{ErrorCode::corrupt_store, 0, static_cast<std::size_t>(id + 32)});
                }
            } else if (kind == RecordKind::delivery) {
                if (seq == 0 || receipt != 0 || next_seq != 0 || !payload.empty())
                    return fail(Status{ErrorCode::corrupt_store, 0, static_cast<std::size_t>(id)});
                const auto* inbound = journal.find_record(dependency);
                if (inbound == nullptr) {
                    if (dependency >= initial_lowest_id)
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
                ++journal.current_epoch_;
                journal.next_inbound_seq_ = 1;
                journal.next_outbound_seq_ = 1;
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
        if (segment_has_record) journal.active_segment_ = slot;
        else if (!saw_any_record && slot == 0) journal.active_segment_ = 0;
    }

    // Enforce: dirty tail is only recoverable in the final data segment.
    for (std::size_t i = 0; i < journal.segments_.size(); ++i) {
        if (!segment_dirty_tail[i]) continue;
        bool later_has_records = false;
        for (std::size_t j = i + 1; j < journal.segments_.size(); ++j) {
            if (journal.segments_[j]->write_offset > superblock_size) {
                later_has_records = true;
                break;
            }
        }
        auto& segment = *journal.segments_[i];
        if (later_has_records)
            return fail(Status{ErrorCode::corrupt_store, 0, segment.write_offset});
        auto* record = static_cast<unsigned char*>(segment.mapping) + segment.write_offset;
        const std::size_t tail_bytes = segment.mapping_size - segment.write_offset;
        if (tail_bytes > sizeof(std::uint32_t)) {
            std::memset(record + sizeof(std::uint32_t), 0, tail_bytes - sizeof(std::uint32_t));
            std::atomic_thread_fence(std::memory_order_release);
        }
        if (tail_bytes >= sizeof(std::uint32_t)) put_u32(record, 0);
        else if (tail_bytes > 0) std::memset(record, 0, tail_bytes);
    }

    if (!epoch_known && !journal.epoch_audit_records_.empty())
        journal.current_epoch_ = journal.epoch_audit_records_.back().next_epoch;
    else if (!epoch_known && checkpoint_usable)
        journal.current_epoch_ = checkpoint_read.checkpoint.epoch;

    if (journal.audit_bytes_ == 0) {
        if (!journal.epoch_audit_records_.empty() || std::any_of(
            journal.records_.begin(), journal.records_.end(), [](const auto& r) {
                return r.kind == RecordKind::epoch_reset;
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
            const auto expected_prev = journal.epoch_audit_records_.empty()
                ? 1U : journal.epoch_audit_records_.back().next_epoch;
            if (record.epoch != expected_prev || journal.audit_count_ >= audit_entry_count)
                return fail(Status{ErrorCode::corrupt_store, 0, static_cast<std::size_t>(record.id)});
            if (const auto st = journal.append_epoch_audit(record.epoch,
                    static_cast<std::uint32_t>(record.next_seq), record.id, record.payload); !st.ok())
                return fail(st);
        }
        for (const auto& audit : journal.epoch_audit_records_) {
            const auto* event = journal.find_record(audit.reset_record_id);
            if (event != nullptr) {
                if (event->kind != RecordKind::epoch_reset || event->epoch != audit.previous_epoch ||
                    event->next_seq != audit.next_epoch || event->payload != audit.reason)
                    return fail(Status{ErrorCode::corrupt_store, 0,
                        static_cast<std::size_t>(audit.reset_record_id)});
            } else if (audit.reset_record_id >= initial_lowest_id) {
                return fail(Status{ErrorCode::corrupt_store, 0,
                    static_cast<std::size_t>(audit.reset_record_id)});
            }
        }
        const std::uint32_t audit_epoch = journal.epoch_audit_records_.empty()
            ? journal_initial_epoch : journal.epoch_audit_records_.back().next_epoch;
        if (audit_epoch != journal.current_epoch_ ||
            journal.epoch_audit_records_.size() != journal.current_epoch_ - 1U)
            return fail(Status{ErrorCode::corrupt_store, 0, journal.current_epoch_});
    }

    if (!saw_inbound && checkpoint_usable && checkpoint_read.checkpoint.epoch == journal.current_epoch_)
        journal.next_inbound_seq_ = checkpoint_read.checkpoint.next_inbound_seq;
    else if (saw_inbound) journal.next_inbound_seq_ = expected_inbound;
    else journal.next_inbound_seq_ = 1;
    if (!saw_outbound && checkpoint_usable && checkpoint_read.checkpoint.epoch == journal.current_epoch_)
        journal.next_outbound_seq_ = checkpoint_read.checkpoint.next_outbound_seq;
    else if (saw_outbound) journal.next_outbound_seq_ = expected_outbound;
    else journal.next_outbound_seq_ = 1;
    if (journal.next_inbound_seq_ == 0 || journal.next_outbound_seq_ == 0)
        return fail(Status{ErrorCode::corrupt_store, 0, 0});

    journal.used_total_bytes_ = journal.audit_file_size_;
    for (const auto& seg : journal.segments_) journal.used_total_bytes_ += seg->write_offset;

    RecordId lowest_id = initial_lowest_id;
    if (journal.records_.empty()) {
        lowest_id = segment_record_id(journal.segments_.front()->index, superblock_size);
    } else if (!checkpoint_usable || journal.first_segment_index_ == 0) {
        lowest_id = journal.records_.front().id;
    }
    journal.lowest_retained_record_id_.store(lowest_id, std::memory_order_release);

    // Preserve monotonic lowest_retained_outbound_seq when valid checkpoint matches current epoch.
    if (!checkpoint_usable) {
        const auto first_out = std::find_if(journal.records_.begin(), journal.records_.end(),
            [&journal](const auto& r) {
                return r.kind == RecordKind::outbound && r.epoch == journal.current_epoch_;
            });
        journal.lowest_retained_outbound_seq_ = first_out == journal.records_.end()
            ? journal.next_outbound_seq_ : first_out->seq;
    } else if (saw_epoch_reset && checkpoint_read.checkpoint.epoch != journal.current_epoch_) {
        journal.lowest_retained_outbound_seq_ = 1;
    } else {
        journal.lowest_retained_outbound_seq_ = checkpoint_read.checkpoint.lowest_retained_outbound_seq;
        if (journal.lowest_retained_outbound_seq_ > journal.next_outbound_seq_)
            return fail(Status{ErrorCode::corrupt_store, 0, 0});
    }

    journal.next_segment_index_ = std::max(journal.next_segment_index_, last_data_segment + 1U);
    if (journal.next_segment_index_ > maximum_segment_index + 1U)
        return fail(Status{ErrorCode::storage_full, 0, 0});

    while (journal.segments_.size() < journal.max_segments_ &&
           journal.next_segment_index_ <= maximum_segment_index) {
        auto created = create_segment_atomic(base, journal.next_segment_index_, segment_bytes,
            config.session_key, config.capacity_bytes, max_segments, audit_budget, config.test_hook);
        if (!created.ok()) return fail(created.status());
        journal.segments_.push_back(std::move(created).value());
        journal.used_total_bytes_ += superblock_size;
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
        if (!record.delivered) { delivered_prefix = false; continue; }
        if (delivered_prefix) rebuilt_delivery_cursor = record.id;
    }
    const RecordId stored_delivery_cursor = journal.checkpoint_.delivery_cursor;
    bool delivery_cursor_valid = true;
    if (stored_delivery_cursor != 0 && stored_delivery_cursor >= lowest_id) {
        const auto* rec = journal.find_record(stored_delivery_cursor);
        if (rec == nullptr || rec->kind != RecordKind::inbound || !rec->delivered)
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
            ? first_current_inbound->seq - 1U : journal.next_inbound_seq_ - 1U;
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
                const auto& r = journal.records_[index];
                return entry.first == r.seq && entry.second == r.id;
            });
    journal.checkpoint_stale_ = !checkpoint_usable ||
        journal.checkpoint_.epoch != journal.current_epoch_ ||
        journal.checkpoint_.last_record_id != last_record_id ||
        journal.checkpoint_.next_inbound_seq != journal.next_inbound_seq_ ||
        journal.checkpoint_.next_outbound_seq != journal.next_outbound_seq_ || !index_matches ||
        completed_cursor_stale;
    journal.checkpoint_.epoch = journal.current_epoch_;
    journal.checkpoint_.next_inbound_seq = journal.next_inbound_seq_;
    journal.checkpoint_.next_outbound_seq = journal.next_outbound_seq_;
    journal.checkpoint_.completed_inbound_seq = journal.completed_inbound_seq_;
    journal.checkpoint_.delivery_cursor = journal.completed_inbound_record_id_;
    journal.checkpoint_.last_record_id = last_record_id;
    journal.checkpoint_.first_segment_index = journal.first_segment_index_;
    journal.checkpoint_.lowest_retained_outbound_seq = journal.lowest_retained_outbound_seq_;
    journal.checkpoint_.lowest_retained_record_id = lowest_id;
    journal.published_record_count_.store(journal.records_.size(), std::memory_order_release);
    journal.records_data_ = journal.records_.data();
    return journal;
} catch (const std::bad_alloc&) {
    return Status{ErrorCode::capacity_exceeded, 0, 0};
} catch (...) {
    return Status{ErrorCode::io_error, 0, 0};
}

JournalInspection Journal::inspect_snapshot(JournalConfig config) noexcept try {
    JournalInspection snapshot{};
    if (config.path.empty() || config.session_key.empty() ||
        config.path.find('\0') != std::string_view::npos || config.path.size() > 4096 ||
        config.session_key.size() > journal_max_session_key_bytes || config.max_records == 0) {
        snapshot.diagnostic = {ErrorCode::invalid_config, 0, 0};
        return snapshot;
    }
    const std::string base(config.path.data(), config.path.size());
    const auto parent = std::filesystem::path(base).parent_path();
    if (!parent.empty() && has_symlink_directory_component(parent)) {
        snapshot.diagnostic = {ErrorCode::invalid_config, 0, 0};
        return snapshot;
    }
    const int fd = ::open(base.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) { snapshot.diagnostic = io_status(errno); return snapshot; }
    struct stat info {};
    if (::fstat(fd, &info) != 0) { snapshot.diagnostic = io_status(errno); ::close(fd); return snapshot; }
    if (!S_ISREG(info.st_mode) || info.st_uid != ::geteuid() || (info.st_mode & 0077) != 0) {
        snapshot.diagnostic = {ErrorCode::invalid_config, 0, 0};
        ::close(fd);
        return snapshot;
    }
    if (info.st_size != static_cast<off_t>(superblock_size)) {
        snapshot.diagnostic = {ErrorCode::corrupt_store, 0, 16};
        ::close(fd);
        return snapshot;
    }
    std::array<unsigned char, superblock_size> header{};
    if (const auto st = pread_exact(fd, header.data(), header.size()); !st.ok()) {
        snapshot.diagnostic = st;
        ::close(fd);
        return snapshot;
    }
    ::close(fd);

    if (std::memcmp(header.data(), v2_manifest_magic.data(), v2_manifest_magic.size()) != 0) {
        snapshot.diagnostic = {ErrorCode::corrupt_store, 0, 0};
        return snapshot;
    }
    const auto version = get_u32(header.data() + 8);
    if (version != 2) { snapshot.diagnostic = {ErrorCode::unsupported_version, 0, 8}; return snapshot; }
    const auto max_segments = static_cast<std::size_t>(get_u32(header.data() + 36));
    const auto segment_bytes = static_cast<std::size_t>(get_u64(header.data() + 40));
    const auto total_bytes = static_cast<std::size_t>(get_u64(header.data() + 48));
    const auto audit_bytes = static_cast<std::size_t>(get_u32(header.data() + 56));
    if (const auto st = validate_v2_header(header.data(), v2_manifest_magic, superblock_size,
            config.session_key, segment_bytes, total_bytes, max_segments, 0, audit_bytes); !st.ok()) {
        snapshot.diagnostic = st;
        return snapshot;
    }
    if (segment_bytes < superblock_size + record_header_size || max_segments == 0 ||
        (audit_bytes != 0 && audit_bytes != audit_file_size) || total_bytes < audit_bytes + segment_bytes ||
        max_segments > (total_bytes - audit_bytes) / segment_bytes) {
        snapshot.diagnostic = {ErrorCode::corrupt_store, 0, 36};
        return snapshot;
    }

    snapshot.format_version = 2;
    const auto cp = read_checkpoint_file(base, config.session_key, config.max_records);
    snapshot.checkpoint_stale = !cp.valid;
    auto first_segment = cp.valid ? cp.checkpoint.first_segment_index : 0U;
    snapshot.lowest_retained_outbound_seq = cp.valid ? cp.checkpoint.lowest_retained_outbound_seq : 1U;
    snapshot.lowest_retained_record_id = cp.valid ? cp.checkpoint.lowest_retained_record_id : 4096U;

    std::vector<std::uint32_t> indices;
    if (const auto st = collect_segment_indices(base, indices); !st.ok()) {
        snapshot.diagnostic = st;
        return snapshot;
    }
    if (!cp.valid) {
        if (indices.empty()) { snapshot.diagnostic = {ErrorCode::corrupt_store, 0, 0}; return snapshot; }
        first_segment = indices.front();
        snapshot.lowest_retained_record_id = segment_record_id(first_segment, superblock_size);
    }
    if (cp.valid && !indices.empty() && indices.front() > first_segment) {
        snapshot.diagnostic = {ErrorCode::corrupt_store, 0, indices.front()};
        return snapshot;
    }
    std::vector<std::uint32_t> retained;
    for (const auto idx : indices) if (idx >= first_segment) retained.push_back(idx);
    if (retained.empty() || (first_segment != 0 && retained.front() != first_segment)) {
        snapshot.diagnostic = {ErrorCode::corrupt_store, 0, first_segment};
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
    bool saw_in = false, saw_out = false, epoch_known = false;
    bool require_one_in = first_segment == 0, require_one_out = first_segment == 0;
    std::uint32_t active_epoch = cp.valid ? cp.checkpoint.epoch : journal_initial_epoch;
    RecordId last_record = 0;
    std::vector<std::pair<std::uint64_t, RecordId>> outbound_index;
    struct ResetSummary { std::uint32_t previous_epoch{}, next_epoch{}; RecordId id{}; std::string reason{}; };
    struct RecordSummary { RecordId id{}; RecordKind kind{}; std::uint64_t seq{}; RecordId dependency{}; std::uint32_t epoch{}; bool delivered{}; };
    std::vector<ResetSummary> reset_events;
    std::vector<RecordSummary> records;
    std::unordered_set<RecordId> delivery_dependencies;
    records.reserve(std::min<std::size_t>(config.max_records, 65'536));
    auto find_summary = [&records](RecordId id) -> RecordSummary* {
        const auto found = std::lower_bound(records.begin(), records.end(), id,
            [](const RecordSummary& item, RecordId value) { return item.id < value; });
        return found != records.end() && found->id == id ? &*found : nullptr;
    };

    std::vector<bool> seg_dirty_tail(retained.size(), false);
    std::vector<bool> seg_has_records(retained.size(), false);
    for (std::size_t slot = 0; slot < retained.size(); ++slot) {
        const auto index = retained[slot];
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
        if (const auto st = validate_v2_header(bytes, v2_segment_magic, segment_bytes,
                config.session_key, segment_bytes, total_bytes, max_segments, index, audit_bytes); !st.ok()) {
            snapshot.diagnostic = st;
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
                else if (has_nonzero_tail(bytes, offset, segment_bytes))
                    seg_dirty_tail[slot] = true;
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
            if (snapshot.committed_records >= config.max_records || id <= last_record ||
                record_epoch == 0 || (epoch_known && record_epoch != active_epoch)) {
                snapshot.diagnostic = {ErrorCode::corrupt_store, 0, static_cast<std::size_t>(id)};
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
                        snapshot.diagnostic = {ErrorCode::corrupt_store, 0, static_cast<std::size_t>(id + 32)};
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
                if (!delivery_dependencies.insert(dependency).second ||
                    (inbound == nullptr && dependency >= snapshot.lowest_retained_record_id) ||
                    (inbound != nullptr && (inbound->kind != RecordKind::inbound ||
                        inbound->seq != seq || inbound->delivered))) {
                    snapshot.diagnostic = {ErrorCode::corrupt_store, 0, static_cast<std::size_t>(id + 32)};
                    break;
                }
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
                for (const auto& prev : records) {
                    if (prev.kind == RecordKind::inbound && prev.epoch == active_epoch && !prev.delivered) {
                        snapshot.diagnostic = {ErrorCode::corrupt_store, 0, static_cast<std::size_t>(id)};
                        break;
                    }
                }
                if (!snapshot.diagnostic.ok()) break;
                ++active_epoch;
                next_in = 1; next_out = 1;
                saw_in = false; saw_out = false;
                require_one_in = true; require_one_out = true;
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
        if (offset > superblock_size) seg_has_records[slot] = true;
        snapshot.used_bytes += offset;
        (void)::munmap(mapping, segment_bytes);
        if (!snapshot.diagnostic.ok()) return snapshot;
    }
    for (std::size_t i = 0; i < retained.size(); ++i) {
        if (!seg_dirty_tail[i]) continue;
        for (std::size_t j = i + 1; j < retained.size(); ++j) {
            if (seg_has_records[j]) {
                snapshot.diagnostic = {ErrorCode::corrupt_store, 0, 0};
                return snapshot;
            }
        }
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
    if (!cp.valid)
        snapshot.lowest_retained_outbound_seq = outbound_index.empty() ? next_out : outbound_index.front().first;
    if (cp.valid) {
        snapshot.checkpoint_stale = cp.checkpoint.last_record_id != last_record ||
            cp.checkpoint.epoch != active_epoch ||
            cp.checkpoint.next_inbound_seq != next_in || cp.checkpoint.next_outbound_seq != next_out ||
            cp.outbound_index != outbound_index;
    }

    if (audit_bytes != 0) {
        const std::string audit_path = base + ".audit";
        const int audit_fd = ::open(audit_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (audit_fd < 0) { snapshot.diagnostic = io_status(errno); return snapshot; }
        struct stat audit_stat {};
        if (::fstat(audit_fd, &audit_stat) != 0 || !S_ISREG(audit_stat.st_mode) ||
            audit_stat.st_uid != ::geteuid() || (audit_stat.st_mode & 0077) != 0 ||
            audit_stat.st_size != static_cast<off_t>(audit_file_size)) {
            snapshot.diagnostic = {ErrorCode::corrupt_store, 0, 0};
            ::close(audit_fd);
            return snapshot;
        }
        void* audit_map = ::mmap(nullptr, audit_file_size, PROT_READ, MAP_SHARED, audit_fd, 0);
        const int map_err = errno;
        ::close(audit_fd);
        if (audit_map == MAP_FAILED) { snapshot.diagnostic = io_status(map_err); return snapshot; }
        const auto* audit_data = static_cast<const unsigned char*>(audit_map);
        if (const auto st = validate_audit_header(audit_data, config.session_key); !st.ok()) {
            snapshot.diagnostic = st;
            (void)::munmap(audit_map, audit_file_size);
            return snapshot;
        }
        std::vector<ResetSummary> audit_records;
        audit_records.reserve(audit_entry_count);
        bool pending_audit_tail = false;
        for (std::size_t i = 0; i < audit_entry_count; ++i) {
            const auto* entry = audit_data + superblock_size + i * audit_entry_size;
            const auto commit = load_u32_acquire(entry + audit_commit_offset);
            const auto entry_offset = superblock_size + i * audit_entry_size;
            if (commit == 0) {
                for (std::size_t later = i + 1; later < audit_entry_count; ++later) {
                    const auto* cand = audit_data + superblock_size + later * audit_entry_size;
                    if (load_u32_acquire(cand + audit_commit_offset) != 0) {
                        snapshot.diagnostic = {ErrorCode::corrupt_store, 0, entry_offset};
                        break;
                    }
                }
                if (snapshot.diagnostic.ok() && has_nonzero_tail(audit_data, entry_offset, audit_file_size))
                    pending_audit_tail = true;
                break;
            }
            if (commit != audit_commit_marker || get_u32(entry) != audit_entry_size ||
                get_u32(entry + 24) != audit_entry_crc(entry)) {
                snapshot.diagnostic = {ErrorCode::corrupt_store, 0, entry_offset};
                break;
            }
            const auto prev_ep = get_u32(entry + 4);
            const auto next_ep = get_u32(entry + 8);
            const auto reason_size = get_u32(entry + 12);
            const auto id = get_u64(entry + 16);
            if (prev_ep == 0 || next_ep != prev_ep + 1U || reason_size == 0 || reason_size > 256 ||
                id == 0 || std::memchr(entry + 64, 0, reason_size) != nullptr ||
                (!audit_records.empty() &&
                 (audit_records.back().next_epoch != prev_ep || audit_records.back().id >= id)) ||
                (audit_records.empty() && prev_ep != journal_initial_epoch)) {
                snapshot.diagnostic = {ErrorCode::corrupt_store, 0, entry_offset};
                break;
            }
            audit_records.push_back(ResetSummary{
                prev_ep, next_ep, id,
                std::string(reinterpret_cast<const char*>(entry + 64), reason_size)});
        }
        if (snapshot.diagnostic.ok()) {
            if (pending_audit_tail) snapshot.checkpoint_stale = true;
            snapshot.audit_records = audit_records.size();
            snapshot.used_bytes += audit_file_size;
            const std::uint32_t audit_epoch = audit_records.empty()
                ? journal_initial_epoch : audit_records.back().next_epoch;
            if (audit_epoch > active_epoch || audit_records.size() > active_epoch - journal_initial_epoch)
                snapshot.diagnostic = {ErrorCode::corrupt_store, 0, active_epoch};
            for (const auto& audit : audit_records) {
                if (!snapshot.diagnostic.ok()) break;
                const auto event = std::find_if(reset_events.begin(), reset_events.end(),
                    [&audit](const auto& item) { return item.id == audit.id; });
                if (event != reset_events.end()) {
                    if (event->previous_epoch != audit.previous_epoch ||
                        event->next_epoch != audit.next_epoch || event->reason != audit.reason)
                        snapshot.diagnostic = {ErrorCode::corrupt_store, 0, static_cast<std::size_t>(audit.id)};
                } else if (audit.id >= snapshot.lowest_retained_record_id) {
                    snapshot.diagnostic = {ErrorCode::corrupt_store, 0, static_cast<std::size_t>(audit.id)};
                }
            }
            for (const auto& event : reset_events) {
                if (!snapshot.diagnostic.ok()) break;
                const auto audit = std::find_if(audit_records.begin(), audit_records.end(),
                    [&event](const auto& item) { return item.id == event.id; });
                if (audit == audit_records.end()) {
                    snapshot.checkpoint_stale = true;
                    continue;
                }
                if (audit->previous_epoch != event.previous_epoch ||
                    audit->next_epoch != event.next_epoch || audit->reason != event.reason)
                    snapshot.diagnostic = {ErrorCode::corrupt_store, 0, static_cast<std::size_t>(event.id)};
            }
        }
        (void)::munmap(audit_map, audit_file_size);
    }
    return snapshot;
} catch (const std::bad_alloc&) {
    JournalInspection snapshot{};
    snapshot.diagnostic = {ErrorCode::capacity_exceeded, 0, 0};
    return snapshot;
} catch (...) {
    JournalInspection snapshot{};
    snapshot.diagnostic = {ErrorCode::io_error, 0, 0};
    return snapshot;
}

} // namespace nimblefix
