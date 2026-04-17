#include "fastfix/store/mmap_store.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>

namespace fastfix::store {

namespace {

constexpr std::size_t kStoreMagicBytes = 8U;
constexpr std::size_t kStoreRecordPaddingBytes = 3U;
constexpr std::size_t kExpectedStoreFileHeaderSize = 32U;
constexpr std::size_t kExpectedStoreRecordHeaderSize = 64U;
constexpr std::array<char, kStoreMagicBytes> kStoreMagic = { 'F', 'F', 'S', 'T', 'O', 'R', 'E', '1' };
constexpr std::uint32_t kStoreVersion = 1U;
constexpr mode_t kDefaultStoreFilePermissions = 0644;
constexpr std::uint8_t kActiveRecoveryStateValue = 1U;

enum class StoreRecordType : std::uint32_t
{
  kOutbound = 1,
  kInbound = 2,
  kRecoveryState = 3,
};

#pragma pack(push, 1)
struct StoreFileHeader
{
  char magic[kStoreMagicBytes];
  std::uint32_t version;
  std::uint32_t header_size;
  std::uint64_t file_size;
  std::uint64_t reserved0;
};

struct StoreRecordHeader
{
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
  std::uint8_t reserved[kStoreRecordPaddingBytes];
  std::uint32_t body_start_offset;
};
#pragma pack(pop)

constexpr std::size_t kStoreFileHeaderSize = sizeof(StoreFileHeader);
constexpr std::size_t kStoreRecordHeaderSize = sizeof(StoreRecordHeader);

static_assert(kStoreFileHeaderSize == kExpectedStoreFileHeaderSize);
static_assert(kStoreRecordHeaderSize == kExpectedStoreRecordHeaderSize);

auto
IoErrorMessage(const std::filesystem::path& path, const char* action) -> std::string
{
  return action + std::string(" '") + path.string() + "': " + std::strerror(errno);
}

auto
ValidateRecord(const MessageRecord& record) -> base::Status
{
  if (record.session_id == 0) {
    return base::Status::InvalidArgument("message record is missing session_id");
  }
  if (record.seq_num == 0) {
    return base::Status::InvalidArgument("message record is missing seq_num");
  }
  if (record.body_start_offset > record.payload.size()) {
    return base::Status::InvalidArgument("message record body_start_offset exceeds payload size");
  }
  return base::Status::Ok();
}

auto
ValidateRecordView(const MessageRecordView& record) -> base::Status
{
  if (record.session_id == 0) {
    return base::Status::InvalidArgument("message record is missing session_id");
  }
  if (record.seq_num == 0) {
    return base::Status::InvalidArgument("message record is missing seq_num");
  }
  if (record.payload.size() > std::numeric_limits<std::uint32_t>::max()) {
    return base::Status::InvalidArgument("message payload exceeds mmap store limits");
  }
  if (record.body_start_offset > record.payload.size()) {
    return base::Status::InvalidArgument("message record body_start_offset exceeds payload size");
  }
  return base::Status::Ok();
}

auto
HasStoreMagic(const StoreFileHeader& header) -> bool
{
  return std::memcmp(header.magic, kStoreMagic.data(), kStoreMagic.size()) == 0;
}

struct MappingHandle
{
  std::byte* data{ nullptr };
  std::size_t size{ 0U };

