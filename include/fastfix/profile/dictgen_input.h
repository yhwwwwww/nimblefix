#pragma once

#include <filesystem>
#include <string_view>

#include "fastfix/base/result.h"
#include "fastfix/profile/normalized_dictionary.h"

namespace fastfix::profile {

auto
LoadNormalizedDictionaryText(std::string_view text) -> base::Result<NormalizedDictionary>;

auto
LoadNormalizedDictionaryFile(const std::filesystem::path& path) -> base::Result<NormalizedDictionary>;

} // namespace fastfix::profile
