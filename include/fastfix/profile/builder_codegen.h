#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "fastfix/base/result.h"
#include "fastfix/base/status.h"
#include "fastfix/profile/normalized_dictionary.h"

namespace fastfix::profile {

auto GenerateCppBuildersHeader(
    const NormalizedDictionary& dictionary,
    std::string_view namespace_name = {}) -> base::Result<std::string>;

auto WriteCppBuildersHeader(
    const std::filesystem::path& path,
    const NormalizedDictionary& dictionary,
    std::string_view namespace_name = {}) -> base::Status;

}  // namespace fastfix::profile