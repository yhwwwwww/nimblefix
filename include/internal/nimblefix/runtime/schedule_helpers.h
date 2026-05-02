#pragma once

#include <cstdint>
#include <ctime>
#include <optional>
#include <vector>

#include "nimblefix/runtime/config.h"

namespace nimble::runtime::detail {

inline constexpr int kSecondsPerDay = 24 * 60 * 60;

struct SessionWindowSpec
{
  bool use_local_time{ false };
  int start_second{ 0 };
  int end_second{ 0 };
  std::optional<int> start_day;
  std::optional<int> end_day;
};

struct CalendarPoint
{
  std::tm civil_time{};
  int weekday{ 0 };
  int second_of_day{ 0 };
};

[[nodiscard]] auto
BuildWindowSpec(const SessionScheduleConfig& schedule, bool logon_window) -> std::optional<SessionWindowSpec>;

/// Build multiple window specs from a chained schedule config.
/// Returns empty vector for non_stop sessions or when no windows configured.
[[nodiscard]] auto
BuildWindowSpecs(const SessionScheduleConfig& schedule, bool logon_window) -> std::vector<SessionWindowSpec>;

[[nodiscard]] auto
BuildCalendarPoint(std::uint64_t unix_time_ns, bool use_local_time) -> CalendarPoint;

[[nodiscard]] auto
MakeUnixTimeNs(std::tm civil_time, bool use_local_time) -> std::optional<std::uint64_t>;

[[nodiscard]] auto
IsWithinWindow(const SessionWindowSpec& window, std::uint64_t unix_time_ns) -> bool;

[[nodiscard]] auto
NextWindowStart(const SessionWindowSpec& window, std::uint64_t unix_time_ns) -> std::optional<std::uint64_t>;

} // namespace nimble::runtime::detail
