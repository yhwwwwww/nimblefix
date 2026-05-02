#include "nimblefix/runtime/session_schedule.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <ctime>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include "nimblefix/runtime/engine.h"
#include "nimblefix/runtime/schedule_helpers.h"

namespace nimble::runtime {

namespace {

constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000ULL;
constexpr std::uint64_t kNanosecondsPerDay = static_cast<std::uint64_t>(detail::kSecondsPerDay) * kNanosecondsPerSecond;
constexpr std::uint32_t kMonthsPerYear = 12U;
constexpr std::uint32_t kMaxDaysPerMonth = 31U;
constexpr std::size_t kCompactDateLength = 8U;
constexpr std::size_t kDashedDateLength = 10U;

auto
WindowCycleSeconds(const detail::SessionWindowSpec& window) -> int
{
  return window.start_day.has_value() ? 7 * detail::kSecondsPerDay : detail::kSecondsPerDay;
}

auto
CurrentWindowStartSecond(const detail::SessionWindowSpec& window, const detail::CalendarPoint& point) -> int
{
  const auto current =
    window.start_day.has_value() ? point.weekday * detail::kSecondsPerDay + point.second_of_day : point.second_of_day;
  const auto start = window.start_day.has_value() ? *window.start_day * detail::kSecondsPerDay + window.start_second
                                                  : window.start_second;
  const auto end =
    window.end_day.has_value() ? *window.end_day * detail::kSecondsPerDay + window.end_second : window.end_second;
  const auto cycle = WindowCycleSeconds(window);

  if (start == end) {
    return current;
  }
  if (start < end) {
    return start;
  }
  return current >= start ? start : start - cycle;
}

auto
NextWindowClose(const detail::SessionWindowSpec& window, std::uint64_t unix_time_ns) -> std::optional<std::uint64_t>
{
  if (!detail::IsWithinWindow(window, unix_time_ns)) {
    return std::nullopt;
  }

  const auto point = detail::BuildCalendarPoint(unix_time_ns, window.use_local_time);
  const auto start = CurrentWindowStartSecond(window, point);
  const auto duration = [&window]() {
    const auto cycle = WindowCycleSeconds(window);
    const auto raw_end =
      window.end_day.has_value() ? *window.end_day * detail::kSecondsPerDay + window.end_second : window.end_second;
    const auto raw_start = window.start_day.has_value()
                             ? *window.start_day * detail::kSecondsPerDay + window.start_second
                             : window.start_second;
    auto span = raw_end - raw_start;
    if (span <= 0) {
      span += cycle;
    }
    return span;
  }();
  const auto close_second = start + duration;

  auto candidate = point.civil_time;
  const auto current_base = window.start_day.has_value() ? point.weekday * detail::kSecondsPerDay : 0;
  const auto delta_seconds = close_second - (current_base + point.second_of_day);
  candidate.tm_sec += delta_seconds;
  return detail::MakeUnixTimeNs(candidate, window.use_local_time);
}

auto
IsLeapYear(std::uint32_t year) -> bool
{
  return (year % 4U == 0U && year % 100U != 0U) || (year % 400U == 0U);
}

auto
DaysInMonth(std::uint32_t year, std::uint32_t month) -> std::uint32_t
{
  static constexpr std::array<std::uint32_t, kMonthsPerYear> kDaysByMonth{ 31U, 28U, 31U, 30U, 31U, 30U,
                                                                           31U, 31U, 30U, 31U, 30U, 31U };
  if (month == 0U || month > kMonthsPerYear) {
    return 0U;
  }
  if (month == 2U && IsLeapYear(year)) {
    return 29U;
  }
  return kDaysByMonth[month - 1U];
}

auto
ValidateDateParts(std::uint32_t year, std::uint32_t month, std::uint32_t day) -> bool
{
  if (month == 0U || month > kMonthsPerYear || day == 0U || day > kMaxDaysPerMonth) {
    return false;
  }
  return day <= DaysInMonth(year, month);
}

auto
ParseDigits(std::string_view text, std::uint32_t& value) -> bool
{
  if (text.empty()) {
    return false;
  }
  const auto* first = text.data();
  const auto* last = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(first, last, value);
  return ec == std::errc{} && ptr == last;
}

auto
DateParts(BlackoutDate date) -> std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>
{
  const auto year = date / 10000U;
  const auto month = (date / 100U) % 100U;
  const auto day = date % 100U;
  return { year, month, day };
}

auto
HasSessionWindowFields(const SessionScheduleConfig& schedule) -> bool
{
  if (!schedule.segments.empty()) {
    return std::any_of(schedule.segments.begin(), schedule.segments.end(), [](const auto& segment) {
      return segment.start_time.has_value() || segment.end_time.has_value() || segment.start_day.has_value() ||
             segment.end_day.has_value();
    });
  }
  return schedule.start_time.has_value() || schedule.end_time.has_value() || schedule.start_day.has_value() ||
         schedule.end_day.has_value();
}

auto
HasLogonWindowFields(const SessionScheduleConfig& schedule) -> bool
{
  if (!schedule.segments.empty()) {
    return std::any_of(schedule.segments.begin(), schedule.segments.end(), [](const auto& segment) {
      return segment.logon_time.has_value() || segment.logout_time.has_value() || segment.logon_day.has_value() ||
             segment.logout_day.has_value();
    });
  }
  return schedule.logon_time.has_value() || schedule.logout_time.has_value() || schedule.logon_day.has_value() ||
         schedule.logout_day.has_value();
}

auto
EarliestWindowStart(const std::vector<detail::SessionWindowSpec>& windows, std::uint64_t unix_time_ns)
  -> std::optional<std::uint64_t>
{
  if (windows.empty()) {
    return unix_time_ns;
  }

  std::optional<std::uint64_t> earliest;
  for (const auto& window : windows) {
    const auto candidate = detail::NextWindowStart(window, unix_time_ns);
    if (candidate.has_value() && (!earliest.has_value() || *candidate < *earliest)) {
      earliest = candidate;
    }
  }
  return earliest;
}

auto
EarliestContainingWindowClose(const std::vector<detail::SessionWindowSpec>& windows, std::uint64_t unix_time_ns)
  -> std::optional<std::uint64_t>
{
  std::optional<std::uint64_t> earliest;
  for (const auto& window : windows) {
    if (!detail::IsWithinWindow(window, unix_time_ns)) {
      continue;
    }
    const auto candidate = NextWindowClose(window, unix_time_ns);
    if (candidate.has_value() && (!earliest.has_value() || *candidate < *earliest)) {
      earliest = candidate;
    }
  }
  return earliest;
}

auto
PreviewEndTime(std::uint64_t start_time_ns, std::uint32_t days) -> std::uint64_t
{
  const auto preview_days = std::max(days, 1U);
  const auto max_days = (std::numeric_limits<std::uint64_t>::max() - start_time_ns) / kNanosecondsPerDay;
  if (preview_days > max_days) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return start_time_ns + static_cast<std::uint64_t>(preview_days) * kNanosecondsPerDay;
}

auto
AdvancePast(std::uint64_t unix_time_ns) -> std::uint64_t
{
  if (unix_time_ns == std::numeric_limits<std::uint64_t>::max()) {
    return unix_time_ns;
  }
  return unix_time_ns + 1U;
}

auto
FormatTimestampUtc(std::uint64_t unix_time_ns) -> std::string
{
  const auto point = detail::BuildCalendarPoint(unix_time_ns, false);
  std::ostringstream out;
  out << std::setfill('0') << std::setw(4) << point.civil_time.tm_year + 1900 << '-' << std::setw(2)
      << point.civil_time.tm_mon + 1 << '-' << std::setw(2) << point.civil_time.tm_mday << ' ' << std::setw(2)
      << point.civil_time.tm_hour << ':' << std::setw(2) << point.civil_time.tm_min << ':' << std::setw(2)
      << point.civil_time.tm_sec << " UTC";
  return out.str();
}

auto
FormatDateUtc(std::uint64_t unix_time_ns) -> std::string
{
  const auto point = detail::BuildCalendarPoint(unix_time_ns, false);
  std::ostringstream out;
  out << std::setfill('0') << std::setw(4) << point.civil_time.tm_year + 1900 << '-' << std::setw(2)
      << point.civil_time.tm_mon + 1 << '-' << std::setw(2) << point.civil_time.tm_mday;
  return out.str();
}

auto
EventKindSortRank(SessionScheduleEventKind kind) -> int
{
  switch (kind) {
    case SessionScheduleEventKind::kBlackoutStarted:
      return 0;
    case SessionScheduleEventKind::kExitedSessionWindow:
      return 1;
    case SessionScheduleEventKind::kExitedLogonWindow:
      return 2;
    case SessionScheduleEventKind::kBlackoutEnded:
      return 3;
    case SessionScheduleEventKind::kEnteredLogonWindow:
      return 4;
    case SessionScheduleEventKind::kEnteredSessionWindow:
      return 5;
    case SessionScheduleEventKind::kDayCutTriggered:
      return 6;
  }
  return 7;
}

auto
SortTimelineEntries(std::vector<ScheduleTimelineEntry>& entries) -> void
{
  std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
    if (left.unix_time_ns != right.unix_time_ns) {
      return left.unix_time_ns < right.unix_time_ns;
    }
    return EventKindSortRank(left.kind) < EventKindSortRank(right.kind);
  });
}

