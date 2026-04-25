#include "nimblefix/runtime/contract_binding.h"

#include <algorithm>

namespace nimble::runtime {

namespace {

auto
LocalRole(const CounterpartyConfig& counterparty) -> profile::ContractRole
{
  return counterparty.session.is_initiator ? profile::ContractRole::kInitiator : profile::ContractRole::kAcceptor;
}

auto
CounterpartyContext(const CounterpartyConfig& counterparty) -> std::string
{
  return "counterparty '" + counterparty.name + "' (profile_id=" + std::to_string(counterparty.session.profile_id) +
         ")";
}

auto
ContractConstrainsInboundApplications(const profile::ContractSidecar& contract, profile::ContractRole role) -> bool
{
  if (!contract.service_messages.empty()) {
    return true;
  }

  return std::any_of(contract.messages.begin(), contract.messages.end(), [&](const auto& message) {
    return !message.admin && profile::MessageDirectionForRole(message, role) != profile::ContractDirection::kNone;
  });
}

} // namespace

auto
LoadContractMap(std::span<const std::filesystem::path> paths) -> base::Result<LoadedContractMap>
{
  LoadedContractMap contracts;
  contracts.reserve(paths.size());
  for (const auto& path : paths) {
    auto loaded = profile::LoadContractSidecarFile(path);
    if (!loaded.ok()) {
      return loaded.status();
    }
    const auto profile_id = loaded.value().profile_id;
    const auto [_, inserted] = contracts.emplace(profile_id, std::move(loaded).value());
    if (!inserted) {
      return base::Status::AlreadyExists("duplicate contract sidecar profile_id " + std::to_string(profile_id));
    }
  }
  return contracts;
}

auto
ResolveEffectiveCounterpartyConfig(const CounterpartyConfig& counterparty, const LoadedContractMap& contracts)
  -> base::Result<CounterpartyConfig>
{
  auto effective = counterparty;
  const auto contract_it = contracts.find(counterparty.session.profile_id);
  if (contract_it == contracts.end()) {
    if (!counterparty.contract_service_subsets.empty()) {
      return base::Status::InvalidArgument(CounterpartyContext(counterparty) +
                                           " selects contract_service_subsets but no contract sidecar was loaded");
    }
    return effective;
  }

  const auto& contract = contract_it->second;
  const auto local_role = LocalRole(counterparty);
  auto selection_status = profile::ValidateContractServiceSelection(contract, counterparty.contract_service_subsets);
  if (!selection_status.ok()) {
    return base::Status::InvalidArgument(CounterpartyContext(counterparty) + ": " +
                                         std::string(selection_status.message()));
  }

  const auto inbound_allowed = profile::CollectContractMessageTypes(
    contract, local_role, profile::ContractDirection::kReceive, counterparty.contract_service_subsets, true);
  if (inbound_allowed.empty()) {
    if (ContractConstrainsInboundApplications(contract, local_role) || !counterparty.contract_service_subsets.empty()) {
      if (!effective.supported_app_msg_types.empty()) {
        return base::Status::InvalidArgument(CounterpartyContext(counterparty) +
                                             " binds to a contract subset that allows no inbound application messages");
      }
      effective.application_messages_available = false;
    }
    return effective;
  }

  if (!effective.supported_app_msg_types.empty()) {
    for (const auto& msg_type : effective.supported_app_msg_types) {
      const auto found = std::binary_search(inbound_allowed.begin(), inbound_allowed.end(), msg_type);
      if (!found) {
        return base::Status::InvalidArgument(
          CounterpartyContext(counterparty) +
          " declares supported_app_msg_types outside the bound contract receive subset: '" + msg_type + "'");
      }
    }
    return effective;
  }

  effective.supported_app_msg_types = inbound_allowed;
  return effective;
}

} // namespace nimble::runtime