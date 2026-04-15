#include "fastfix/store/durable_batch_store.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fastfix::store {

namespace {

constexpr std::uint32_t kStoreVersion = 1U;
constexpr std::uint64_t kNanosPerDay = 86'400ULL * 1'000'000'000ULL;
constexpr std::array<char, 8> kLogMagic = {'F', 'F', 'D', 'B', 'L', 'O', 'G', '1'};
constexpr std::array<char, 8> kIndexMagic = {'F', 'F', 'D', 'B', 'I', 'D', 'X', '1'};
constexpr std::array<char, 8> kRecoveryMagic = {'F', 'F', 'D', 'B', 'R', 'E', 'C', '1'};
constexpr std::size_t kPendingPayloadArenaFloorBytes = 16U * 1024U;
constexpr std::size_t kPendingPayloadBytesPerEntry = 512U;
constexpr std::string_view kSegmentFilePrefix = "segment-";
constexpr std::size_t kSegmentIdWidth = 16U;
constexpr std::string_view kSegmentLogSuffix = ".log";

enum class StoreRecordType : std::uint32_t {
    kOutbound = 1U,
    kInbound = 2U,
};

enum class PendingEntryKind : std::uint32_t {
    kOutbound = 1U,
    kInbound = 2U,
    kRecovery = 3U,
};

#pragma pack(push, 1)
struct StoreFileHeader {
    char magic[8];
    std::uint32_t version;
    std::uint32_t header_size;
    std::uint64_t file_size;
    std::uint64_t segment_id;
};

struct StoreRecordHeader {
    std::uint32_t record_type;
    std::uint16_t header_size;
    std::uint16_t flags;
    std::uint64_t session_id;
    std::uint32_t seq_num;
    std::uint32_t payload_size;
    std::uint64_t timestamp_ns;
    std::uint32_t reserved0;
    std::uint32_t reserved1;
    std::uint64_t reserved2;
    std::uint64_t reserved3;
    std::uint64_t reserved4;
};

struct OutboundIndexEntry {
    std::uint64_t session_id;
    std::uint32_t seq_num;
    std::uint16_t flags;
    std::uint16_t reserved0;
    std::uint64_t timestamp_ns;
    std::uint64_t log_offset;
    std::uint32_t payload_size;
    std::uint32_t reserved1;
};

struct RecoveryRecord {
    std::uint64_t session_id;
    std::uint32_t next_in_seq;
    std::uint32_t next_out_seq;
    std::uint64_t last_inbound_ns;
    std::uint64_t last_outbound_ns;
    std::uint8_t active;
    std::uint8_t reserved[7];
};
#pragma pack(pop)

static_assert(sizeof(StoreFileHeader) == 32U);
static_assert(sizeof(StoreRecordHeader) == 64U);
static_assert(sizeof(OutboundIndexEntry) == 40U);
static_assert(sizeof(RecoveryRecord) == 40U);

struct OpenedStoreFile {
    int fd{-1};
    std::uint64_t size{0};
    StoreFileHeader header{};
};

struct OutboundLocation {
    std::uint64_t segment_id{0};
    std::uint64_t log_offset{0};
    std::uint32_t payload_size{0};
    std::uint16_t flags{0};
    std::uint64_t timestamp_ns{0};
};

struct SegmentHandle {
    std::uint64_t segment_id{0};
    std::filesystem::path log_path;
    std::filesystem::path index_path;
    int log_fd{-1};
    int index_fd{-1};
    std::uint64_t log_size{0};
    std::uint64_t index_size{0};
    bool has_records{false};
};

struct PendingEntry {
    PendingEntryKind kind{PendingEntryKind::kOutbound};
    std::uint64_t session_id{0};
    std::uint32_t seq_num{0};
    std::uint64_t timestamp_ns{0};
    std::uint16_t flags{0};
    std::uint32_t payload_offset{0};
    std::uint32_t payload_size{0};
    SessionRecoveryState recovery_state;
};

auto IoErrorMessage(const std::filesystem::path& path, const char* action) -> std::string {
    return action + std::string(" '") + path.string() + "': " + std::strerror(errno);
}

auto ValidateRecord(const MessageRecord& record) -> base::Status {
    if (record.session_id == 0) {
        return base::Status::InvalidArgument("message record is missing session_id");
    }
    if (record.seq_num == 0) {
        return base::Status::InvalidArgument("message record is missing seq_num");
    }
    if (record.payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        return base::Status::InvalidArgument("message payload exceeds durable store limits");
    }
    return base::Status::Ok();
}

auto ValidateRecordView(const MessageRecordView& record) -> base::Status {
    if (record.session_id == 0) {
        return base::Status::InvalidArgument("message record is missing session_id");
    }
    if (record.seq_num == 0) {
        return base::Status::InvalidArgument("message record is missing seq_num");
    }
    if (record.payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        return base::Status::InvalidArgument("message payload exceeds durable store limits");
    }
    return base::Status::Ok();
}

auto ValidateRecoveryState(const SessionRecoveryState& state) -> base::Status {
    if (state.session_id == 0) {
        return base::Status::InvalidArgument("recovery state is missing session_id");
    }
    return base::Status::Ok();
}

auto CloseFd(int& fd) -> void {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

auto CloseSegment(SegmentHandle& segment) -> void {
    CloseFd(segment.log_fd);
    CloseFd(segment.index_fd);
}

auto HasMagic(const StoreFileHeader& header, const std::array<char, 8>& expected) -> bool {
    return std::memcmp(header.magic, expected.data(), expected.size()) == 0;
}

auto WriteExact(int fd, const void* data, std::size_t size, std::uint64_t offset) -> bool {
    const auto* bytes = static_cast<const std::byte*>(data);
    std::size_t written = 0;
    while (written < size) {
        const auto result = ::pwrite(
            fd,
            bytes + written,
            size - written,
            static_cast<off_t>(offset + written));
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (result == 0) {
            return false;
        }
        written += static_cast<std::size_t>(result);
    }
    return true;
}

auto ReadExact(int fd, void* data, std::size_t size, std::uint64_t offset) -> bool {
    auto* bytes = static_cast<std::byte*>(data);
    std::size_t read = 0;
    while (read < size) {
        const auto result = ::pread(
            fd,
            bytes + read,
            size - read,
            static_cast<off_t>(offset + read));
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (result == 0) {
            return false;
        }
        read += static_cast<std::size_t>(result);
    }
    return true;
}

auto EnsureDirectory(const std::filesystem::path& path) -> base::Status {
    std::error_code error;
    if (path.empty()) {
        return base::Status::InvalidArgument("durable batch store requires a root directory");
    }
    std::filesystem::create_directories(path, error);
    if (error) {
        return base::Status::IoError(
            "unable to create durable batch store directory '" + path.string() + "': " + error.message());
    }
    if (!std::filesystem::is_directory(path, error)) {
        return base::Status::InvalidArgument("durable batch store root must be a directory");
    }
    return base::Status::Ok();
}

auto BuildFileHeader(const std::array<char, 8>& magic, std::uint64_t segment_id) -> StoreFileHeader {
    StoreFileHeader header{};
    std::memcpy(header.magic, magic.data(), magic.size());
    header.version = kStoreVersion;
    header.header_size = sizeof(StoreFileHeader);
    header.file_size = sizeof(StoreFileHeader);
    header.segment_id = segment_id;
    return header;
}

auto SyncFileHeader(int fd, StoreFileHeader* header, std::uint64_t size) -> bool {
    header->file_size = size;
    return WriteExact(fd, header, sizeof(*header), 0U);
}

auto OpenStoreFile(
    const std::filesystem::path& path,
    const std::array<char, 8>& magic,
    std::uint64_t default_segment_id,
    std::optional<std::uint64_t> expected_segment_id = std::nullopt)
    -> base::Result<OpenedStoreFile> {
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        return base::Status::IoError(IoErrorMessage(path, "unable to open durable batch store file"));
    }

    struct stat stat_buffer {};
    if (::fstat(fd, &stat_buffer) != 0) {
        CloseFd(fd);
        return base::Status::IoError(IoErrorMessage(path, "unable to stat durable batch store file"));
    }

    if (stat_buffer.st_size == 0) {
        auto header = BuildFileHeader(magic, default_segment_id);
        if (::ftruncate(fd, static_cast<off_t>(sizeof(StoreFileHeader))) != 0 ||
            !WriteExact(fd, &header, sizeof(header), 0U)) {
            const auto message = IoErrorMessage(path, "unable to initialize durable batch store file");
            CloseFd(fd);
            return base::Status::IoError(message);
        }
        stat_buffer.st_size = sizeof(StoreFileHeader);
    }

    if (stat_buffer.st_size < static_cast<off_t>(sizeof(StoreFileHeader))) {
        CloseFd(fd);
        return base::Status::FormatError("durable batch store file is smaller than its header");
    }

    OpenedStoreFile opened;
    opened.fd = fd;
    opened.size = static_cast<std::uint64_t>(stat_buffer.st_size);
    if (!ReadExact(opened.fd, &opened.header, sizeof(opened.header), 0U)) {
        const auto message = IoErrorMessage(path, "unable to read durable batch store header");
        CloseFd(opened.fd);
        return base::Status::IoError(message);
    }

    if (!HasMagic(opened.header, magic) || opened.header.version != kStoreVersion ||
        opened.header.header_size != sizeof(StoreFileHeader)) {
        CloseFd(opened.fd);
        return base::Status::FormatError("durable batch store file has an invalid header");
    }

    if (expected_segment_id.has_value() && opened.header.segment_id != expected_segment_id.value()) {
        CloseFd(opened.fd);
        return base::Status::FormatError("durable batch store file segment id does not match its companion file");
    }

    if (opened.header.file_size != opened.size) {
        if (!SyncFileHeader(opened.fd, &opened.header, opened.size)) {
            const auto message = IoErrorMessage(path, "unable to repair durable batch store header");
            CloseFd(opened.fd);
            return base::Status::IoError(message);
        }
    }

    return opened;
}

auto MakeSegmentFileName(std::uint64_t segment_id, std::string_view suffix) -> std::string {
    std::ostringstream stream;
    stream << kSegmentFilePrefix << std::setw(kSegmentIdWidth) << std::setfill('0') << segment_id << suffix;
    return stream.str();
}

auto ParseSegmentId(const std::filesystem::path& path) -> std::optional<std::uint64_t> {
    const auto filename = path.filename().string();
    if (!filename.starts_with(kSegmentFilePrefix) || !filename.ends_with(kSegmentLogSuffix)) {
        return std::nullopt;
    }

    const auto digits = filename.substr(
        kSegmentFilePrefix.size(),
        filename.size() - kSegmentFilePrefix.size() - kSegmentLogSuffix.size());
    try {
        return static_cast<std::uint64_t>(std::stoull(digits, nullptr, 10));
    } catch (...) {
        return std::nullopt;
    }
}

auto OpenSegmentHandle(
    const std::filesystem::path& log_path,
    const std::filesystem::path& index_path,
    std::uint64_t default_segment_id,
    std::optional<std::uint64_t> expected_segment_id = std::nullopt)
    -> base::Result<SegmentHandle> {
    auto log = OpenStoreFile(log_path, kLogMagic, default_segment_id, expected_segment_id);
    if (!log.ok()) {
        return log.status();
    }
    auto index = OpenStoreFile(index_path, kIndexMagic, log.value().header.segment_id, log.value().header.segment_id);
    if (!index.ok()) {
        CloseFd(log.value().fd);
        return index.status();
    }

    SegmentHandle segment;
    segment.segment_id = log.value().header.segment_id;
    segment.log_path = log_path;
    segment.index_path = index_path;
    segment.log_fd = log.value().fd;
    segment.index_fd = index.value().fd;
    segment.log_size = log.value().size;
    segment.index_size = index.value().size;
    segment.has_records = segment.log_size > sizeof(StoreFileHeader);
    return segment;
}

auto LoadSegmentIndex(
    const SegmentHandle& segment,
    std::unordered_map<std::uint64_t, std::map<std::uint32_t, OutboundLocation>>* outbound_index)
    -> base::Status {
    std::uint64_t offset = sizeof(StoreFileHeader);
    while (offset < segment.index_size) {
        if (offset + sizeof(OutboundIndexEntry) > segment.index_size) {
            return base::Status::FormatError("durable batch store index contains a truncated entry");
        }

        OutboundIndexEntry entry{};
        if (!ReadExact(segment.index_fd, &entry, sizeof(entry), offset)) {
            return base::Status::IoError(
                IoErrorMessage(segment.index_path, "unable to read durable batch index entry"));
        }
        if (entry.log_offset + sizeof(StoreRecordHeader) + entry.payload_size > segment.log_size) {
            return base::Status::FormatError("durable batch store index points past the end of the log");
        }

        (*outbound_index)[entry.session_id][entry.seq_num] = OutboundLocation{
            .segment_id = segment.segment_id,
            .log_offset = entry.log_offset,
            .payload_size = entry.payload_size,
            .flags = entry.flags,
            .timestamp_ns = entry.timestamp_ns,
        };
        offset += sizeof(OutboundIndexEntry);
    }

    return base::Status::Ok();
}

auto LoadRecoveryLog(
    int fd,
    std::uint64_t file_size,
    const std::filesystem::path& path,
    std::unordered_map<std::uint64_t, SessionRecoveryState>* recovery_states)
    -> base::Status {
    std::uint64_t offset = sizeof(StoreFileHeader);
    while (offset < file_size) {
        if (offset + sizeof(RecoveryRecord) > file_size) {
            return base::Status::FormatError("durable batch store recovery log contains a truncated entry");
        }

        RecoveryRecord record{};
        if (!ReadExact(fd, &record, sizeof(record), offset)) {
            return base::Status::IoError(IoErrorMessage(path, "unable to read durable recovery record"));
        }

        (*recovery_states)[record.session_id] = SessionRecoveryState{
            .session_id = record.session_id,
            .next_in_seq = record.next_in_seq,
            .next_out_seq = record.next_out_seq,
            .last_inbound_ns = record.last_inbound_ns,
            .last_outbound_ns = record.last_outbound_ns,
            .active = record.active != 0,
        };
        offset += sizeof(RecoveryRecord);
    }

    return base::Status::Ok();
}

auto DetectActiveUtcDay(const SegmentHandle& segment, std::int64_t offset_ns = 0) -> base::Result<std::optional<std::int64_t>> {
    std::uint64_t offset = sizeof(StoreFileHeader);
    std::optional<std::int64_t> day;
    while (offset < segment.log_size) {
        if (offset + sizeof(StoreRecordHeader) > segment.log_size) {
            return base::Status::FormatError("durable batch store log contains a truncated record header");
        }

        StoreRecordHeader header{};
        if (!ReadExact(segment.log_fd, &header, sizeof(header), offset)) {
            return base::Status::IoError(IoErrorMessage(segment.log_path, "unable to read durable log record"));
        }
        if (header.header_size != sizeof(StoreRecordHeader)) {
            return base::Status::FormatError("durable batch store log contains a record with an unexpected header size");
        }
        const auto record_end = offset + sizeof(StoreRecordHeader) + header.payload_size;
        if (record_end > segment.log_size) {
            return base::Status::FormatError("durable batch store log contains a truncated payload");
        }
        if ((header.record_type == static_cast<std::uint32_t>(StoreRecordType::kOutbound) ||
             header.record_type == static_cast<std::uint32_t>(StoreRecordType::kInbound)) &&
            header.timestamp_ns != 0) {
            day = (static_cast<std::int64_t>(header.timestamp_ns) + offset_ns) / static_cast<std::int64_t>(kNanosPerDay);
        }
        offset = record_end;
    }

    return day;
}

auto RemoveSegmentEntries(
    std::unordered_map<std::uint64_t, std::map<std::uint32_t, OutboundLocation>>* outbound_index,
    std::uint64_t segment_id) -> void {
    for (auto session_it = outbound_index->begin(); session_it != outbound_index->end();) {
        auto& ranges = session_it->second;
        for (auto range_it = ranges.begin(); range_it != ranges.end();) {
            if (range_it->second.segment_id == segment_id) {
                range_it = ranges.erase(range_it);
            } else {
                ++range_it;
            }
        }
        if (ranges.empty()) {
            session_it = outbound_index->erase(session_it);
        } else {
            ++session_it;
        }
    }
}

auto SyncDataFile(int fd) -> base::Status {
    if (::fdatasync(fd) != 0) {
        return base::Status::IoError(std::string("fdatasync failed: ") + std::strerror(errno));
    }
    return base::Status::Ok();
}

}  // namespace

