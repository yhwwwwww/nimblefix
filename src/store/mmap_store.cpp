#include "fastfix/store/mmap_store.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace fastfix::store {

namespace {

constexpr std::size_t kStoreMagicBytes = 8U;
constexpr std::size_t kStoreRecordReservedBytes = 7U;
constexpr std::size_t kExpectedStoreFileHeaderSize = 32U;
constexpr std::size_t kExpectedStoreRecordHeaderSize = 64U;
constexpr std::array<char, kStoreMagicBytes> kStoreMagic = {'F', 'F', 'S', 'T', 'O', 'R', 'E', '1'};
constexpr std::uint32_t kStoreVersion = 1U;
constexpr mode_t kDefaultStoreFilePermissions = 0644;
constexpr std::uint8_t kActiveRecoveryStateValue = 1U;

enum class StoreRecordType : std::uint32_t {
    kOutbound = 1,
    kInbound = 2,
    kRecoveryState = 3,
};

#pragma pack(push, 1)
struct StoreFileHeader {
    char magic[kStoreMagicBytes];
    std::uint32_t version;
    std::uint32_t header_size;
    std::uint64_t file_size;
    std::uint64_t reserved0;
};

struct StoreRecordHeader {
    std::uint32_t record_type;
    std::uint16_t header_size;
    std::uint16_t flags;
    std::uint64_t session_id;
    std::uint32_t seq_num;
    std::uint32_t payload_size;
    std::uint64_t timestamp_ns;
    std::uint32_t next_in_seq;
    std::uint32_t next_out_seq;
    std::uint64_t last_inbound_ns;
    std::uint64_t last_outbound_ns;
    std::uint8_t active;
    std::uint8_t reserved[kStoreRecordReservedBytes];
};
#pragma pack(pop)

constexpr std::size_t kStoreFileHeaderSize = sizeof(StoreFileHeader);
constexpr std::size_t kStoreRecordHeaderSize = sizeof(StoreRecordHeader);

static_assert(kStoreFileHeaderSize == kExpectedStoreFileHeaderSize);
static_assert(kStoreRecordHeaderSize == kExpectedStoreRecordHeaderSize);

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
        return base::Status::InvalidArgument("message payload exceeds mmap store limits");
    }
    return base::Status::Ok();
}

auto HasStoreMagic(const StoreFileHeader& header) -> bool {
    return std::memcmp(header.magic, kStoreMagic.data(), kStoreMagic.size()) == 0;
}

}  // namespace

struct MmapSessionStore::Impl {
    int fd{-1};
    std::byte* mapping{nullptr};
    std::size_t mapping_size{0};
    std::unordered_map<std::uint64_t, std::map<std::uint32_t, std::uint64_t>> outbound_offsets;
    std::unordered_map<std::uint64_t, SessionRecoveryState> recovery_states;

    ~Impl() {
        if (mapping != nullptr && mapping_size != 0) {
            ::munmap(mapping, mapping_size);
        }
        if (fd >= 0) {
            ::close(fd);
        }
    }
};

namespace {

auto Header(const MmapSessionStore::Impl& impl) -> const StoreFileHeader* {
    return reinterpret_cast<const StoreFileHeader*>(impl.mapping);
}

auto RecordAt(const MmapSessionStore::Impl& impl, std::uint64_t offset) -> const StoreRecordHeader* {
    return reinterpret_cast<const StoreRecordHeader*>(impl.mapping + offset);
}

auto PayloadAt(const MmapSessionStore::Impl& impl, std::uint64_t offset) -> const std::byte* {
    return impl.mapping + offset + kStoreRecordHeaderSize;
}

}  // namespace

MmapSessionStore::MmapSessionStore(std::filesystem::path path)
    : path_(std::move(path)), impl_(std::make_unique<Impl>()) {
}

MmapSessionStore::~MmapSessionStore() = default;