auto
AddTimelineEntry(std::vector<ScheduleTimelineEntry>& entries,
                 std::uint64_t unix_time_ns,
                 std::uint64_t start_time_ns,
                 std::uint64_t end_time_ns,
                 SessionScheduleEventKind kind,
                 std::string description) -> void
{
  if (unix_time_ns < start_time_ns || unix_time_ns >= end_time_ns) {
    return;
  }
  entries.push_back(ScheduleTimelineEntry{
    .unix_time_ns = unix_time_ns,
    .kind = kind,
    .description = std::move(description),
  });
}

auto
CollectWindowTimeline(std::vector<ScheduleTimelineEntry>& entries,
                      const CounterpartyConfig& config,
                      const BlackoutCalendar& calendar,
                      std::uint64_t start_time_ns,
                      std::uint64_t end_time_ns,
                      bool logon_window) -> void
{
  const auto& schedule = config.session_schedule;
  auto cursor = start_time_ns;
  while (cursor < end_time_ns) {
    const auto status = QueryScheduleStatus(config, cursor);
    const auto in_window = logon_window ? status.in_logon_window : status.in_session_window;
    if (in_window) {
      const auto close_time =
        logon_window ? NextLogonWindowClose(schedule, cursor) : NextSessionWindowClose(schedule, cursor);
      if (!close_time.has_value() || *close_time >= end_time_ns) {
        break;
      }
      if (!calendar.IsBlackout(*close_time, schedule.use_local_time)) {
        AddTimelineEntry(entries,
                         *close_time,
                         start_time_ns,
                         end_time_ns,
                         logon_window ? SessionScheduleEventKind::kExitedLogonWindow
                                      : SessionScheduleEventKind::kExitedSessionWindow,
                         logon_window ? "logon window closes" : "session window closes");
      }
      const auto next_cursor = AdvancePast(*close_time);
      if (next_cursor <= cursor) {
        break;
      }
      cursor = next_cursor;
      continue;
    }

    const auto open_time =
      logon_window ? NextLogonWindowStart(schedule, cursor) : NextSessionWindowStart(schedule, cursor);
    if (!open_time.has_value() || *open_time >= end_time_ns) {
      break;
    }
    if (!calendar.IsBlackout(*open_time, schedule.use_local_time)) {
      AddTimelineEntry(entries,
                       *open_time,
                       start_time_ns,
                       end_time_ns,
                       logon_window ? SessionScheduleEventKind::kEnteredLogonWindow
                                    : SessionScheduleEventKind::kEnteredSessionWindow,
                       logon_window ? "logon window opens" : "session window opens");
    }
    const auto next_cursor = AdvancePast(*open_time);
    if (next_cursor <= cursor) {
      break;
    }
    cursor = next_cursor;
  }
}

