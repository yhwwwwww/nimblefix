#include "journal_format.hpp"
#include "nimblefix/journal.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace nimblefix {

using namespace store;

namespace {

struct MappedSegmentRO {
    void* mapping{nullptr};
    std::size_t size{0};
    std::uint32_t index{0};

    MappedSegmentRO() noexcept = default;
    MappedSegmentRO(const MappedSegmentRO&) = delete;
    MappedSegmentRO& operator=(const MappedSegmentRO&) = delete;
    MappedSegmentRO(MappedSegmentRO&& other) noexcept
        : mapping(std::exchange(other.mapping, nullptr)),
          size(std::exchange(other.size, 0)),
          index(std::exchange(other.index, 0)) {}
    MappedSegmentRO& operator=(MappedSegmentRO&& other) noexcept {
        if (this == &other) return *this;
        reset();
        mapping = std::exchange(other.mapping, nullptr);
        size = std::exchange(other.size, 0);
        index = std::exchange(other.index, 0);
        return *this;
    }
    ~MappedSegmentRO() { reset(); }

    void reset() noexcept {
        if (mapping != nullptr && size != 0) {
            (void)::munmap(mapping, size);
        }
        mapping = nullptr;
        size = 0;
        index = 0;
    }

    [[nodiscard]] bool valid() const noexcept { return mapping != nullptr; }
    [[nodiscard]] const unsigned char* bytes() const noexcept {
        return static_cast<const unsigned char*>(mapping);
    }
};

enum class OpenSegmentResult : std::uint8_t {
    ok,
    not_found,
    error,
};

[[nodiscard]] Status collect_tailer_segments(const std::string& base,
                                             std::vector<std::uint32_t>& out) noexcept {
    out.clear();
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
                if (index > maximum_segment_index) return Status{ErrorCode::corrupt_store, 0, 0};
                out.push_back(static_cast<std::uint32_t>(index));
            }
        }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return Status::success();
    } catch (...) {
        return Status{ErrorCode::io_error, 0, 0};
    }
}

[[nodiscard]] OpenSegmentResult open_segment_readonly(const std::string& base,
                                                      std::uint32_t index,
                                                      std::size_t segment_bytes,
                                                      std::string_view session_key,
                                                      std::size_t capacity_bytes,
                                                      std::size_t max_segments,
                                                      std::size_t audit_bytes,
                                                      MappedSegmentRO& out,
                                                      Status& out_error) noexcept {
    out.reset();
    out_error = Status::success();
    std::string path;
    try {
        path = segment_path(base, index);
    } catch (...) {
        out_error = Status{ErrorCode::capacity_exceeded, 0, 0};
        return OpenSegmentResult::error;
    }
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENOENT) return OpenSegmentResult::not_found;
        out_error = io_status(errno);
        return OpenSegmentResult::error;
    }
    struct stat info{};
    if (::fstat(fd, &info) != 0) {
        const int saved = errno;
        ::close(fd);
        out_error = io_status(saved);
        return OpenSegmentResult::error;
    }
    if (!S_ISREG(info.st_mode) || info.st_size != static_cast<off_t>(segment_bytes)) {
        ::close(fd);
        out_error = Status{ErrorCode::corrupt_store, 0, 16};
        return OpenSegmentResult::error;
    }
    void* mapping = ::mmap(nullptr, segment_bytes, PROT_READ, MAP_SHARED, fd, 0);
    const int map_err = errno;
    ::close(fd);
    if (mapping == MAP_FAILED) {
        out_error = io_status(map_err);
        return OpenSegmentResult::error;
    }
    const auto st = validate_v2_header(static_cast<const unsigned char*>(mapping),
        v2_segment_magic, segment_bytes, session_key, segment_bytes,
        capacity_bytes, max_segments, index, audit_bytes);
    if (!st.ok()) {
        (void)::munmap(mapping, segment_bytes);
        out_error = st;
        return OpenSegmentResult::error;
    }
    out.mapping = mapping;
    out.size = segment_bytes;
    out.index = index;
    return OpenSegmentResult::ok;
}

