#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "nimblefix/runtime/engine.h"
#include "nimblefix/runtime/session_schedule.h"

namespace {

auto
MakeUtcNs(int year, int month, int day, int hour, int minute, int second) -> std::uint64_t
{
  std::tm value{};
  value.tm_year = year - 1900;
  value.tm_mon = month - 1;
  value.tm_mday = day;
  value.tm_hour = hour;
  value.tm_min = minute;
  value.tm_sec = second;
  return static_cast<std::uint64_t>(timegm(&value)) * 1'000'000'000ULL;
}

auto
MakeDailySchedule() -> nimble::runtime::SessionScheduleConfig
{
  nimble::runtime::SessionScheduleConfig schedule;
  schedule.start_time = nimble::runtime::SessionTimeOfDay{ 9U, 0U, 0U };
  schedule.end_time = nimble::runtime::SessionTimeOfDay{ 17U, 0U, 0U };
  return schedule;
}

auto
MakeWeeklySchedule() -> nimble::runtime::SessionScheduleConfig
{
  auto schedule = MakeDailySchedule();
  schedule.start_day = nimble::runtime::SessionDayOfWeek::kMonday;
  schedule.end_day = nimble::runtime::SessionDayOfWeek::kFriday;
  return schedule;
}

auto
MakeChainedSchedule() -> nimble::runtime::SessionScheduleConfig
{
  nimble::runtime::SessionScheduleConfig schedule;
  schedule.segments.push_back(nimble::runtime::SessionScheduleSegment{
    .start_time = nimble::runtime::SessionTimeOfDay{ 1U, 0U, 0U },
    .end_time = nimble::runtime::SessionTimeOfDay{ 9U, 0U, 0U },
  });
  schedule.segments.push_back(nimble::runtime::SessionScheduleSegment{
    .start_time = nimble::runtime::SessionTimeOfDay{ 14U, 0U, 0U },
    .end_time = nimble::runtime::SessionTimeOfDay{ 21U, 0U, 0U },
  });
  return schedule;
}

auto
MakeCounterparty(std::uint64_t session_id = 1001U) -> nimble::runtime::CounterpartyConfig
{
  return nimble::runtime::CounterpartyConfigBuilder::Initiator(
           "schedule-test",
           session_id,
           nimble::session::SessionKey{ .sender_comp_id = "BUY", .target_comp_id = "SELL" },
           4400U)
    .build();
}

auto
MakeEngineConfig(std::uint64_t session_id = 1001U) -> nimble::runtime::EngineConfig
{
  nimble::runtime::EngineConfig config;
  config.profile_artifacts.push_back(std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / "build" / "bench" /
                                     "quickfix_FIX44.nfa");
  config.counterparties.push_back(MakeCounterparty(session_id));
  return config;
}

auto
HasDiagnosticField(const nimble::runtime::ConfigValidationResult& result, std::string_view field_path) -> bool
{
  return std::any_of(result.errors.begin(), result.errors.end(), [&](const auto& error) {
    return error.severity == nimble::runtime::ConfigErrorSeverity::kError && error.field_path == field_path;
  });
}

class ScopedTimezone
{
public:
  explicit ScopedTimezone(const char* timezone)
  {
    if (const auto* current = std::getenv("TZ"); current != nullptr) {
      had_original_ = true;
      original_ = current;
    }
    setenv("TZ", timezone, 1);
    tzset();
  }

  ~ScopedTimezone()
  {
    if (had_original_) {
      setenv("TZ", original_.c_str(), 1);
    } else {
      unsetenv("TZ");
    }
    tzset();
  }

  ScopedTimezone(const ScopedTimezone&) = delete;
  auto operator=(const ScopedTimezone&) -> ScopedTimezone& = delete;

private:
  bool had_original_{ false };
  std::string original_;
};

} // namespace