struct DurableBatchSessionStore::Impl {
    SegmentHandle active_segment;
    std::map<std::uint64_t, SegmentHandle> archived_segments;
    std::filesystem::path recovery_path;
    int recovery_fd{-1};
    std::uint64_t recovery_size{0};
    std::unordered_map<std::uint64_t, std::map<std::uint32_t, OutboundLocation>> outbound_index;
    std::unordered_map<std::uint64_t, SessionRecoveryState> recovery_states;
    std::vector<PendingEntry> pending_entries;
    std::vector<std::byte> pending_payload_arena;
    std::optional<std::int64_t> active_utc_day;
    std::int32_t effective_utc_offset_seconds{0};

    ~Impl() {
        CloseSegment(active_segment);
        for (auto& [segment_id, segment] : archived_segments) {
            (void)segment_id;
            CloseSegment(segment);
        }
        CloseFd(recovery_fd);
    }
};

namespace {

auto ResolveSegmentHandle(
    const DurableBatchSessionStore::Impl& impl,
    std::uint64_t segment_id) -> const SegmentHandle* {
    if (impl.active_segment.segment_id == segment_id) {
        return &impl.active_segment;
    }

    const auto archived = impl.archived_segments.find(segment_id);
    if (archived == impl.archived_segments.end()) {
        return nullptr;
    }
    return &archived->second;
}

}  // namespace