  ~MappingHandle()
  {
    if (data != nullptr && size != 0U) {
      ::munmap(data, size);
    }
  }
};

struct ScanState
{
  std::uint64_t committed_size{ kStoreFileHeaderSize };
  bool truncated_tail{ false };
  std::unordered_map<std::uint64_t, std::map<std::uint32_t, std::uint64_t>> outbound_offsets;
  std::unordered_map<std::uint64_t, SessionRecoveryState> recovery_states;
};

auto
BuildFileHeader(std::uint64_t file_size) -> StoreFileHeader
{
  StoreFileHeader header{};
  std::memcpy(header.magic, kStoreMagic.data(), kStoreMagic.size());
  header.version = kStoreVersion;
  header.header_size = static_cast<std::uint32_t>(kStoreFileHeaderSize);
  header.file_size = file_size;
  return header;
}

auto
WriteExact(int fd, const void* data, std::size_t size, std::uint64_t offset) -> bool
{
  const auto* bytes = static_cast<const std::byte*>(data);
  std::size_t written = 0U;
  while (written < size) {
    const auto result = ::pwrite(fd, bytes + written, size - written, static_cast<off_t>(offset + written));
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

auto
MapFile(int fd, std::size_t size) -> base::Result<std::shared_ptr<MappingHandle>>
{
  auto mapping = std::shared_ptr<MappingHandle>(new MappingHandle{});
  mapping->size = size;
  mapping->data = static_cast<std::byte*>(::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
  if (mapping->data == MAP_FAILED) {
    return base::Status::IoError(std::string("unable to mmap store file: ") + std::strerror(errno));
  }
  return mapping;
}

auto
Header(const MappingHandle& mapping) -> const StoreFileHeader*
{
  return reinterpret_cast<const StoreFileHeader*>(mapping.data);
}

auto
RecordAt(const MappingHandle& mapping, std::uint64_t offset) -> const StoreRecordHeader*
{
  return reinterpret_cast<const StoreRecordHeader*>(mapping.data + offset);
}

auto
PayloadAt(const MappingHandle& mapping, std::uint64_t offset) -> const std::byte*
{
  return mapping.data + offset + kStoreRecordHeaderSize;
}

auto
ScanMapping(const MappingHandle& mapping, std::uint64_t scan_end) -> ScanState
{
  ScanState state;
  state.committed_size = kStoreFileHeaderSize;

  std::uint64_t offset = kStoreFileHeaderSize;
  while (offset < scan_end) {
    if (offset + kStoreRecordHeaderSize > scan_end) {
      state.truncated_tail = true;
      break;
    }

    const auto* record = RecordAt(mapping, offset);
    if (record->header_size != kStoreRecordHeaderSize) {
      state.truncated_tail = true;
      break;
    }
    if (record->body_start_offset > record->payload_size) {
      state.truncated_tail = true;
      break;
    }

    const auto record_end = offset + kStoreRecordHeaderSize + record->payload_size;
    if (record_end > scan_end) {
      state.truncated_tail = true;
      break;
    }

    switch (static_cast<StoreRecordType>(record->record_type)) {
      case StoreRecordType::kOutbound:
        state.outbound_offsets[record->session_id][record->seq_num] = offset;
        break;
      case StoreRecordType::kRecoveryState:
        state.recovery_states[record->session_id] = SessionRecoveryState{
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
        state.truncated_tail = true;
        break;
    }

    if (state.truncated_tail) {
      break;
    }

    offset = record_end;
    state.committed_size = offset;
  }

  return state;
}

} // namespace

struct MmapSessionStore::Impl
{
  int fd{ -1 };
  std::shared_ptr<MappingHandle> mapping;
  std::unordered_map<std::uint64_t, std::map<std::uint32_t, std::uint64_t>> outbound_offsets;
  std::unordered_map<std::uint64_t, SessionRecoveryState> recovery_states;

  ~Impl()
  {
    if (fd >= 0) {
      ::close(fd);
    }
  }
};

MmapSessionStore::MmapSessionStore(std::filesystem::path path, SyncPolicy sync_policy)
  : path_(std::move(path))
  , sync_policy_(sync_policy)
  , impl_(std::make_unique<Impl>())
{
}

MmapSessionStore::~MmapSessionStore() = default;

auto
MmapSessionStore::Open() -> base::Status
{
  if (impl_->fd < 0) {
    impl_->fd = ::open(path_.c_str(), O_RDWR | O_CREAT, kDefaultStoreFilePermissions);
    if (impl_->fd < 0) {
      return base::Status::IoError(IoErrorMessage(path_, "unable to open mmap store"));
    }
  }

  struct stat stat_buffer{};
  if (::fstat(impl_->fd, &stat_buffer) != 0) {
    return base::Status::IoError(IoErrorMessage(path_, "unable to stat mmap store"));
  }

  if (stat_buffer.st_size == 0) {
    const auto header = BuildFileHeader(kStoreFileHeaderSize);
    if (::ftruncate(impl_->fd, static_cast<off_t>(kStoreFileHeaderSize)) != 0) {
      return base::Status::IoError(IoErrorMessage(path_, "unable to size mmap store"));
    }
    if (!WriteExact(impl_->fd, &header, sizeof(header), 0U)) {
      return base::Status::IoError(IoErrorMessage(path_, "unable to initialize mmap store"));
    }
    if (sync_policy_ == SyncPolicy::kEveryWrite) {
      if (::fdatasync(impl_->fd) != 0) {
        return base::Status::IoError(IoErrorMessage(path_, "unable to fdatasync mmap store"));
      }
    }
  }

  if (::fstat(impl_->fd, &stat_buffer) != 0) {
    return base::Status::IoError(IoErrorMessage(path_, "unable to stat mmap store"));
  }

  auto mapping = MapFile(impl_->fd, static_cast<std::size_t>(stat_buffer.st_size));
  if (!mapping.ok()) {
    return mapping.status();
  }

  const auto* header = Header(*mapping.value());
  if (!HasStoreMagic(*header) || header->version != kStoreVersion || header->header_size != kStoreFileHeaderSize ||
      header->file_size < kStoreFileHeaderSize) {
    return base::Status::FormatError("mmap store has an invalid header");
  }

  auto committed_size = std::min<std::uint64_t>(header->file_size, mapping.value()->size);
  auto scan = ScanMapping(*mapping.value(), committed_size);
  auto desired_size = std::max<std::uint64_t>(kStoreFileHeaderSize, scan.committed_size);
  if (!scan.truncated_tail && header->file_size <= mapping.value()->size) {
    desired_size = header->file_size;
  }

  const bool needs_repair =
    scan.truncated_tail || header->file_size != desired_size || mapping.value()->size != desired_size;
  if (needs_repair) {
    mapping.value().reset();
    if (::ftruncate(impl_->fd, static_cast<off_t>(desired_size)) != 0) {
      return base::Status::IoError(IoErrorMessage(path_, "unable to repair mmap store size"));
    }
    const auto repaired_header = BuildFileHeader(desired_size);
    if (!WriteExact(impl_->fd, &repaired_header, sizeof(repaired_header), 0U)) {
      return base::Status::IoError(IoErrorMessage(path_, "unable to repair mmap store header"));
    }
    if (sync_policy_ == SyncPolicy::kEveryWrite && ::fdatasync(impl_->fd) != 0) {
      return base::Status::IoError(IoErrorMessage(path_, "unable to fdatasync mmap store"));
    }
    mapping = MapFile(impl_->fd, static_cast<std::size_t>(desired_size));
    if (!mapping.ok()) {
      return mapping.status();
    }
    scan = ScanMapping(*mapping.value(), desired_size);
  }

  impl_->mapping = std::move(mapping).value();
  impl_->outbound_offsets = std::move(scan.outbound_offsets);
  impl_->recovery_states = std::move(scan.recovery_states);
  return base::Status::Ok();
}

auto
MmapSessionStore::AppendOutboundLike(std::uint32_t record_type, const MessageRecord& record) -> base::Status
{
  auto status = ValidateRecord(record);
  if (!status.ok()) {
    return status;
  }

  return AppendOutboundLikeView(record_type, record.view());
}

auto
MmapSessionStore::AppendOutboundLikeView(std::uint32_t record_type, const MessageRecordView& record) -> base::Status
{
  auto status = ValidateRecordView(record);
  if (!status.ok()) {
    return status;
  }

  status = Open();
  if (!status.ok()) {
    return status;
  }

  const auto old_size = Header(*impl_->mapping)->file_size;
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
  header.body_start_offset = record.body_start_offset;

  if (!record.payload.empty()) {
    if (!WriteExact(impl_->fd,
                    record.payload.data(),
                    static_cast<size_t>(record.payload.size()),
                    old_size + kStoreRecordHeaderSize)) {
      return base::Status::IoError(IoErrorMessage(path_, "unable to append mmap record payload"));
    }
  }

  if (!WriteExact(impl_->fd, &header, sizeof(header), old_size)) {
    return base::Status::IoError(IoErrorMessage(path_, "unable to append mmap record header"));
  }

  auto file_header = *Header(*impl_->mapping);
  file_header.file_size = new_size;
  if (!WriteExact(impl_->fd, &file_header, sizeof(file_header), 0U)) {
    return base::Status::IoError(IoErrorMessage(path_, "unable to update mmap store header"));
  }

  if (sync_policy_ == SyncPolicy::kEveryWrite) {
    if (::fdatasync(impl_->fd) != 0) {
      return base::Status::IoError(IoErrorMessage(path_, "unable to fdatasync mmap store"));
    }
  }

  status = RemapFile(static_cast<std::size_t>(new_size));
  if (!status.ok()) {
    return status;
  }

  if (record_type == static_cast<std::uint32_t>(StoreRecordType::kOutbound)) {
    impl_->outbound_offsets[record.session_id][record.seq_num] = old_size;
  }

  return base::Status::Ok();
}

auto
MmapSessionStore::AppendRecoveryState(const SessionRecoveryState& state) -> base::Status
{
  if (state.session_id == 0) {
    return base::Status::InvalidArgument("recovery state is missing session_id");
  }

  auto status = Open();
  if (!status.ok()) {
    return status;
  }

  const auto old_size = Header(*impl_->mapping)->file_size;
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

  if (!WriteExact(impl_->fd, &header, sizeof(header), old_size)) {
    return base::Status::IoError(IoErrorMessage(path_, "unable to append mmap recovery state"));
  }

  auto file_header = *Header(*impl_->mapping);
  file_header.file_size = new_size;
  if (!WriteExact(impl_->fd, &file_header, sizeof(file_header), 0U)) {
    return base::Status::IoError(IoErrorMessage(path_, "unable to update mmap store header"));
  }

  if (sync_policy_ == SyncPolicy::kEveryWrite) {
    if (::fdatasync(impl_->fd) != 0) {
      return base::Status::IoError(IoErrorMessage(path_, "unable to fdatasync mmap store"));
    }
  }

  status = RemapFile(static_cast<std::size_t>(new_size));
  if (!status.ok()) {
    return status;
  }

  impl_->recovery_states[state.session_id] = state;

  return base::Status::Ok();
}

auto
MmapSessionStore::SaveOutbound(const MessageRecord& record) -> base::Status
{
  return AppendOutboundLike(static_cast<std::uint32_t>(StoreRecordType::kOutbound), record);
}

auto
MmapSessionStore::SaveInbound(const MessageRecord& record) -> base::Status
{
  return AppendOutboundLike(static_cast<std::uint32_t>(StoreRecordType::kInbound), record);
}

auto
MmapSessionStore::SaveOutboundView(const MessageRecordView& record) -> base::Status
{
  return AppendOutboundLikeView(static_cast<std::uint32_t>(StoreRecordType::kOutbound), record);
}

auto
MmapSessionStore::SaveInboundView(const MessageRecordView& record) -> base::Status
{
  return AppendOutboundLikeView(static_cast<std::uint32_t>(StoreRecordType::kInbound), record);
}

auto
MmapSessionStore::EnsureMapping() -> base::Status
{
  if (impl_->fd < 0 || !impl_->mapping) {
    return Open();
  }

  struct stat stat_buffer{};
  if (::fstat(impl_->fd, &stat_buffer) != 0) {
    return base::Status::IoError(IoErrorMessage(path_, "unable to stat mmap store"));
  }

  const auto file_size = static_cast<std::size_t>(stat_buffer.st_size);
  if (file_size != impl_->mapping->size) {
    return Open();
  }

  return base::Status::Ok();
}

auto
MmapSessionStore::LoadOutboundRange(std::uint64_t session_id, std::uint32_t begin_seq, std::uint32_t end_seq) const
  -> base::Result<std::vector<MessageRecord>>
{
  if (begin_seq == 0 || end_seq == 0 || begin_seq > end_seq) {
    return base::Status::InvalidArgument("invalid outbound load range");
  }

  auto status = const_cast<MmapSessionStore*>(this)->EnsureMapping();
  if (!status.ok()) {
    return status;
  }

  std::vector<MessageRecord> records;
  const auto session_it = impl_->outbound_offsets.find(session_id);
  if (session_it == impl_->outbound_offsets.end()) {
    return records;
  }

  for (auto it = session_it->second.lower_bound(begin_seq); it != session_it->second.end() && it->first <= end_seq;
       ++it) {
    const auto* header = RecordAt(*impl_->mapping, it->second);
    MessageRecord record;
    record.session_id = header->session_id;
    record.seq_num = header->seq_num;
    record.timestamp_ns = header->timestamp_ns;
    record.flags = header->flags;
    record.body_start_offset = header->body_start_offset;
    record.payload.resize(header->payload_size);
    if (header->payload_size != 0) {
      std::memcpy(record.payload.data(), PayloadAt(*impl_->mapping, it->second), header->payload_size);
    }
    records.push_back(std::move(record));
  }

  return records;
}

auto
MmapSessionStore::LoadOutboundRangeViews(std::uint64_t session_id, std::uint32_t begin_seq, std::uint32_t end_seq) const
  -> base::Result<MessageRecordViewRange>
{
  if (begin_seq == 0 || end_seq == 0 || begin_seq > end_seq) {
    return base::Status::InvalidArgument("invalid outbound load range");
  }

  auto status = const_cast<MmapSessionStore*>(this)->EnsureMapping();
  if (!status.ok()) {
    return status;
  }

  MessageRecordViewRange records;
  const auto session_it = impl_->outbound_offsets.find(session_id);
  if (session_it == impl_->outbound_offsets.end()) {
    return records;
  }

  records.borrowed_payload_owner = impl_->mapping;
  records.records.reserve(static_cast<std::size_t>(end_seq - begin_seq + 1U));
  for (auto it = session_it->second.lower_bound(begin_seq); it != session_it->second.end() && it->first <= end_seq;
       ++it) {
    const auto* header = RecordAt(*impl_->mapping, it->second);
    records.records.push_back(MessageRecordView{
      .session_id = header->session_id,
      .seq_num = header->seq_num,
      .timestamp_ns = header->timestamp_ns,
      .flags = header->flags,
      .payload = std::span<const std::byte>(PayloadAt(*impl_->mapping, it->second), header->payload_size),
      .body_start_offset = header->body_start_offset,
    });
  }

  return records;
}

auto
MmapSessionStore::SaveRecoveryState(const SessionRecoveryState& state) -> base::Status
{
  return AppendRecoveryState(state);
}

auto
MmapSessionStore::LoadRecoveryState(std::uint64_t session_id) const -> base::Result<SessionRecoveryState>
{
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

auto
MmapSessionStore::RemapFile(std::size_t new_size) -> base::Status
{
  if (!impl_->mapping) {
    return Open();
  }

  auto mapping = MapFile(impl_->fd, new_size);
  if (!mapping.ok()) {
    return mapping.status();
  }
  impl_->mapping = std::move(mapping).value();
  return base::Status::Ok();
}

auto
MmapSessionStore::CloseResources() -> void
{
  impl_->mapping.reset();
  if (impl_->fd >= 0) {
    ::close(impl_->fd);
    impl_->fd = -1;
  }
  impl_->outbound_offsets.clear();
  impl_->recovery_states.clear();
}

auto
MmapSessionStore::Flush() -> base::Status
{
  if (impl_->fd < 0) {
    return base::Status::IoError("mmap store is not open");
  }
  if (::fdatasync(impl_->fd) != 0) {
    return base::Status::IoError(IoErrorMessage(path_, "unable to fdatasync mmap store"));
  }
  return base::Status::Ok();
}

auto
MmapSessionStore::Refresh() -> base::Status
{
  CloseResources();
  return Open();
}

auto
MmapSessionStore::ResetSession(std::uint64_t session_id) -> base::Status
{
  if (session_id == 0U) {
    return base::Status::InvalidArgument("session reset requires a valid session id");
  }

  auto status = Open();
  if (!status.ok()) {
    return status;
  }

  if (path_.has_parent_path()) {
    std::error_code error;
    std::filesystem::create_directories(path_.parent_path(), error);
    if (error) {
      return base::Status::IoError("unable to prepare mmap store directory '" + path_.parent_path().string() +
                                   "': " + error.message());
    }
  }

  std::filesystem::path rebuilt_path = path_;
  rebuilt_path += ".reset.tmp";
  std::error_code remove_error;
  std::filesystem::remove(rebuilt_path, remove_error);
  if (remove_error) {
    return base::Status::IoError("unable to prepare temporary mmap store '" + rebuilt_path.string() +
                                 "': " + remove_error.message());
  }

  MmapSessionStore rebuilt(rebuilt_path, sync_policy_);
  status = rebuilt.Open();
  if (!status.ok()) {
    return status;
  }

  const auto mapping = impl_->mapping;
  const auto scan = ScanMapping(*mapping, Header(*mapping)->file_size);
  std::uint64_t offset = kStoreFileHeaderSize;
  while (offset < scan.committed_size) {
    const auto* record = RecordAt(*mapping, offset);
    const auto record_end = offset + kStoreRecordHeaderSize + record->payload_size;
    if (record->session_id != session_id) {
      switch (static_cast<StoreRecordType>(record->record_type)) {
        case StoreRecordType::kOutbound: {
          status = rebuilt.SaveOutboundView(MessageRecordView{
            .session_id = record->session_id,
            .seq_num = record->seq_num,
            .timestamp_ns = record->timestamp_ns,
            .flags = record->flags,
            .payload = std::span<const std::byte>(PayloadAt(*mapping, offset), record->payload_size),
            .body_start_offset = record->body_start_offset,
          });
          break;
        }
        case StoreRecordType::kInbound: {
          status = rebuilt.SaveInboundView(MessageRecordView{
            .session_id = record->session_id,
            .seq_num = record->seq_num,
            .timestamp_ns = record->timestamp_ns,
            .flags = record->flags,
            .payload = std::span<const std::byte>(PayloadAt(*mapping, offset), record->payload_size),
            .body_start_offset = record->body_start_offset,
          });
          break;
        }
        case StoreRecordType::kRecoveryState:
          status = rebuilt.SaveRecoveryState(SessionRecoveryState{
            .session_id = record->session_id,
            .next_in_seq = record->next_in_seq,
            .next_out_seq = record->next_out_seq,
            .last_inbound_ns = record->last_inbound_ns,
            .last_outbound_ns = record->last_outbound_ns,
            .active = record->active != 0,
          });
          break;
        default:
          status = base::Status::FormatError("mmap store contains an unknown record type");
          break;
      }
      if (!status.ok()) {
        return status;
      }
    }
    offset = record_end;
  }

  status = rebuilt.Flush();
  if (!status.ok()) {
    return status;
  }

  CloseResources();

  std::error_code rename_error;
  std::filesystem::rename(rebuilt_path, path_, rename_error);
  if (rename_error) {
    return base::Status::IoError("unable to replace mmap store '" + path_.string() + "': " + rename_error.message());
  }

  return Open();
}

} // namespace fastfix::store