auto
BlackoutDateBoundary(BlackoutDate date, bool use_local_time, bool end_boundary) -> std::optional<std::uint64_t>
{
  const auto [year, month, day] = DateParts(date);
  if (!ValidateDateParts(year, month, day)) {
    return std::nullopt;
  }

  std::tm civil_time{};
  civil_time.tm_year = static_cast<int>(year) - 1900;
  civil_time.tm_mon = static_cast<int>(month) - 1;
  civil_time.tm_mday = static_cast<int>(day) + (end_boundary ? 1 : 0);
  civil_time.tm_isdst = -1;
  return detail::MakeUnixTimeNs(civil_time, use_local_time);
}

auto
CollectBlackoutTimeline(std::vector<ScheduleTimelineEntry>& entries,
                        const SessionScheduleConfig& schedule,
                        const BlackoutCalendar& calendar,
                        std::uint64_t start_time_ns,
                        std::uint64_t end_time_ns) -> void
{
  for (const auto date : calendar.dates()) {
    const auto blackout_start = BlackoutDateBoundary(date, schedule.use_local_time, false);
    const auto blackout_end = BlackoutDateBoundary(date, schedule.use_local_time, true);
    if (!blackout_start.has_value() || !blackout_end.has_value()) {
      continue;
    }
    if (*blackout_end <= start_time_ns || *blackout_start >= end_time_ns) {
      continue;
    }

    const auto date_text = BlackoutDateToString(date);
    AddTimelineEntry(entries,
                     *blackout_start,
                     start_time_ns,
                     end_time_ns,
                     SessionScheduleEventKind::kBlackoutStarted,
                     "blackout starts: " + date_text);
    if (*blackout_end > start_time_ns && *blackout_end <= end_time_ns) {
      entries.push_back(ScheduleTimelineEntry{
        .unix_time_ns = *blackout_end,
        .kind = SessionScheduleEventKind::kBlackoutEnded,
        .description = "blackout ends: " + date_text,
      });
    }
  }
}