[[nodiscard]] Status decode_committed_record(const unsigned char* base,
                                             std::uint32_t seg_index,
                                             std::size_t offset,
                                             std::size_t seg_bytes,
                                             bool verify_crc,
                                             JournalRecord& out,
                                             std::size_t& out_total_size) noexcept {
    const auto* rec = base + offset;
    const std::uint32_t total_size = get_u32(rec);
    const std::uint32_t payload_size = get_u32(rec + 4);
    const std::uint32_t raw_kind = get_u32(rec + 8);
    const std::uint32_t epoch = get_u32(rec + 12);
    if (total_size < record_header_size || (total_size & 7U) != 0 ||
        total_size > seg_bytes - offset ||
        payload_size > total_size - record_header_size ||
        align_record(record_header_size + static_cast<std::size_t>(payload_size)) != total_size ||
        !valid_record_kind(raw_kind) || epoch == 0) {
        return Status{ErrorCode::corrupt_store, 0, offset};
    }
    if (verify_crc && get_u32(rec + record_crc_offset) != record_crc(rec, total_size)) {
        return Status{ErrorCode::corrupt_store, 0, offset + record_crc_offset};
    }
    out.id = segment_record_id(seg_index, offset);
    out.kind = static_cast<RecordKind>(raw_kind);
    out.epoch = epoch;
    out.seq = get_u64(rec + 16);
    out.receipt = get_u64(rec + 24);
    out.dependency = get_u64(rec + 32);
    out.next_seq = get_u64(rec + 40);
    out.payload = payload_size == 0
        ? std::string_view{}
        : std::string_view{reinterpret_cast<const char*>(rec + record_header_size), payload_size};
    out.delivered = false;
    out_total_size = total_size;
    return Status::success();
}

} // namespace

struct JournalTailer::Impl {
    std::string base_path{};
    std::string session_key{};
    std::size_t segment_bytes{};
    std::size_t capacity_bytes{};
    std::size_t max_segments{};
    std::size_t audit_bytes{};
    bool verify_crc{true};

    MappedSegmentRO current{};
    MappedSegmentRO next_cached{};
    std::uint32_t current_seg_index{};
    std::size_t current_offset{superblock_size};
    RecordId cursor{};
    RecordId min_record_id{};
    bool expired{false};

    [[nodiscard]] bool segment_was_pruned(std::uint32_t missing_index) const noexcept {
        // Check checkpoint first (atomic rename), then directory segment listing.
        const auto cp = read_checkpoint_file(base_path, session_key, 16'777'216U);
        if (cp.valid && cp.checkpoint.first_segment_index > missing_index) {
            return true;
        }
        std::vector<std::uint32_t> indices;
        if (collect_tailer_segments(base_path, indices).ok() && !indices.empty()) {
            // If any segment strictly newer than missing_index already exists while
            // missing_index does not exist, missing_index was pruned.
            if (indices.front() > missing_index) return true;
        }
        return false;
    }

    [[nodiscard]] Status ensure_current_mapped() noexcept {
        if (current.valid()) return Status::success();
        Status err{};
        const auto res = open_segment_readonly(base_path, current_seg_index, segment_bytes,
            session_key, capacity_bytes, max_segments, audit_bytes, current, err);
        if (res == OpenSegmentResult::ok) return Status::success();
        if (res == OpenSegmentResult::error) return err;
        if (segment_was_pruned(current_seg_index)) {
            expired = true;
            return Status{ErrorCode::reader_expired, 0, static_cast<std::size_t>(cursor)};
        }
        return Status{ErrorCode::need_more, 0, 0};
    }

