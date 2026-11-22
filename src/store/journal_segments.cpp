#include "journal_internal.hpp"

#include <cerrno>
#include <new>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace nimblefix::store {

Result<std::shared_ptr<Segment>> create_segment_atomic(
    std::string_view base_path,
    std::uint32_t index,
    std::size_t segment_bytes,
    std::string_view session_key,
    std::size_t capacity_bytes,
    std::size_t max_segments,
    std::size_t audit_bytes,
    const JournalTestHook& test_hook) noexcept try {
    const auto final_path = segment_path(base_path, index);
    const auto temp_path = make_temp_path(final_path);

    const int fd = ::open(temp_path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return io_status(errno);

    auto cleanup_fd = [&]() noexcept {
        (void)::close(fd);
        (void)::unlink(temp_path.c_str());
    };

    if (::ftruncate(fd, static_cast<off_t>(segment_bytes)) != 0) {
        const int saved = errno;
        cleanup_fd();
        return io_status(saved);
    }
    const int alloc_err = ::posix_fallocate(fd, 0, static_cast<off_t>(segment_bytes));
    if (alloc_err != 0) {
        cleanup_fd();
        return io_status(alloc_err);
    }

    int map_flags = MAP_SHARED;
#if defined(MAP_POPULATE)
    map_flags |= MAP_POPULATE;
#endif
    void* mapping = ::mmap(nullptr, segment_bytes, PROT_READ | PROT_WRITE, map_flags, fd, 0);
    if (mapping == MAP_FAILED) {
        const int saved = errno;
        cleanup_fd();
        return io_status(saved);
    }

    std::shared_ptr<Segment> segment;
    try {
        segment = std::make_shared<Segment>();
        segment->fd = fd;
        segment->mapping = mapping;
        segment->mapping_size = segment_bytes;
        segment->index = index;
        segment->path = final_path;
    } catch (...) {
        (void)::munmap(mapping, segment_bytes);
        cleanup_fd();
        return Status{ErrorCode::capacity_exceeded, 0, 0};
    }

    write_v2_header(static_cast<unsigned char*>(mapping), v2_segment_magic,
                    segment_bytes, session_key, segment_bytes, capacity_bytes,
                    max_segments, index, audit_bytes);
    touch_mapping(mapping, segment_bytes);
    (void)::madvise(mapping, segment_bytes, MADV_WILLNEED);

    // Atomically publish the initialized segment file at its final name.
    if (::rename(temp_path.c_str(), final_path.c_str()) != 0) {
        const int saved = errno;
        (void)::unlink(temp_path.c_str());
        return io_status(saved);
    }

    if (test_hook.callback != nullptr)
        test_hook.callback(JournalFaultPoint::segment_created, test_hook.context);

    return segment;
} catch (const std::bad_alloc&) {
    return Status{ErrorCode::capacity_exceeded, 0, 0};
} catch (...) {
    return Status{ErrorCode::io_error, 0, 0};
}

Result<std::shared_ptr<Segment>> open_existing_segment(
    std::string_view base_path,
    std::uint32_t index,
    std::size_t segment_bytes,
    std::string_view session_key,
    std::size_t capacity_bytes,
    std::size_t max_segments,
    std::size_t audit_bytes) noexcept try {
    const auto data_path = segment_path(base_path, index);
    const int fd = ::open(data_path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return io_status(errno);

    struct stat info {};
    if (::fstat(fd, &info) != 0) {
        const int saved = errno;
        ::close(fd);
        return io_status(saved);
    }
    if (!S_ISREG(info.st_mode) || info.st_uid != ::geteuid() || (info.st_mode & 0077) != 0) {
        ::close(fd);
        return Status{ErrorCode::invalid_config, 0, 0};
    }
    if (info.st_size != static_cast<off_t>(segment_bytes)) {
        ::close(fd);
        return Status{ErrorCode::corrupt_store, 0, 16};
    }

    int flags = MAP_SHARED;
#if defined(MAP_POPULATE)
    flags |= MAP_POPULATE;
#endif
    void* mapping = ::mmap(nullptr, segment_bytes, PROT_READ | PROT_WRITE, flags, fd, 0);
    if (mapping == MAP_FAILED) {
        const int saved = errno;
        ::close(fd);
        return io_status(saved);
    }

    std::shared_ptr<Segment> segment;
    try {
        segment = std::make_shared<Segment>();
        segment->fd = fd;
        segment->mapping = mapping;
        segment->mapping_size = segment_bytes;
        segment->index = index;
        segment->path = data_path;
    } catch (...) {
        (void)::munmap(mapping, segment_bytes);
        ::close(fd);
        return Status{ErrorCode::capacity_exceeded, 0, 0};
    }

    (void)::madvise(mapping, segment_bytes, MADV_WILLNEED);
    const auto status = validate_v2_header(static_cast<const unsigned char*>(mapping),
        v2_segment_magic, segment_bytes, session_key, segment_bytes, capacity_bytes,
        max_segments, index, audit_bytes);
    if (!status.ok()) return status;

    return segment;
} catch (const std::bad_alloc&) {
    return Status{ErrorCode::capacity_exceeded, 0, 0};
} catch (...) {
    return Status{ErrorCode::io_error, 0, 0};
}

std::shared_ptr<void> acquire_segment_lease(std::shared_ptr<Segment> segment) noexcept {
    if (!segment) return {};
    segment->active_leases.fetch_add(1, std::memory_order_acq_rel);
    auto* raw = segment.get();
    try {
        return std::shared_ptr<void>(raw, [held = std::move(segment)](void*) noexcept {
            held->active_leases.fetch_sub(1, std::memory_order_release);
        });
    } catch (...) {
        segment->active_leases.fetch_sub(1, std::memory_order_release);
        return {};
    }
}

} // namespace nimblefix::store
