#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

#include "fastfix/profile/dictgen_input.h"
#include "fastfix/runtime/config.h"
#include "fastfix/runtime/config_io.h"

#include "test_support.h"

namespace {

auto ReadText(const std::filesystem::path& path) -> std::string {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("parser-surface", "[parser-surface]") {    const auto root = std::filesystem::path(FASTFIX_PROJECT_DIR) / "tests" / "data" / "fuzz";

    const auto valid_config = ReadText(root / "runtime_config_valid.ffcfg");
    const auto invalid_config = ReadText(root / "runtime_config_invalid.ffcfg");
    const auto valid_dict = ReadText(root / "dictgen_valid.ffd");
    const auto invalid_dict = ReadText(root / "dictgen_invalid.ffd");
    const auto valid_overlay = ReadText(root / "overlay_valid.ffd");

    auto config = fastfix::runtime::LoadEngineConfigText(valid_config, root);
    REQUIRE(config.ok());
    REQUIRE(config.value().worker_count == 2U);
    REQUIRE(config.value().counterparties.size() == 1U);

    REQUIRE(!fastfix::runtime::LoadEngineConfigText(invalid_config, root).ok());

    auto dictionary = fastfix::profile::LoadNormalizedDictionaryText(valid_dict);
    REQUIRE(dictionary.ok());
    REQUIRE(dictionary.value().fields.size() == 4U);

    REQUIRE(!fastfix::profile::LoadNormalizedDictionaryText(invalid_dict).ok());

    auto overlay = fastfix::profile::LoadNormalizedDictionaryText(valid_overlay);
    REQUIRE(overlay.ok());
    REQUIRE(overlay.value().fields.size() == 1U);
}