TEST_CASE("query schedule status non-stop session", "[session-schedule]")
{
  auto counterparty = MakeCounterparty();
  counterparty.session_schedule.non_stop_session = true;

  const auto status = nimble::runtime::QueryScheduleStatus(counterparty, MakeUtcNs(2026, 7, 4, 12, 0, 0));
  REQUIRE(status.session_id == counterparty.session.session_id);
  REQUIRE(status.in_session_window);
  REQUIRE(status.in_logon_window);
  REQUIRE(status.non_stop);
  REQUIRE(!status.next_session_window_open_ns.has_value());
  REQUIRE(!status.next_logon_window_open_ns.has_value());
  REQUIRE(!status.next_session_window_close_ns.has_value());
}

TEST_CASE("query schedule status within window", "[session-schedule]")
{
  auto counterparty = MakeCounterparty();
  counterparty.session_schedule = MakeDailySchedule();

  const auto status = nimble::runtime::QueryScheduleStatus(counterparty, MakeUtcNs(2026, 7, 6, 12, 0, 0));
  REQUIRE(status.in_session_window);
  REQUIRE(status.in_logon_window);
  REQUIRE(!status.non_stop);
  REQUIRE(!status.next_session_window_open_ns.has_value());
  REQUIRE(!status.next_logon_window_open_ns.has_value());
  REQUIRE(status.next_session_window_close_ns == MakeUtcNs(2026, 7, 6, 17, 0, 0));
}

TEST_CASE("query schedule status outside window", "[session-schedule]")
{
  auto counterparty = MakeCounterparty();
  counterparty.session_schedule = MakeDailySchedule();

  const auto status = nimble::runtime::QueryScheduleStatus(counterparty, MakeUtcNs(2026, 7, 6, 20, 0, 0));
  REQUIRE(!status.in_session_window);
  REQUIRE(!status.in_logon_window);
  REQUIRE(status.next_session_window_open_ns == MakeUtcNs(2026, 7, 7, 9, 0, 0));
  REQUIRE(status.next_logon_window_open_ns == MakeUtcNs(2026, 7, 7, 9, 0, 0));
  REQUIRE(!status.next_session_window_close_ns.has_value());
}

TEST_CASE("next session window close within daily window", "[session-schedule]")
{
  const auto schedule = MakeDailySchedule();
  REQUIRE(nimble::runtime::NextSessionWindowClose(schedule, MakeUtcNs(2026, 7, 6, 12, 0, 0)) ==
          MakeUtcNs(2026, 7, 6, 17, 0, 0));
}

TEST_CASE("next session window close no window configured", "[session-schedule]")
{
  const nimble::runtime::SessionScheduleConfig schedule;
  REQUIRE(!nimble::runtime::NextSessionWindowClose(schedule, MakeUtcNs(2026, 7, 6, 12, 0, 0)).has_value());
}

TEST_CASE("next session window start outside window", "[session-schedule]")
{
  const auto schedule = MakeDailySchedule();
  REQUIRE(nimble::runtime::NextSessionWindowStart(schedule, MakeUtcNs(2026, 7, 6, 20, 0, 0)) ==
          MakeUtcNs(2026, 7, 7, 9, 0, 0));
}

TEST_CASE("chained schedule two non-overlapping segments", "[session-schedule]")
{
  const auto schedule = MakeChainedSchedule();

  REQUIRE(nimble::runtime::IsWithinSessionWindow(schedule, MakeUtcNs(2026, 7, 6, 2, 0, 0)));
  REQUIRE(nimble::runtime::IsWithinSessionWindow(schedule, MakeUtcNs(2026, 7, 6, 15, 0, 0)));
  REQUIRE(!nimble::runtime::IsWithinSessionWindow(schedule, MakeUtcNs(2026, 7, 6, 10, 0, 0)));
  REQUIRE(!nimble::runtime::IsWithinSessionWindow(schedule, MakeUtcNs(2026, 7, 6, 22, 0, 0)));
}

