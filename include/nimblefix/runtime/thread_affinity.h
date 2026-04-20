#pragma once

#include <cstdint>
#include <string_view>

#include "nimblefix/base/status.h"

namespace nimble::runtime {

auto
ApplyCurrentThreadAffinity(std::uint32_t cpu_id, std::string_view role) -> base::Status;
auto
SetCurrentThreadName(std::string_view name) -> void;

} // namespace nimble::runtime