    [[nodiscard]] Status try_advance_segment(bool& advanced) noexcept {
        advanced = false;
        if (current_seg_index >= maximum_segment_index) {
            return Status{ErrorCode::need_more, 0, 0};
        }
        const std::uint32_t next_idx = current_seg_index + 1U;
        if (!next_cached.valid() || next_cached.index != next_idx) {
            Status err{};
            const auto res = open_segment_readonly(base_path, next_idx, segment_bytes,
                session_key, capacity_bytes, max_segments, audit_bytes, next_cached, err);
            if (res == OpenSegmentResult::error) return err;
            if (res == OpenSegmentResult::not_found) {
                if (segment_was_pruned(next_idx)) {
                    expired = true;
                    return Status{ErrorCode::reader_expired, 0, static_cast<std::size_t>(cursor)};
                }
                return Status{ErrorCode::need_more, 0, 0};
            }
        }
        // Only rotate once the writer has actually committed the first record in next_idx
        // (spare segments are pre-created empty before rotation).
        const auto next_commit = load_u32_acquire(
            next_cached.bytes() + superblock_size + record_commit_offset);
        if (next_commit == 0) {
            return Status{ErrorCode::need_more, 0, 0};
        }
        if (next_commit != record_commit_marker) {
            return Status{ErrorCode::corrupt_store, 0, superblock_size + record_commit_offset};
        }
        // Release-acquire transitivity check: re-verify current_offset in current segment
        // AFTER observing next_cached's commit marker. If the writer committed one final
        // record in current right before rotating, this acquire load is guaranteed to see it.
        if (current.valid() && current_offset + record_header_size <= segment_bytes) {
            const auto recheck = load_u32_acquire(
                current.bytes() + current_offset + record_commit_offset);
            if (recheck == record_commit_marker) {
                return Status::success(); // Caller will read the final record in current first!
            }
            if (recheck != 0) {
                return Status{ErrorCode::corrupt_store, 0, current_offset + record_commit_offset};
            }
        }
        current = std::move(next_cached);
        next_cached.reset();
        current_seg_index = next_idx;
        current_offset = superblock_size;
        advanced = true;
        return Status::success();
    }

    [[nodiscard]] Status next(JournalRecord& out) noexcept {
        if (expired) return Status{ErrorCode::reader_expired, 0, static_cast<std::size_t>(cursor)};
        for (;;) {
            if (const auto st = ensure_current_mapped(); !st.ok()) return st;

            if (current_offset + record_header_size <= segment_bytes) {
                const auto commit = load_u32_acquire(
                    current.bytes() + current_offset + record_commit_offset);
                if (commit == record_commit_marker) {
                    std::size_t total_size = 0;
                    const auto dec = decode_committed_record(current.bytes(), current_seg_index,
                        current_offset, segment_bytes, verify_crc, out, total_size);
                    if (!dec.ok()) return dec;
                    current_offset += total_size;
                    cursor = out.id;
                    if (min_record_id != 0 && out.id < min_record_id) {
                        continue;
                    }
                    min_record_id = 0;
                    return Status::success();
                }
                if (commit != 0) {
                    return Status{ErrorCode::corrupt_store, 0, current_offset + record_commit_offset};
                }
            }

            bool advanced = false;
            const auto adv_st = try_advance_segment(advanced);
            if (!adv_st.ok()) return adv_st;
            // Loop either to read the newly visible tail record in current or the first record of next.
        }
    }
};