TEST_CASE("chained schedule next window start picks earliest", "[session-schedule]")
{
  const auto schedule = MakeChainedSchedule();

  REQUIRE(nimble::runtime::NextSessionWindowStart(schedule, MakeUtcNs(2026, 7, 6, 10, 0, 0)) ==
          MakeUtcNs(2026, 7, 6, 14, 0, 0));
  REQUIRE(nimble::runtime::NextSessionWindowStart(schedule, MakeUtcNs(2026, 7, 6, 22, 0, 0)) ==
          MakeUtcNs(2026, 7, 7, 1, 0, 0));
}

TEST_CASE("chained schedule next window close", "[session-schedule]")
{
  const auto schedule = MakeChainedSchedule();

  REQUIRE(nimble::runtime::NextSessionWindowClose(schedule, MakeUtcNs(2026, 7, 6, 2, 0, 0)) ==
          MakeUtcNs(2026, 7, 6, 9, 0, 0));
}

TEST_CASE("blackout calendar add and check dates", "[session-schedule]")
{
  nimble::runtime::BlackoutCalendar calendar;
  calendar.AddDate(20260704U);
  calendar.AddDate(20261225U);

  REQUIRE(calendar.IsBlackoutDate(20260704U));
  REQUIRE(calendar.IsBlackoutDate(20261225U));
  REQUIRE(!calendar.IsBlackoutDate(20260705U));
  REQUIRE(calendar.size() == 2U);
}

TEST_CASE("blackout calendar from timestamp", "[session-schedule]")
{
  nimble::runtime::BlackoutCalendar calendar;
  calendar.AddDate(20260704U);

  REQUIRE(calendar.IsBlackout(MakeUtcNs(2026, 7, 4, 12, 0, 0)));
  REQUIRE(!calendar.IsBlackout(MakeUtcNs(2026, 7, 5, 12, 0, 0)));
}

TEST_CASE("blackout calendar sorted order", "[session-schedule]")
{
  nimble::runtime::BlackoutCalendar calendar;
  calendar.AddDate(20261225U);
  calendar.AddDate(20260704U);
  calendar.AddDate(20260101U);
  calendar.AddDate(20260704U);

  REQUIRE(calendar.dates() == std::vector<nimble::runtime::BlackoutDate>{ 20260101U, 20260704U, 20261225U });
}

TEST_CASE("extract date from timestamp", "[session-schedule]")
{
  REQUIRE(nimble::runtime::ExtractDate(MakeUtcNs(2026, 7, 4, 0, 0, 0)) == 20260704U);
  REQUIRE(nimble::runtime::ExtractDate(MakeUtcNs(2026, 12, 25, 23, 59, 59)) == 20261225U);
}

TEST_CASE("parse blackout date string", "[session-schedule]")
{
  auto dashed = nimble::runtime::ParseBlackoutDate("2026-07-04");
  REQUIRE(dashed.ok());
  REQUIRE(dashed.value() == 20260704U);

  auto compact = nimble::runtime::ParseBlackoutDate("20261225");
  REQUIRE(compact.ok());
  REQUIRE(compact.value() == 20261225U);

  REQUIRE(!nimble::runtime::ParseBlackoutDate("2026-13-01").ok());
  REQUIRE(!nimble::runtime::ParseBlackoutDate("not-a-date").ok());
}

TEST_CASE("blackout date to string", "[session-schedule]")
{
  REQUIRE(nimble::runtime::BlackoutDateToString(20260704U) == "2026-07-04");
}

TEST_CASE("session window with blackouts", "[session-schedule]")
{
  const auto schedule = MakeDailySchedule();
  nimble::runtime::BlackoutCalendar calendar;
  calendar.AddDate(20260706U);

  REQUIRE(!nimble::runtime::IsWithinSessionWindowWithBlackouts(schedule, calendar, MakeUtcNs(2026, 7, 6, 12, 0, 0)));
}

