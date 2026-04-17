#include "fastfix/profile/profile_loader.h"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "fastfix/profile/artifact_builder.h"
#include "fastfix/profile/dictgen_input.h"
#include "fastfix/profile/overlay.h"

namespace fastfix::profile {

namespace {

auto
IoMessage(const std::filesystem::path& path, const char* action) -> std::string
{
  return action + std::string(" '") + path.string() + "': " + std::strerror(errno);
}

auto
InBounds(std::uint64_t offset, std::uint64_t length, std::uint64_t total) -> bool
{
  if (offset > total) {
    return false;
  }
  if (length > total - offset) {
    return false;
  }
  return true;
}

auto
MultiplyWillOverflow(std::uint64_t lhs, std::uint64_t rhs) -> bool
{
  if (lhs == 0 || rhs == 0) {
    return false;
  }
  return lhs > (std::numeric_limits<std::uint64_t>::max() / rhs);
}

auto
ValidateArtifact(const std::filesystem::path& path,
                 const std::byte* data,
                 std::size_t size,
                 const ArtifactHeader*& header_out,
                 std::span<const ArtifactSection>& sections_out) -> base::Status
{
  if constexpr (std::endian::native != std::endian::little) {
    return base::Status::VersionMismatch("artifact loading currently requires a little-endian host");
  }

  if (size < sizeof(ArtifactHeader)) {
    return base::Status::FormatError("artifact '" + path.string() + "' is smaller than the header");
  }

  const auto* header = reinterpret_cast<const ArtifactHeader*>(data);
  if (!HasArtifactMagic(*header)) {
    return base::Status::FormatError("artifact '" + path.string() + "' has an invalid magic");
  }

  if (header->format_version != kArtifactFormatVersion) {
    return base::Status::VersionMismatch("artifact '" + path.string() + "' has an unsupported format version");
  }

  if (header->header_size != sizeof(ArtifactHeader)) {
    return base::Status::FormatError("artifact '" + path.string() + "' has an unexpected header size");
  }

  if (header->section_entry_size != sizeof(ArtifactSection)) {
    return base::Status::FormatError("artifact '" + path.string() + "' has an unexpected section entry size");
  }

  if (!HasLittleEndianTag(*header)) {
    return base::Status::VersionMismatch("artifact '" + path.string() + "' has an unsupported endian tag");
  }

  if (header->file_size != size) {
    return base::Status::FormatError("artifact '" + path.string() + "' has a mismatched file size");
  }

  const auto section_table_bytes = static_cast<std::uint64_t>(header->section_count) * sizeof(ArtifactSection);
  if (header->section_table_offset < header->header_size) {
    return base::Status::FormatError("artifact '" + path.string() + "' has a section table overlapping the header");
  }
  if (!InBounds(header->section_table_offset, section_table_bytes, header->file_size)) {
    return base::Status::FormatError("artifact '" + path.string() + "' has an out-of-bounds section table");
  }

  const auto section_table_end = header->section_table_offset + section_table_bytes;

  const auto* sections = reinterpret_cast<const ArtifactSection*>(data + header->section_table_offset);
  sections_out = std::span<const ArtifactSection>(sections, header->section_count);

  std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
  ranges.reserve(sections_out.size());

  for (const auto& section : sections_out) {
    if (!InBounds(section.offset, section.size, header->file_size)) {
      return base::Status::FormatError("artifact '" + path.string() + "' has an out-of-bounds section");
    }

    if (section.offset < section_table_end) {
      return base::Status::FormatError("artifact '" + path.string() +
                                       "' has a section overlapping the header or section table");
    }

    if (section.entry_size == 0) {
      if (section.entry_count != 0 || section.size != 0) {
        return base::Status::FormatError("artifact '" + path.string() +
                                         "' has a malformed zero-sized section entry descriptor");
      }
    } else {
      if (MultiplyWillOverflow(section.entry_count, section.entry_size)) {
        return base::Status::FormatError("artifact '" + path.string() +
                                         "' has an overflowing section size declaration");
      }

      if (section.entry_count * section.entry_size != section.size) {
        return base::Status::FormatError("artifact '" + path.string() +
                                         "' has a section whose size does not "
                                         "match entry_count * entry_size");
      }
    }

    ranges.emplace_back(section.offset, section.offset + section.size);
  }

  std::sort(ranges.begin(), ranges.end());
  for (std::size_t i = 1; i < ranges.size(); ++i) {
    if (ranges[i - 1].second > ranges[i].first) {
      return base::Status::FormatError("artifact '" + path.string() + "' has overlapping sections");
    }
  }

  header_out = header;
  return base::Status::Ok();
}

} // namespace

auto
LoadProfileArtifact(const std::filesystem::path& path, const ProfileLoadOptions& options) -> base::Result<LoadedProfile>
{
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    return base::Status::IoError(IoMessage(path, "unable to open artifact"));
  }

