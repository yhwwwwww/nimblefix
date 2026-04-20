#pragma once

#include <filesystem>
#include <string_view>

#include "nimblefix/base/result.h"
#include "nimblefix/profile/normalized_dictionary.h"

namespace nimble::profile {

auto
LoadNormalizedDictionaryText(std::string_view text) -> base::Result<NormalizedDictionary>;

auto
LoadNormalizedDictionaryFile(const std::filesystem::path& path) -> base::Result<NormalizedDictionary>;

} // namespace nimble::profile
