#pragma once

#include <cstdint>
#include <unordered_map>

#include "nimblefix/base/status.h"
#include "nimblefix/profile/artifact.h"

namespace nimble::runtime {

class ProfileRegistry
{
public:
  auto Clear() -> void;
  auto Register(profile::LoadedProfile profile) -> base::Status;

  [[nodiscard]] auto Find(std::uint64_t profile_id) const -> const profile::LoadedProfile*;

  [[nodiscard]] auto size() const -> std::size_t { return profiles_.size(); }

private:
  std::unordered_map<std::uint64_t, profile::LoadedProfile> profiles_;
};

} // namespace nimble::runtime
