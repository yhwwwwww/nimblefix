#pragma once

#include "fastfix/base/result.h"
#include "fastfix/profile/normalized_dictionary.h"

namespace fastfix::profile {

auto ApplyOverlay(const NormalizedDictionary& baseline, const NormalizedDictionary& overlay)
    -> base::Result<NormalizedDictionary>;

}  // namespace fastfix::profile
