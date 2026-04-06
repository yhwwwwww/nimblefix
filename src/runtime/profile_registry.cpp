#include "fastfix/runtime/profile_registry.h"

namespace fastfix::runtime {

auto ProfileRegistry::Clear() -> void {
    profiles_.clear();
}

auto ProfileRegistry::Register(profile::LoadedProfile profile) -> base::Status {
    if (!profile.valid()) {
        return base::Status::InvalidArgument("cannot register an invalid profile");
    }

    const auto [_, inserted] = profiles_.emplace(profile.profile_id(), std::move(profile));
    if (!inserted) {
        return base::Status::AlreadyExists("profile already registered");
    }

    return base::Status::Ok();
}

auto ProfileRegistry::Find(std::uint64_t profile_id) const -> const profile::LoadedProfile* {
    const auto it = profiles_.find(profile_id);
    if (it == profiles_.end()) {
        return nullptr;
    }

    return &it->second;
}

}  // namespace fastfix::runtime
