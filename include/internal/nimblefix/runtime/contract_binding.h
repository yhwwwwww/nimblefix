#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <unordered_map>

#include "nimblefix/base/result.h"
#include "nimblefix/profile/contract_sidecar.h"
#include "nimblefix/runtime/config.h"

namespace nimble::runtime {

using LoadedContractMap = std::unordered_map<std::uint64_t, profile::ContractSidecar>;

auto
LoadContractMap(std::span<const std::filesystem::path> paths) -> base::Result<LoadedContractMap>;

auto
ResolveEffectiveCounterpartyConfig(const CounterpartyConfig& counterparty, const LoadedContractMap& contracts)
  -> base::Result<CounterpartyConfig>;

} // namespace nimble::runtime