#pragma once

#include "nimblefix/base/result.h"
#include "nimblefix/profile/normalized_dictionary.h"

namespace nimble::profile {

auto
ApplyOverlay(const NormalizedDictionary& baseline, const NormalizedDictionary& overlay)
  -> base::Result<NormalizedDictionary>;

} // namespace nimble::profile
