#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>

#include "fastfix/profile/artifact.h"
#include "fastfix/profile/profile_loader.h"
#include "fastfix/runtime/engine.h"

#include "test_support.h"

namespace {

#pragma pack(push, 1)
struct TestFieldDef {
    std::uint32_t tag;
    std::uint32_t name_offset;
};
#pragma pack(pop)

static_assert(sizeof(TestFieldDef) == 8);

auto WriteTestArtifact(const std::filesystem::path& path) -> void {
    constexpr std::array<char, 15> kStringTable = {
        '\0', 'f', 'a', 's', 't', 'f', 'i', 'x', '-', 't', 'e', 's', 't', '\0', '\0'};
    constexpr std::array<TestFieldDef, 2> kFieldDefs = {{
        {35U, 1U},
        {49U, 1U},
    }};

    fastfix::profile::ArtifactHeader header{};
    std::memcpy(header.magic, fastfix::profile::kArtifactMagic.data(), fastfix::profile::kArtifactMagic.size());
    header.format_version = fastfix::profile::kArtifactFormatVersion;
    header.header_size = sizeof(fastfix::profile::ArtifactHeader);
    header.section_entry_size = sizeof(fastfix::profile::ArtifactSection);
    header.endian_tag = fastfix::profile::kArtifactEndianLittle;
    header.file_size = sizeof(fastfix::profile::ArtifactHeader) +
        (2 * sizeof(fastfix::profile::ArtifactSection)) +
        kStringTable.size() +
        sizeof(kFieldDefs);
    header.section_table_offset = sizeof(fastfix::profile::ArtifactHeader);
    header.section_count = 2;
    header.flags = static_cast<std::uint32_t>(fastfix::profile::ArtifactFlags::kNone);
    header.schema_hash = 0x1122334455667788ULL;
    header.profile_id = 42ULL;
    header.reserved1 = 0;

    const std::uint64_t string_table_offset = sizeof(fastfix::profile::ArtifactHeader) +
        (2 * sizeof(fastfix::profile::ArtifactSection));
    const std::uint64_t field_defs_offset = string_table_offset + kStringTable.size();

    fastfix::profile::ArtifactSection string_section{};
    string_section.kind = static_cast<std::uint32_t>(fastfix::profile::SectionKind::kStringTable);
    string_section.flags = static_cast<std::uint32_t>(fastfix::profile::SectionFlags::kNone);
    string_section.offset = string_table_offset;
    string_section.size = kStringTable.size();
    string_section.entry_count = kStringTable.size();
    string_section.entry_size = 1;

    fastfix::profile::ArtifactSection field_section{};
    field_section.kind = static_cast<std::uint32_t>(fastfix::profile::SectionKind::kFieldDefs);
    field_section.flags = static_cast<std::uint32_t>(fastfix::profile::SectionFlags::kNone);
    field_section.offset = field_defs_offset;
    field_section.size = sizeof(kFieldDefs);
    field_section.entry_count = kFieldDefs.size();
    field_section.entry_size = sizeof(TestFieldDef);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.write(reinterpret_cast<const char*>(&string_section), sizeof(string_section));
    out.write(reinterpret_cast<const char*>(&field_section), sizeof(field_section));
    out.write(kStringTable.data(), static_cast<std::streamsize>(kStringTable.size()));
    out.write(reinterpret_cast<const char*>(kFieldDefs.data()), static_cast<std::streamsize>(sizeof(kFieldDefs)));
}

}  // namespace

TEST_CASE("profile-loader", "[profile-loader]") {    const auto artifact_path = std::filesystem::temp_directory_path() / "fastfix-profile-loader-test.art";
    WriteTestArtifact(artifact_path);

    auto loaded = fastfix::profile::LoadProfileArtifact(artifact_path);
    REQUIRE(loaded.ok());
    REQUIRE(loaded.value().valid());
    REQUIRE(loaded.value().profile_id() == 42ULL);
    REQUIRE(loaded.value().schema_hash() == 0x1122334455667788ULL);
    REQUIRE(loaded.value().sections().size() == 2U);

    const auto string_table = loaded.value().string_table();
    REQUIRE(string_table.has_value());
    const auto loaded_string = string_table->string_at(1);
    REQUIRE(loaded_string.has_value());
    REQUIRE(*loaded_string == "fastfix-test");

    const auto field_defs = loaded.value().fixed_section<TestFieldDef>(fastfix::profile::SectionKind::kFieldDefs);
    REQUIRE(field_defs.has_value());
    REQUIRE(field_defs->size() == 2U);
    REQUIRE((*field_defs)[0].tag == 35U);
    REQUIRE((*field_defs)[0].name_offset == 1U);
    REQUIRE((*field_defs)[1].tag == 49U);

    fastfix::runtime::Engine engine;
    fastfix::runtime::EngineConfig config;
    config.profile_artifacts.push_back(artifact_path);

    const auto status = engine.LoadProfiles(config);
    REQUIRE(status.ok());
    REQUIRE(engine.profiles().size() == 1U);
    REQUIRE(engine.profiles().Find(42ULL) != nullptr);

    std::filesystem::remove(artifact_path);
}

TEST_CASE("profile madvise warming", "[profile-loader]") {
    const auto artifact_path = std::filesystem::temp_directory_path() / "fastfix-madvise-test.art";
    WriteTestArtifact(artifact_path);

    fastfix::profile::ProfileLoadOptions options;
    options.madvise = true;

    auto loaded = fastfix::profile::LoadProfileArtifact(artifact_path, options);
    REQUIRE(loaded.ok());
    REQUIRE(loaded.value().valid());
    REQUIRE(loaded.value().profile_id() == 42ULL);

    std::filesystem::remove(artifact_path);
}

TEST_CASE("profile mlock warming", "[profile-loader]") {
    const auto artifact_path = std::filesystem::temp_directory_path() / "fastfix-mlock-test.art";
    WriteTestArtifact(artifact_path);

    fastfix::profile::ProfileLoadOptions options;
    options.mlock = true;

    // mlock may fail due to RLIMIT_MEMLOCK but must not abort the load
    auto loaded = fastfix::profile::LoadProfileArtifact(artifact_path, options);
    REQUIRE(loaded.ok());
    REQUIRE(loaded.value().valid());
    REQUIRE(loaded.value().profile_id() == 42ULL);

    std::filesystem::remove(artifact_path);
}

TEST_CASE("profile warming disabled by default", "[profile-loader]") {
    fastfix::runtime::EngineConfig config;
    REQUIRE(config.profile_madvise == false);
    REQUIRE(config.profile_mlock == false);

    fastfix::profile::ProfileLoadOptions options;
    REQUIRE(options.madvise == false);
    REQUIRE(options.mlock == false);
}

