#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <vector>

#include "nimblefix/base/result.h"
#include "nimblefix/base/status.h"
#include "nimblefix/profile/normalized_dictionary.h"

namespace nimble::profile {

auto
BuildProfileArtifact(const NormalizedDictionary& dictionary) -> base::Result<std::vector<std::byte>>;

auto
WriteProfileArtifact(const std::filesystem::path& path, std::span<const std::byte> bytes) -> base::Status;

} // namespace nimble::profile
