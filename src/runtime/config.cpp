#include "fastfix/runtime/config.h"
#include "fastfix/runtime/config_io.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <string_view>
#include <unordered_set>

namespace fastfix::runtime {

namespace {

constexpr char kConfigCommentPrefix = '#';
constexpr char kConfigAssignmentSeparator = '=';
constexpr char kConfigFieldSeparator = '|';
constexpr char kConfigListSeparator = ',';

constexpr std::string_view kProfileRecordPrefix = "profile=";
constexpr std::string_view kDictionaryRecordPrefix = "dictionary=";
constexpr std::string_view kListenerRecordKind = "listener";
constexpr std::string_view kCounterpartyRecordKind = "counterparty";

namespace listener_columns {
constexpr std::size_t kName = 1U;
constexpr std::size_t kHost = 2U;
constexpr std::size_t kPort = 3U;
constexpr std::size_t kWorkerHint = 4U;
constexpr std::size_t kCount = 5U;
}  // namespace listener_columns

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
constexpr std::size_t kMinFieldCount = kIsInitiator + 1U;
constexpr std::size_t kMaxFieldCount = kDurableUseSystemTimezone + 1U;
}  // namespace counterparty_columns

auto Trim(std::string_view input) -> std::string_view {
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

auto Split(std::string_view input, char delimiter) -> std::vector<std::string> {
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

auto ParseBool(std::string_view token) -> base::Result<bool> {
    std::string value(token);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    if (value == "true" || value == "1" || value == "yes" || value == "on") {
        return true;
    }
    if (value == "false" || value == "0" || value == "no" || value == "off") {
        return false;
    }
    return base::Status::InvalidArgument("invalid boolean value in runtime config");
}

template <typename Integer>
auto ParseInteger(std::string_view token, const char* label) -> base::Result<Integer> {
    try {
        const auto value = std::stoull(std::string(token), nullptr, 0);
        if (value > std::numeric_limits<Integer>::max()) {
            return base::Status::InvalidArgument(std::string(label) + " is out of range");
        }
        return static_cast<Integer>(value);
    } catch (...) {
        return base::Status::InvalidArgument(std::string("invalid ") + label + " value");
    }
}

auto ParseTraceMode(std::string_view token) -> base::Result<TraceMode> {
    const auto value = Trim(token);
    if (value == "disabled") {
        return TraceMode::kDisabled;
    }
    if (value == "ring") {
        return TraceMode::kRing;
    }
    return base::Status::InvalidArgument("unknown trace mode in runtime config");
}

auto ParseQueueAppThreadingMode(std::string_view token) -> base::Result<QueueAppThreadingMode> {
    const auto value = Trim(token);
    if (value.empty() || value == "co-scheduled" || value == "co_scheduled") {
        return QueueAppThreadingMode::kCoScheduled;
    }
    if (value == "threaded") {
        return QueueAppThreadingMode::kThreaded;
    }
    return base::Status::InvalidArgument("unknown queue_app_mode in runtime config");
}

auto ParsePollMode(std::string_view token) -> base::Result<PollMode> {
    const auto value = Trim(token);
    if (value.empty() || value == "blocking") {
        return PollMode::kBlocking;
    }
    if (value == "busy") {
        return PollMode::kBusy;
    }
    return base::Status::InvalidArgument("unknown poll_mode in runtime config");
}

auto ParseIoBackend(std::string_view token) -> base::Result<IoBackend> {
    const auto value = Trim(token);
    if (value.empty() || value == "poll") {
        return IoBackend::kPoll;
    }
    if (value == "epoll") {
        return IoBackend::kEpoll;
    }
    if (value == "io_uring") {
        return IoBackend::kIoUring;
    }
    return base::Status::InvalidArgument("unknown io_backend in runtime config");
}

auto ParseCpuAffinityList(std::string_view token) -> base::Result<std::vector<std::uint32_t>> {
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

auto ParseStoreMode(std::string_view token) -> base::Result<StoreMode> {
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

auto ParseDurableRolloverMode(std::string_view token) -> base::Result<store::DurableStoreRolloverMode> {
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

auto ParseRecoveryMode(std::string_view token) -> base::Result<session::RecoveryMode> {
    const auto value = Trim(token);
    if (value == "memory") {
        return session::RecoveryMode::kMemoryOnly;
    }
    if (value == "warm") {
        return session::RecoveryMode::kWarmRestart;
    }
    if (value == "cold") {
        return session::RecoveryMode::kColdStart;
    }
    return base::Status::InvalidArgument("unknown recovery mode in runtime config");
}

auto ParseDispatchMode(std::string_view token) -> base::Result<AppDispatchMode> {
    const auto value = Trim(token);
    if (value == "inline") {
        return AppDispatchMode::kInline;
    }
    if (value == "queue") {
        return AppDispatchMode::kQueueDecoupled;
    }
    return base::Status::InvalidArgument("unknown dispatch mode in runtime config");
}

auto ParseValidationMode(std::string_view token) -> base::Result<session::ValidationMode> {
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

auto ResolvePath(const std::filesystem::path& base_dir, std::string_view raw_path) -> std::filesystem::path {
    if (raw_path.empty()) {
        return {};
    }

    std::filesystem::path path{std::string(raw_path)};
    if (path.is_absolute()) {
        return path;
    }
    return base_dir / path;
}

auto SplitTextLines(std::string_view text) -> std::vector<std::string> {
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

auto IsTransportSession(std::string_view begin_string) -> bool {
    return begin_string == "FIXT.1.1";
}

}  // namespace

auto ValidateEngineConfig(const EngineConfig& config) -> base::Status {
    if (config.worker_count == 0) {
        return base::Status::InvalidArgument("engine worker_count must be positive");
    }
    if (config.trace_mode == TraceMode::kRing && config.trace_capacity == 0) {
        return base::Status::InvalidArgument("ring trace mode requires a positive trace_capacity");
    }
    if (config.worker_cpu_affinity.size() > config.worker_count) {
        return base::Status::InvalidArgument(
            "worker_cpu_affinity must not contain more entries than worker_count");
    }
    if (config.app_cpu_affinity.size() > config.worker_count) {
        return base::Status::InvalidArgument(
            "app_cpu_affinity must not contain more entries than worker_count");
    }
    if (config.queue_app_mode == QueueAppThreadingMode::kCoScheduled && !config.app_cpu_affinity.empty()) {
        return base::Status::InvalidArgument(
            "app_cpu_affinity requires engine.queue_app_mode=threaded");
    }
    if (!config.counterparties.empty() && config.profile_artifacts.empty() && config.profile_dictionaries.empty()) {
        return base::Status::InvalidArgument("counterparty configs require at least one profile artifact");
    }

    std::unordered_set<std::string> listener_names;
    for (const auto& listener : config.listeners) {
        if (listener.name.empty()) {
            return base::Status::InvalidArgument("listener name must not be empty");
        }
        if (!listener_names.emplace(listener.name).second) {
            return base::Status::AlreadyExists("duplicate listener name in runtime config");
        }
        if (listener.worker_hint >= config.worker_count) {
            return base::Status::InvalidArgument("listener worker_hint must be less than worker_count");
        }
    }

    std::unordered_set<std::uint64_t> session_ids;
    for (const auto& counterparty : config.counterparties) {
        if (counterparty.name.empty()) {
            return base::Status::InvalidArgument("counterparty name must not be empty");
        }
        if (counterparty.session.session_id == 0) {
            return base::Status::InvalidArgument("counterparty session_id must be positive");
        }
        if (!session_ids.emplace(counterparty.session.session_id).second) {
            return base::Status::AlreadyExists("duplicate session_id in runtime config");
        }
        if (counterparty.session.profile_id == 0) {
            return base::Status::InvalidArgument("counterparty profile_id must be positive");
        }
        if (counterparty.session.key.begin_string.empty() ||
            counterparty.session.key.sender_comp_id.empty() ||
            counterparty.session.key.target_comp_id.empty()) {
            return base::Status::InvalidArgument("counterparty session key fields must not be empty");
        }
        if (IsTransportSession(counterparty.session.key.begin_string) && counterparty.default_appl_ver_id.empty()) {
            return base::Status::InvalidArgument("FIXT counterparties require default_appl_ver_id");
        }
        if ((counterparty.store_mode == StoreMode::kMmap ||
             counterparty.store_mode == StoreMode::kDurableBatch) &&
            counterparty.store_path.empty()) {
            return base::Status::InvalidArgument("persistent store modes require a store_path");
        }
        if (counterparty.recovery_mode == session::RecoveryMode::kWarmRestart &&
            counterparty.store_mode == StoreMode::kMemory) {
            return base::Status::InvalidArgument("warm restart recovery requires a persistent store mode");
        }
    }

    return base::Status::Ok();
}

auto LoadEngineConfigText(std::string_view text, const std::filesystem::path& base_dir)
    -> base::Result<EngineConfig> {
    EngineConfig config;
    for (const auto& line : SplitTextLines(text)) {
        const auto trimmed = Trim(line);
        if (trimmed.empty() || trimmed.starts_with(kConfigCommentPrefix)) {
            continue;
        }

        if (trimmed.starts_with(kProfileRecordPrefix)) {
            config.profile_artifacts.push_back(
                ResolvePath(base_dir, Trim(trimmed.substr(kProfileRecordPrefix.size()))));
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
            if (parts.size() < counterparty_columns::kMinFieldCount ||
                parts.size() > counterparty_columns::kMaxFieldCount) {
                return base::Status::InvalidArgument(
                    "counterparty config must have between 13 and 24 pipe-separated parts");
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
            auto validation_mode = ParseValidationMode(
                parts.size() > counterparty_columns::kValidationMode
                    ? std::string_view(parts[counterparty_columns::kValidationMode])
                    : std::string_view{});
            if (!validation_mode.ok()) {
                return validation_mode.status();
            }
            auto durable_flush_threshold = base::Result<std::uint32_t>(0U);
            if (parts.size() > counterparty_columns::kDurableFlushThreshold) {
                durable_flush_threshold = ParseInteger<std::uint32_t>(
                    parts[counterparty_columns::kDurableFlushThreshold],
                    "durable_flush_threshold");
                if (!durable_flush_threshold.ok()) {
                    return durable_flush_threshold.status();
                }
            }
            auto durable_rollover_mode = ParseDurableRolloverMode(
                parts.size() > counterparty_columns::kDurableRolloverMode
                    ? std::string_view(parts[counterparty_columns::kDurableRolloverMode])
                    : std::string_view{});
            if (!durable_rollover_mode.ok()) {
                return durable_rollover_mode.status();
            }
            auto durable_archive_limit = base::Result<std::uint32_t>(0U);
            if (parts.size() > counterparty_columns::kDurableArchiveLimit) {
                durable_archive_limit = ParseInteger<std::uint32_t>(
                    parts[counterparty_columns::kDurableArchiveLimit],
                    "durable_archive_limit");
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
                reconnect_initial_ms = ParseInteger<std::uint32_t>(
                    parts[counterparty_columns::kReconnectInitialMs],
                    "reconnect_initial_ms");
                if (!reconnect_initial_ms.ok()) {
                    return reconnect_initial_ms.status();
                }
            }
            auto reconnect_max_ms = base::Result<std::uint32_t>(kDefaultReconnectMaxMs);
            if (parts.size() > counterparty_columns::kReconnectMaxMs &&
                !Trim(parts[counterparty_columns::kReconnectMaxMs]).empty()) {
                reconnect_max_ms = ParseInteger<std::uint32_t>(
                    parts[counterparty_columns::kReconnectMaxMs],
                    "reconnect_max_ms");
                if (!reconnect_max_ms.ok()) {
                    return reconnect_max_ms.status();
                }
            }
            auto reconnect_max_retries = base::Result<std::uint32_t>(kUnlimitedReconnectRetries);
            if (parts.size() > counterparty_columns::kReconnectMaxRetries &&
                !Trim(parts[counterparty_columns::kReconnectMaxRetries]).empty()) {
                reconnect_max_retries = ParseInteger<std::uint32_t>(
                    parts[counterparty_columns::kReconnectMaxRetries],
                    "reconnect_max_retries");
                if (!reconnect_max_retries.ok()) {
                    return reconnect_max_retries.status();
                }
            }
            auto durable_local_utc_offset = base::Result<std::int32_t>(0);
            if (parts.size() > counterparty_columns::kDurableLocalUtcOffsetSeconds &&
                !Trim(parts[counterparty_columns::kDurableLocalUtcOffsetSeconds]).empty()) {
                auto raw = ParseInteger<std::uint32_t>(
                    parts[counterparty_columns::kDurableLocalUtcOffsetSeconds],
                    "durable_local_utc_offset_seconds");
                if (!raw.ok()) {
                    return raw.status();
                }
                durable_local_utc_offset = static_cast<std::int32_t>(raw.value());
            }
            auto durable_use_system_tz = base::Result<bool>(true);
            if (parts.size() > counterparty_columns::kDurableUseSystemTimezone &&
                !Trim(parts[counterparty_columns::kDurableUseSystemTimezone]).empty()) {
                durable_use_system_tz = ParseBool(parts[counterparty_columns::kDurableUseSystemTimezone]);
                if (!durable_use_system_tz.ok()) {
                    return durable_use_system_tz.status();
                }
            }
            auto heartbeat = ParseInteger<std::uint32_t>(
                parts[counterparty_columns::kHeartbeatIntervalSeconds],
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
                .store_path = ResolvePath(base_dir, parts[counterparty_columns::kStorePath]),
                .default_appl_ver_id = parts.size() > counterparty_columns::kDefaultApplVerId
                                           ? parts[counterparty_columns::kDefaultApplVerId]
                                           : std::string{},
                .store_mode = store_mode.value(),
                .durable_flush_threshold = durable_flush_threshold.value(),
                .durable_rollover_mode = durable_rollover_mode.value(),
                .durable_archive_limit = durable_archive_limit.value(),
                .durable_local_utc_offset_seconds = durable_local_utc_offset.value(),
                .durable_use_system_timezone = durable_use_system_tz.value(),
                .recovery_mode = recovery_mode.value(),
                .dispatch_mode = dispatch_mode.value(),
                .validation_policy = session::MakeValidationPolicy(validation_mode.value()),
                .reconnect_enabled = reconnect_enabled.value(),
                .reconnect_initial_ms = reconnect_initial_ms.value(),
                .reconnect_max_ms = reconnect_max_ms.value(),
                .reconnect_max_retries = reconnect_max_retries.value(),
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

auto LoadEngineConfigFile(const std::filesystem::path& path) -> base::Result<EngineConfig> {
    std::ifstream in(path);
    if (!in.is_open()) {
        return base::Status::IoError("unable to open runtime config: '" + path.string() + "'");
    }

    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return LoadEngineConfigText(text, path.parent_path());
}

}  // namespace fastfix::runtime