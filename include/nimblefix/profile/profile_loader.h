#pragma once

#include <filesystem>
#include <span>

#include "nimblefix/base/result.h"
#include "nimblefix/profile/artifact.h"
#include "nimblefix/profile/normalized_dictionary.h"

namespace nimble::profile {

struct ProfileLoadOptions
{
  bool madvise{ false };
  bool mlock{ false };
};

auto
LoadProfileArtifact(const std::filesystem::path& path, const ProfileLoadOptions& options = {})
  -> base::Result<LoadedProfile>;

auto
LoadProfileFromDictionary(const NormalizedDictionary& dictionary) -> base::Result<LoadedProfile>;

auto
LoadProfileFromDictionaryFiles(std::span<const std::filesystem::path> paths) -> base::Result<LoadedProfile>;

auto
ValidateSchemaHash(const LoadedProfile& profile, std::uint64_t expected_hash) -> base::Status;

} // namespace nimble::profile