TEST_CASE("chained schedule with blackout", "[session-schedule]")
{
  const auto schedule = MakeChainedSchedule();
  nimble::runtime::BlackoutCalendar calendar;
  calendar.AddDate(20260706U);

  REQUIRE(!nimble::runtime::IsWithinSessionWindowWithBlackouts(schedule, calendar, MakeUtcNs(2026, 7, 6, 2, 0, 0)));
  REQUIRE(nimble::runtime::IsWithinSessionWindowWithBlackouts(schedule, calendar, MakeUtcNs(2026, 7, 7, 2, 0, 0)));
}

TEST_CASE("logon window with blackouts", "[session-schedule]")
{
  auto schedule = MakeDailySchedule();
  schedule.logon_time = nimble::runtime::SessionTimeOfDay{ 8U, 0U, 0U };
  schedule.logout_time = nimble::runtime::SessionTimeOfDay{ 8U, 30U, 0U };
  nimble::runtime::BlackoutCalendar calendar;
  calendar.AddDate(20260706U);

  REQUIRE(!nimble::runtime::IsWithinLogonWindowWithBlackouts(schedule, calendar, MakeUtcNs(2026, 7, 6, 8, 15, 0)));
}

TEST_CASE("trigger day cut with valid session", "[session-schedule]")
{
  nimble::runtime::Engine engine;
  const auto boot = engine.Boot(MakeEngineConfig());
  REQUIRE(boot.ok());

  REQUIRE(nimble::runtime::TriggerDayCut(engine, 1001U).ok());
}

TEST_CASE("trigger day cut with unknown session", "[session-schedule]")
{
  nimble::runtime::Engine engine;
  const auto boot = engine.Boot(MakeEngineConfig());
  REQUIRE(boot.ok());

  const auto status = nimble::runtime::TriggerDayCut(engine, 999U);
  REQUIRE(!status.ok());
  REQUIRE(status.code() == nimble::base::ErrorCode::kNotFound);
}

TEST_CASE("engine schedule query accessors", "[session-schedule]")
{
  auto config = MakeEngineConfig();
  config.trace_mode = nimble::runtime::TraceMode::kRing;
  config.trace_capacity = 8U;
  config.counterparties.front().session_schedule = MakeDailySchedule();

  nimble::runtime::Engine engine;
  REQUIRE(engine.Boot(config).ok());

  const auto status = engine.QueryScheduleStatus(1001U, MakeUtcNs(2026, 7, 6, 12, 0, 0));
  REQUIRE(status.ok());
  REQUIRE(status.value().in_session_window);
  const auto events = engine.trace().Snapshot();
  REQUIRE(std::any_of(events.begin(), events.end(), [](const auto& event) {
    return event.kind == nimble::runtime::TraceEventKind::kScheduleEvent;
  }));
}

TEST_CASE("day cut validation rejects invalid hour", "[session-schedule]")
{
  auto config = MakeEngineConfig();
  config.counterparties.front().day_cut = nimble::session::DayCutConfig{
    .mode = nimble::session::DayCutMode::kFixedUtcTime,
    .reset_hour = 25,
    .reset_minute = 0,
  };

  const auto result = nimble::runtime::ValidateEngineConfigFull(config);
  REQUIRE(!result.ok());
  REQUIRE(HasDiagnosticField(result, "counterparties[0].day_cut.reset_hour"));
}

TEST_CASE("day cut validation rejects invalid minute", "[session-schedule]")
{
  auto config = MakeEngineConfig();
  config.counterparties.front().day_cut = nimble::session::DayCutConfig{
    .mode = nimble::session::DayCutMode::kFixedUtcTime,
    .reset_hour = 17,
    .reset_minute = 60,
  };

  const auto result = nimble::runtime::ValidateEngineConfigFull(config);
  REQUIRE(!result.ok());
  REQUIRE(HasDiagnosticField(result, "counterparties[0].day_cut.reset_minute"));
}