JournalTailer::JournalTailer(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
JournalTailer::JournalTailer(JournalTailer&& other) noexcept = default;
JournalTailer& JournalTailer::operator=(JournalTailer&& other) noexcept = default;
JournalTailer::~JournalTailer() = default;

Result<JournalTailer> JournalTailer::open(JournalTailerConfig config) noexcept try {
    if (config.path.empty() || config.path.find('\0') != std::string_view::npos ||
        config.path.size() > 4096 || config.session_key.size() > journal_max_session_key_bytes) {
        return Status{ErrorCode::invalid_config, 0, 0};
    }

    std::string base(config.path.data(), config.path.size());
    const auto parent = std::filesystem::path(base).parent_path();
    if (!parent.empty() && has_symlink_directory_component(parent)) {
        return Status{ErrorCode::invalid_config, 0, 0};
    }

    // Open manifest read-only (O_RDONLY, NO flock) so live writers are never blocked.
    const int mfd = ::open(base.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (mfd < 0) return io_status(errno);
    struct stat mstat{};
    if (::fstat(mfd, &mstat) != 0) {
        const int saved = errno;
        ::close(mfd);
        return io_status(saved);
    }
    if (!S_ISREG(mstat.st_mode) || mstat.st_size != static_cast<off_t>(superblock_size)) {
        ::close(mfd);
        return Status{ErrorCode::corrupt_store, 0, 16};
    }
    std::array<unsigned char, superblock_size> mhdr{};
    const auto rd = pread_exact(mfd, mhdr.data(), mhdr.size());
    ::close(mfd);
    if (!rd.ok()) return rd;

    if (std::memcmp(mhdr.data(), v2_manifest_magic.data(), v2_manifest_magic.size()) != 0)
        return Status{ErrorCode::corrupt_store, 0, 0};
    if (get_u32(mhdr.data() + 8) != 2U)
        return Status{ErrorCode::unsupported_version, 0, 8};
    if (get_u32(mhdr.data() + 12) != superblock_size ||
        get_u64(mhdr.data() + 16) != superblock_size)
        return Status{ErrorCode::corrupt_store, 0, 12};
    const std::size_t key_len = get_u32(mhdr.data() + 24);
    if (key_len == 0 || key_len > journal_max_session_key_bytes)
        return Status{ErrorCode::corrupt_store, 0, 24};
    if (get_u32(mhdr.data() + 28) != current_epoch_initial)
        return Status{ErrorCode::unsupported_version, 0, 28};
    const std::size_t max_segments = get_u32(mhdr.data() + 36);
    const std::size_t segment_bytes = static_cast<std::size_t>(get_u64(mhdr.data() + 40));
    const std::size_t capacity_bytes = static_cast<std::size_t>(get_u64(mhdr.data() + 48));
    const std::size_t audit_bytes = get_u32(mhdr.data() + 56);
    if (max_segments == 0 || segment_bytes < superblock_size + record_header_size ||
        (segment_bytes & 7U) != 0 || segment_bytes > record_offset_mask ||
        capacity_bytes < segment_bytes) {
        return Status{ErrorCode::corrupt_store, 0, 36};
    }
    if (get_u32(mhdr.data() + v2_header_crc_offset) !=
        crc32c_with_hole(mhdr.data(), superblock_size, v2_header_crc_offset, sizeof(std::uint32_t))) {
        return Status{ErrorCode::corrupt_store, 0, v2_header_crc_offset};
    }

    const std::string_view manifest_key{
        reinterpret_cast<const char*>(mhdr.data() + superblock_key_offset), key_len};
    if (!config.session_key.empty() && config.session_key != manifest_key) {
        return Status{ErrorCode::identity_mismatch, 0, superblock_key_offset};
    }

    auto impl = std::make_unique<Impl>();
    impl->base_path = std::move(base);
    impl->session_key.assign(manifest_key.data(), manifest_key.size());
    impl->segment_bytes = segment_bytes;
    impl->capacity_bytes = capacity_bytes;
    impl->max_segments = max_segments;
    impl->audit_bytes = audit_bytes;
    impl->verify_crc = config.verify_crc;

    // Discover currently live segments (skipping any segment below checkpoint's first_segment_index
    // which may be mid-unlink right after retention publication).
    std::uint32_t cp_first_seg = 0;
    RecordId cp_lowest_id = 0;
    const auto cp = read_checkpoint_file(impl->base_path, impl->session_key, 16'777'216U);
    if (cp.valid) {
        cp_first_seg = cp.checkpoint.first_segment_index;
        cp_lowest_id = cp.checkpoint.lowest_retained_record_id;
    }

    std::vector<std::uint32_t> indices;
    if (const auto st = collect_tailer_segments(impl->base_path, indices); !st.ok()) return st;
    indices.erase(std::remove_if(indices.begin(), indices.end(),
        [cp_first_seg](std::uint32_t idx) { return idx < cp_first_seg; }), indices.end());

    if (config.start == JournalTailStart::from_id && config.start_id != 0) {
        const std::uint32_t target_seg = segment_index_from_id(config.start_id);
        if ((cp.valid && config.start_id < cp_lowest_id) ||
            (!indices.empty() && target_seg < indices.front())) {
            return Status{ErrorCode::history_unavailable, 0, static_cast<std::size_t>(config.start_id)};
        }
        impl->min_record_id = config.start_id;
        if (indices.empty()) {
            impl->current_seg_index = target_seg;
            impl->current_offset = superblock_size;
        } else if (target_seg <= indices.back()) {
            // Find the segment >= target_seg that exists.
            auto it = std::lower_bound(indices.begin(), indices.end(), target_seg);
            if (it == indices.end() || *it != target_seg) {
                return Status{ErrorCode::history_unavailable, 0, static_cast<std::size_t>(config.start_id)};
            }
            impl->current_seg_index = target_seg;
            impl->current_offset = superblock_size;
            Status err{};
            const auto r = open_segment_readonly(impl->base_path, target_seg, segment_bytes,
                impl->session_key, capacity_bytes, max_segments, audit_bytes, impl->current, err);
            if (r == OpenSegmentResult::error) return err;
            if (r == OpenSegmentResult::not_found) {
                return Status{ErrorCode::history_unavailable, 0, static_cast<std::size_t>(config.start_id)};
            }
        } else {
            // start_id points to a future segment not yet created; position at the tail of existing data.
            impl->current_seg_index = indices.back();
            impl->current_offset = superblock_size;
        }
    } else if (config.start == JournalTailStart::end) {
        if (indices.empty()) {
            impl->current_seg_index = cp_first_seg;
            impl->current_offset = superblock_size;
        } else {
            // Locate the highest segment that has at least one committed record at offset superblock_size.
            bool found_committed = false;
            for (std::size_t i = indices.size(); i > 0; --i) {
                const std::uint32_t seg_idx = indices[i - 1];
                MappedSegmentRO seg{};
                Status err{};
                const auto r = open_segment_readonly(impl->base_path, seg_idx, segment_bytes,
                    impl->session_key, capacity_bytes, max_segments, audit_bytes, seg, err);
                if (r == OpenSegmentResult::error) return err;
                if (r == OpenSegmentResult::not_found) continue;
                const auto first_commit = load_u32_acquire(
                    seg.bytes() + superblock_size + record_commit_offset);
                if (first_commit == record_commit_marker) {
                    std::size_t off = superblock_size;
                    RecordId last_id = 0;
                    while (off + record_header_size <= segment_bytes) {
                        const auto c = load_u32_acquire(seg.bytes() + off + record_commit_offset);
                        if (c == 0) break;
                        if (c != record_commit_marker) {
                            return Status{ErrorCode::corrupt_store, 0, off + record_commit_offset};
                        }
                        JournalRecord tmp{};
                        std::size_t total = 0;
                        if (const auto dec = decode_committed_record(seg.bytes(), seg_idx, off,
                                segment_bytes, impl->verify_crc, tmp, total); !dec.ok()) {
                            return dec;
                        }
                        last_id = tmp.id;
                        off += total;
                    }
                    impl->current = std::move(seg);
                    impl->current_seg_index = seg_idx;
                    impl->current_offset = off;
                    impl->cursor = last_id;
                    found_committed = true;
                    break;
                }
            }
            if (!found_committed) {
                impl->current_seg_index = indices.front();
                impl->current_offset = superblock_size;
            }
        }
    } else {
        // JournalTailStart::beginning
        if (indices.empty()) {
            impl->current_seg_index = cp_first_seg;
            impl->current_offset = superblock_size;
        } else {
            // Retry across concurrently pruned front segments if writer rotated during open().
            bool mapped = false;
            for (const std::uint32_t seg_idx : indices) {
                Status err{};
                const auto r = open_segment_readonly(impl->base_path, seg_idx, segment_bytes,
                    impl->session_key, capacity_bytes, max_segments, audit_bytes, impl->current, err);
                if (r == OpenSegmentResult::error) return err;
                if (r == OpenSegmentResult::ok) {
                    impl->current_seg_index = seg_idx;
                    impl->current_offset = superblock_size;
                    mapped = true;
                    break;
                }
            }
            if (!mapped) {
                impl->current_seg_index = indices.back();
                impl->current_offset = superblock_size;
            }
        }
    }

    return JournalTailer{std::move(impl)};
} catch (const std::bad_alloc&) {
    return Status{ErrorCode::capacity_exceeded, 0, 0};
} catch (...) {
    return Status{ErrorCode::io_error, 0, 0};
}

Status JournalTailer::next(JournalRecord& record) noexcept {
    if (!impl_) return Status{ErrorCode::not_ready, 0, 0};
    return impl_->next(record);
}

RecordId JournalTailer::cursor() const noexcept {
    return impl_ ? impl_->cursor : 0;
}

std::uint32_t JournalTailer::current_segment_index() const noexcept {
    return impl_ ? impl_->current_seg_index : 0;
}

std::string_view JournalTailer::session_key() const noexcept {
    return impl_ ? std::string_view{impl_->session_key} : std::string_view{};
}

std::size_t JournalTailer::segment_bytes() const noexcept {
    return impl_ ? impl_->segment_bytes : 0;
}

std::size_t JournalTailer::capacity_bytes() const noexcept {
    return impl_ ? impl_->capacity_bytes : 0;
}

std::size_t JournalTailer::max_segments() const noexcept {
    return impl_ ? impl_->max_segments : 0;
}

} // namespace nimblefix
