#pragma once

#include <filesystem>
#include <string_view>

#include "nimblefix/base/result.h"
#include "nimblefix/runtime/config.h"

namespace nimble::runtime {

// Internal .nfcfg format parser used by tools and tests.
// Applications should populate EngineConfig directly from their own
// configuration source.
auto
LoadEngineConfigText(std::string_view text, const std::filesystem::path& base_dir = {}) -> base::Result<EngineConfig>;
auto
LoadEngineConfigFile(const std::filesystem::path& path) -> base::Result<EngineConfig>;

} // namespace nimble::runtime