TEST_CASE("day cut validation accepts valid config", "[session-schedule]")
{
  auto config = MakeEngineConfig();
  config.counterparties.front().day_cut = nimble::session::DayCutConfig{
    .mode = nimble::session::DayCutMode::kFixedUtcTime,
    .reset_hour = 17,
    .reset_minute = 0,
  };

  REQUIRE(nimble::runtime::ValidateEngineConfigFull(config).ok());
}

TEST_CASE("blackout calendar remove date", "[session-schedule]")
{
  nimble::runtime::BlackoutCalendar calendar;
  calendar.AddDate(20260704U);
  calendar.RemoveDate(20260704U);

  REQUIRE(!calendar.IsBlackoutDate(20260704U));
  REQUIRE(calendar.empty());
}

TEST_CASE("blackout calendar clear", "[session-schedule]")
{
  nimble::runtime::BlackoutCalendar calendar;
  calendar.AddDates({ 20260704U, 20261225U });
  calendar.Clear();

  REQUIRE(calendar.empty());
  REQUIRE(calendar.size() == 0U);
}

TEST_CASE("next logon window close", "[session-schedule]")
{
  auto schedule = MakeDailySchedule();
  schedule.logon_time = nimble::runtime::SessionTimeOfDay{ 8U, 0U, 0U };
  schedule.logout_time = nimble::runtime::SessionTimeOfDay{ 8U, 30U, 0U };

  REQUIRE(nimble::runtime::NextLogonWindowClose(schedule, MakeUtcNs(2026, 7, 6, 8, 15, 0)) ==
          MakeUtcNs(2026, 7, 6, 8, 30, 0));
}

TEST_CASE("explain schedule non-stop session", "[session-schedule]")
{
  auto counterparty = MakeCounterparty();
  counterparty.session_schedule.non_stop_session = true;

  const nimble::runtime::BlackoutCalendar calendar;
  const auto timeline = nimble::runtime::ExplainSchedule(counterparty, calendar, MakeUtcNs(2026, 7, 6, 0, 0, 0), 7U);

  REQUIRE(timeline.session_id == counterparty.session.session_id);
  REQUIRE(timeline.non_stop);
  REQUIRE(timeline.entries.empty());
}

TEST_CASE("explain schedule daily window 3 days", "[session-schedule]")
{
  auto counterparty = MakeCounterparty();
  counterparty.session_schedule = MakeDailySchedule();

  const nimble::runtime::BlackoutCalendar calendar;
  const auto timeline = nimble::runtime::ExplainSchedule(counterparty, calendar, MakeUtcNs(2026, 7, 6, 0, 0, 0), 3U);

  REQUIRE(!timeline.non_stop);
  REQUIRE(timeline.entries.size() == 6U);
  REQUIRE(timeline.entries[0].kind == nimble::runtime::SessionScheduleEventKind::kEnteredSessionWindow);
  REQUIRE(timeline.entries[0].unix_time_ns == MakeUtcNs(2026, 7, 6, 9, 0, 0));
  REQUIRE(timeline.entries[1].kind == nimble::runtime::SessionScheduleEventKind::kExitedSessionWindow);
  REQUIRE(timeline.entries[1].unix_time_ns == MakeUtcNs(2026, 7, 6, 17, 0, 0));
  REQUIRE(timeline.entries[4].kind == nimble::runtime::SessionScheduleEventKind::kEnteredSessionWindow);
  REQUIRE(timeline.entries[4].unix_time_ns == MakeUtcNs(2026, 7, 8, 9, 0, 0));
  REQUIRE(timeline.entries[5].kind == nimble::runtime::SessionScheduleEventKind::kExitedSessionWindow);
  REQUIRE(timeline.entries[5].unix_time_ns == MakeUtcNs(2026, 7, 8, 17, 0, 0));
}

