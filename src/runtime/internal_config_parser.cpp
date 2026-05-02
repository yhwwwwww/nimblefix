#include "nimblefix/runtime/internal_config_parser.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace nimble::runtime {

namespace {

constexpr char kConfigCommentPrefix = '#';
constexpr char kConfigAssignmentSeparator = '=';
constexpr char kConfigFieldSeparator = '|';
constexpr char kConfigListSeparator = ',';

constexpr std::string_view kProfileRecordPrefix = "profile=";
constexpr std::string_view kContractRecordPrefix = "contract=";
constexpr std::string_view kDictionaryRecordPrefix = "dictionary=";
constexpr std::string_view kListenerRecordKind = "listener";
constexpr std::string_view kCounterpartyRecordKind = "counterparty";

namespace listener_columns {
constexpr std::size_t kName = 1U;
constexpr std::size_t kHost = 2U;
constexpr std::size_t kPort = 3U;
constexpr std::size_t kWorkerHint = 4U;
constexpr std::size_t kCount = 5U;
} // namespace listener_columns

namespace counterparty_columns {
constexpr std::size_t kName = 1U;
constexpr std::size_t kSessionId = 2U;
constexpr std::size_t kProfileId = 3U;
constexpr std::size_t kBeginString = 4U;
constexpr std::size_t kSenderCompId = 5U;
constexpr std::size_t kTargetCompId = 6U;
constexpr std::size_t kStoreMode = 7U;
constexpr std::size_t kStorePath = 8U;
constexpr std::size_t kRecoveryMode = 9U;
constexpr std::size_t kDispatchMode = 10U;
constexpr std::size_t kHeartbeatIntervalSeconds = 11U;
constexpr std::size_t kIsInitiator = 12U;
constexpr std::size_t kDefaultApplVerId = 13U;
constexpr std::size_t kValidationMode = 14U;
constexpr std::size_t kDurableFlushThreshold = 15U;
constexpr std::size_t kDurableRolloverMode = 16U;
constexpr std::size_t kDurableArchiveLimit = 17U;
constexpr std::size_t kReconnectEnabled = 18U;
constexpr std::size_t kReconnectInitialMs = 19U;
constexpr std::size_t kReconnectMaxMs = 20U;
constexpr std::size_t kReconnectMaxRetries = 21U;
constexpr std::size_t kDurableLocalUtcOffsetSeconds = 22U;
constexpr std::size_t kDurableUseSystemTimezone = 23U;
constexpr std::size_t kDayCutMode = 24U;
constexpr std::size_t kDayCutHour = 25U;
constexpr std::size_t kDayCutMinute = 26U;
constexpr std::size_t kDayCutUtcOffset = 27U;
constexpr std::size_t kResetSeqNumOnLogon = 28U;
constexpr std::size_t kResetSeqNumOnLogout = 29U;
constexpr std::size_t kResetSeqNumOnDisconnect = 30U;
constexpr std::size_t kRefreshOnLogon = 31U;
constexpr std::size_t kSendNextExpectedMsgSeqNum = 32U;
constexpr std::size_t kUseLocalTime = 33U;
constexpr std::size_t kNonStopSession = 34U;
constexpr std::size_t kStartTime = 35U;
constexpr std::size_t kEndTime = 36U;
constexpr std::size_t kStartDay = 37U;
constexpr std::size_t kEndDay = 38U;
constexpr std::size_t kLogonTime = 39U;
constexpr std::size_t kLogoutTime = 40U;
constexpr std::size_t kLogonDay = 41U;
constexpr std::size_t kLogoutDay = 42U;
constexpr std::size_t kSendingTimeThresholdSeconds = 43U;
constexpr std::size_t kSupportedAppMsgTypes = 44U;
constexpr std::size_t kApplicationMessagesAvailable = 45U;
constexpr std::size_t kContractServiceSubsets = 46U;
constexpr std::size_t kTimestampResolution = 47U;
constexpr std::size_t kUnknownFieldAction = 48U;
constexpr std::size_t kMalformedFieldAction = 49U;
constexpr std::size_t kValidateEnumValues = 50U;
constexpr std::size_t kAlternateEndpoints = 51U;
constexpr std::size_t kWarmupMessageCount = 52U;
constexpr std::size_t kMinFieldCount = kIsInitiator + 1U;
constexpr std::size_t kMaxFieldCount = kWarmupMessageCount + 1U;
} // namespace counterparty_columns

auto
Trim(std::string_view input) -> std::string_view
{
  std::size_t begin = 0;
  while (begin < input.size() && std::isspace(static_cast<unsigned char>(input[begin])) != 0) {
    ++begin;
  }

  std::size_t end = input.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
    --end;
  }

  return input.substr(begin, end - begin);
}

auto
Split(std::string_view input, char delimiter) -> std::vector<std::string>
{
  std::vector<std::string> parts;
  std::size_t begin = 0;
  while (begin <= input.size()) {
    const auto end = input.find(delimiter, begin);
    if (end == std::string_view::npos) {
      parts.emplace_back(Trim(input.substr(begin)));
      break;
    }
    parts.emplace_back(Trim(input.substr(begin, end - begin)));
    begin = end + 1;
  }
  return parts;
}

auto
SplitCsvList(std::string_view input) -> std::vector<std::string>
{
  if (Trim(input).empty()) {
    return {};
  }
  return Split(input, kConfigListSeparator);
}

