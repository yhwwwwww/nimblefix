#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <vector>

#include "fastfix/base/result.h"
#include "fastfix/base/status.h"
#include "fastfix/profile/normalized_dictionary.h"

namespace fastfix::profile {

auto BuildProfileArtifact(const NormalizedDictionary& dictionary)
    -> base::Result<std::vector<std::byte>>;

auto WriteProfileArtifact(const std::filesystem::path& path, std::span<const std::byte> bytes)
    -> base::Status;

}  // namespace fastfix::profile