TEST_CASE("chained schedule explain timeline", "[session-schedule]")
{
  auto counterparty = MakeCounterparty();
  counterparty.session_schedule = MakeChainedSchedule();

  const nimble::runtime::BlackoutCalendar calendar;
  const auto timeline = nimble::runtime::ExplainSchedule(counterparty, calendar, MakeUtcNs(2026, 7, 6, 0, 0, 0), 3U);

  REQUIRE(!timeline.non_stop);
  REQUIRE(timeline.entries.size() == 12U);
  REQUIRE(std::is_sorted(timeline.entries.begin(), timeline.entries.end(), [](const auto& left, const auto& right) {
    return left.unix_time_ns <= right.unix_time_ns;
  }));
  REQUIRE(timeline.entries[0].kind == nimble::runtime::SessionScheduleEventKind::kEnteredSessionWindow);
  REQUIRE(timeline.entries[0].unix_time_ns == MakeUtcNs(2026, 7, 6, 1, 0, 0));
  REQUIRE(timeline.entries[1].kind == nimble::runtime::SessionScheduleEventKind::kExitedSessionWindow);
  REQUIRE(timeline.entries[1].unix_time_ns == MakeUtcNs(2026, 7, 6, 9, 0, 0));
  REQUIRE(timeline.entries[2].kind == nimble::runtime::SessionScheduleEventKind::kEnteredSessionWindow);
  REQUIRE(timeline.entries[2].unix_time_ns == MakeUtcNs(2026, 7, 6, 14, 0, 0));
  REQUIRE(timeline.entries[3].kind == nimble::runtime::SessionScheduleEventKind::kExitedSessionWindow);
  REQUIRE(timeline.entries[3].unix_time_ns == MakeUtcNs(2026, 7, 6, 21, 0, 0));
  REQUIRE(timeline.entries[8].kind == nimble::runtime::SessionScheduleEventKind::kEnteredSessionWindow);
  REQUIRE(timeline.entries[8].unix_time_ns == MakeUtcNs(2026, 7, 8, 1, 0, 0));
  REQUIRE(timeline.entries[11].kind == nimble::runtime::SessionScheduleEventKind::kExitedSessionWindow);
  REQUIRE(timeline.entries[11].unix_time_ns == MakeUtcNs(2026, 7, 8, 21, 0, 0));
}

TEST_CASE("explain schedule with blackout", "[session-schedule]")
{
  auto counterparty = MakeCounterparty();
  counterparty.session_schedule = MakeDailySchedule();

  nimble::runtime::BlackoutCalendar calendar;
  calendar.AddDate(20260707U);
  const auto timeline = nimble::runtime::ExplainSchedule(counterparty, calendar, MakeUtcNs(2026, 7, 6, 0, 0, 0), 3U);

  const auto has_blackout_start = std::any_of(timeline.entries.begin(), timeline.entries.end(), [](const auto& entry) {
    return entry.kind == nimble::runtime::SessionScheduleEventKind::kBlackoutStarted &&
           entry.unix_time_ns == MakeUtcNs(2026, 7, 7, 0, 0, 0);
  });
  const auto has_blackout_end = std::any_of(timeline.entries.begin(), timeline.entries.end(), [](const auto& entry) {
    return entry.kind == nimble::runtime::SessionScheduleEventKind::kBlackoutEnded &&
           entry.unix_time_ns == MakeUtcNs(2026, 7, 8, 0, 0, 0);
  });
  REQUIRE(has_blackout_start);
  REQUIRE(has_blackout_end);
  REQUIRE(std::none_of(timeline.entries.begin(), timeline.entries.end(), [](const auto& entry) {
    const auto is_session_boundary = entry.kind == nimble::runtime::SessionScheduleEventKind::kEnteredSessionWindow ||
                                     entry.kind == nimble::runtime::SessionScheduleEventKind::kExitedSessionWindow;
    return is_session_boundary && nimble::runtime::ExtractDate(entry.unix_time_ns) == 20260707U;
  }));
}