auto
ParseBool(std::string_view token) -> base::Result<bool>
{
  std::string value(token);
  std::transform(
    value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

  if (value == "true" || value == "1" || value == "yes" || value == "on") {
    return true;
  }
  if (value == "false" || value == "0" || value == "no" || value == "off") {
    return false;
  }
  return base::Status::InvalidArgument("invalid boolean value in runtime config");
}

auto
Lowercase(std::string_view token) -> std::string
{
  std::string value(token);
  std::transform(
    value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value;
}

template<typename Integer>
auto
ParseInteger(std::string_view token, const char* label) -> base::Result<Integer>
{
  try {
    if constexpr (std::is_signed_v<Integer>) {
      const auto value = std::stoll(std::string(token), nullptr, 0);
      if (value < std::numeric_limits<Integer>::min() || value > std::numeric_limits<Integer>::max()) {
        return base::Status::InvalidArgument(std::string(label) + " is out of range");
      }
      return static_cast<Integer>(value);
    } else {
      const auto value = std::stoull(std::string(token), nullptr, 0);
      if (value > std::numeric_limits<Integer>::max()) {
        return base::Status::InvalidArgument(std::string(label) + " is out of range");
      }
      return static_cast<Integer>(value);
    }
  } catch (...) {
    return base::Status::InvalidArgument(std::string("invalid ") + label + " value");
  }
}

auto
ParseTraceMode(std::string_view token) -> base::Result<TraceMode>
{
  const auto value = Trim(token);
  if (value == "disabled") {
    return TraceMode::kDisabled;
  }
  if (value == "ring") {
    return TraceMode::kRing;
  }
  return base::Status::InvalidArgument("unknown trace mode in runtime config");
}

auto
ParseQueueAppThreadingMode(std::string_view token) -> base::Result<QueueAppThreadingMode>
{
  const auto value = Trim(token);
  if (value.empty() || value == "co-scheduled" || value == "co_scheduled") {
    return QueueAppThreadingMode::kCoScheduled;
  }
  if (value == "threaded") {
    return QueueAppThreadingMode::kThreaded;
  }
  return base::Status::InvalidArgument("unknown queue_app_mode in runtime config");
}

auto
ParsePollMode(std::string_view token) -> base::Result<PollMode>
{
  const auto value = Trim(token);
  if (value.empty() || value == "blocking") {
    return PollMode::kBlocking;
  }
  if (value == "busy") {
    return PollMode::kBusy;
  }
  return base::Status::InvalidArgument("unknown poll_mode in runtime config");
}

auto
ParseIoBackend(std::string_view token) -> base::Result<IoBackend>
{
  const auto value = Trim(token);
  if (value.empty() || value == "epoll") {
    return IoBackend::kEpoll;
  }
  if (value == "poll") {
    return base::Status::InvalidArgument("poll backend has been removed, use epoll or io_uring");
  }
  if (value == "io_uring") {
    return IoBackend::kIoUring;
  }
  return base::Status::InvalidArgument("unknown io_backend in runtime config");
}

auto
ParseCpuAffinityList(std::string_view token) -> base::Result<std::vector<std::uint32_t>>
{
  std::vector<std::uint32_t> cpu_ids;
  const auto value = Trim(token);
  if (value.empty()) {
    return cpu_ids;
  }

  for (const auto& part : Split(value, kConfigListSeparator)) {
    auto cpu_id = ParseInteger<std::uint32_t>(part, "cpu_id");
    if (!cpu_id.ok()) {
      return cpu_id.status();
    }
    cpu_ids.push_back(cpu_id.value());
  }
  return cpu_ids;
}

auto
ParseEndpointList(std::string_view token) -> base::Result<std::vector<ConnectionEndpoint>>
{
  std::vector<ConnectionEndpoint> endpoints;
  const auto value = Trim(token);
  if (value.empty()) {
    return endpoints;
  }

  for (const auto& part : Split(value, kConfigListSeparator)) {
    const auto separator = part.rfind(':');
    if (separator == std::string::npos || separator == 0U || separator + 1U >= part.size()) {
      return base::Status::InvalidArgument("invalid alternate endpoint value in runtime config");
    }
    auto port = ParseInteger<std::uint16_t>(std::string_view(part).substr(separator + 1U), "alternate_endpoint_port");
    if (!port.ok()) {
      return port.status();
    }
    endpoints.push_back(ConnectionEndpoint{
      .host = part.substr(0U, separator),
      .port = port.value(),
    });
  }

  return endpoints;
}

auto
ParseStoreMode(std::string_view token) -> base::Result<StoreMode>
{
  const auto value = Trim(token);
  if (value == "memory") {
    return StoreMode::kMemory;
  }
  if (value == "mmap") {
    return StoreMode::kMmap;
  }
  if (value == "durable") {
    return StoreMode::kDurableBatch;
  }
  return base::Status::InvalidArgument("unknown store mode in runtime config");
}

auto
ParseDurableRolloverMode(std::string_view token) -> base::Result<store::DurableStoreRolloverMode>
{
  const auto value = Trim(token);
  if (value.empty() || value == "utc-day") {
    return store::DurableStoreRolloverMode::kUtcDay;
  }
  if (value == "disabled") {
    return store::DurableStoreRolloverMode::kDisabled;
  }
  if (value == "external") {
    return store::DurableStoreRolloverMode::kExternal;
  }
  if (value == "local-time") {
    return store::DurableStoreRolloverMode::kLocalTime;
  }
  return base::Status::InvalidArgument("unknown durable rollover mode in runtime config");
}

auto
ParseRecoveryMode(std::string_view token) -> base::Result<session::RecoveryMode>
{
  const auto value = Trim(token);
  if (value == "memory") {
    return session::RecoveryMode::kMemoryOnly;
  }
  if (value == "warm" || value == "warm-restart") {
    return session::RecoveryMode::kWarmRestart;
  }
  if (value == "cold" || value == "cold-start") {
    return session::RecoveryMode::kColdStart;
  }
  if (value == "no-recovery") {
    return session::RecoveryMode::kNoRecovery;
  }
  return base::Status::InvalidArgument("unknown recovery mode in runtime config");
}

auto
ParseDayCutMode(std::string_view token) -> base::Result<session::DayCutMode>
{
  const auto value = Trim(token);
  if (value.empty() || value == "no-auto-reset") {
    return session::DayCutMode::kNoAutoReset;
  }
  if (value == "fixed-local-time") {
    return session::DayCutMode::kFixedLocalTime;
  }
  if (value == "fixed-utc-time") {
    return session::DayCutMode::kFixedUtcTime;
  }
  if (value == "external-control") {
    return session::DayCutMode::kExternalControl;
  }
  return base::Status::InvalidArgument("unknown day_cut_mode in runtime config");
}

auto
ParseSessionTimeOfDay(std::string_view token) -> base::Result<SessionTimeOfDay>
{
  const auto value = Trim(token);
  const auto first_colon = value.find(':');
  if (first_colon == std::string_view::npos) {
    return base::Status::InvalidArgument("invalid session time value");
  }

  const auto second_colon = value.find(':', first_colon + 1U);
  if (second_colon == std::string_view::npos) {
    return base::Status::InvalidArgument("invalid session time value");
  }

  const auto parse_component = [](std::string_view field, const char* label) -> base::Result<std::uint32_t> {
    try {
      return static_cast<std::uint32_t>(std::stoul(std::string(field), nullptr, 10));
    } catch (...) {
      return base::Status::InvalidArgument(std::string("invalid ") + label + " value");
    }
  };

  auto hour = parse_component(value.substr(0, first_colon), "session_time_hour");
  if (!hour.ok()) {
    return hour.status();
  }
  auto minute = parse_component(value.substr(first_colon + 1U, second_colon - first_colon - 1U), "session_time_minute");
  if (!minute.ok()) {
    return minute.status();
  }
  auto second = parse_component(value.substr(second_colon + 1U), "session_time_second");
  if (!second.ok()) {
    return second.status();
  }
  if (hour.value() > 23U || minute.value() > 59U || second.value() > 59U) {
    return base::Status::InvalidArgument("session time value is out of range");
  }

  return SessionTimeOfDay{
    .hour = static_cast<std::uint8_t>(hour.value()),
    .minute = static_cast<std::uint8_t>(minute.value()),
    .second = static_cast<std::uint8_t>(second.value()),
  };
}

auto
ParseSessionDayOfWeek(std::string_view token) -> base::Result<SessionDayOfWeek>
{
  const auto value = Lowercase(Trim(token));
  if (value == "sun" || value == "sunday") {
    return SessionDayOfWeek::kSunday;
  }
  if (value == "mon" || value == "monday") {
    return SessionDayOfWeek::kMonday;
  }
  if (value == "tue" || value == "tuesday") {
    return SessionDayOfWeek::kTuesday;
  }
  if (value == "wed" || value == "wednesday") {
    return SessionDayOfWeek::kWednesday;
  }
  if (value == "thu" || value == "thursday") {
    return SessionDayOfWeek::kThursday;
  }
  if (value == "fri" || value == "friday") {
    return SessionDayOfWeek::kFriday;
  }
  if (value == "sat" || value == "saturday") {
    return SessionDayOfWeek::kSaturday;
  }
  return base::Status::InvalidArgument("invalid session day value");
}

auto
ParseDispatchMode(std::string_view token) -> base::Result<AppDispatchMode>
{
  const auto value = Trim(token);
  if (value == "inline") {
    return AppDispatchMode::kInline;
  }
  if (value == "queue") {
    return AppDispatchMode::kQueueDecoupled;
  }
  return base::Status::InvalidArgument("unknown dispatch mode in runtime config");
}

auto
ParseValidationMode(std::string_view token) -> base::Result<session::ValidationMode>
{
  const auto value = Trim(token);
  if (value.empty() || value == "strict") {
    return session::ValidationMode::kStrict;
  }
  if (value == "compatible") {
    return session::ValidationMode::kCompatible;
  }
  if (value == "permissive") {
    return session::ValidationMode::kPermissive;
  }
  if (value == "raw-pass-through") {
    return session::ValidationMode::kRawPassThrough;
  }
  return base::Status::InvalidArgument("unknown validation mode in runtime config");
}

auto
ParseUnknownFieldAction(std::string_view token) -> base::Result<session::UnknownFieldAction>
{
  const auto value = Lowercase(Trim(token));
  if (value == "reject") {
    return session::UnknownFieldAction::kReject;
  }
  if (value == "ignore") {
    return session::UnknownFieldAction::kIgnore;
  }
  if (value == "log-and-process" || value == "log_and_process") {
    return session::UnknownFieldAction::kLogAndProcess;
  }
  return base::Status::InvalidArgument("unknown unknown_field_action in runtime config");
}

auto
ParseMalformedFieldAction(std::string_view token) -> base::Result<session::MalformedFieldAction>
{
  const auto value = Lowercase(Trim(token));
  if (value == "reject") {
    return session::MalformedFieldAction::kReject;
  }
  if (value == "ignore") {
    return session::MalformedFieldAction::kIgnore;
  }
  if (value == "log") {
    return session::MalformedFieldAction::kLog;
  }
  return base::Status::InvalidArgument("unknown malformed_field_action in runtime config");
}

auto
ParseTimestampResolution(std::string_view token) -> base::Result<codec::TimestampResolution>
{
  const auto value = Lowercase(Trim(token));
  if (value.empty() || value == "milliseconds") {
    return codec::TimestampResolution::kMilliseconds;
  }
  if (value == "seconds") {
    return codec::TimestampResolution::kSeconds;
  }
  if (value == "microseconds") {
    return codec::TimestampResolution::kMicroseconds;
  }
  if (value == "nanoseconds") {
    return codec::TimestampResolution::kNanoseconds;
  }
  return base::Status::InvalidArgument("unknown timestamp_resolution in runtime config");
}

auto
ResolvePath(const std::filesystem::path& base_dir, std::string_view raw_path) -> std::filesystem::path
{
  if (raw_path.empty()) {
    return {};
  }

  std::filesystem::path path{ std::string(raw_path) };
  if (path.is_absolute()) {
    return path;
  }
  return base_dir / path;
}

auto
SplitTextLines(std::string_view text) -> std::vector<std::string>
{
  std::vector<std::string> lines;
  std::string current;
  current.reserve(text.size());
  for (const auto ch : text) {
    if (ch == '\r') {
      continue;
    }
    if (ch == '\n') {
      lines.push_back(current);
      current.clear();
      continue;
    }
    current.push_back(ch);
  }
  if (!current.empty() || text.empty()) {
    lines.push_back(std::move(current));
  }
  return lines;
}

auto
ParseOptionalBoolColumn(const std::vector<std::string>& parts, std::size_t index, bool default_value)
  -> base::Result<bool>
{
  if (parts.size() <= index || Trim(parts[index]).empty()) {
    return default_value;
  }
  return ParseBool(parts[index]);
}

auto
ParseOptionalTimeColumn(const std::vector<std::string>& parts, std::size_t index)
  -> base::Result<std::optional<SessionTimeOfDay>>
{
  if (parts.size() <= index || Trim(parts[index]).empty()) {
    return std::optional<SessionTimeOfDay>{};
  }
  auto parsed = ParseSessionTimeOfDay(parts[index]);
  if (!parsed.ok()) {
    return parsed.status();
  }
  return std::optional<SessionTimeOfDay>(parsed.value());
}

auto
ParseOptionalDayColumn(const std::vector<std::string>& parts, std::size_t index)
  -> base::Result<std::optional<SessionDayOfWeek>>
{
  if (parts.size() <= index || Trim(parts[index]).empty()) {
    return std::optional<SessionDayOfWeek>{};
  }
  auto parsed = ParseSessionDayOfWeek(parts[index]);
  if (!parsed.ok()) {
    return parsed.status();
  }
  return std::optional<SessionDayOfWeek>(parsed.value());
}

} // namespace

auto
LoadEngineConfigText(std::string_view text, const std::filesystem::path& base_dir) -> base::Result<EngineConfig>
{
  EngineConfig config;
  for (const auto& line : SplitTextLines(text)) {
    const auto trimmed = Trim(line);
    if (trimmed.empty() || trimmed.starts_with(kConfigCommentPrefix)) {
      continue;
    }

    if (trimmed.starts_with(kProfileRecordPrefix)) {
      config.profile_artifacts.push_back(ResolvePath(base_dir, Trim(trimmed.substr(kProfileRecordPrefix.size()))));
      continue;
    }

    if (trimmed.starts_with(kContractRecordPrefix)) {
      config.profile_contracts.push_back(ResolvePath(base_dir, Trim(trimmed.substr(kContractRecordPrefix.size()))));
      continue;
    }

    if (trimmed.starts_with(kDictionaryRecordPrefix)) {
      const auto raw = Trim(trimmed.substr(kDictionaryRecordPrefix.size()));
      std::vector<std::filesystem::path> paths;
      std::string_view remaining = raw;
      while (!remaining.empty()) {
        const auto comma = remaining.find(kConfigListSeparator);
        if (comma == std::string_view::npos) {
          paths.push_back(ResolvePath(base_dir, Trim(remaining)));
          break;
        }
        paths.push_back(ResolvePath(base_dir, Trim(remaining.substr(0, comma))));
        remaining = remaining.substr(comma + 1U);
      }
      if (!paths.empty()) {
        config.profile_dictionaries.push_back(std::move(paths));
      }
      continue;
    }

    if (trimmed.find(kConfigFieldSeparator) == std::string_view::npos) {
      const auto eq = trimmed.find(kConfigAssignmentSeparator);
      if (eq == std::string_view::npos) {
        return base::Status::InvalidArgument("invalid runtime config line");
      }

      const auto key = Trim(trimmed.substr(0, eq));
      const auto value = Trim(trimmed.substr(eq + 1));
      if (key == "engine.worker_count") {
        auto parsed = ParseInteger<std::uint32_t>(value, "worker_count");
        if (!parsed.ok()) {
          return parsed.status();
        }
        config.worker_count = parsed.value();
      } else if (key == "engine.enable_metrics") {
        auto parsed = ParseBool(value);
        if (!parsed.ok()) {
          return parsed.status();
        }
        config.enable_metrics = parsed.value();
      } else if (key == "engine.trace_mode") {
        auto parsed = ParseTraceMode(value);
        if (!parsed.ok()) {
          return parsed.status();
        }
        config.trace_mode = parsed.value();
      } else if (key == "engine.trace_capacity") {
        auto parsed = ParseInteger<std::uint32_t>(value, "trace_capacity");
        if (!parsed.ok()) {
          return parsed.status();
        }
        config.trace_capacity = parsed.value();
      } else if (key == "engine.front_door_cpu") {
        auto parsed = ParseInteger<std::uint32_t>(value, "front_door_cpu");
        if (!parsed.ok()) {
          return parsed.status();
        }
        config.front_door_cpu = parsed.value();
      } else if (key == "engine.worker_cpu_affinity") {
        auto parsed = ParseCpuAffinityList(value);
        if (!parsed.ok()) {
          return parsed.status();
        }
        config.worker_cpu_affinity = std::move(parsed).value();
      } else if (key == "engine.queue_app_mode") {
        auto parsed = ParseQueueAppThreadingMode(value);
        if (!parsed.ok()) {
          return parsed.status();
        }
        config.queue_app_mode = parsed.value();
      } else if (key == "engine.app_cpu_affinity") {
        auto parsed = ParseCpuAffinityList(value);
        if (!parsed.ok()) {
          return parsed.status();
        }
        config.app_cpu_affinity = std::move(parsed).value();
      } else if (key == "engine.accept_unknown_sessions") {
        auto parsed = ParseBool(value);
        if (!parsed.ok()) {
          return parsed.status();
        }
        config.accept_unknown_sessions = parsed.value();
      } else if (key == "engine.backlog_warn_threshold_ms") {
        auto parsed = ParseInteger<std::uint32_t>(value, "backlog_warn_threshold_ms");
        if (!parsed.ok()) {
          return parsed.status();
        }
        config.backlog_warn_threshold_ms = parsed.value();
      } else if (key == "engine.backlog_warn_throttle_ms") {
        auto parsed = ParseInteger<std::uint32_t>(value, "backlog_warn_throttle_ms");
        if (!parsed.ok()) {
          return parsed.status();
        }
        config.backlog_warn_throttle_ms = parsed.value();
      } else if (key == "engine.poll_mode") {
        auto parsed = ParsePollMode(value);
        if (!parsed.ok()) {
          return parsed.status();
        }
        config.poll_mode = parsed.value();
      } else if (key == "engine.io_backend") {
        auto parsed = ParseIoBackend(value);
        if (!parsed.ok()) {
          return parsed.status();
        }
        config.io_backend = parsed.value();
      } else if (key == "engine.profile_madvise") {
        auto parsed = ParseBool(value);
        if (!parsed.ok()) {
          return parsed.status();
        }
        config.profile_madvise = parsed.value();
      } else if (key == "engine.profile_mlock") {
        auto parsed = ParseBool(value);
        if (!parsed.ok()) {
          return parsed.status();
        }
        config.profile_mlock = parsed.value();
      } else {
        return base::Status::InvalidArgument("unknown engine config key");
      }
      continue;
    }

    const auto parts = Split(trimmed, kConfigFieldSeparator);
    if (parts.empty()) {
      continue;
    }

    if (parts[0] == kListenerRecordKind) {
      if (parts.size() != listener_columns::kCount) {
        return base::Status::InvalidArgument("listener config must have 5 pipe-separated parts");
      }

      auto port = ParseInteger<std::uint16_t>(parts[listener_columns::kPort], "listener port");
      if (!port.ok()) {
        return port.status();
      }
      auto worker_hint = ParseInteger<std::uint32_t>(parts[listener_columns::kWorkerHint], "listener worker_hint");
      if (!worker_hint.ok()) {
        return worker_hint.status();
      }

      config.listeners.push_back(ListenerConfig{
        .name = parts[listener_columns::kName],
        .host = parts[listener_columns::kHost],
        .port = port.value(),
        .worker_hint = worker_hint.value(),
      });
      continue;
    }

    if (parts[0] == kCounterpartyRecordKind) {
      if (parts.size() < counterparty_columns::kMinFieldCount || parts.size() > counterparty_columns::kMaxFieldCount) {
        return base::Status::InvalidArgument("counterparty config must have between 13 and 48 pipe-separated "
                                             "parts");
      }

      auto session_id = ParseInteger<std::uint64_t>(parts[counterparty_columns::kSessionId], "session_id");
      if (!session_id.ok()) {
        return session_id.status();
      }
      auto profile_id = ParseInteger<std::uint64_t>(parts[counterparty_columns::kProfileId], "profile_id");
      if (!profile_id.ok()) {
        return profile_id.status();
      }
      auto store_mode = ParseStoreMode(parts[counterparty_columns::kStoreMode]);
      if (!store_mode.ok()) {
        return store_mode.status();
      }
      auto recovery_mode = ParseRecoveryMode(parts[counterparty_columns::kRecoveryMode]);
      if (!recovery_mode.ok()) {
        return recovery_mode.status();
      }
      auto dispatch_mode = ParseDispatchMode(parts[counterparty_columns::kDispatchMode]);
      if (!dispatch_mode.ok()) {
        return dispatch_mode.status();
      }
      auto validation_mode = ParseValidationMode(parts.size() > counterparty_columns::kValidationMode
                                                   ? std::string_view(parts[counterparty_columns::kValidationMode])
                                                   : std::string_view{});
      if (!validation_mode.ok()) {
        return validation_mode.status();
      }
      auto durable_flush_threshold = base::Result<std::uint32_t>(0U);
      if (parts.size() > counterparty_columns::kDurableFlushThreshold) {
        durable_flush_threshold =
          ParseInteger<std::uint32_t>(parts[counterparty_columns::kDurableFlushThreshold], "durable_flush_threshold");
        if (!durable_flush_threshold.ok()) {
          return durable_flush_threshold.status();
        }
      }
      auto durable_rollover_mode =
        ParseDurableRolloverMode(parts.size() > counterparty_columns::kDurableRolloverMode
                                   ? std::string_view(parts[counterparty_columns::kDurableRolloverMode])
                                   : std::string_view{});
      if (!durable_rollover_mode.ok()) {
        return durable_rollover_mode.status();
      }
      auto durable_archive_limit = base::Result<std::uint32_t>(0U);
      if (parts.size() > counterparty_columns::kDurableArchiveLimit) {
        durable_archive_limit =
          ParseInteger<std::uint32_t>(parts[counterparty_columns::kDurableArchiveLimit], "durable_archive_limit");
        if (!durable_archive_limit.ok()) {
          return durable_archive_limit.status();
        }
      }
      auto reconnect_enabled = base::Result<bool>(false);
      if (parts.size() > counterparty_columns::kReconnectEnabled &&
          !Trim(parts[counterparty_columns::kReconnectEnabled]).empty()) {
        reconnect_enabled = ParseBool(parts[counterparty_columns::kReconnectEnabled]);
        if (!reconnect_enabled.ok()) {
          return reconnect_enabled.status();
        }
      }
      auto reconnect_initial_ms = base::Result<std::uint32_t>(kDefaultReconnectInitialMs);
      if (parts.size() > counterparty_columns::kReconnectInitialMs &&
          !Trim(parts[counterparty_columns::kReconnectInitialMs]).empty()) {
        reconnect_initial_ms =
          ParseInteger<std::uint32_t>(parts[counterparty_columns::kReconnectInitialMs], "reconnect_initial_ms");
        if (!reconnect_initial_ms.ok()) {
          return reconnect_initial_ms.status();
        }
      }
      auto reconnect_max_ms = base::Result<std::uint32_t>(kDefaultReconnectMaxMs);
      if (parts.size() > counterparty_columns::kReconnectMaxMs &&
          !Trim(parts[counterparty_columns::kReconnectMaxMs]).empty()) {
        reconnect_max_ms =
          ParseInteger<std::uint32_t>(parts[counterparty_columns::kReconnectMaxMs], "reconnect_max_ms");
        if (!reconnect_max_ms.ok()) {
          return reconnect_max_ms.status();
        }
      }
      auto reconnect_max_retries = base::Result<std::uint32_t>(kUnlimitedReconnectRetries);
      if (parts.size() > counterparty_columns::kReconnectMaxRetries &&
          !Trim(parts[counterparty_columns::kReconnectMaxRetries]).empty()) {
        reconnect_max_retries =
          ParseInteger<std::uint32_t>(parts[counterparty_columns::kReconnectMaxRetries], "reconnect_max_retries");
        if (!reconnect_max_retries.ok()) {
          return reconnect_max_retries.status();
        }
      }
      auto durable_local_utc_offset = base::Result<std::int32_t>(0);
      if (parts.size() > counterparty_columns::kDurableLocalUtcOffsetSeconds &&
          !Trim(parts[counterparty_columns::kDurableLocalUtcOffsetSeconds]).empty()) {
        auto raw = ParseInteger<std::int32_t>(parts[counterparty_columns::kDurableLocalUtcOffsetSeconds],
                                              "durable_local_utc_offset_seconds");
        if (!raw.ok()) {
          return raw.status();
        }
        durable_local_utc_offset = raw.value();
      }
      auto durable_use_system_tz = base::Result<bool>(true);
      if (parts.size() > counterparty_columns::kDurableUseSystemTimezone &&
          !Trim(parts[counterparty_columns::kDurableUseSystemTimezone]).empty()) {
        durable_use_system_tz = ParseBool(parts[counterparty_columns::kDurableUseSystemTimezone]);
        if (!durable_use_system_tz.ok()) {
          return durable_use_system_tz.status();
        }
      }
      auto day_cut_mode = base::Result<session::DayCutMode>(session::DayCutMode::kNoAutoReset);
      if (parts.size() > counterparty_columns::kDayCutMode && !Trim(parts[counterparty_columns::kDayCutMode]).empty()) {
        day_cut_mode = ParseDayCutMode(parts[counterparty_columns::kDayCutMode]);
        if (!day_cut_mode.ok()) {
          return day_cut_mode.status();
        }
      }
      auto day_cut_hour = base::Result<std::int32_t>(0);
      if (parts.size() > counterparty_columns::kDayCutHour && !Trim(parts[counterparty_columns::kDayCutHour]).empty()) {
        day_cut_hour = ParseInteger<std::int32_t>(parts[counterparty_columns::kDayCutHour], "day_cut_hour");
        if (!day_cut_hour.ok()) {
          return day_cut_hour.status();
        }
      }
      auto day_cut_minute = base::Result<std::int32_t>(0);
      if (parts.size() > counterparty_columns::kDayCutMinute &&
          !Trim(parts[counterparty_columns::kDayCutMinute]).empty()) {
        day_cut_minute = ParseInteger<std::int32_t>(parts[counterparty_columns::kDayCutMinute], "day_cut_minute");
        if (!day_cut_minute.ok()) {
          return day_cut_minute.status();
        }
      }
      auto day_cut_utc_offset = base::Result<std::int32_t>(0);
      if (parts.size() > counterparty_columns::kDayCutUtcOffset &&
          !Trim(parts[counterparty_columns::kDayCutUtcOffset]).empty()) {
        day_cut_utc_offset =
          ParseInteger<std::int32_t>(parts[counterparty_columns::kDayCutUtcOffset], "day_cut_utc_offset");
        if (!day_cut_utc_offset.ok()) {
          return day_cut_utc_offset.status();
        }
      }
      auto reset_seq_num_on_logon = ParseOptionalBoolColumn(parts, counterparty_columns::kResetSeqNumOnLogon, false);
      if (!reset_seq_num_on_logon.ok()) {
        return reset_seq_num_on_logon.status();
      }
      auto reset_seq_num_on_logout = ParseOptionalBoolColumn(parts, counterparty_columns::kResetSeqNumOnLogout, false);
      if (!reset_seq_num_on_logout.ok()) {
        return reset_seq_num_on_logout.status();
      }
      auto reset_seq_num_on_disconnect =
        ParseOptionalBoolColumn(parts, counterparty_columns::kResetSeqNumOnDisconnect, false);
      if (!reset_seq_num_on_disconnect.ok()) {
        return reset_seq_num_on_disconnect.status();
      }
      auto refresh_on_logon = ParseOptionalBoolColumn(parts, counterparty_columns::kRefreshOnLogon, false);
      if (!refresh_on_logon.ok()) {
        return refresh_on_logon.status();
      }
      auto send_next_expected_msg_seq_num =
        ParseOptionalBoolColumn(parts, counterparty_columns::kSendNextExpectedMsgSeqNum, false);
      if (!send_next_expected_msg_seq_num.ok()) {
        return send_next_expected_msg_seq_num.status();
      }
      auto use_local_time = ParseOptionalBoolColumn(parts, counterparty_columns::kUseLocalTime, false);
      if (!use_local_time.ok()) {
        return use_local_time.status();
      }
      auto non_stop_session = ParseOptionalBoolColumn(parts, counterparty_columns::kNonStopSession, false);
      if (!non_stop_session.ok()) {
        return non_stop_session.status();
      }
      auto start_time = ParseOptionalTimeColumn(parts, counterparty_columns::kStartTime);
      if (!start_time.ok()) {
        return start_time.status();
      }
      auto end_time = ParseOptionalTimeColumn(parts, counterparty_columns::kEndTime);
      if (!end_time.ok()) {
        return end_time.status();
      }
      auto start_day = ParseOptionalDayColumn(parts, counterparty_columns::kStartDay);
      if (!start_day.ok()) {
        return start_day.status();
      }
      auto end_day = ParseOptionalDayColumn(parts, counterparty_columns::kEndDay);
      if (!end_day.ok()) {
        return end_day.status();
      }
      auto logon_time = ParseOptionalTimeColumn(parts, counterparty_columns::kLogonTime);
      if (!logon_time.ok()) {
        return logon_time.status();
      }
      auto logout_time = ParseOptionalTimeColumn(parts, counterparty_columns::kLogoutTime);
      if (!logout_time.ok()) {
        return logout_time.status();
      }
      auto logon_day = ParseOptionalDayColumn(parts, counterparty_columns::kLogonDay);
      if (!logon_day.ok()) {
        return logon_day.status();
      }
      auto logout_day = ParseOptionalDayColumn(parts, counterparty_columns::kLogoutDay);
      if (!logout_day.ok()) {
        return logout_day.status();
      }
      std::uint32_t sending_time_threshold_seconds = 0U;
      if (parts.size() > counterparty_columns::kSendingTimeThresholdSeconds &&
          !Trim(parts[counterparty_columns::kSendingTimeThresholdSeconds]).empty()) {
        auto threshold = ParseInteger<std::uint32_t>(parts[counterparty_columns::kSendingTimeThresholdSeconds],
                                                     "sending_time_threshold_seconds");
        if (!threshold.ok()) {
          return threshold.status();
        }
        sending_time_threshold_seconds = threshold.value();
      }
      std::uint32_t warmup_message_count = 0U;
      if (parts.size() > counterparty_columns::kWarmupMessageCount &&
          !Trim(parts[counterparty_columns::kWarmupMessageCount]).empty()) {
        auto count =
          ParseInteger<std::uint32_t>(parts[counterparty_columns::kWarmupMessageCount], "warmup_message_count");
        if (!count.ok()) {
          return count.status();
        }
        warmup_message_count = count.value();
      }
      const auto supported_app_msg_types = parts.size() > counterparty_columns::kSupportedAppMsgTypes
                                             ? SplitCsvList(parts[counterparty_columns::kSupportedAppMsgTypes])
                                             : std::vector<std::string>{};
      auto application_messages_available =
        ParseOptionalBoolColumn(parts, counterparty_columns::kApplicationMessagesAvailable, true);
      if (!application_messages_available.ok()) {
        return application_messages_available.status();
      }
      const auto contract_service_subsets = parts.size() > counterparty_columns::kContractServiceSubsets
                                              ? SplitCsvList(parts[counterparty_columns::kContractServiceSubsets])
                                              : std::vector<std::string>{};
      auto timestamp_resolution =
        ParseTimestampResolution(parts.size() > counterparty_columns::kTimestampResolution
                                   ? std::string_view(parts[counterparty_columns::kTimestampResolution])
                                   : std::string_view{});
      if (!timestamp_resolution.ok()) {
        return timestamp_resolution.status();
      }
      auto validation_policy = session::MakeValidationPolicy(validation_mode.value());
      if (parts.size() > counterparty_columns::kUnknownFieldAction &&
          !Trim(parts[counterparty_columns::kUnknownFieldAction]).empty()) {
        auto unknown_field_action = ParseUnknownFieldAction(parts[counterparty_columns::kUnknownFieldAction]);
        if (!unknown_field_action.ok()) {
          return unknown_field_action.status();
        }
        validation_policy.unknown_field_action = unknown_field_action.value();
      }
      if (parts.size() > counterparty_columns::kMalformedFieldAction &&
          !Trim(parts[counterparty_columns::kMalformedFieldAction]).empty()) {
        auto malformed_field_action = ParseMalformedFieldAction(parts[counterparty_columns::kMalformedFieldAction]);
        if (!malformed_field_action.ok()) {
          return malformed_field_action.status();
        }
        validation_policy.malformed_field_action = malformed_field_action.value();
      }
      if (parts.size() > counterparty_columns::kValidateEnumValues &&
          !Trim(parts[counterparty_columns::kValidateEnumValues]).empty()) {
        auto validate_enum_values = ParseBool(parts[counterparty_columns::kValidateEnumValues]);
        if (!validate_enum_values.ok()) {
          return validate_enum_values.status();
        }
        validation_policy.validate_enum_values = validate_enum_values.value();
      }
      auto alternate_endpoints = base::Result<std::vector<ConnectionEndpoint>>(std::vector<ConnectionEndpoint>{});
      if (parts.size() > counterparty_columns::kAlternateEndpoints) {
        alternate_endpoints = ParseEndpointList(parts[counterparty_columns::kAlternateEndpoints]);
        if (!alternate_endpoints.ok()) {
          return alternate_endpoints.status();
        }
      }
      auto heartbeat = ParseInteger<std::uint32_t>(parts[counterparty_columns::kHeartbeatIntervalSeconds],
                                                   "heartbeat_interval_seconds");
      if (!heartbeat.ok()) {
        return heartbeat.status();
      }
      auto initiator = ParseBool(parts[counterparty_columns::kIsInitiator]);
      if (!initiator.ok()) {
        return initiator.status();
      }

      session::SessionConfig session;
      session.session_id = session_id.value();
      session.profile_id = profile_id.value();
      session.key.begin_string = parts[counterparty_columns::kBeginString];
      session.key.sender_comp_id = parts[counterparty_columns::kSenderCompId];
      session.key.target_comp_id = parts[counterparty_columns::kTargetCompId];
      if (parts.size() > counterparty_columns::kDefaultApplVerId) {
        session.default_appl_ver_id = parts[counterparty_columns::kDefaultApplVerId];
      }
      session.heartbeat_interval_seconds = heartbeat.value();
      session.is_initiator = initiator.value();

      config.counterparties.push_back(CounterpartyConfig{
        .name = parts[counterparty_columns::kName],
        .session = std::move(session),
        .transport_profile =
          session::TransportSessionProfile::FromBeginString(parts[counterparty_columns::kBeginString]),
        .store_path = ResolvePath(base_dir, parts[counterparty_columns::kStorePath]),
        .default_appl_ver_id = parts.size() > counterparty_columns::kDefaultApplVerId
                                 ? parts[counterparty_columns::kDefaultApplVerId]
                                 : std::string{},
        .supported_app_msg_types = supported_app_msg_types,
        .contract_service_subsets = contract_service_subsets,
        .sending_time_threshold_seconds = sending_time_threshold_seconds,
        .warmup_message_count = warmup_message_count,
        .timestamp_resolution = timestamp_resolution.value(),
        .application_messages_available = application_messages_available.value(),
        .store_mode = store_mode.value(),
        .durable_flush_threshold = durable_flush_threshold.value(),
        .durable_rollover_mode = durable_rollover_mode.value(),
        .durable_archive_limit = durable_archive_limit.value(),
        .durable_local_utc_offset_seconds = durable_local_utc_offset.value(),
        .durable_use_system_timezone = durable_use_system_tz.value(),
        .recovery_mode = recovery_mode.value(),
        .dispatch_mode = dispatch_mode.value(),
        .validation_policy = validation_policy,
        .reset_seq_num_on_logon = reset_seq_num_on_logon.value(),
        .reset_seq_num_on_logout = reset_seq_num_on_logout.value(),
        .reset_seq_num_on_disconnect = reset_seq_num_on_disconnect.value(),
        .refresh_on_logon = refresh_on_logon.value(),
        .send_next_expected_msg_seq_num = send_next_expected_msg_seq_num.value(),
        .session_schedule =
          SessionScheduleConfig{
            .use_local_time = use_local_time.value(),
            .non_stop_session = non_stop_session.value(),
            .start_time = start_time.value(),
            .end_time = end_time.value(),
            .start_day = start_day.value(),
            .end_day = end_day.value(),
            .logon_time = logon_time.value(),
            .logout_time = logout_time.value(),
            .logon_day = logon_day.value(),
            .logout_day = logout_day.value(),
          },
        .reconnect_enabled = reconnect_enabled.value(),
        .reconnect_initial_ms = reconnect_initial_ms.value(),
        .reconnect_max_ms = reconnect_max_ms.value(),
        .reconnect_max_retries = reconnect_max_retries.value(),
        .alternate_endpoints = alternate_endpoints.value(),
        .day_cut =
          session::DayCutConfig{
            .mode = day_cut_mode.value(),
            .reset_hour = day_cut_hour.value(),
            .reset_minute = day_cut_minute.value(),
            .utc_offset_seconds = day_cut_utc_offset.value(),
          },
      });
      continue;
    }

    return base::Status::InvalidArgument("unknown runtime config record kind");
  }

  auto validation = ValidateEngineConfig(config);
  if (!validation.ok()) {
    return validation;
  }
  return config;
}

auto
LoadEngineConfigFile(const std::filesystem::path& path) -> base::Result<EngineConfig>
{
  std::ifstream in(path);
  if (!in.is_open()) {
    return base::Status::IoError("unable to open runtime config: '" + path.string() + "'");
  }

  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return LoadEngineConfigText(text, path.parent_path());
}

} // namespace nimble::runtime