  struct stat stat_buffer{};
  if (::fstat(fd, &stat_buffer) != 0) {
    const auto status = base::Status::IoError(IoMessage(path, "unable to stat artifact"));
    ::close(fd);
    return status;
  }

  if (stat_buffer.st_size <= 0) {
    ::close(fd);
    return base::Status::FormatError("artifact '" + path.string() + "' is empty");
  }

  const auto size = static_cast<std::size_t>(stat_buffer.st_size);
  void* mapping = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
  const int mmap_errno = errno;
  ::close(fd);

  if (mapping == MAP_FAILED) {
    errno = mmap_errno;
    return base::Status::IoError(IoMessage(path, "unable to mmap artifact"));
  }

  if (options.madvise) {
    if (::madvise(mapping, size, MADV_WILLNEED) != 0) {
      std::fprintf(stderr, "fastfix: madvise(MADV_WILLNEED) failed for '%s': %s\n", path.c_str(), std::strerror(errno));
    }
  }

  if (options.mlock) {
    if (::mlock(mapping, size) != 0) {
      std::fprintf(stderr, "fastfix: mlock failed for '%s': %s\n", path.c_str(), std::strerror(errno));
    }
  }

  auto storage =
    std::shared_ptr<const std::byte>(static_cast<const std::byte*>(mapping),
                                     [size](const std::byte* bytes) { ::munmap(const_cast<std::byte*>(bytes), size); });

  const ArtifactHeader* header = nullptr;
  std::span<const ArtifactSection> sections;
  auto status = ValidateArtifact(path, storage.get(), size, header, sections);
  if (!status.ok()) {
    return status;
  }

  return MakeLoadedProfile(std::move(storage), size, header, sections);
}

auto
LoadProfileFromDictionary(const NormalizedDictionary& dictionary) -> base::Result<LoadedProfile>
{
  auto artifact = BuildProfileArtifact(dictionary);
  if (!artifact.ok()) {
    return artifact.status();
  }

  auto bytes = std::move(artifact).value();
  const auto size = bytes.size();

  auto storage_vec = std::make_shared<std::vector<std::byte>>(std::move(bytes));
  auto storage = std::shared_ptr<const std::byte>(storage_vec, storage_vec->data());

  const ArtifactHeader* header = nullptr;
  std::span<const ArtifactSection> sections;
  auto status = ValidateArtifact("<in-memory>", storage.get(), size, header, sections);
  if (!status.ok()) {
    return status;
  }

  return MakeLoadedProfile(std::move(storage), size, header, sections);
}

auto
LoadProfileFromDictionaryFiles(std::span<const std::filesystem::path> paths) -> base::Result<LoadedProfile>
{
  if (paths.empty()) {
    return base::Status::InvalidArgument("no dictionary files provided");
  }

  auto baseline = LoadNormalizedDictionaryFile(paths[0]);
  if (!baseline.ok()) {
    return baseline.status();
  }

  if (baseline.value().profile_id == 0) {
    return base::Status::InvalidArgument("baseline dictionary is missing profile_id");
  }

  auto merged = std::move(baseline).value();
  for (std::size_t i = 1; i < paths.size(); ++i) {
    auto additional = LoadNormalizedDictionaryFile(paths[i]);
    if (!additional.ok()) {
      return additional.status();
    }
    auto result = ApplyOverlay(merged, additional.value());
    if (!result.ok()) {
      return result.status();
    }
    merged = std::move(result).value();
  }

  return LoadProfileFromDictionary(merged);
}

auto
ValidateSchemaHash(const LoadedProfile& profile, std::uint64_t expected_hash) -> base::Status
{
  if (!profile.valid()) {
    return base::Status::InvalidArgument("ValidateSchemaHash: loaded profile is not valid");
  }
  const auto actual_hash = profile.schema_hash();
  if (actual_hash != expected_hash) {
    return base::Status::VersionMismatch("schema_hash mismatch: artifact has " + std::to_string(actual_hash) +
                                         " but generated code expects " + std::to_string(expected_hash));
  }
  return base::Status::Ok();
}

} // namespace fastfix::profile
