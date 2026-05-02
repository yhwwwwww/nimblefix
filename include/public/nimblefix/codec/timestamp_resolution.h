#pragma once

#include <cstdint>
#include <string_view>

namespace nimble::codec {

enum class TimestampResolution : std::uint32_t
{
  kSeconds = 0,      // YYYYMMDD-HH:MM:SS (17 chars)
  kMilliseconds = 1, // YYYYMMDD-HH:MM:SS.sss (21 chars)
  kMicroseconds = 2, // YYYYMMDD-HH:MM:SS.ssssss (24 chars)
  kNanoseconds = 3,  // YYYYMMDD-HH:MM:SS.sssssssss (27 chars)
};

[[nodiscard]] constexpr auto
TimestampResolutionName(TimestampResolution resolution) noexcept -> std::string_view
{
  switch (resolution) {
    case TimestampResolution::kSeconds:
      return "seconds";
    case TimestampResolution::kMicroseconds:
      return "microseconds";
    case TimestampResolution::kNanoseconds:
      return "nanoseconds";
    case TimestampResolution::kMilliseconds:
    default:
      return "milliseconds";
  }
}

} // namespace nimble::codec