auto
Plural(std::uint32_t count, std::string_view singular, std::string_view plural) -> std::string_view
{
  return count == 1U ? singular : plural;
}

} // namespace

auto
ScheduleTimeline::summary() const -> std::string
{
  std::ostringstream out;
  out << "Schedule timeline for session " << session_id;
  if (days > 0U) {
    out << " (" << days << ' ' << Plural(days, "day", "days") << " from " << FormatDateUtc(start_time_ns) << ')';
  }
  if (non_stop) {
    out << ":\n  non-stop session (no schedule boundaries)";
    return out.str();
  }
  if (entries.empty()) {
    out << ":\n  no schedule events";
    return out.str();
  }

  out << ':';
  for (const auto& entry : entries) {
    out << "\n  " << FormatTimestampUtc(entry.unix_time_ns) << "  " << entry.description;
  }
  return out.str();
}

auto
QueryScheduleStatus(const CounterpartyConfig& config, std::uint64_t unix_time_ns) -> SessionScheduleStatus
{
  SessionScheduleStatus status;
  status.session_id = config.session.session_id;
  if (config.session_schedule.non_stop_session) {
    status.in_session_window = true;
    status.in_logon_window = true;
    status.non_stop = true;
    return status;
  }

  status.in_session_window = IsWithinSessionWindow(config.session_schedule, unix_time_ns);
  status.in_logon_window = IsWithinLogonWindow(config.session_schedule, unix_time_ns);
  if (!status.in_logon_window) {
    status.next_logon_window_open_ns = NextLogonWindowStart(config.session_schedule, unix_time_ns);
  }
  if (!status.in_session_window) {
    status.next_session_window_open_ns = NextSessionWindowStart(config.session_schedule, unix_time_ns);
  }
  if (status.in_session_window) {
    status.next_session_window_close_ns = NextSessionWindowClose(config.session_schedule, unix_time_ns);
  }
  return status;
}

auto
ExplainSchedule(const CounterpartyConfig& config,
                const BlackoutCalendar& calendar,
                std::uint64_t start_time_ns,
                std::uint32_t days) -> ScheduleTimeline
{
  const auto preview_days = std::max(days, 1U);
  ScheduleTimeline timeline;
  timeline.session_id = config.session.session_id;
  timeline.non_stop = config.session_schedule.non_stop_session;
  timeline.start_time_ns = start_time_ns;
  timeline.days = preview_days;
  if (timeline.non_stop) {
    return timeline;
  }

  const auto end_time_ns = PreviewEndTime(start_time_ns, preview_days);
  if (HasSessionWindowFields(config.session_schedule)) {
    CollectWindowTimeline(timeline.entries, config, calendar, start_time_ns, end_time_ns, false);
  }
  if (HasLogonWindowFields(config.session_schedule)) {
    CollectWindowTimeline(timeline.entries, config, calendar, start_time_ns, end_time_ns, true);
  }
  CollectBlackoutTimeline(timeline.entries, config.session_schedule, calendar, start_time_ns, end_time_ns);
  SortTimelineEntries(timeline.entries);
  return timeline;
}

auto
NextSessionWindowClose(const SessionScheduleConfig& schedule, std::uint64_t unix_time_ns)
  -> std::optional<std::uint64_t>
{
  const auto windows = detail::BuildWindowSpecs(schedule, false);
  if (windows.empty()) {
    return std::nullopt;
  }
  return EarliestContainingWindowClose(windows, unix_time_ns);
}

auto
NextLogonWindowClose(const SessionScheduleConfig& schedule, std::uint64_t unix_time_ns) -> std::optional<std::uint64_t>
{
  const auto windows = detail::BuildWindowSpecs(schedule, true);
  if (windows.empty()) {
    return std::nullopt;
  }
  return EarliestContainingWindowClose(windows, unix_time_ns);
}

auto
NextSessionWindowStart(const SessionScheduleConfig& schedule, std::uint64_t unix_time_ns)
  -> std::optional<std::uint64_t>
{
  return EarliestWindowStart(detail::BuildWindowSpecs(schedule, false), unix_time_ns);
}