TEST_CASE("explain schedule weekly window", "[session-schedule]")
{
  auto counterparty = MakeCounterparty();
  counterparty.session_schedule = MakeWeeklySchedule();

  const nimble::runtime::BlackoutCalendar calendar;
  const auto timeline = nimble::runtime::ExplainSchedule(counterparty, calendar, MakeUtcNs(2026, 7, 6, 0, 0, 0), 7U);

  REQUIRE(timeline.entries.size() == 2U);
  REQUIRE(timeline.entries[0].kind == nimble::runtime::SessionScheduleEventKind::kEnteredSessionWindow);
  REQUIRE(timeline.entries[0].unix_time_ns == MakeUtcNs(2026, 7, 6, 9, 0, 0));
  REQUIRE(timeline.entries[1].kind == nimble::runtime::SessionScheduleEventKind::kExitedSessionWindow);
  REQUIRE(timeline.entries[1].unix_time_ns == MakeUtcNs(2026, 7, 10, 17, 0, 0));
  REQUIRE(std::none_of(timeline.entries.begin(), timeline.entries.end(), [](const auto& entry) {
    const auto date = nimble::runtime::ExtractDate(entry.unix_time_ns);
    return date == 20260711U || date == 20260712U;
  }));
}

TEST_CASE("explain schedule summary output", "[session-schedule]")
{
  auto counterparty = MakeCounterparty();
  counterparty.session_schedule = MakeDailySchedule();

  const nimble::runtime::BlackoutCalendar calendar;
  const auto timeline = nimble::runtime::ExplainSchedule(counterparty, calendar, MakeUtcNs(2026, 7, 6, 0, 0, 0), 1U);
  const auto summary = timeline.summary();

  REQUIRE(!summary.empty());
  REQUIRE(summary.find("Schedule timeline for session 1001") != std::string::npos);
  REQUIRE(summary.find("2026-07-06 09:00:00 UTC") != std::string::npos);
  REQUIRE(summary.find("session window opens") != std::string::npos);
}

TEST_CASE("explain schedule local time handles dst transition", "[session-schedule]")
{
  const ScopedTimezone timezone("EST5EDT,M3.2.0/2,M11.1.0/2");
  auto counterparty = MakeCounterparty();
  counterparty.session_schedule = MakeDailySchedule();
  counterparty.session_schedule.use_local_time = true;

  const nimble::runtime::BlackoutCalendar calendar;
  const auto timeline = nimble::runtime::ExplainSchedule(counterparty, calendar, MakeUtcNs(2026, 3, 7, 0, 0, 0), 3U);

  const auto has_pre_dst_open = std::any_of(timeline.entries.begin(), timeline.entries.end(), [](const auto& entry) {
    return entry.kind == nimble::runtime::SessionScheduleEventKind::kEnteredSessionWindow &&
           entry.unix_time_ns == MakeUtcNs(2026, 3, 7, 14, 0, 0);
  });
  const auto has_post_dst_open = std::any_of(timeline.entries.begin(), timeline.entries.end(), [](const auto& entry) {
    return entry.kind == nimble::runtime::SessionScheduleEventKind::kEnteredSessionWindow &&
           entry.unix_time_ns == MakeUtcNs(2026, 3, 8, 13, 0, 0);
  });

  REQUIRE(has_pre_dst_open);
  REQUIRE(has_post_dst_open);
}

TEST_CASE("chained schedule validation rejects mixing", "[session-schedule]")
{
  auto schedule = MakeChainedSchedule();
  schedule.start_time = nimble::runtime::SessionTimeOfDay{ 9U, 0U, 0U };

  const auto status = nimble::runtime::ValidateSessionSchedule(schedule);
  REQUIRE(!status.ok());
  REQUIRE(status.message().find("cannot mix legacy single-window fields with chained segments") != std::string::npos);
}

TEST_CASE("chained schedule validation rejects non_stop with segments", "[session-schedule]")
{
  auto schedule = MakeChainedSchedule();
  schedule.non_stop_session = true;

  const auto status = nimble::runtime::ValidateSessionSchedule(schedule);
  REQUIRE(!status.ok());
  REQUIRE(status.message().find("non_stop_session cannot be combined with chained segments") != std::string::npos);
}
