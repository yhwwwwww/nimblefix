#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "nimblefix/base/result.h"
#include "nimblefix/profile/contract_sidecar.h"
#include "nimblefix/profile/normalized_dictionary.h"

namespace nimble::tools {

struct OrchestraImportResult
{
  profile::NormalizedDictionary dictionary;
  profile::ContractSidecar contract;
  std::vector<std::string> warnings;
};

struct OrchestraCacheLayout
{
  std::filesystem::path root;
  std::filesystem::path dictionary_nfd;
  std::filesystem::path contract_sidecar;
};

auto
ImportOrchestraXml(std::string_view xml_content, std::uint64_t profile_id, std::string_view source_name = {})
  -> base::Result<OrchestraImportResult>;

auto
SerializeDictionaryAsNfd(const profile::NormalizedDictionary& dictionary) -> std::string;

auto
ResolveOrchestraCacheLayout(const std::filesystem::path& cache_root, std::uint64_t profile_id) -> OrchestraCacheLayout;

} // namespace nimble::tools