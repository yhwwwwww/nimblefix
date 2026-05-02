#include "nimblefix/runtime/config.h"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "nimblefix/runtime/contract_binding.h"
#include "nimblefix/runtime/schedule_helpers.h"

namespace nimble::runtime {

namespace {

auto
TransportProfileForVersion(session::TransportVersion version) -> session::TransportSessionProfile
{
  switch (version) {
    case session::TransportVersion::kFix40:
      return session::TransportSessionProfile::Fix40();
    case session::TransportVersion::kFix41:
      return session::TransportSessionProfile::Fix41();
    case session::TransportVersion::kFix42:
      return session::TransportSessionProfile::Fix42();
    case session::TransportVersion::kFix43:
      return session::TransportSessionProfile::Fix43();
    case session::TransportVersion::kFix44:
      return session::TransportSessionProfile::Fix44();
    case session::TransportVersion::kFixT11:
      return session::TransportSessionProfile::FixT11();
  }

  return session::TransportSessionProfile::Fix44();
}

auto
IsTransportSession(std::string_view begin_string) -> bool
{
  return begin_string == "FIXT.1.1";
}

auto
IsValidTimeOfDay(const SessionTimeOfDay& time) -> bool
{
  return time.hour < 24U && time.minute < 60U && time.second < 60U;
}

auto
HasSessionWindowFields(const SessionScheduleConfig& schedule) -> bool
{
  return schedule.start_time.has_value() || schedule.end_time.has_value() || schedule.start_day.has_value() ||
         schedule.end_day.has_value();
}

auto
HasLogonWindowFields(const SessionScheduleConfig& schedule) -> bool
{
  return schedule.logon_time.has_value() || schedule.logout_time.has_value() || schedule.logon_day.has_value() ||
         schedule.logout_day.has_value();
}

auto
SecondsOfDay(const SessionTimeOfDay& time) -> int
{
  return static_cast<int>(time.hour) * 3600 + static_cast<int>(time.minute) * 60 + static_cast<int>(time.second);
}

} // namespace

namespace detail {

auto
BuildWindowSpec(const SessionScheduleConfig& schedule, bool logon_window) -> std::optional<SessionWindowSpec>
{
  if (schedule.non_stop_session) {
    return std::nullopt;
  }

  if (logon_window && HasLogonWindowFields(schedule)) {
    return SessionWindowSpec{
      .use_local_time = schedule.use_local_time,
      .start_second = SecondsOfDay(*schedule.logon_time),
      .end_second = SecondsOfDay(*schedule.logout_time),
      .start_day =
        schedule.logon_day.has_value() ? std::optional<int>(static_cast<int>(*schedule.logon_day)) : std::nullopt,
      .end_day =
        schedule.logout_day.has_value() ? std::optional<int>(static_cast<int>(*schedule.logout_day)) : std::nullopt,
    };
  }

  if (!HasSessionWindowFields(schedule)) {
    return std::nullopt;
  }

  return SessionWindowSpec{
    .use_local_time = schedule.use_local_time,
    .start_second = SecondsOfDay(*schedule.start_time),
    .end_second = SecondsOfDay(*schedule.end_time),
    .start_day =
      schedule.start_day.has_value() ? std::optional<int>(static_cast<int>(*schedule.start_day)) : std::nullopt,
    .end_day = schedule.end_day.has_value() ? std::optional<int>(static_cast<int>(*schedule.end_day)) : std::nullopt,
  };
}

auto
BuildCalendarPoint(std::uint64_t unix_time_ns, bool use_local_time) -> CalendarPoint
{
  const auto unix_seconds = static_cast<std::time_t>(unix_time_ns / 1'000'000'000ULL);
  std::tm civil_time{};
  if (use_local_time) {
    localtime_r(&unix_seconds, &civil_time);
  } else {
    gmtime_r(&unix_seconds, &civil_time);
  }
  return CalendarPoint{
    .civil_time = civil_time,
    .weekday = civil_time.tm_wday,
    .second_of_day = civil_time.tm_hour * 3600 + civil_time.tm_min * 60 + civil_time.tm_sec,
  };
}

auto
MakeUnixTimeNs(std::tm civil_time, bool use_local_time) -> std::optional<std::uint64_t>
{
  const auto unix_seconds = use_local_time ? mktime(&civil_time) : timegm(&civil_time);
  if (unix_seconds < 0) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(unix_seconds) * 1'000'000'000ULL;
}

auto
IsWithinWindow(const SessionWindowSpec& window, std::uint64_t unix_time_ns) -> bool
{
  const auto point = BuildCalendarPoint(unix_time_ns, window.use_local_time);
  const auto current =
    window.start_day.has_value() ? point.weekday * kSecondsPerDay + point.second_of_day : point.second_of_day;
  const auto start =
    window.start_day.has_value() ? *window.start_day * kSecondsPerDay + window.start_second : window.start_second;
  const auto end =
    window.end_day.has_value() ? *window.end_day * kSecondsPerDay + window.end_second : window.end_second;
  if (start == end) {
    return true;
  }
  if (start < end) {
    return current >= start && current < end;
  }
  return current >= start || current < end;
}

auto
NextWindowStart(const SessionWindowSpec& window, std::uint64_t unix_time_ns) -> std::optional<std::uint64_t>
{
  if (IsWithinWindow(window, unix_time_ns)) {
    return unix_time_ns;
  }

  const auto point = BuildCalendarPoint(unix_time_ns, window.use_local_time);
  auto candidate = point.civil_time;
  candidate.tm_hour = window.start_second / 3600;
  candidate.tm_min = (window.start_second / 60) % 60;
  candidate.tm_sec = window.start_second % 60;

  if (window.start_day.has_value()) {
    int delta_days = *window.start_day - point.weekday;
    if (delta_days < 0) {
      delta_days += 7;
    }
    candidate.tm_mday += delta_days;
    auto candidate_ns = MakeUnixTimeNs(candidate, window.use_local_time);
    if (!candidate_ns.has_value()) {
      return std::nullopt;
    }
    if (*candidate_ns <= unix_time_ns) {
      candidate.tm_mday += 7;
      return MakeUnixTimeNs(candidate, window.use_local_time);
    }
    return candidate_ns;
  }

  auto candidate_ns = MakeUnixTimeNs(candidate, window.use_local_time);
  if (!candidate_ns.has_value()) {
    return std::nullopt;
  }
  if (*candidate_ns <= unix_time_ns) {
    candidate.tm_mday += 1;
    return MakeUnixTimeNs(candidate, window.use_local_time);
  }
  return candidate_ns;
}

} // namespace detail

auto
ValidateSessionSchedule(const SessionScheduleConfig& schedule) -> base::Status
{
  const auto validate_time_pair = [](std::string_view label,
                                     const std::optional<SessionTimeOfDay>& start_time,
                                     const std::optional<SessionTimeOfDay>& end_time,
                                     const std::optional<SessionDayOfWeek>& start_day,
                                     const std::optional<SessionDayOfWeek>& end_day) -> base::Status {
    if (start_time.has_value() != end_time.has_value()) {
      return base::Status::InvalidArgument(std::string(label) + " requires both start and end times");
    }
    if (start_day.has_value() != end_day.has_value()) {
      return base::Status::InvalidArgument(std::string(label) + " requires both start and end days");
    }
    if ((start_day.has_value() || end_day.has_value()) && !start_time.has_value()) {
      return base::Status::InvalidArgument(std::string(label) + " days require matching times");
    }
    if (start_time.has_value() && !IsValidTimeOfDay(*start_time)) {
      return base::Status::InvalidArgument(std::string(label) + " start time is out of range");
    }
    if (end_time.has_value() && !IsValidTimeOfDay(*end_time)) {
      return base::Status::InvalidArgument(std::string(label) + " end time is out of range");
    }
    return base::Status::Ok();
  };

  if (schedule.non_stop_session && (HasSessionWindowFields(schedule) || HasLogonWindowFields(schedule))) {
    return base::Status::InvalidArgument("non_stop_session cannot be combined with session or logon window "
                                         "settings");
  }

  auto session_window_status =
    validate_time_pair("session window", schedule.start_time, schedule.end_time, schedule.start_day, schedule.end_day);
  if (!session_window_status.ok()) {
    return session_window_status;
  }

  auto logon_window_status = validate_time_pair(
    "logon window", schedule.logon_time, schedule.logout_time, schedule.logon_day, schedule.logout_day);
  if (!logon_window_status.ok()) {
    return logon_window_status;
  }

  return base::Status::Ok();
}

auto
IsWithinSessionWindow(const SessionScheduleConfig& schedule, std::uint64_t unix_time_ns) -> bool
{
  const auto window = detail::BuildWindowSpec(schedule, false);
  return !window.has_value() || detail::IsWithinWindow(*window, unix_time_ns);
}

auto
IsWithinLogonWindow(const SessionScheduleConfig& schedule, std::uint64_t unix_time_ns) -> bool
{
  const auto window = detail::BuildWindowSpec(schedule, true);
  return !window.has_value() || detail::IsWithinWindow(*window, unix_time_ns);
}

auto
NextLogonWindowStart(const SessionScheduleConfig& schedule, std::uint64_t unix_time_ns) -> std::optional<std::uint64_t>
{
  const auto window = detail::BuildWindowSpec(schedule, true);
  if (!window.has_value()) {
    return unix_time_ns;
  }
  return detail::NextWindowStart(*window, unix_time_ns);
}

namespace {

auto
TlsVersionRank(TlsProtocolVersion version) -> int
{
  switch (version) {
    case TlsProtocolVersion::kSystemDefault:
      return 0;
    case TlsProtocolVersion::kTls10:
      return 10;
    case TlsProtocolVersion::kTls11:
      return 11;
    case TlsProtocolVersion::kTls12:
      return 12;
    case TlsProtocolVersion::kTls13:
      return 13;
  }
  return 0;
}

auto
IsValidDayOfWeek(SessionDayOfWeek day) -> bool
{
  const auto value = static_cast<std::uint32_t>(day);
  return value <= static_cast<std::uint32_t>(SessionDayOfWeek::kSaturday);
}

auto
PathExists(const std::filesystem::path& path) -> bool
{
  return path.empty() || std::filesystem::exists(path);
}

auto
ValidateTlsVersionRange(TlsProtocolVersion min_version, TlsProtocolVersion max_version, std::string_view owner_label)
  -> base::Status
{
  const auto min_rank = TlsVersionRank(min_version);
  const auto max_rank = TlsVersionRank(max_version);
  if (min_rank != 0 && max_rank != 0 && min_rank > max_rank) {
    return base::Status::InvalidArgument(std::string(owner_label) + " config has min_version above max_version");
  }
  return base::Status::Ok();
}

auto
StatusFromCode(base::ErrorCode code, std::string message) -> base::Status
{
  switch (code) {
    case base::ErrorCode::kInvalidArgument:
      return base::Status::InvalidArgument(std::move(message));
    case base::ErrorCode::kIoError:
      return base::Status::IoError(std::move(message));
    case base::ErrorCode::kBusy:
      return base::Status::Busy(std::move(message));
    case base::ErrorCode::kFormatError:
      return base::Status::FormatError(std::move(message));
    case base::ErrorCode::kVersionMismatch:
      return base::Status::VersionMismatch(std::move(message));
    case base::ErrorCode::kNotFound:
      return base::Status::NotFound(std::move(message));
    case base::ErrorCode::kAlreadyExists:
      return base::Status::AlreadyExists(std::move(message));
    case base::ErrorCode::kOk:
      return base::Status::Ok();
  }
  return base::Status::InvalidArgument(std::move(message));
}

auto
Plural(std::size_t count, std::string_view singular, std::string_view plural) -> std::string_view
{
  return count == 1U ? singular : plural;
}

auto
DiagnosticPrefix(ConfigErrorSeverity severity) -> std::string_view
{
  return severity == ConfigErrorSeverity::kWarning ? "warning" : "error";
}

auto
CounterpartyFieldPath(std::size_t index, std::string_view field) -> std::string
{
  return "counterparties[" + std::to_string(index) + "]." + std::string(field);
}

auto
SessionFieldPath(std::size_t index, std::string_view field) -> std::string
{
  return CounterpartyFieldPath(index, "session.") + std::string(field);
}

auto
SessionScheduleFieldPath(std::size_t index, std::string_view field) -> std::string
{
  return CounterpartyFieldPath(index, "session_schedule.") + std::string(field);
}

auto
DayCutFieldPath(std::size_t index, std::string_view field) -> std::string
{
  return CounterpartyFieldPath(index, "day_cut.") + std::string(field);
}

auto
ListenerFieldPath(std::size_t index, std::string_view field) -> std::string
{
  return "listeners[" + std::to_string(index) + "]." + std::string(field);
}

auto
StatusMessage(const base::Status& status) -> std::string
{
  return std::string(status.message());
}

auto
AddDiagnostic(ConfigValidationResult& result,
              std::string field_path,
              std::string message,
              ConfigErrorSeverity severity = ConfigErrorSeverity::kError,
              base::ErrorCode code = base::ErrorCode::kInvalidArgument) -> void
{
  result.errors.push_back(ConfigError{
    .field_path = std::move(field_path),
    .severity = severity,
    .code = code,
    .message = std::move(message),
  });
}

auto
AddStatusDiagnostic(ConfigValidationResult& result,
                    std::string field_path,
                    const base::Status& status,
                    ConfigErrorSeverity severity = ConfigErrorSeverity::kError) -> void
{
  AddDiagnostic(result, std::move(field_path), StatusMessage(status), severity, status.code());
}

auto
ValidateSessionScheduleFull(const SessionScheduleConfig& schedule, std::size_t counterparty_index)
  -> ConfigValidationResult
{
  ConfigValidationResult result;

  const auto add_time_pair_diagnostics = [&](std::string_view label,
                                             std::string_view start_time_field,
                                             const std::optional<SessionTimeOfDay>& start_time,
                                             std::string_view end_time_field,
                                             const std::optional<SessionTimeOfDay>& end_time,
                                             std::string_view start_day_field,
                                             const std::optional<SessionDayOfWeek>& start_day,
                                             std::string_view end_day_field,
                                             const std::optional<SessionDayOfWeek>& end_day) {
    if (start_time.has_value() != end_time.has_value()) {
      AddDiagnostic(
        result,
        SessionScheduleFieldPath(counterparty_index, start_time.has_value() ? end_time_field : start_time_field),
        std::string(label) + " requires both start and end times");
    }
    if (start_day.has_value() != end_day.has_value()) {
      AddDiagnostic(
        result,
        SessionScheduleFieldPath(counterparty_index, start_day.has_value() ? end_day_field : start_day_field),
        std::string(label) + " requires both start and end days");
    }
    if ((start_day.has_value() || end_day.has_value()) && !start_time.has_value()) {
      AddDiagnostic(result,
                    SessionScheduleFieldPath(counterparty_index, start_time_field),
                    std::string(label) + " days require matching times");
    }
    if (start_time.has_value() && !IsValidTimeOfDay(*start_time)) {
      AddDiagnostic(result,
                    SessionScheduleFieldPath(counterparty_index, start_time_field),
                    std::string(label) + " start time is out of range");
    }
    if (end_time.has_value() && !IsValidTimeOfDay(*end_time)) {
      AddDiagnostic(result,
                    SessionScheduleFieldPath(counterparty_index, end_time_field),
                    std::string(label) + " end time is out of range");
    }
    if (start_day.has_value() && !IsValidDayOfWeek(*start_day)) {
      AddDiagnostic(result,
                    SessionScheduleFieldPath(counterparty_index, start_day_field),
                    std::string(label) + " start day is out of range");
    }
    if (end_day.has_value() && !IsValidDayOfWeek(*end_day)) {
      AddDiagnostic(result,
                    SessionScheduleFieldPath(counterparty_index, end_day_field),
                    std::string(label) + " end day is out of range");
    }
  };

  if (schedule.non_stop_session && (HasSessionWindowFields(schedule) || HasLogonWindowFields(schedule))) {
    AddDiagnostic(result,
                  SessionScheduleFieldPath(counterparty_index, "non_stop_session"),
                  "non_stop_session cannot be combined with session or logon window settings");
  }

  add_time_pair_diagnostics("session window",
                            "start_time",
                            schedule.start_time,
                            "end_time",
                            schedule.end_time,
                            "start_day",
                            schedule.start_day,
                            "end_day",
                            schedule.end_day);
  add_time_pair_diagnostics("logon window",
                            "logon_time",
                            schedule.logon_time,
                            "logout_time",
                            schedule.logout_time,
                            "logon_day",
                            schedule.logon_day,
                            "logout_day",
                            schedule.logout_day);

  return result;
}

auto
ValidateDayCutFull(const session::DayCutConfig& day_cut, std::size_t counterparty_index) -> ConfigValidationResult
{
  ConfigValidationResult result;
  constexpr std::int32_t kHoursPerDay = 24;
  constexpr std::int32_t kMinutesPerHour = 60;
  constexpr std::int32_t kMaxUtcOffsetSeconds = 24 * 60 * 60;

  const auto fixed_time_mode =
    day_cut.mode == session::DayCutMode::kFixedLocalTime || day_cut.mode == session::DayCutMode::kFixedUtcTime;
  if (fixed_time_mode && (day_cut.reset_hour < 0 || day_cut.reset_hour >= kHoursPerDay)) {
    AddDiagnostic(result, DayCutFieldPath(counterparty_index, "reset_hour"), "day_cut reset_hour must be 0-23");
  }
  if (fixed_time_mode && (day_cut.reset_minute < 0 || day_cut.reset_minute >= kMinutesPerHour)) {
    AddDiagnostic(result, DayCutFieldPath(counterparty_index, "reset_minute"), "day_cut reset_minute must be 0-59");
  }
  if (day_cut.utc_offset_seconds < -kMaxUtcOffsetSeconds || day_cut.utc_offset_seconds > kMaxUtcOffsetSeconds) {
    AddDiagnostic(result,
                  DayCutFieldPath(counterparty_index, "utc_offset_seconds"),
                  "day_cut utc_offset_seconds absolute value must be <= 86400");
  }

  return result;
}

auto
AppendDiagnostics(ConfigValidationResult& destination, ConfigValidationResult source) -> void
{
  destination.errors.insert(destination.errors.end(), source.errors.begin(), source.errors.end());
}

auto
BoolToText(bool value) -> std::string_view
{
  return value ? "true" : "false";
}

auto
TraceModeToText(TraceMode mode) -> std::string_view
{
  switch (mode) {
    case TraceMode::kRing:
      return "ring";
    case TraceMode::kDisabled:
      return "disabled";
  }
  return "disabled";
}

auto
QueueAppThreadingModeToText(QueueAppThreadingMode mode) -> std::string_view
{
  switch (mode) {
    case QueueAppThreadingMode::kThreaded:
      return "threaded";
    case QueueAppThreadingMode::kCoScheduled:
      return "co-scheduled";
  }
  return "co-scheduled";
}

auto
PollModeToText(PollMode mode) -> std::string_view
{
  switch (mode) {
    case PollMode::kBusy:
      return "busy";
    case PollMode::kBlocking:
      return "blocking";
  }
  return "blocking";
}

auto
IoBackendToText(IoBackend backend) -> std::string_view
{
  switch (backend) {
    case IoBackend::kIoUring:
      return "io_uring";
    case IoBackend::kEpoll:
      return "epoll";
  }
  return "epoll";
}

auto
StoreModeToText(StoreMode mode) -> std::string_view
{
  switch (mode) {
    case StoreMode::kMmap:
      return "mmap";
    case StoreMode::kDurableBatch:
      return "durable";
    case StoreMode::kMemory:
      return "memory";
  }
  return "memory";
}

auto
RecoveryModeToText(session::RecoveryMode mode) -> std::string_view
{
  switch (mode) {
    case session::RecoveryMode::kWarmRestart:
      return "warm";
    case session::RecoveryMode::kColdStart:
      return "cold";
    case session::RecoveryMode::kNoRecovery:
      return "no-recovery";
    case session::RecoveryMode::kMemoryOnly:
      return "memory";
  }
  return "memory";
}

auto
DispatchModeToText(AppDispatchMode mode) -> std::string_view
{
  switch (mode) {
    case AppDispatchMode::kQueueDecoupled:
      return "queue";
    case AppDispatchMode::kInline:
      return "inline";
  }
  return "inline";
}

auto
DurableRolloverModeToText(store::DurableStoreRolloverMode mode) -> std::string_view
{
  switch (mode) {
    case store::DurableStoreRolloverMode::kDisabled:
      return "disabled";
    case store::DurableStoreRolloverMode::kExternal:
      return "external";
    case store::DurableStoreRolloverMode::kLocalTime:
      return "local-time";
    case store::DurableStoreRolloverMode::kUtcDay:
      return "utc-day";
  }
  return "utc-day";
}

auto
DayCutModeToText(session::DayCutMode mode) -> std::string_view
{
  switch (mode) {
    case session::DayCutMode::kFixedLocalTime:
      return "fixed-local-time";
    case session::DayCutMode::kFixedUtcTime:
      return "fixed-utc-time";
    case session::DayCutMode::kExternalControl:
      return "external-control";
    case session::DayCutMode::kNoAutoReset:
      return "no-auto-reset";
  }
  return "no-auto-reset";
}

auto
DayOfWeekToText(SessionDayOfWeek day) -> std::string_view
{
  switch (day) {
    case SessionDayOfWeek::kSunday:
      return "sun";
    case SessionDayOfWeek::kMonday:
      return "mon";
    case SessionDayOfWeek::kTuesday:
      return "tue";
    case SessionDayOfWeek::kWednesday:
      return "wed";
    case SessionDayOfWeek::kThursday:
      return "thu";
    case SessionDayOfWeek::kFriday:
      return "fri";
    case SessionDayOfWeek::kSaturday:
      return "sat";
  }
  return "sun";
}

auto
TimeOfDayToText(const SessionTimeOfDay& time) -> std::string
{
  std::ostringstream out;
  out << std::setfill('0') << std::setw(2) << static_cast<int>(time.hour) << ':' << std::setw(2)
      << static_cast<int>(time.minute) << ':' << std::setw(2) << static_cast<int>(time.second);
  return out.str();
}

auto
OptionalTimeToText(const std::optional<SessionTimeOfDay>& time) -> std::string
{
  return time.has_value() ? TimeOfDayToText(*time) : std::string{};
}

auto
OptionalDayToText(const std::optional<SessionDayOfWeek>& day) -> std::string
{
  return day.has_value() ? std::string(DayOfWeekToText(*day)) : std::string{};
}

template<typename Values>
auto
JoinCsv(const Values& values) -> std::string
{
  std::string joined;
  for (const auto& value : values) {
    if (!joined.empty()) {
      joined.push_back(',');
    }
    joined += value;
  }
  return joined;
}

template<typename Values, typename Formatter>
auto
JoinCsv(const Values& values, Formatter formatter) -> std::string
{
  std::string joined;
  for (const auto& value : values) {
    if (!joined.empty()) {
      joined.push_back(',');
    }
    joined += formatter(value);
  }
  return joined;
}

auto
PathToText(const std::filesystem::path& path) -> std::string
{
  return path.string();
}

auto
ValidateTlsClientConfigFull(std::size_t counterparty_index,
                            std::string_view counterparty_name,
                            const TlsClientConfig& config) -> ConfigValidationResult
{
  ConfigValidationResult result;
  if (!config.enabled) {
    return result;
  }

  const auto field = [&](std::string_view name) {
    return CounterpartyFieldPath(counterparty_index, "tls_client.") + std::string(name);
  };

  if (!TlsTransportEnabledAtBuild()) {
    AddDiagnostic(result,
                  field("enabled"),
                  "counterparty '" + std::string(counterparty_name) +
                    "' sets tls_client.enabled=true but this build was compiled without optional TLS support");
  }
  if (config.certificate_chain_file.empty() != config.private_key_file.empty()) {
    AddDiagnostic(result,
                  field(config.certificate_chain_file.empty() ? "certificate_chain_file" : "private_key_file"),
                  "counterparty '" + std::string(counterparty_name) +
                    "' TLS client certificate_chain_file and private_key_file must be configured together");
  }
  if (!PathExists(config.certificate_chain_file)) {
    AddDiagnostic(result,
                  field("certificate_chain_file"),
                  "counterparty '" + std::string(counterparty_name) +
                    "' TLS client certificate_chain_file does not exist");
  }
  if (!PathExists(config.private_key_file)) {
    AddDiagnostic(result,
                  field("private_key_file"),
                  "counterparty '" + std::string(counterparty_name) + "' TLS client private_key_file does not exist");
  }
  if (!PathExists(config.ca_file)) {
    AddDiagnostic(result,
                  field("ca_file"),
                  "counterparty '" + std::string(counterparty_name) + "' TLS client ca_file does not exist");
  }
  if (!PathExists(config.ca_path)) {
    AddDiagnostic(result,
                  field("ca_path"),
                  "counterparty '" + std::string(counterparty_name) + "' TLS client ca_path does not exist");
  }
  if (!config.expected_peer_name.empty() && !config.verify_peer) {
    AddDiagnostic(result,
                  field("expected_peer_name"),
                  "counterparty '" + std::string(counterparty_name) +
                    "' sets expected_peer_name while verify_peer is disabled");
  }
  auto version_status = ValidateTlsVersionRange(
    config.min_version, config.max_version, "counterparty '" + std::string(counterparty_name) + "' TLS client");
  if (!version_status.ok()) {
    AddStatusDiagnostic(result, field("min_version"), version_status);
  }
  return result;
}

auto
ValidateTlsServerConfigFull(std::size_t listener_index, std::string_view listener_name, const TlsServerConfig& config)
  -> ConfigValidationResult
{
  ConfigValidationResult result;
  if (!config.enabled) {
    return result;
  }

  const auto field = [&](std::string_view name) {
    return ListenerFieldPath(listener_index, "tls_server.") + std::string(name);
  };

  if (!TlsTransportEnabledAtBuild()) {
    AddDiagnostic(result,
                  field("enabled"),
                  "listener '" + std::string(listener_name) +
                    "' sets tls_server.enabled=true but this build was compiled without optional TLS support");
  }
  if (config.certificate_chain_file.empty() || config.private_key_file.empty()) {
    AddDiagnostic(result,
                  field(config.certificate_chain_file.empty() ? "certificate_chain_file" : "private_key_file"),
                  "listener '" + std::string(listener_name) +
                    "' TLS server requires certificate_chain_file and private_key_file");
  }
  if (!PathExists(config.certificate_chain_file)) {
    AddDiagnostic(result,
                  field("certificate_chain_file"),
                  "listener '" + std::string(listener_name) + "' TLS server certificate_chain_file does not exist");
  }
  if (!PathExists(config.private_key_file)) {
    AddDiagnostic(result,
                  field("private_key_file"),
                  "listener '" + std::string(listener_name) + "' TLS server private_key_file does not exist");
  }
  if (!PathExists(config.ca_file)) {
    AddDiagnostic(
      result, field("ca_file"), "listener '" + std::string(listener_name) + "' TLS server ca_file does not exist");
  }
  if (!PathExists(config.ca_path)) {
    AddDiagnostic(
      result, field("ca_path"), "listener '" + std::string(listener_name) + "' TLS server ca_path does not exist");
  }
  if (config.require_client_certificate && !config.verify_peer) {
    AddDiagnostic(result,
                  field("require_client_certificate"),
                  "listener '" + std::string(listener_name) +
                    "' TLS server require_client_certificate implies verify_peer=true");
  }
  if (config.require_client_certificate && config.ca_file.empty() && config.ca_path.empty()) {
    AddDiagnostic(result,
                  field("ca_file"),
                  "listener '" + std::string(listener_name) +
                    "' TLS server require_client_certificate requires ca_file or ca_path");
  }
  auto version_status = ValidateTlsVersionRange(
    config.min_version, config.max_version, "listener '" + std::string(listener_name) + "' TLS server");
  if (!version_status.ok()) {
    AddStatusDiagnostic(result, field("min_version"), version_status);
  }
  return result;
}

} // namespace

auto
TlsTransportEnabledAtBuild() noexcept -> bool
{
#if defined(NIMBLEFIX_ENABLE_TLS)
  return true;
#else
  return false;
#endif
}

auto
ConfigValidationResult::ok() const noexcept -> bool
{
  return !has_errors();
}

auto
ConfigValidationResult::has_errors() const noexcept -> bool
{
  return std::any_of(
    errors.begin(), errors.end(), [](const auto& error) { return error.severity == ConfigErrorSeverity::kError; });
}

auto
ConfigValidationResult::has_warnings() const noexcept -> bool
{
  return std::any_of(
    errors.begin(), errors.end(), [](const auto& error) { return error.severity == ConfigErrorSeverity::kWarning; });
}

auto
ConfigValidationResult::summary() const -> std::string
{
  const auto error_count = static_cast<std::size_t>(std::count_if(
    errors.begin(), errors.end(), [](const auto& error) { return error.severity == ConfigErrorSeverity::kError; }));
  const auto warning_count = errors.size() - error_count;

  std::ostringstream out;
  out << error_count << ' ' << Plural(error_count, "error", "errors") << ", " << warning_count << ' '
      << Plural(warning_count, "warning", "warnings");
  if (errors.empty()) {
    return out.str();
  }
  out << ':';
  for (const auto& error : errors) {
    out << "\n  [" << DiagnosticPrefix(error.severity) << "] " << error.field_path << ": " << error.message;
  }
  return out.str();
}

auto
ConfigValidationResult::first_error_status() const -> base::Status
{
  const auto it = std::find_if(
    errors.begin(), errors.end(), [](const auto& error) { return error.severity == ConfigErrorSeverity::kError; });
  if (it == errors.end()) {
    return base::Status::Ok();
  }
  return StatusFromCode(it->code, it->message);
}

auto
ValidateEngineConfigFull(const EngineConfig& config) -> ConfigValidationResult
{
  ConfigValidationResult result;

  if (config.worker_count == 0) {
    AddDiagnostic(result, "worker_count", "engine worker_count must be positive");
  }
  if (!config.enable_metrics) {
    AddDiagnostic(result, "enable_metrics", "metrics collection is disabled", ConfigErrorSeverity::kWarning);
  }
  if (config.profile_mlock) {
    AddDiagnostic(result,
                  "profile_mlock",
                  "profile mlock may fail at runtime if RLIMIT_MEMLOCK is insufficient",
                  ConfigErrorSeverity::kWarning);
  }
  if (config.trace_mode == TraceMode::kRing && config.trace_capacity == 0) {
    AddDiagnostic(result, "trace_capacity", "ring trace mode requires a positive trace_capacity");
  }
  if (config.worker_count > 0 && config.worker_cpu_affinity.size() > config.worker_count) {
    AddDiagnostic(result, "worker_cpu_affinity", "worker_cpu_affinity must not contain more entries than worker_count");
  }
  if (config.worker_count > 0 && config.app_cpu_affinity.size() > config.worker_count) {
    AddDiagnostic(result, "app_cpu_affinity", "app_cpu_affinity must not contain more entries than worker_count");
  }
  if (config.queue_app_mode == QueueAppThreadingMode::kCoScheduled && !config.app_cpu_affinity.empty()) {
    AddDiagnostic(result, "app_cpu_affinity", "app_cpu_affinity requires engine.queue_app_mode=threaded");
  }
  if (!config.counterparties.empty() && config.profile_artifacts.empty() && config.profile_dictionaries.empty()) {
    AddDiagnostic(result, "profile_artifacts", "counterparty configs require at least one profile artifact");
  }

  auto loaded_contracts = LoadContractMap(config.profile_contracts);
  if (!loaded_contracts.ok()) {
    AddStatusDiagnostic(result, "profile_contracts", loaded_contracts.status());
  }

  std::unordered_set<std::string> listener_names;
  bool has_plain_listener = false;
  bool has_tls_listener = false;
  for (std::size_t listener_index = 0; listener_index < config.listeners.size(); ++listener_index) {
    const auto& listener = config.listeners[listener_index];
    if (listener.name.empty()) {
      AddDiagnostic(result, ListenerFieldPath(listener_index, "name"), "listener name must not be empty");
    }
    if (!listener.name.empty() && !listener_names.emplace(listener.name).second) {
      AddDiagnostic(result,
                    ListenerFieldPath(listener_index, "name"),
                    "duplicate listener name in runtime config",
                    ConfigErrorSeverity::kError,
                    base::ErrorCode::kAlreadyExists);
    }
    if (config.worker_count > 0 && listener.worker_hint >= config.worker_count) {
      AddDiagnostic(result,
                    ListenerFieldPath(listener_index, "worker_hint"),
                    "listener worker_hint must be less than worker_count");
    }
    AppendDiagnostics(result, ValidateTlsServerConfigFull(listener_index, listener.name, listener.tls_server));
    if (listener.tls_server.enabled) {
      has_tls_listener = true;
    } else {
      has_plain_listener = true;
    }
  }

  std::unordered_set<std::uint64_t> session_ids;
  for (std::size_t counterparty_index = 0; counterparty_index < config.counterparties.size(); ++counterparty_index) {
    const auto& counterparty = config.counterparties[counterparty_index];
    if (counterparty.name.empty()) {
      AddDiagnostic(result, CounterpartyFieldPath(counterparty_index, "name"), "counterparty name must not be empty");
    }
    if (counterparty.session.session_id == 0) {
      AddDiagnostic(
        result, SessionFieldPath(counterparty_index, "session_id"), "counterparty session_id must be positive");
    }
    if (counterparty.session.session_id != 0 && !session_ids.emplace(counterparty.session.session_id).second) {
      AddDiagnostic(result,
                    SessionFieldPath(counterparty_index, "session_id"),
                    "duplicate session_id in runtime config",
                    ConfigErrorSeverity::kError,
                    base::ErrorCode::kAlreadyExists);
    }
    if (counterparty.session.profile_id == 0) {
      AddDiagnostic(
        result, SessionFieldPath(counterparty_index, "profile_id"), "counterparty profile_id must be positive");
    }
    if (counterparty.session.key.begin_string.empty()) {
      AddDiagnostic(result,
                    SessionFieldPath(counterparty_index, "key.begin_string"),
                    "counterparty session key fields must not be empty");
    }
    if (counterparty.session.key.sender_comp_id.empty()) {
      AddDiagnostic(result,
                    SessionFieldPath(counterparty_index, "key.sender_comp_id"),
                    "counterparty session key fields must not be empty");
    }
    if (counterparty.session.key.target_comp_id.empty()) {
      AddDiagnostic(result,
                    SessionFieldPath(counterparty_index, "key.target_comp_id"),
                    "counterparty session key fields must not be empty");
    }
    if (IsTransportSession(counterparty.session.key.begin_string) && counterparty.default_appl_ver_id.empty()) {
      AddDiagnostic(result,
                    CounterpartyFieldPath(counterparty_index, "default_appl_ver_id"),
                    "FIXT counterparties require default_appl_ver_id");
    }
    if ((counterparty.store_mode == StoreMode::kMmap || counterparty.store_mode == StoreMode::kDurableBatch) &&
        counterparty.store_path.empty()) {
      AddDiagnostic(
        result, CounterpartyFieldPath(counterparty_index, "store_path"), "persistent store modes require a store_path");
    }
    if (counterparty.recovery_mode == session::RecoveryMode::kWarmRestart &&
        counterparty.store_mode == StoreMode::kMemory) {
      AddDiagnostic(result,
                    CounterpartyFieldPath(counterparty_index, "recovery_mode"),
                    "warm restart recovery requires a persistent store mode");
    }
    if (counterparty.reset_seq_num_on_logon && !counterparty.transport_profile.supports_reset_on_logon) {
      AddDiagnostic(result,
                    CounterpartyFieldPath(counterparty_index, "reset_seq_num_on_logon"),
                    "reset_seq_num_on_logon is not supported by the configured transport version");
    }
    if (counterparty.send_next_expected_msg_seq_num &&
        !counterparty.transport_profile.supports_next_expected_msg_seq_num) {
      AddDiagnostic(result,
                    CounterpartyFieldPath(counterparty_index, "send_next_expected_msg_seq_num"),
                    "send_next_expected_msg_seq_num is not supported by the configured transport version");
    }
    AppendDiagnostics(result, ValidateSessionScheduleFull(counterparty.session_schedule, counterparty_index));
    AppendDiagnostics(result, ValidateDayCutFull(counterparty.day_cut, counterparty_index));
    AppendDiagnostics(result,
                      ValidateTlsClientConfigFull(counterparty_index, counterparty.name, counterparty.tls_client));
    if (!counterparty.session.is_initiator && counterparty.tls_client.enabled) {
      AddDiagnostic(result,
                    CounterpartyFieldPath(counterparty_index, "tls_client.enabled"),
                    "counterparty '" + counterparty.name +
                      "' is acceptor-mode but configures initiator TLS client settings");
    }
    if (counterparty.session.is_initiator &&
        counterparty.acceptor_transport_security != TransportSecurityRequirement::kAny) {
      AddDiagnostic(result,
                    CounterpartyFieldPath(counterparty_index, "acceptor_transport_security"),
                    "counterparty '" + counterparty.name +
                      "' is initiator-mode and must not set acceptor_transport_security");
    }
    if (!counterparty.session.is_initiator) {
      if (counterparty.acceptor_transport_security == TransportSecurityRequirement::kTlsOnly && !has_tls_listener) {
        AddDiagnostic(result,
                      CounterpartyFieldPath(counterparty_index, "acceptor_transport_security"),
                      "counterparty '" + counterparty.name +
                        "' requires TLS-only acceptor transport but no TLS listener is configured");
      }
      if (counterparty.acceptor_transport_security == TransportSecurityRequirement::kPlainOnly && !has_plain_listener) {
        AddDiagnostic(result,
                      CounterpartyFieldPath(counterparty_index, "acceptor_transport_security"),
                      "counterparty '" + counterparty.name +
                        "' requires plain-only acceptor transport but no plain listener is configured");
      }
    }
    if (counterparty.reconnect_enabled && counterparty.reconnect_initial_ms > counterparty.reconnect_max_ms) {
      AddDiagnostic(result,
                    CounterpartyFieldPath(counterparty_index, "reconnect_enabled"),
                    "reconnect_initial_ms is greater than reconnect_max_ms",
                    ConfigErrorSeverity::kWarning);
    }

    if (loaded_contracts.ok()) {
      auto effective_counterparty = ResolveEffectiveCounterpartyConfig(counterparty, loaded_contracts.value());
      if (!effective_counterparty.ok()) {
        AddStatusDiagnostic(result,
                            CounterpartyFieldPath(counterparty_index, "contract_service_subsets"),
                            effective_counterparty.status());
      }
    }
  }

  return result;
}

auto
ValidateEngineConfig(const EngineConfig& config) -> base::Status
{
  auto result = ValidateEngineConfigFull(config);
  return result.first_error_status();
}

auto
ConfigToText(const EngineConfig& config) -> std::string
{
  std::ostringstream out;
  out << "engine.worker_count=" << config.worker_count << '\n';
  out << "engine.enable_metrics=" << BoolToText(config.enable_metrics) << '\n';
  out << "engine.trace_mode=" << TraceModeToText(config.trace_mode) << '\n';
  out << "engine.trace_capacity=" << config.trace_capacity << '\n';
  if (config.front_door_cpu.has_value()) {
    out << "engine.front_door_cpu=" << *config.front_door_cpu << '\n';
  }
  out << "engine.worker_cpu_affinity="
      << JoinCsv(config.worker_cpu_affinity, [](std::uint32_t cpu) { return std::to_string(cpu); }) << '\n';
  out << "engine.queue_app_mode=" << QueueAppThreadingModeToText(config.queue_app_mode) << '\n';
  out << "engine.app_cpu_affinity="
      << JoinCsv(config.app_cpu_affinity, [](std::uint32_t cpu) { return std::to_string(cpu); }) << '\n';
  out << "engine.accept_unknown_sessions=" << BoolToText(config.accept_unknown_sessions) << '\n';
  out << "engine.backlog_warn_threshold_ms=" << config.backlog_warn_threshold_ms << '\n';
  out << "engine.backlog_warn_throttle_ms=" << config.backlog_warn_throttle_ms << '\n';
  out << "engine.poll_mode=" << PollModeToText(config.poll_mode) << '\n';
  out << "engine.io_backend=" << IoBackendToText(config.io_backend) << '\n';
  out << "engine.profile_madvise=" << BoolToText(config.profile_madvise) << '\n';
  out << "engine.profile_mlock=" << BoolToText(config.profile_mlock) << '\n';

  for (const auto& artifact : config.profile_artifacts) {
    out << "profile=" << PathToText(artifact) << '\n';
  }
  for (const auto& dictionary_group : config.profile_dictionaries) {
    out << "dictionary="
        << JoinCsv(dictionary_group, [](const std::filesystem::path& path) { return PathToText(path); }) << '\n';
  }
  for (const auto& contract : config.profile_contracts) {
    out << "contract=" << PathToText(contract) << '\n';
  }
  for (const auto& listener : config.listeners) {
    out << "listener|" << listener.name << '|' << listener.host << '|' << listener.port << '|' << listener.worker_hint
        << '\n';
  }
  for (const auto& counterparty : config.counterparties) {
    const auto& schedule = counterparty.session_schedule;
    out << "counterparty|" << counterparty.name << '|' << counterparty.session.session_id << '|'
        << counterparty.session.profile_id << '|' << counterparty.session.key.begin_string << '|'
        << counterparty.session.key.sender_comp_id << '|' << counterparty.session.key.target_comp_id << '|'
        << StoreModeToText(counterparty.store_mode) << '|' << PathToText(counterparty.store_path) << '|'
        << RecoveryModeToText(counterparty.recovery_mode) << '|' << DispatchModeToText(counterparty.dispatch_mode)
        << '|' << counterparty.session.heartbeat_interval_seconds << '|'
        << BoolToText(counterparty.session.is_initiator) << '|' << counterparty.default_appl_ver_id << '|'
        << session::ValidationModeName(counterparty.validation_policy.mode) << '|'
        << counterparty.durable_flush_threshold << '|' << DurableRolloverModeToText(counterparty.durable_rollover_mode)
        << '|' << counterparty.durable_archive_limit << '|' << BoolToText(counterparty.reconnect_enabled) << '|'
        << counterparty.reconnect_initial_ms << '|' << counterparty.reconnect_max_ms << '|'
        << counterparty.reconnect_max_retries << '|' << counterparty.durable_local_utc_offset_seconds << '|'
        << BoolToText(counterparty.durable_use_system_timezone) << '|' << DayCutModeToText(counterparty.day_cut.mode)
        << '|' << counterparty.day_cut.reset_hour << '|' << counterparty.day_cut.reset_minute << '|'
        << counterparty.day_cut.utc_offset_seconds << '|' << BoolToText(counterparty.reset_seq_num_on_logon) << '|'
        << BoolToText(counterparty.reset_seq_num_on_logout) << '|'
        << BoolToText(counterparty.reset_seq_num_on_disconnect) << '|' << BoolToText(counterparty.refresh_on_logon)
        << '|' << BoolToText(counterparty.send_next_expected_msg_seq_num) << '|' << BoolToText(schedule.use_local_time)
        << '|' << BoolToText(schedule.non_stop_session) << '|' << OptionalTimeToText(schedule.start_time) << '|'
        << OptionalTimeToText(schedule.end_time) << '|' << OptionalDayToText(schedule.start_day) << '|'
        << OptionalDayToText(schedule.end_day) << '|' << OptionalTimeToText(schedule.logon_time) << '|'
        << OptionalTimeToText(schedule.logout_time) << '|' << OptionalDayToText(schedule.logon_day) << '|'
        << OptionalDayToText(schedule.logout_day) << '|' << counterparty.sending_time_threshold_seconds << '|'
        << JoinCsv(counterparty.supported_app_msg_types) << '|'
        << BoolToText(counterparty.application_messages_available) << '|'
        << JoinCsv(counterparty.contract_service_subsets) << '|'
        << codec::TimestampResolutionName(counterparty.timestamp_resolution) << '\n';
  }

  return out.str();
}

ListenerConfigBuilder::ListenerConfigBuilder(ListenerConfig config)
  : config_(std::move(config))
{
}

auto
ListenerConfigBuilder::Named(std::string name) -> ListenerConfigBuilder
{
  return ListenerConfigBuilder(ListenerConfig{ .name = std::move(name) });
}

auto
ListenerConfigBuilder::bind(std::string host, std::uint16_t port) -> ListenerConfigBuilder&
{
  config_.host = std::move(host);
  config_.port = port;
  return *this;
}

auto
ListenerConfigBuilder::worker_hint(std::uint32_t worker_id) -> ListenerConfigBuilder&
{
  config_.worker_hint = worker_id;
  return *this;
}

auto
ListenerConfigBuilder::tls_server(TlsServerConfig config) -> ListenerConfigBuilder&
{
  config_.tls_server = std::move(config);
  return *this;
}

auto
ListenerConfigBuilder::build() const -> ListenerConfig
{
  return config_;
}

CounterpartyConfigBuilder::CounterpartyConfigBuilder(CounterpartyConfig config)
  : config_(std::move(config))
{
}

auto
CounterpartyConfigBuilder::Initiator(std::string name,
                                     std::uint64_t session_id,
                                     session::SessionKey key,
                                     std::uint64_t profile_id,
                                     session::TransportVersion transport_version) -> CounterpartyConfigBuilder
{
  CounterpartyConfig config;
  config.name = std::move(name);
  config.session.session_id = session_id;
  config.session.key = std::move(key);
  config.session.profile_id = profile_id;
  config.session.heartbeat_interval_seconds =
    TransportProfileForVersion(transport_version).default_heartbeat_interval_seconds;
  config.session.is_initiator = true;
  return CounterpartyConfigBuilder(std::move(config)).transport_version(transport_version).disable_reconnect();
}

auto
CounterpartyConfigBuilder::Acceptor(std::string name,
                                    std::uint64_t session_id,
                                    session::SessionKey key,
                                    std::uint64_t profile_id,
                                    session::TransportVersion transport_version) -> CounterpartyConfigBuilder
{
  CounterpartyConfig config;
  config.name = std::move(name);
  config.session.session_id = session_id;
  config.session.key = std::move(key);
  config.session.profile_id = profile_id;
  config.session.heartbeat_interval_seconds =
    TransportProfileForVersion(transport_version).default_heartbeat_interval_seconds;
  config.session.is_initiator = false;
  return CounterpartyConfigBuilder(std::move(config)).transport_version(transport_version).disable_reconnect();
}

auto
CounterpartyConfigBuilder::transport_version(session::TransportVersion version) -> CounterpartyConfigBuilder&
{
  config_.transport_profile = TransportProfileForVersion(version);
  config_.session.key.begin_string = config_.transport_profile.begin_string;
  return *this;
}

auto
CounterpartyConfigBuilder::default_appl_ver_id(std::string value) -> CounterpartyConfigBuilder&
{
  config_.default_appl_ver_id = value;
  config_.session.default_appl_ver_id = std::move(value);
  return *this;
}

auto
CounterpartyConfigBuilder::heartbeat_interval_seconds(std::uint32_t seconds) -> CounterpartyConfigBuilder&
{
  config_.session.heartbeat_interval_seconds = seconds;
  return *this;
}

auto
CounterpartyConfigBuilder::sending_time_threshold_seconds(std::uint32_t seconds) -> CounterpartyConfigBuilder&
{
  config_.sending_time_threshold_seconds = seconds;
  return *this;
}

auto
CounterpartyConfigBuilder::timestamp_resolution(codec::TimestampResolution resolution) -> CounterpartyConfigBuilder&
{
  config_.timestamp_resolution = resolution;
  return *this;
}

auto
CounterpartyConfigBuilder::supported_app_msg_types(std::vector<std::string> values) -> CounterpartyConfigBuilder&
{
  config_.supported_app_msg_types = std::move(values);
  return *this;
}

auto
CounterpartyConfigBuilder::contract_service_subsets(std::vector<std::string> values) -> CounterpartyConfigBuilder&
{
  config_.contract_service_subsets = std::move(values);
  return *this;
}

auto
CounterpartyConfigBuilder::application_messages_available(bool available) -> CounterpartyConfigBuilder&
{
  config_.application_messages_available = available;
  return *this;
}

auto
CounterpartyConfigBuilder::store(StoreMode mode, std::filesystem::path path) -> CounterpartyConfigBuilder&
{
  config_.store_mode = mode;
  config_.store_path = std::move(path);
  return *this;
}

auto
CounterpartyConfigBuilder::recovery_mode(session::RecoveryMode mode) -> CounterpartyConfigBuilder&
{
  config_.recovery_mode = mode;
  return *this;
}

auto
CounterpartyConfigBuilder::dispatch_mode(AppDispatchMode mode) -> CounterpartyConfigBuilder&
{
  config_.dispatch_mode = mode;
  return *this;
}

auto
CounterpartyConfigBuilder::validation_policy(session::ValidationPolicy policy) -> CounterpartyConfigBuilder&
{
  config_.validation_policy = std::move(policy);
  return *this;
}

auto
CounterpartyConfigBuilder::tls_client(TlsClientConfig config) -> CounterpartyConfigBuilder&
{
  config_.tls_client = std::move(config);
  return *this;
}

auto
CounterpartyConfigBuilder::acceptor_transport_security(TransportSecurityRequirement requirement)
  -> CounterpartyConfigBuilder&
{
  config_.acceptor_transport_security = requirement;
  return *this;
}

auto
CounterpartyConfigBuilder::reconnect(std::uint32_t initial_ms, std::uint32_t max_ms, std::uint32_t max_retries)
  -> CounterpartyConfigBuilder&
{
  config_.reconnect_enabled = true;
  config_.reconnect_initial_ms = initial_ms;
  config_.reconnect_max_ms = max_ms;
  config_.reconnect_max_retries = max_retries;
  return *this;
}

auto
CounterpartyConfigBuilder::disable_reconnect() -> CounterpartyConfigBuilder&
{
  config_.reconnect_enabled = false;
  config_.reconnect_initial_ms = kDefaultReconnectInitialMs;
  config_.reconnect_max_ms = kDefaultReconnectMaxMs;
  config_.reconnect_max_retries = kUnlimitedReconnectRetries;
  return *this;
}

auto
CounterpartyConfigBuilder::build() const -> CounterpartyConfig
{
  return config_;
}

} // namespace nimble::runtime
