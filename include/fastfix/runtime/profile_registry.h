#pragma once

#include <cstdint>
#include <unordered_map>

#include "fastfix/base/status.h"
#include "fastfix/profile/artifact.h"

namespace fastfix::runtime {

class ProfileRegistry {
  public:
    auto Clear() -> void;
    auto Register(profile::LoadedProfile profile) -> base::Status;

    [[nodiscard]] auto Find(std::uint64_t profile_id) const -> const profile::LoadedProfile*;

    [[nodiscard]] auto size() const -> std::size_t {
        return profiles_.size();
    }

  private:
    std::unordered_map<std::uint64_t, profile::LoadedProfile> profiles_;
};

}  // namespace fastfix::runtime