auto MmapSessionStore::Open() -> base::Status {
    if (impl_->fd < 0) {
        impl_->fd = ::open(path_.c_str(), O_RDWR | O_CREAT, kDefaultStoreFilePermissions);
        if (impl_->fd < 0) {
            return base::Status::IoError(IoErrorMessage(path_, "unable to open mmap store"));
        }
    }

    struct stat stat_buffer {};
    if (::fstat(impl_->fd, &stat_buffer) != 0) {
        return base::Status::IoError(IoErrorMessage(path_, "unable to stat mmap store"));
    }

    if (stat_buffer.st_size == 0) {
        StoreFileHeader header{};
        std::memcpy(header.magic, kStoreMagic.data(), kStoreMagic.size());
        header.version = kStoreVersion;
        header.header_size = static_cast<std::uint32_t>(kStoreFileHeaderSize);
        header.file_size = kStoreFileHeaderSize;
        if (::ftruncate(impl_->fd, static_cast<off_t>(kStoreFileHeaderSize)) != 0) {
            return base::Status::IoError(IoErrorMessage(path_, "unable to size mmap store"));
        }
        if (::pwrite(impl_->fd, &header, sizeof(header), 0) != static_cast<ssize_t>(sizeof(header))) {
            return base::Status::IoError(IoErrorMessage(path_, "unable to initialize mmap store"));
        }
    }

    if (impl_->mapping != nullptr && impl_->mapping_size != 0) {
        ::munmap(impl_->mapping, impl_->mapping_size);
        impl_->mapping = nullptr;
        impl_->mapping_size = 0;
    }

    if (::fstat(impl_->fd, &stat_buffer) != 0) {
        return base::Status::IoError(IoErrorMessage(path_, "unable to stat mmap store"));
    }

    impl_->mapping_size = static_cast<std::size_t>(stat_buffer.st_size);
    impl_->mapping = static_cast<std::byte*>(
        ::mmap(nullptr, impl_->mapping_size, PROT_READ | PROT_WRITE, MAP_SHARED, impl_->fd, 0));
    if (impl_->mapping == MAP_FAILED) {
        impl_->mapping = nullptr;
        impl_->mapping_size = 0;
        return base::Status::IoError(IoErrorMessage(path_, "unable to mmap store file"));
    }

    const auto* header = Header(*impl_);
    if (!HasStoreMagic(*header) || header->version != kStoreVersion ||
        header->header_size != kStoreFileHeaderSize || header->file_size != impl_->mapping_size) {
        return base::Status::FormatError("mmap store has an invalid header");
    }

    impl_->outbound_offsets.clear();
    impl_->recovery_states.clear();

    std::uint64_t offset = kStoreFileHeaderSize;
    while (offset < header->file_size) {
        if (offset + kStoreRecordHeaderSize > header->file_size) {
            return base::Status::FormatError("mmap store contains a truncated record header");
        }

        const auto* record = RecordAt(*impl_, offset);
        if (record->header_size != kStoreRecordHeaderSize) {
            return base::Status::FormatError("mmap store contains a record with an unexpected header size");
        }

        const auto record_end = offset + kStoreRecordHeaderSize + record->payload_size;
        if (record_end > header->file_size) {
            return base::Status::FormatError("mmap store contains a truncated record payload");
        }

        switch (static_cast<StoreRecordType>(record->record_type)) {
            case StoreRecordType::kOutbound:
                impl_->outbound_offsets[record->session_id][record->seq_num] = offset;
                break;
            case StoreRecordType::kRecoveryState:
                impl_->recovery_states[record->session_id] = SessionRecoveryState{
                    .session_id = record->session_id,
                    .next_in_seq = record->next_in_seq,
                    .next_out_seq = record->next_out_seq,
                    .last_inbound_ns = record->last_inbound_ns,
                    .last_outbound_ns = record->last_outbound_ns,
                    .active = record->active != 0,
                };
                break;
            case StoreRecordType::kInbound:
                break;
            default:
                return base::Status::FormatError("mmap store contains an unknown record type");
        }

        offset = record_end;
    }

    return base::Status::Ok();
}

auto MmapSessionStore::AppendOutboundLike(std::uint32_t record_type, const MessageRecord& record) -> base::Status {
    auto status = ValidateRecord(record);
    if (!status.ok()) {
        return status;
    }

    return AppendOutboundLikeView(record_type, record.view());
}