auto
BlackoutCalendar::AddDate(BlackoutDate date) -> void
{
  const auto it = std::lower_bound(dates_.begin(), dates_.end(), date);
  if (it == dates_.end() || *it != date) {
    dates_.insert(it, date);
  }
}

auto
BlackoutCalendar::AddDates(std::vector<BlackoutDate> dates) -> void
{
  for (const auto date : dates) {
    AddDate(date);
  }
}

auto
BlackoutCalendar::RemoveDate(BlackoutDate date) -> void
{
  const auto it = std::lower_bound(dates_.begin(), dates_.end(), date);
  if (it != dates_.end() && *it == date) {
    dates_.erase(it);
  }
}

auto
BlackoutCalendar::Clear() -> void
{
  dates_.clear();
}

auto
BlackoutCalendar::IsBlackout(std::uint64_t unix_time_ns, bool use_local_time) const -> bool
{
  return IsBlackoutDate(ExtractDate(unix_time_ns, use_local_time));
}

auto
BlackoutCalendar::IsBlackoutDate(BlackoutDate date) const -> bool
{
  return std::binary_search(dates_.begin(), dates_.end(), date);
}

auto
BlackoutCalendar::dates() const -> std::vector<BlackoutDate>
{
  return dates_;
}

auto
BlackoutCalendar::size() const -> std::size_t
{
  return dates_.size();
}

auto
BlackoutCalendar::empty() const -> bool
{
  return dates_.empty();
}

auto
BlackoutDateToString(BlackoutDate date) -> std::string
{
  const auto [year, month, day] = DateParts(date);
  std::ostringstream out;
  out << std::setfill('0') << std::setw(4) << year << '-' << std::setw(2) << month << '-' << std::setw(2) << day;
  return out.str();
}

auto
ParseBlackoutDate(std::string_view text) -> base::Result<BlackoutDate>
{
  std::uint32_t year = 0U;
  std::uint32_t month = 0U;
  std::uint32_t day = 0U;

  if (text.size() == kDashedDateLength && text[4] == '-' && text[7] == '-') {
    if (!ParseDigits(text.substr(0, 4), year) || !ParseDigits(text.substr(5, 2), month) ||
        !ParseDigits(text.substr(8, 2), day)) {
      return base::Status::InvalidArgument("blackout date must use YYYY-MM-DD digits");
    }
  } else if (text.size() == kCompactDateLength) {
    if (!ParseDigits(text.substr(0, 4), year) || !ParseDigits(text.substr(4, 2), month) ||
        !ParseDigits(text.substr(6, 2), day)) {
      return base::Status::InvalidArgument("blackout date must use YYYYMMDD digits");
    }
  } else {
    return base::Status::InvalidArgument("blackout date must be YYYY-MM-DD or YYYYMMDD");
  }

  if (!ValidateDateParts(year, month, day)) {
    return base::Status::InvalidArgument("blackout date is out of range");
  }
  return year * 10000U + month * 100U + day;
}

auto
ExtractDate(std::uint64_t unix_time_ns, bool use_local_time) -> BlackoutDate
{
  const auto unix_seconds = static_cast<std::time_t>(unix_time_ns / kNanosecondsPerSecond);
  std::tm civil_time{};
  if (use_local_time) {
    localtime_r(&unix_seconds, &civil_time);
  } else {
    gmtime_r(&unix_seconds, &civil_time);
  }
  return static_cast<BlackoutDate>((civil_time.tm_year + 1900) * 10000 + (civil_time.tm_mon + 1) * 100 +
                                   civil_time.tm_mday);
}

auto
IsWithinSessionWindowWithBlackouts(const SessionScheduleConfig& schedule,
                                   const BlackoutCalendar& calendar,
                                   std::uint64_t unix_time_ns) -> bool
{
  if (calendar.IsBlackout(unix_time_ns, schedule.use_local_time)) {
    return false;
  }
  return IsWithinSessionWindow(schedule, unix_time_ns);
}

auto
IsWithinLogonWindowWithBlackouts(const SessionScheduleConfig& schedule,
                                 const BlackoutCalendar& calendar,
                                 std::uint64_t unix_time_ns) -> bool
{
  if (calendar.IsBlackout(unix_time_ns, schedule.use_local_time)) {
    return false;
  }
  return IsWithinLogonWindow(schedule, unix_time_ns);
}

auto
TriggerDayCut(Engine& engine, std::uint64_t session_id) -> base::Status
{
  if (engine.FindCounterpartyConfig(session_id) == nullptr) {
    return base::Status::NotFound("counterparty session not found");
  }
  return base::Status::Ok();
}

} // namespace nimble::runtime
