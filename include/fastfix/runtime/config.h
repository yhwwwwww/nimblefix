#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fastfix/base/result.h"
#include "fastfix/base/status.h"
#include "fastfix/session/resend_recovery.h"
#include "fastfix/session/session_core.h"
#include "fastfix/session/validation_policy.h"
#include "fastfix/store/durable_batch_store.h"

namespace fastfix::runtime {

inline constexpr std::uint32_t kDefaultReconnectInitialMs = 1'000U;
inline constexpr std::uint32_t kDefaultReconnectMaxMs = 30'000U;
inline constexpr std::uint32_t kUnlimitedReconnectRetries = 0U;

enum class StoreMode : std::uint32_t {
    kMemory = 0,
    kMmap,
    kDurableBatch,
};

enum class AppDispatchMode : std::uint32_t {
    // Inline callbacks execute on the session owner runtime worker thread.
    kInline = 0,
    // Queue-decoupled mode keeps one worker-local queue per runtime worker.
    kQueueDecoupled,
};

enum class TraceMode : std::uint32_t {
    kDisabled = 0,
    kRing,
};

enum class QueueAppThreadingMode : std::uint32_t {
    // Queue-decoupled handlers drain on the owning FIX worker thread.
    kCoScheduled = 0,
    // Queue-decoupled handlers run on one paired application thread per worker.
    kThreaded,
};

enum class PollMode : std::uint32_t {
    kBlocking = 0,
    kBusy = 1,
};

struct ListenerConfig {
    std::string name;
    std::string host{"0.0.0.0"};
    std::uint16_t port{0};
    // Seeds accept-side routing into the worker pool; it does not create a dedicated listener thread.
    std::uint32_t worker_hint{0};
};

struct CounterpartyConfig {
    std::string name;
    session::SessionConfig session;
    std::filesystem::path store_path;
    std::string default_appl_ver_id;
    StoreMode store_mode{StoreMode::kMemory};
    std::uint32_t durable_flush_threshold{0};
    store::DurableStoreRolloverMode durable_rollover_mode{store::DurableStoreRolloverMode::kUtcDay};
    std::uint32_t durable_archive_limit{0};
    std::int32_t durable_local_utc_offset_seconds{0};
    bool durable_use_system_timezone{true};
    session::RecoveryMode recovery_mode{session::RecoveryMode::kMemoryOnly};
    AppDispatchMode dispatch_mode{AppDispatchMode::kInline};
    session::ValidationPolicy validation_policy{session::ValidationPolicy::Strict()};
    // Reconnect backoff (initiator only)
    bool reconnect_enabled = false;
    std::uint32_t reconnect_initial_ms = kDefaultReconnectInitialMs;
    std::uint32_t reconnect_max_ms = kDefaultReconnectMaxMs;
    std::uint32_t reconnect_max_retries = kUnlimitedReconnectRetries;  // 0 = unlimited
};

struct EngineConfig {
    std::uint32_t worker_count{1};
    bool enable_metrics{true};
    TraceMode trace_mode{TraceMode::kDisabled};
    std::uint32_t trace_capacity{0};
    std::optional<std::uint32_t> front_door_cpu;
    std::vector<std::uint32_t> worker_cpu_affinity;
    QueueAppThreadingMode queue_app_mode{QueueAppThreadingMode::kCoScheduled};
    PollMode poll_mode{PollMode::kBlocking};
    std::vector<std::uint32_t> app_cpu_affinity;
    std::vector<std::filesystem::path> profile_artifacts;
    std::vector<std::vector<std::filesystem::path>> profile_dictionaries;
    bool profile_madvise{false};
    bool profile_mlock{false};
    std::vector<ListenerConfig> listeners;
    std::vector<CounterpartyConfig> counterparties;
    bool accept_unknown_sessions{false};
};

auto ValidateEngineConfig(const EngineConfig& config) -> base::Status;

}  // namespace fastfix::runtime