DurableBatchSessionStore::DurableBatchSessionStore(
    std::filesystem::path root,
    DurableBatchStoreOptions options)
    : root_(std::move(root)), options_(std::move(options)), impl_(std::make_unique<Impl>()) {
    if (options_.flush_threshold == 0U) {
        options_.flush_threshold = 1U;
    }
    impl_->pending_entries.reserve(options_.flush_threshold * 2U);
    impl_->pending_payload_arena.reserve(std::max<std::size_t>(
        kPendingPayloadArenaFloorBytes,
        options_.flush_threshold * kPendingPayloadBytesPerEntry));
}

DurableBatchSessionStore::~DurableBatchSessionStore() {
    static_cast<void>(Flush());
}

auto DurableBatchSessionStore::Open() -> base::Status {
    if (impl_->active_segment.log_fd >= 0 && impl_->active_segment.index_fd >= 0 && impl_->recovery_fd >= 0) {
        return base::Status::Ok();
    }

    auto status = EnsureDirectory(root_);
    if (!status.ok()) {
        return status;
    }

    const auto archive_dir = root_ / "archive";
    status = EnsureDirectory(archive_dir);
    if (!status.ok()) {
        return status;
    }

    CloseSegment(impl_->active_segment);
    for (auto& [segment_id, segment] : impl_->archived_segments) {
        (void)segment_id;
        CloseSegment(segment);
    }
    impl_->archived_segments.clear();
    impl_->outbound_index.clear();
    impl_->recovery_states.clear();
    CloseFd(impl_->recovery_fd);
    impl_->recovery_size = 0;
    impl_->active_utc_day.reset();

    std::vector<std::uint64_t> archive_ids;
    std::error_code iter_error;
    for (const auto& entry : std::filesystem::directory_iterator(archive_dir, iter_error)) {
        if (iter_error) {
            return base::Status::IoError(
                "unable to enumerate durable batch archive directory '" + archive_dir.string() + "': " +
                iter_error.message());
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        const auto segment_id = ParseSegmentId(entry.path());
        if (segment_id.has_value()) {
            archive_ids.push_back(segment_id.value());
        }
    }

    std::sort(archive_ids.begin(), archive_ids.end());
    archive_ids.erase(std::unique(archive_ids.begin(), archive_ids.end()), archive_ids.end());
    for (const auto segment_id : archive_ids) {
        auto segment = OpenSegmentHandle(
            archive_dir / MakeSegmentFileName(segment_id, ".log"),
            archive_dir / MakeSegmentFileName(segment_id, ".out.idx"),
            segment_id,
            segment_id);
        if (!segment.ok()) {
            return segment.status();
        }
        status = LoadSegmentIndex(segment.value(), &impl_->outbound_index);
        if (!status.ok()) {
            return status;
        }
        impl_->archived_segments.emplace(segment_id, std::move(segment).value());
    }

    const auto active_log_path = root_ / "active.log";
    const auto active_index_path = root_ / "active.out.idx";
    std::error_code exists_error;
    const bool active_log_exists = std::filesystem::exists(active_log_path, exists_error);
    if (exists_error) {
        return base::Status::IoError(
            "unable to inspect durable batch active log path '" + active_log_path.string() + "': " +
            exists_error.message());
    }
    const bool active_index_exists = std::filesystem::exists(active_index_path, exists_error);
    if (exists_error) {
        return base::Status::IoError(
            "unable to inspect durable batch active index path '" + active_index_path.string() + "': " +
            exists_error.message());
    }
    if (active_log_exists != active_index_exists) {
        return base::Status::FormatError("durable batch store active segment is missing a companion file");
    }

    const auto default_segment_id = archive_ids.empty() ? 1U : archive_ids.back() + 1U;
    auto active = OpenSegmentHandle(
        active_log_path,
        active_index_path,
        default_segment_id,
        active_log_exists ? std::optional<std::uint64_t>{} : std::optional<std::uint64_t>{default_segment_id});
    if (!active.ok()) {
        return active.status();
    }
    status = LoadSegmentIndex(active.value(), &impl_->outbound_index);
    if (!status.ok()) {
        return status;
    }
    impl_->active_segment = std::move(active).value();

    if (options_.rollover_mode == DurableStoreRolloverMode::kLocalTime) {
        if (options_.use_system_timezone) {
            auto now = std::time(nullptr);
            auto* local = std::localtime(&now);
            impl_->effective_utc_offset_seconds = static_cast<std::int32_t>(local->tm_gmtoff);
        } else {
            impl_->effective_utc_offset_seconds = options_.local_utc_offset_seconds;
        }
    }

    const auto day_offset_ns = (options_.rollover_mode == DurableStoreRolloverMode::kLocalTime)
        ? static_cast<std::int64_t>(impl_->effective_utc_offset_seconds) * 1'000'000'000LL
        : 0LL;
    auto active_day = DetectActiveUtcDay(impl_->active_segment, day_offset_ns);
    if (!active_day.ok()) {
        return active_day.status();
    }
    impl_->active_utc_day = std::move(active_day).value();

    impl_->recovery_path = root_ / "recovery.log";
    auto recovery_file = OpenStoreFile(impl_->recovery_path, kRecoveryMagic, 0U, 0U);
    if (!recovery_file.ok()) {
        return recovery_file.status();
    }
    impl_->recovery_fd = recovery_file.value().fd;
    impl_->recovery_size = recovery_file.value().size;
    status = LoadRecoveryLog(
        impl_->recovery_fd,
        impl_->recovery_size,
        impl_->recovery_path,
        &impl_->recovery_states);
    if (!status.ok()) {
        return status;
    }

    if (options_.max_archived_segments != 0U) {
        while (impl_->archived_segments.size() > options_.max_archived_segments) {
            auto oldest = impl_->archived_segments.begin();
            RemoveSegmentEntries(&impl_->outbound_index, oldest->first);
            CloseSegment(oldest->second);
            std::error_code remove_error;
            std::filesystem::remove(oldest->second.log_path, remove_error);
            if (remove_error) {
                return base::Status::IoError(
                    "unable to prune archived durable store log '" + oldest->second.log_path.string() + "': " +
                    remove_error.message());
            }
            std::filesystem::remove(oldest->second.index_path, remove_error);
            if (remove_error) {
                return base::Status::IoError(
                    "unable to prune archived durable store index '" + oldest->second.index_path.string() + "': " +
                    remove_error.message());
            }
            impl_->archived_segments.erase(oldest);
        }
    }

    return base::Status::Ok();
}

auto DurableBatchSessionStore::AppendMessage(bool outbound, const MessageRecord& record) -> base::Status {
    auto status = ValidateRecord(record);
    if (!status.ok()) {
        return status;
    }

    return AppendMessageView(outbound, record.view());
}

auto DurableBatchSessionStore::AppendMessageView(bool outbound, const MessageRecordView& record) -> base::Status {
    auto status = ValidateRecordView(record);
    if (!status.ok()) {
        return status;
    }

    status = Open();
    if (!status.ok()) {
        return status;
    }

    if (options_.rollover_mode == DurableStoreRolloverMode::kUtcDay) {
        const auto record_day = static_cast<std::int64_t>(record.timestamp_ns / kNanosPerDay);
        const bool active_has_messages = impl_->active_segment.has_records ||
            std::any_of(
                impl_->pending_entries.begin(),
                impl_->pending_entries.end(),
                [](const PendingEntry& entry) {
                    return entry.kind == PendingEntryKind::kOutbound || entry.kind == PendingEntryKind::kInbound;
                });
        if (active_has_messages && impl_->active_utc_day.has_value() && impl_->active_utc_day.value() != record_day) {
            status = Rollover();
            if (!status.ok()) {
                return status;
            }
        }
        if (!impl_->active_utc_day.has_value()) {
            impl_->active_utc_day = record_day;
        }
    } else if (options_.rollover_mode == DurableStoreRolloverMode::kLocalTime) {
        const auto offset_ns = static_cast<std::int64_t>(impl_->effective_utc_offset_seconds) * 1'000'000'000LL;
        const auto local_ns = static_cast<std::int64_t>(record.timestamp_ns) + offset_ns;
        const auto record_day = local_ns / static_cast<std::int64_t>(kNanosPerDay);
        const bool active_has_messages = impl_->active_segment.has_records ||
            std::any_of(
                impl_->pending_entries.begin(),
                impl_->pending_entries.end(),
                [](const PendingEntry& entry) {
                    return entry.kind == PendingEntryKind::kOutbound || entry.kind == PendingEntryKind::kInbound;
                });
        if (active_has_messages && impl_->active_utc_day.has_value() && impl_->active_utc_day.value() != record_day) {
            status = Rollover();
            if (!status.ok()) {
                return status;
            }
        }
        if (!impl_->active_utc_day.has_value()) {
            impl_->active_utc_day = record_day;
        }
    }

    PendingEntry entry;
    entry.kind = outbound ? PendingEntryKind::kOutbound : PendingEntryKind::kInbound;
    entry.session_id = record.session_id;
    entry.seq_num = record.seq_num;
    entry.timestamp_ns = record.timestamp_ns;
    entry.flags = record.flags;
    entry.payload_offset = static_cast<std::uint32_t>(impl_->pending_payload_arena.size());
    entry.payload_size = static_cast<std::uint32_t>(record.payload.size());
    impl_->pending_payload_arena.insert(
        impl_->pending_payload_arena.end(),
        record.payload.begin(),
        record.payload.end());
    impl_->pending_entries.push_back(std::move(entry));

    if (impl_->pending_entries.size() >= options_.flush_threshold) {
        return Flush();
    }
    return base::Status::Ok();
}

auto DurableBatchSessionStore::QueueRecoveryState(const SessionRecoveryState& state) -> base::Status {
    auto status = ValidateRecoveryState(state);
    if (!status.ok()) {
        return status;
    }

    status = Open();
    if (!status.ok()) {
        return status;
    }

    PendingEntry entry;
    entry.kind = PendingEntryKind::kRecovery;
    entry.recovery_state = state;
    impl_->pending_entries.push_back(std::move(entry));

    if (impl_->pending_entries.size() >= options_.flush_threshold) {
        return Flush();
    }
    return base::Status::Ok();
}

auto DurableBatchSessionStore::Flush() -> base::Status {
    auto status = Open();
    if (!status.ok()) {
        return status;
    }
    if (impl_->pending_entries.empty()) {
        return base::Status::Ok();
    }

    StoreFileHeader active_log_header{};
    if (!ReadExact(impl_->active_segment.log_fd, &active_log_header, sizeof(active_log_header), 0U)) {
        return base::Status::IoError(
            IoErrorMessage(impl_->active_segment.log_path, "unable to read durable batch log header"));
    }
    StoreFileHeader active_index_header{};
    if (!ReadExact(impl_->active_segment.index_fd, &active_index_header, sizeof(active_index_header), 0U)) {
        return base::Status::IoError(
            IoErrorMessage(impl_->active_segment.index_path, "unable to read durable batch index header"));
    }
    StoreFileHeader recovery_header{};
    if (!ReadExact(impl_->recovery_fd, &recovery_header, sizeof(recovery_header), 0U)) {
        return base::Status::IoError(
            IoErrorMessage(impl_->recovery_path, "unable to read durable batch recovery header"));
    }

    bool wrote_log = false;
    bool wrote_index = false;
    bool wrote_recovery = false;
    for (const auto& entry : impl_->pending_entries) {
        if (entry.kind == PendingEntryKind::kRecovery) {
            RecoveryRecord record{};
            record.session_id = entry.recovery_state.session_id;
            record.next_in_seq = entry.recovery_state.next_in_seq;
            record.next_out_seq = entry.recovery_state.next_out_seq;
            record.last_inbound_ns = entry.recovery_state.last_inbound_ns;
            record.last_outbound_ns = entry.recovery_state.last_outbound_ns;
            record.active = entry.recovery_state.active ? 1U : 0U;
            if (!WriteExact(impl_->recovery_fd, &record, sizeof(record), impl_->recovery_size)) {
                return base::Status::IoError(
                    IoErrorMessage(impl_->recovery_path, "unable to append durable recovery record"));
            }
            impl_->recovery_size += sizeof(record);
            impl_->recovery_states[record.session_id] = entry.recovery_state;
            wrote_recovery = true;
            continue;
        }

        const bool outbound = entry.kind == PendingEntryKind::kOutbound;
        StoreRecordHeader header{};
        header.record_type = static_cast<std::uint32_t>(
            outbound ? StoreRecordType::kOutbound : StoreRecordType::kInbound);
        header.header_size = sizeof(StoreRecordHeader);
        header.flags = entry.flags;
        header.session_id = entry.session_id;
        header.seq_num = entry.seq_num;
        header.payload_size = entry.payload_size;
        header.timestamp_ns = entry.timestamp_ns;

        const auto log_offset = impl_->active_segment.log_size;
        if (!WriteExact(impl_->active_segment.log_fd, &header, sizeof(header), log_offset)) {
            return base::Status::IoError(
                IoErrorMessage(impl_->active_segment.log_path, "unable to append durable log header"));
        }
        if (entry.payload_size != 0U &&
            !WriteExact(
                impl_->active_segment.log_fd,
                impl_->pending_payload_arena.data() + entry.payload_offset,
                entry.payload_size,
                log_offset + sizeof(StoreRecordHeader))) {
            return base::Status::IoError(
                IoErrorMessage(impl_->active_segment.log_path, "unable to append durable log payload"));
        }
        impl_->active_segment.log_size += sizeof(StoreRecordHeader) + entry.payload_size;
        impl_->active_segment.has_records = true;
        wrote_log = true;

        if (outbound) {
            OutboundIndexEntry index_entry{};
            index_entry.session_id = entry.session_id;
            index_entry.seq_num = entry.seq_num;
            index_entry.flags = entry.flags;
            index_entry.timestamp_ns = entry.timestamp_ns;
            index_entry.log_offset = log_offset;
            index_entry.payload_size = entry.payload_size;
            if (!WriteExact(
                    impl_->active_segment.index_fd,
                    &index_entry,
                    sizeof(index_entry),
                    impl_->active_segment.index_size)) {
                return base::Status::IoError(
                    IoErrorMessage(impl_->active_segment.index_path, "unable to append durable index entry"));
            }
            impl_->active_segment.index_size += sizeof(index_entry);
            impl_->outbound_index[index_entry.session_id][index_entry.seq_num] = OutboundLocation{
                .segment_id = impl_->active_segment.segment_id,
                .log_offset = index_entry.log_offset,
                .payload_size = index_entry.payload_size,
                .flags = index_entry.flags,
                .timestamp_ns = index_entry.timestamp_ns,
            };
            wrote_index = true;
        }
    }

    // Crash-safe WAL semantics: sync data to disk BEFORE updating headers.
    // This ensures headers never reference unsynced data.
    if (wrote_log) {
        status = SyncDataFile(impl_->active_segment.log_fd);
        if (!status.ok()) {
            return status;
        }
    }
    if (wrote_index) {
        status = SyncDataFile(impl_->active_segment.index_fd);
        if (!status.ok()) {
            return status;
        }
    }
    if (wrote_recovery) {
        status = SyncDataFile(impl_->recovery_fd);
        if (!status.ok()) {
            return status;
        }
    }

    // Now that data is durable, update headers to commit the new sizes.
    if (wrote_log && !SyncFileHeader(impl_->active_segment.log_fd, &active_log_header, impl_->active_segment.log_size)) {
        return base::Status::IoError(
            IoErrorMessage(impl_->active_segment.log_path, "unable to update durable batch log header"));
    }
    if (wrote_index &&
        !SyncFileHeader(impl_->active_segment.index_fd, &active_index_header, impl_->active_segment.index_size)) {
        return base::Status::IoError(
            IoErrorMessage(impl_->active_segment.index_path, "unable to update durable batch index header"));
    }
    if (wrote_recovery && !SyncFileHeader(impl_->recovery_fd, &recovery_header, impl_->recovery_size)) {
        return base::Status::IoError(
            IoErrorMessage(impl_->recovery_path, "unable to update durable recovery header"));
    }

    // Sync headers to disk so the committed sizes are durable.
    if (wrote_log) {
        status = SyncDataFile(impl_->active_segment.log_fd);
        if (!status.ok()) {
            return status;
        }
    }
    if (wrote_index) {
        status = SyncDataFile(impl_->active_segment.index_fd);
        if (!status.ok()) {
            return status;
        }
    }
    if (wrote_recovery) {
        status = SyncDataFile(impl_->recovery_fd);
        if (!status.ok()) {
            return status;
        }
    }

    impl_->pending_entries.clear();
    impl_->pending_payload_arena.clear();

    return base::Status::Ok();
}

auto DurableBatchSessionStore::Rollover() -> base::Status {
    auto status = Flush();
    if (!status.ok()) {
        return status;
    }
    if (!impl_->active_segment.has_records) {
        return base::Status::Ok();
    }

    const auto archive_dir = root_ / "archive";
    status = EnsureDirectory(archive_dir);
    if (!status.ok()) {
        return status;
    }

    SegmentHandle archived = impl_->active_segment;
    const auto archived_log_path = archive_dir / MakeSegmentFileName(archived.segment_id, ".log");
    const auto archived_index_path = archive_dir / MakeSegmentFileName(archived.segment_id, ".out.idx");

    CloseSegment(impl_->active_segment);

    std::error_code rename_error;
    std::filesystem::rename(archived.log_path, archived_log_path, rename_error);
    if (rename_error) {
        return base::Status::IoError(
            "unable to archive durable batch log '" + archived.log_path.string() + "': " +
            rename_error.message());
    }

    std::filesystem::rename(archived.index_path, archived_index_path, rename_error);
    if (rename_error) {
        std::error_code rollback_error;
        std::filesystem::rename(archived_log_path, archived.log_path, rollback_error);
        return base::Status::IoError(
            "unable to archive durable batch index '" + archived.index_path.string() + "': " +
            rename_error.message());
    }

    auto reopened = OpenSegmentHandle(
        archived_log_path,
        archived_index_path,
        archived.segment_id,
        archived.segment_id);
    if (!reopened.ok()) {
        return reopened.status();
    }
    impl_->archived_segments[archived.segment_id] = std::move(reopened).value();

    impl_->active_segment = {};
    impl_->active_utc_day.reset();

    const auto next_segment_id = archived.segment_id + 1U;
    auto active = OpenSegmentHandle(root_ / "active.log", root_ / "active.out.idx", next_segment_id, next_segment_id);
    if (!active.ok()) {
        return active.status();
    }
    impl_->active_segment = std::move(active).value();

    if (options_.max_archived_segments != 0U) {
        while (impl_->archived_segments.size() > options_.max_archived_segments) {
            auto oldest = impl_->archived_segments.begin();
            RemoveSegmentEntries(&impl_->outbound_index, oldest->first);
            CloseSegment(oldest->second);
            std::error_code remove_error;
            std::filesystem::remove(oldest->second.log_path, remove_error);
            if (remove_error) {
                return base::Status::IoError(
                    "unable to prune archived durable store log '" + oldest->second.log_path.string() + "': " +
                    remove_error.message());
            }
            std::filesystem::remove(oldest->second.index_path, remove_error);
            if (remove_error) {
                return base::Status::IoError(
                    "unable to prune archived durable store index '" + oldest->second.index_path.string() + "': " +
                    remove_error.message());
            }
            impl_->archived_segments.erase(oldest);
        }
    }

    // Compact recovery log: rewrite only the latest state per session.
    if (impl_->recovery_fd >= 0 && !impl_->recovery_states.empty()) {
        CloseFd(impl_->recovery_fd);
        impl_->recovery_size = 0;

        int fd = ::open(impl_->recovery_path.c_str(), O_RDWR | O_TRUNC, 0644);
        if (fd < 0) {
            return base::Status::IoError(
                IoErrorMessage(impl_->recovery_path, "unable to truncate recovery log"));
        }
        auto rec_header = BuildFileHeader(kRecoveryMagic, 0U);
        if (!WriteExact(fd, &rec_header, sizeof(rec_header), 0U)) {
            CloseFd(fd);
            return base::Status::IoError(
                IoErrorMessage(impl_->recovery_path, "unable to write recovery log header"));
        }
        std::uint64_t offset = sizeof(StoreFileHeader);
        for (const auto& [sid, state] : impl_->recovery_states) {
            RecoveryRecord record{};
            record.session_id = state.session_id;
            record.next_in_seq = state.next_in_seq;
            record.next_out_seq = state.next_out_seq;
            record.last_inbound_ns = state.last_inbound_ns;
            record.last_outbound_ns = state.last_outbound_ns;
            record.active = state.active ? 1U : 0U;
            if (!WriteExact(fd, &record, sizeof(record), offset)) {
                CloseFd(fd);
                return base::Status::IoError(
                    IoErrorMessage(impl_->recovery_path, "unable to write compacted recovery record"));
            }
            offset += sizeof(record);
        }
        if (!SyncFileHeader(fd, &rec_header, offset)) {
            CloseFd(fd);
            return base::Status::IoError(
                IoErrorMessage(impl_->recovery_path, "unable to update compacted recovery header"));
        }
        auto sync_status = SyncDataFile(fd);
        if (!sync_status.ok()) {
            CloseFd(fd);
            return sync_status;
        }
        impl_->recovery_fd = fd;
        impl_->recovery_size = offset;
    }

    return base::Status::Ok();
}

auto DurableBatchSessionStore::SaveOutbound(const MessageRecord& record) -> base::Status {
    return AppendMessage(true, record);
}

auto DurableBatchSessionStore::SaveInbound(const MessageRecord& record) -> base::Status {
    return AppendMessage(false, record);
}

auto DurableBatchSessionStore::SaveOutboundView(const MessageRecordView& record) -> base::Status {
    return AppendMessageView(true, record);
}

auto DurableBatchSessionStore::SaveInboundView(const MessageRecordView& record) -> base::Status {
    return AppendMessageView(false, record);
}

auto DurableBatchSessionStore::LoadOutboundRange(
    std::uint64_t session_id,
    std::uint32_t begin_seq,
    std::uint32_t end_seq) const -> base::Result<std::vector<MessageRecord>> {
    if (begin_seq == 0U || end_seq == 0U || begin_seq > end_seq) {
        return base::Status::InvalidArgument("invalid outbound load range");
    }

    auto status = impl_->pending_entries.empty() ? base::Status::Ok()
        : const_cast<DurableBatchSessionStore*>(this)->Flush();
    if (!status.ok()) {
        return status;
    }

    std::vector<MessageRecord> records;
    const auto session_it = impl_->outbound_index.find(session_id);
    if (session_it == impl_->outbound_index.end()) {
        return records;
    }

    for (auto it = session_it->second.lower_bound(begin_seq);
         it != session_it->second.end() && it->first <= end_seq;
         ++it) {
        const SegmentHandle* segment = nullptr;
        if (impl_->active_segment.segment_id == it->second.segment_id) {
            segment = &impl_->active_segment;
        } else {
            const auto archived = impl_->archived_segments.find(it->second.segment_id);
            if (archived == impl_->archived_segments.end()) {
                continue;
            }
            segment = &archived->second;
        }

        StoreRecordHeader header{};
        if (!ReadExact(segment->log_fd, &header, sizeof(header), it->second.log_offset)) {
            return base::Status::IoError(
                IoErrorMessage(segment->log_path, "unable to read durable outbound record"));
        }
        if (header.record_type != static_cast<std::uint32_t>(StoreRecordType::kOutbound) ||
            header.header_size != sizeof(StoreRecordHeader)) {
            return base::Status::FormatError("durable batch store outbound index points to an invalid log record");
        }

        MessageRecord record;
        record.session_id = header.session_id;
        record.seq_num = header.seq_num;
        record.timestamp_ns = header.timestamp_ns;
        record.flags = header.flags;
        record.payload.resize(header.payload_size);
        if (header.payload_size != 0U &&
            !ReadExact(
                segment->log_fd,
                record.payload.data(),
                header.payload_size,
                it->second.log_offset + sizeof(StoreRecordHeader))) {
            return base::Status::IoError(
                IoErrorMessage(segment->log_path, "unable to read durable outbound payload"));
        }
        records.push_back(std::move(record));
    }

    return records;
}

auto DurableBatchSessionStore::LoadOutboundRangeViews(
    std::uint64_t session_id,
    std::uint32_t begin_seq,
    std::uint32_t end_seq) const -> base::Result<MessageRecordViewRange> {
    if (begin_seq == 0U || end_seq == 0U || begin_seq > end_seq) {
        return base::Status::InvalidArgument("invalid outbound load range");
    }

    auto status = impl_->pending_entries.empty() ? base::Status::Ok()
        : const_cast<DurableBatchSessionStore*>(this)->Flush();
    if (!status.ok()) {
        return status;
    }

    MessageRecordViewRange records;
    const auto session_it = impl_->outbound_index.find(session_id);
    if (session_it == impl_->outbound_index.end()) {
        return records;
    }

    struct IndexedRecord {
        const SegmentHandle* segment{nullptr};
        std::uint32_t seq_num{0};
        std::uint16_t flags{0};
        std::uint64_t timestamp_ns{0};
        std::uint64_t log_offset{0};
        std::uint32_t payload_size{0};
    };

    std::vector<IndexedRecord> indexed;
    indexed.reserve(static_cast<std::size_t>(end_seq - begin_seq + 1U));

    std::size_t total_payload_size = 0U;
    for (auto it = session_it->second.lower_bound(begin_seq);
         it != session_it->second.end() && it->first <= end_seq;
         ++it) {
        const auto* segment = ResolveSegmentHandle(*impl_, it->second.segment_id);
        if (segment == nullptr) {
            continue;
        }
        indexed.push_back(IndexedRecord{
            .segment = segment,
            .seq_num = it->first,
            .flags = it->second.flags,
            .timestamp_ns = it->second.timestamp_ns,
            .log_offset = it->second.log_offset,
            .payload_size = it->second.payload_size,
        });
        total_payload_size += it->second.payload_size;
    }

    records.records.reserve(indexed.size());
    records.payload_storage.resize(total_payload_size);

    std::size_t payload_offset = 0U;
    for (const auto& entry : indexed) {
        if (entry.payload_size != 0U &&
            !ReadExact(
                entry.segment->log_fd,
                records.payload_storage.data() + payload_offset,
                entry.payload_size,
                entry.log_offset + sizeof(StoreRecordHeader))) {
            return base::Status::IoError(
                IoErrorMessage(entry.segment->log_path, "unable to read durable outbound payload"));
        }

        const auto* payload = entry.payload_size == 0U ? nullptr : records.payload_storage.data() + payload_offset;
        records.records.push_back(MessageRecordView{
            .session_id = session_id,
            .seq_num = entry.seq_num,
            .timestamp_ns = entry.timestamp_ns,
            .flags = entry.flags,
            .payload = std::span<const std::byte>(payload, entry.payload_size),
        });
        payload_offset += entry.payload_size;
    }

    return records;
}

auto DurableBatchSessionStore::SaveRecoveryState(const SessionRecoveryState& state) -> base::Status {
    return QueueRecoveryState(state);
}

auto DurableBatchSessionStore::LoadRecoveryState(std::uint64_t session_id) const
    -> base::Result<SessionRecoveryState> {
    auto status = impl_->pending_entries.empty() ? base::Status::Ok()
        : const_cast<DurableBatchSessionStore*>(this)->Flush();
    if (!status.ok()) {
        return status;
    }

    const auto it = impl_->recovery_states.find(session_id);
    if (it == impl_->recovery_states.end()) {
        return base::Status::NotFound("recovery state not found");
    }
    return it->second;
}

auto DurableBatchSessionStore::Refresh() -> base::Status {
    CloseSegment(impl_->active_segment);
    impl_->active_segment = {};
    for (auto& [segment_id, segment] : impl_->archived_segments) {
        (void)segment_id;
        CloseSegment(segment);
    }
    impl_->archived_segments.clear();
    CloseFd(impl_->recovery_fd);
    impl_->recovery_size = 0U;
    impl_->outbound_index.clear();
    impl_->recovery_states.clear();
    impl_->pending_entries.clear();
    impl_->pending_payload_arena.clear();
    impl_->active_utc_day.reset();
    return Open();
}

auto DurableBatchSessionStore::ResetSession(std::uint64_t session_id) -> base::Status {
    (void)session_id;

    auto status = Flush();
    if (!status.ok()) {
        return status;
    }

    status = Refresh();
    if (!status.ok()) {
        return status;
    }

    CloseSegment(impl_->active_segment);
    impl_->active_segment = {};
    for (auto& [segment_id, segment] : impl_->archived_segments) {
        (void)segment_id;
        CloseSegment(segment);
    }
    impl_->archived_segments.clear();
    CloseFd(impl_->recovery_fd);
    impl_->recovery_size = 0U;

    std::error_code error;
    std::filesystem::remove_all(root_, error);
    if (error) {
        return base::Status::IoError(
            "unable to reset durable batch store root '" + root_.string() + "': " + error.message());
    }

    impl_->outbound_index.clear();
    impl_->recovery_states.clear();
    impl_->pending_entries.clear();
    impl_->pending_payload_arena.clear();
    impl_->active_utc_day.reset();
    return Open();
}

}  // namespace fastfix::store