auto MmapSessionStore::AppendOutboundLikeView(std::uint32_t record_type, const MessageRecordView& record) -> base::Status {
    auto status = ValidateRecordView(record);
    if (!status.ok()) {
        return status;
    }

    status = Open();
    if (!status.ok()) {
        return status;
    }

    const auto old_size = Header(*impl_)->file_size;
    const auto record_size = kStoreRecordHeaderSize + record.payload.size();
    const auto new_size = old_size + record_size;

    if (::ftruncate(impl_->fd, static_cast<off_t>(new_size)) != 0) {
        return base::Status::IoError(IoErrorMessage(path_, "unable to extend mmap store"));
    }

    StoreRecordHeader header{};
    header.record_type = record_type;
    header.header_size = static_cast<std::uint16_t>(kStoreRecordHeaderSize);
    header.flags = record.flags;
    header.session_id = record.session_id;
    header.seq_num = record.seq_num;
    header.payload_size = static_cast<std::uint32_t>(record.payload.size());
    header.timestamp_ns = record.timestamp_ns;

    if (::pwrite(impl_->fd, &header, sizeof(header), static_cast<off_t>(old_size)) !=
        static_cast<ssize_t>(sizeof(header))) {
        return base::Status::IoError(IoErrorMessage(path_, "unable to append mmap record header"));
    }

    if (!record.payload.empty()) {
        if (::pwrite(
                impl_->fd,
                record.payload.data(),
                static_cast<size_t>(record.payload.size()),
                static_cast<off_t>(old_size + kStoreRecordHeaderSize)) !=
            static_cast<ssize_t>(record.payload.size())) {
            return base::Status::IoError(IoErrorMessage(path_, "unable to append mmap record payload"));
        }
    }

    auto file_header = *Header(*impl_);
    file_header.file_size = new_size;
    if (::pwrite(impl_->fd, &file_header, sizeof(file_header), 0) != static_cast<ssize_t>(sizeof(file_header))) {
        return base::Status::IoError(IoErrorMessage(path_, "unable to update mmap store header"));
    }

    status = Open();
    if (!status.ok()) {
        return status;
    }

    return base::Status::Ok();
}

auto MmapSessionStore::AppendRecoveryState(const SessionRecoveryState& state) -> base::Status {
    if (state.session_id == 0) {
        return base::Status::InvalidArgument("recovery state is missing session_id");
    }

    auto status = Open();
    if (!status.ok()) {
        return status;
    }

    const auto old_size = Header(*impl_)->file_size;
    const auto record_size = kStoreRecordHeaderSize;
    const auto new_size = old_size + record_size;

    if (::ftruncate(impl_->fd, static_cast<off_t>(new_size)) != 0) {
        return base::Status::IoError(IoErrorMessage(path_, "unable to extend mmap store"));
    }

    StoreRecordHeader header{};
    header.record_type = static_cast<std::uint32_t>(StoreRecordType::kRecoveryState);
    header.header_size = static_cast<std::uint16_t>(kStoreRecordHeaderSize);
    header.session_id = state.session_id;
    header.next_in_seq = state.next_in_seq;
    header.next_out_seq = state.next_out_seq;
    header.last_inbound_ns = state.last_inbound_ns;
    header.last_outbound_ns = state.last_outbound_ns;
    header.active = state.active ? kActiveRecoveryStateValue : 0U;

    if (::pwrite(impl_->fd, &header, sizeof(header), static_cast<off_t>(old_size)) !=
        static_cast<ssize_t>(sizeof(header))) {
        return base::Status::IoError(IoErrorMessage(path_, "unable to append mmap recovery state"));
    }

    auto file_header = *Header(*impl_);
    file_header.file_size = new_size;
    if (::pwrite(impl_->fd, &file_header, sizeof(file_header), 0) != static_cast<ssize_t>(sizeof(file_header))) {
        return base::Status::IoError(IoErrorMessage(path_, "unable to update mmap store header"));
    }

    status = Open();
    if (!status.ok()) {
        return status;
    }

    return base::Status::Ok();
}

