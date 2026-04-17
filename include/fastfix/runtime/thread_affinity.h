#pragma once

#include <cstdint>
#include <string_view>

#include "fastfix/base/status.h"

namespace fastfix::runtime {

auto
ApplyCurrentThreadAffinity(std::uint32_t cpu_id, std::string_view role) -> base::Status;
auto
SetCurrentThreadName(std::string_view name) -> void;

} // namespace fastfix::runtime