#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

namespace fastfix::profile {

inline constexpr std::array<std::uint8_t, 8> kArtifactMagic = {
    'F', 'X', 'A', 'R', 'T', '0', '0', '1'};
inline constexpr std::uint32_t kArtifactFormatVersion = 1;
inline constexpr std::uint32_t kArtifactEndianLittle = 0x01020304u;

enum class SectionKind : std::uint32_t {
    kStringTable = 1,
    kFieldDefs = 2,
    kMessageDefs = 3,
    kGroupDefs = 4,
    kAdminRules = 5,
    kValidationRules = 6,
    kLookupTables = 7,
    kTemplateDescriptors = 8,
    kMessageFieldRules = 9,
    kGroupFieldRules = 10,
};

enum class ArtifactFlags : std::uint32_t {
    kNone = 0,
};

enum class SectionFlags : std::uint32_t {
    kNone = 0,
};

#pragma pack(push, 1)
struct ArtifactHeader {
    std::uint8_t magic[8];
    std::uint32_t format_version;
    std::uint32_t header_size;
    std::uint32_t section_entry_size;
    std::uint32_t endian_tag;
    std::uint64_t file_size;
    std::uint64_t section_table_offset;
    std::uint32_t section_count;
    std::uint32_t flags;
    std::uint64_t schema_hash;
    std::uint64_t profile_id;
    std::uint64_t reserved1;
    std::uint64_t reserved0;
};

struct ArtifactSection {
    std::uint32_t kind;
    std::uint32_t flags;
    std::uint64_t offset;
    std::uint64_t size;
    std::uint64_t entry_count;
    std::uint64_t entry_size;
};
#pragma pack(pop)

static_assert(sizeof(ArtifactHeader) == 80);
static_assert(sizeof(ArtifactSection) == 40);

struct ArtifactSpan {
    const std::byte* data{nullptr};
    std::size_t size{0};
};

class StringTableView {
  public:
    StringTableView() = default;

    explicit StringTableView(std::span<const std::byte> bytes)
        : bytes_(bytes) {
    }

    [[nodiscard]] bool empty() const {
        return bytes_.empty();
    }

    [[nodiscard]] std::size_t size() const {
        return bytes_.size();
    }

    [[nodiscard]] std::span<const std::byte> bytes() const {
        return bytes_;
    }

    [[nodiscard]] std::optional<std::string_view> string_at(std::size_t offset) const {
        if (offset >= bytes_.size()) {
            return std::nullopt;
        }

        const auto* begin = reinterpret_cast<const char*>(bytes_.data() + offset);
        const auto* end = reinterpret_cast<const char*>(bytes_.data() + bytes_.size());
        const auto* terminator = static_cast<const char*>(
            std::memchr(begin, '\0', static_cast<std::size_t>(end - begin)));
        if (terminator == nullptr) {
            return std::nullopt;
        }

        return std::string_view(begin, static_cast<std::size_t>(terminator - begin));
    }

  private:
    std::span<const std::byte> bytes_{};
};

template <typename Entry>
class FixedSectionView {
  public:
    static_assert(std::is_trivially_copyable_v<Entry>);

    FixedSectionView() = default;

    FixedSectionView(const ArtifactSection* descriptor, std::span<const Entry> entries)
        : descriptor_(descriptor), entries_(entries) {
    }

    [[nodiscard]] bool valid() const {
        return descriptor_ != nullptr;
    }

    [[nodiscard]] const ArtifactSection& descriptor() const {
        return *descriptor_;
    }

    [[nodiscard]] std::span<const Entry> entries() const {
        return entries_;
    }

    [[nodiscard]] std::size_t size() const {
        return entries_.size();
    }

    [[nodiscard]] bool empty() const {
        return entries_.empty();
    }

    [[nodiscard]] const Entry& operator[](std::size_t index) const {
        return entries_[index];
    }

  private:
    const ArtifactSection* descriptor_{nullptr};
    std::span<const Entry> entries_{};
};

class LoadedProfile {
  public:
    LoadedProfile() = default;

    [[nodiscard]] bool valid() const {
        return header_ != nullptr;
    }

    [[nodiscard]] const ArtifactHeader& header() const {
        return *header_;
    }

    [[nodiscard]] std::span<const ArtifactSection> sections() const {
        return sections_;
    }

    [[nodiscard]] std::uint64_t profile_id() const {
        return header().profile_id;
    }

    [[nodiscard]] std::uint64_t schema_hash() const {
        return header().schema_hash;
    }

    [[nodiscard]] const ArtifactSection* find_section_descriptor(SectionKind kind) const {
        for (const auto& section : sections_) {
            if (section.kind == static_cast<std::uint32_t>(kind)) {
                return &section;
            }
        }

        return nullptr;
    }

    [[nodiscard]] std::optional<ArtifactSpan> find_section(SectionKind kind) const {
        const auto* section = find_section_descriptor(kind);
        if (section == nullptr) {
            return std::nullopt;
        }

        return ArtifactSpan{
            .data = data_ + section->offset,
            .size = static_cast<std::size_t>(section->size),
        };
    }

    [[nodiscard]] std::optional<StringTableView> string_table() const {
        const auto raw = find_section(SectionKind::kStringTable);
        if (!raw.has_value()) {
            return std::nullopt;
        }

        return StringTableView(std::span<const std::byte>(raw->data, raw->size));
    }

    template <typename Entry>
    [[nodiscard]] std::optional<FixedSectionView<Entry>> fixed_section(SectionKind kind) const {
        static_assert(std::is_trivially_copyable_v<Entry>);

        const auto* section = find_section_descriptor(kind);
        if (section == nullptr) {
            return std::nullopt;
        }

        if (section->entry_size != sizeof(Entry)) {
            return std::nullopt;
        }

        if (section->entry_count == 0) {
            return FixedSectionView<Entry>(section, {});
        }

        if ((section->offset % alignof(Entry)) != 0) {
            return std::nullopt;
        }

        const auto* entries = reinterpret_cast<const Entry*>(data_ + section->offset);
        return FixedSectionView<Entry>(
            section,
            std::span<const Entry>(entries, static_cast<std::size_t>(section->entry_count)));
    }

  private:
    friend class ProfileLoaderAccess;
    friend auto MakeLoadedProfile(
        std::shared_ptr<const std::byte> storage,
        std::size_t size,
        const ArtifactHeader* header,
        std::span<const ArtifactSection> sections) -> LoadedProfile;

    std::shared_ptr<const std::byte> storage_;
    const std::byte* data_{nullptr};
    std::size_t size_{0};
    const ArtifactHeader* header_{nullptr};
    std::span<const ArtifactSection> sections_{};
};

inline bool HasArtifactMagic(const ArtifactHeader& header) {
    return std::memcmp(header.magic, kArtifactMagic.data(), kArtifactMagic.size()) == 0;
}

inline bool HasLittleEndianTag(const ArtifactHeader& header) {
    return header.endian_tag == kArtifactEndianLittle;
}

inline auto MakeLoadedProfile(
    std::shared_ptr<const std::byte> storage,
    std::size_t size,
    const ArtifactHeader* header,
    std::span<const ArtifactSection> sections) -> LoadedProfile {
    LoadedProfile profile;
    profile.data_ = storage.get();
    profile.storage_ = std::move(storage);
    profile.size_ = size;
    profile.header_ = header;
    profile.sections_ = sections;
    return profile;
}

}  // namespace fastfix::profile