auto MmapSessionStore::SaveOutbound(const MessageRecord& record) -> base::Status {
    return AppendOutboundLike(static_cast<std::uint32_t>(StoreRecordType::kOutbound), record);
}

auto MmapSessionStore::SaveInbound(const MessageRecord& record) -> base::Status {
    return AppendOutboundLike(static_cast<std::uint32_t>(StoreRecordType::kInbound), record);
}

auto MmapSessionStore::SaveOutboundView(const MessageRecordView& record) -> base::Status {
    return AppendOutboundLikeView(static_cast<std::uint32_t>(StoreRecordType::kOutbound), record);
}

auto MmapSessionStore::SaveInboundView(const MessageRecordView& record) -> base::Status {
    return AppendOutboundLikeView(static_cast<std::uint32_t>(StoreRecordType::kInbound), record);
}

auto MmapSessionStore::LoadOutboundRange(
    std::uint64_t session_id,
    std::uint32_t begin_seq,
    std::uint32_t end_seq) const -> base::Result<std::vector<MessageRecord>> {
    if (begin_seq == 0 || end_seq == 0 || begin_seq > end_seq) {
        return base::Status::InvalidArgument("invalid outbound load range");
    }

    auto status = const_cast<MmapSessionStore*>(this)->Open();
    if (!status.ok()) {
        return status;
    }

    std::vector<MessageRecord> records;
    const auto session_it = impl_->outbound_offsets.find(session_id);
    if (session_it == impl_->outbound_offsets.end()) {
        return records;
    }

    for (auto it = session_it->second.lower_bound(begin_seq);
         it != session_it->second.end() && it->first <= end_seq;
         ++it) {
        const auto* header = RecordAt(*impl_, it->second);
        MessageRecord record;
        record.session_id = header->session_id;
        record.seq_num = header->seq_num;
        record.timestamp_ns = header->timestamp_ns;
        record.flags = header->flags;
        record.payload.resize(header->payload_size);
        if (header->payload_size != 0) {
            std::memcpy(record.payload.data(), PayloadAt(*impl_, it->second), header->payload_size);
        }
        records.push_back(std::move(record));
    }

    return records;
}

auto MmapSessionStore::LoadOutboundRangeViews(
    std::uint64_t session_id,
    std::uint32_t begin_seq,
    std::uint32_t end_seq) const -> base::Result<MessageRecordViewRange> {
    if (begin_seq == 0 || end_seq == 0 || begin_seq > end_seq) {
        return base::Status::InvalidArgument("invalid outbound load range");
    }

    auto status = const_cast<MmapSessionStore*>(this)->Open();
    if (!status.ok()) {
        return status;
    }

    MessageRecordViewRange records;
    const auto session_it = impl_->outbound_offsets.find(session_id);
    if (session_it == impl_->outbound_offsets.end()) {
        return records;
    }

    records.records.reserve(static_cast<std::size_t>(end_seq - begin_seq + 1U));
    for (auto it = session_it->second.lower_bound(begin_seq);
         it != session_it->second.end() && it->first <= end_seq;
         ++it) {
        const auto* header = RecordAt(*impl_, it->second);
        records.records.push_back(MessageRecordView{
            .session_id = header->session_id,
            .seq_num = header->seq_num,
            .timestamp_ns = header->timestamp_ns,
            .flags = header->flags,
            .payload = std::span<const std::byte>(PayloadAt(*impl_, it->second), header->payload_size),
        });
    }

    return records;
}

auto MmapSessionStore::SaveRecoveryState(const SessionRecoveryState& state) -> base::Status {
    return AppendRecoveryState(state);
}

auto MmapSessionStore::LoadRecoveryState(std::uint64_t session_id) const
    -> base::Result<SessionRecoveryState> {
    auto status = const_cast<MmapSessionStore*>(this)->Open();
    if (!status.ok()) {
        return status;
    }

    const auto it = impl_->recovery_states.find(session_id);
    if (it == impl_->recovery_states.end()) {
        return base::Status::NotFound("recovery state not found");
    }

    return it->second;
}

}  // namespace fastfix::store