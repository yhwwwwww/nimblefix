#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#include "nimblefix/profile/contract_sidecar.h"
#include "nimblefix/profile/dictgen_input.h"

#include "../tools/orchestra-import/orchestra_import.h"

namespace {

auto
Contains(std::string_view haystack, std::string_view needle) -> bool
{
  return haystack.find(needle) != std::string_view::npos;
}

auto
ReadTextFile(const std::filesystem::path& path) -> std::string
{
  std::ifstream stream(path);
  REQUIRE(stream.is_open());
  return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("orchestra importer converts fixture xml into dictionary and contract sidecar", "[orchestra-import]")
{
  const auto root = std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / "tests" / "data" / "orchestra";
  const auto xml_text = ReadTextFile(root / "minimal_orchestra.xml");

  auto imported = nimble::tools::ImportOrchestraXml(xml_text, 9911U, "minimal_orchestra.xml");
  REQUIRE(imported.ok());
  REQUIRE(imported.value().dictionary.profile_id == 9911U);
  REQUIRE(imported.value().dictionary.schema_hash == imported.value().contract.schema_hash);
  REQUIRE(imported.value().contract.contract_id != 0U);
  REQUIRE(imported.value().contract.conditional_rules.size() == 1U);
  REQUIRE(!imported.value().warnings.empty());

  auto service_it =
    std::find_if(imported.value().contract.service_messages.begin(),
                 imported.value().contract.service_messages.end(),
                 [](const auto& service_message) {
                   return service_message.service_name == "admin-only" && service_message.msg_type == "A";
                 });
  REQUIRE(service_it != imported.value().contract.service_messages.end());
  REQUIRE(service_it->direction == nimble::profile::ContractDirection::kBoth);

  const auto nfd_text = nimble::tools::SerializeDictionaryAsNfd(imported.value().dictionary);
  auto parsed_dictionary = nimble::profile::LoadNormalizedDictionaryText(nfd_text);
  REQUIRE(parsed_dictionary.ok());
  REQUIRE(parsed_dictionary.value().profile_id == 9911U);
  REQUIRE(parsed_dictionary.value().schema_hash == imported.value().dictionary.schema_hash);
  REQUIRE(parsed_dictionary.value().messages.size() == 3U);

  const auto contract_text = nimble::profile::SerializeContractSidecar(imported.value().contract);
  auto round_trip_contract = nimble::profile::LoadContractSidecarText(contract_text);
  REQUIRE(round_trip_contract.ok());
  REQUIRE(round_trip_contract.value().contract_id == imported.value().contract.contract_id);
  REQUIRE(round_trip_contract.value().enum_rules.size() >= 1U);

  const auto markdown = nimble::profile::RenderContractMarkdown(round_trip_contract.value());
  REQUIRE(Contains(markdown, "price-required-for-limit"));
  REQUIRE(Contains(markdown, "order-entry"));

  const auto augmentations = nimble::profile::GenerateInteropScenarioAugmentations(round_trip_contract.value());
  REQUIRE(augmentations.size() >= 2U);
  REQUIRE(augmentations.front().file_name.ends_with(".nfscenario"));

  const auto cache_layout = nimble::tools::ResolveOrchestraCacheLayout("/tmp/nimblefix-orchestra-cache", 9911U);
  REQUIRE(cache_layout.dictionary_nfd ==
          std::filesystem::path("/tmp/nimblefix-orchestra-cache/profiles/9911/dictionary.nfd"));
  REQUIRE(cache_layout.contract_sidecar ==
          std::filesystem::path("/tmp/nimblefix-orchestra-cache/profiles/9911/contract.nfct"));
}

TEST_CASE("orchestra importer rejects unresolved field references", "[orchestra-import]")
{
  const auto root = std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / "tests" / "data" / "orchestra";
  const auto xml_text = ReadTextFile(root / "bad_unknown_field.xml");

  auto imported = nimble::tools::ImportOrchestraXml(xml_text, 9912U, "bad_unknown_field.xml");
  REQUIRE(!imported.ok());
  REQUIRE(Contains(imported.status().message(), "unknown field"));
}