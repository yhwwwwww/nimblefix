#pragma once

#include <filesystem>
#include <span>

#include "fastfix/base/result.h"
#include "fastfix/profile/artifact.h"
#include "fastfix/profile/normalized_dictionary.h"

namespace fastfix::profile {

struct ProfileLoadOptions {
    bool madvise{false};
    bool mlock{false};
};

auto LoadProfileArtifact(const std::filesystem::path& path,
                         const ProfileLoadOptions& options = {}) -> base::Result<LoadedProfile>;

auto LoadProfileFromDictionary(const NormalizedDictionary& dictionary) -> base::Result<LoadedProfile>;

auto LoadProfileFromDictionaryFiles(std::span<const std::filesystem::path> paths) -> base::Result<LoadedProfile>;

}  // namespace fastfix::profile
