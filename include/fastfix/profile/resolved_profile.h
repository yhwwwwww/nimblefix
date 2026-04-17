#pragma once

#include <cstdint>

#include "fastfix/profile/normalized_dictionary.h"
#include "fastfix/session/validation_policy.h"

namespace fastfix::profile {

/// Runtime-immutable binding of transport profile, dictionary, and validation
/// configuration.  Constructed once during session setup and referenced
/// throughout the session lifetime.
struct ResolvedProtocolProfile
{
  std::uint64_t transport_profile_id{ 0 };
  std::uint64_t profile_id{ 0 };
  session::ValidationPolicy validation_policy{ session::ValidationPolicy::Compatible() };
  const NormalizedDictionaryView* dictionary{ nullptr };
  // admin_rules_ref is implicitly part of the dictionary
};

} // namespace fastfix::profile
