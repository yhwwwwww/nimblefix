#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "fastfix/base/status.h"
#include "fastfix/codec/fix_codec.h"
#include "fastfix/profile/normalized_dictionary.h"
#include "fastfix/runtime/application.h"
#include "fastfix/runtime/config.h"
#include "fastfix/runtime/metrics.h"
#include "fastfix/runtime/profile_registry.h"
#include "fastfix/runtime/sharded_runtime.h"
#include "fastfix/runtime/trace.h"
#include "fastfix/session/session_key.h"

namespace fastfix::runtime {

inline constexpr std::uint64_t kFirstDynamicSessionId = 0x8000'0000'0000'0000ULL;

/// Callback invoked when an inbound Logon arrives from an unknown CompID.
/// Return a CounterpartyConfig to accept the session, or an error Status to reject.
using SessionFactory = std::function<base::Result<CounterpartyConfig>(
    const session::SessionKey& key)>;

/// A simple factory that accepts sessions matching a whitelist of CompID patterns.
class WhitelistSessionFactory {
  public:
    void Allow(std::string_view begin_string, std::string_view sender_comp_id,
               const CounterpartyConfig& config_template);
    void AllowAny(const CounterpartyConfig& config_template);

    auto operator()(const session::SessionKey& key) const
        -> base::Result<CounterpartyConfig>;

  private:
    struct Entry {
        std::string begin_string;
        std::string sender_comp_id;
        CounterpartyConfig config_template;
    };
    std::vector<Entry> entries_;
    std::optional<CounterpartyConfig> allow_any_template_;
};

struct ResolvedCounterparty {
    CounterpartyConfig counterparty;
    profile::NormalizedDictionaryView dictionary;
};

class Engine {
  public:
    auto LoadProfiles(const EngineConfig& config) -> base::Status;
    auto Boot(const EngineConfig& config) -> base::Status;
        auto EnsureManagedQueueRunnerStarted(
                const void* owner,
                ApplicationCallbacks* application,
                std::optional<ManagedQueueApplicationRunnerOptions>* options) -> base::Status;
        auto StopManagedQueueRunner(const void* owner) -> base::Status;
        auto ReleaseManagedQueueRunner(const void* owner) -> base::Status;
        auto PollManagedQueueWorkerOnce(const void* owner, std::uint32_t worker_id)
            -> base::Result<std::size_t>;

    [[nodiscard]] auto profiles() const -> const ProfileRegistry& {
        return profiles_;
    }

    [[nodiscard]] auto runtime() const -> const ShardedRuntime* {
        return runtime_.has_value() ? &*runtime_ : nullptr;
    }

    [[nodiscard]] auto mutable_runtime() -> ShardedRuntime* {
        return runtime_.has_value() ? &*runtime_ : nullptr;
    }

    [[nodiscard]] auto metrics() const -> const MetricsRegistry& {
        return metrics_;
    }

    [[nodiscard]] auto mutable_metrics() -> MetricsRegistry* {
        return &metrics_;
    }

    [[nodiscard]] auto trace() const -> const TraceRecorder& {
        return trace_;
    }

    [[nodiscard]] auto mutable_trace() -> TraceRecorder* {
        return &trace_;
    }

    [[nodiscard]] auto config() const -> const EngineConfig* {
        return config_.has_value() ? &*config_ : nullptr;
    }

    [[nodiscard]] auto FindCounterpartyConfig(std::uint64_t session_id) const -> const CounterpartyConfig*;
    [[nodiscard]] auto FindListenerConfig(std::string_view name) const -> const ListenerConfig*;
    auto LoadDictionaryView(std::uint64_t profile_id) const -> base::Result<profile::NormalizedDictionaryView>;
    auto ResolveInboundSession(const codec::SessionHeader& header) const -> base::Result<ResolvedCounterparty>;
    auto ResolveInboundSession(const codec::SessionHeaderView& header) const -> base::Result<ResolvedCounterparty>;

    void SetSessionFactory(SessionFactory factory);

  private:
        struct ManagedQueueRunnerSlot {
            ManagedQueueApplicationRunnerMode mode{ManagedQueueApplicationRunnerMode::kCoScheduled};
            QueueApplication* application{nullptr};
            std::vector<std::unique_ptr<QueueApplicationEventHandler>> handlers;
            QueueApplicationPollerOptions poller_options{};
                std::unique_ptr<QueueApplicationRunner> runner;
            bool active{false};
        };

    std::optional<EngineConfig> config_;
    std::optional<ShardedRuntime> runtime_;
    std::unordered_map<std::uint64_t, CounterpartyConfig> counterparties_;
    std::optional<SessionFactory> session_factory_;
        mutable std::mutex managed_queue_runner_mutex_;
        std::unordered_map<const void*, ManagedQueueRunnerSlot> managed_queue_runners_;
    ProfileRegistry profiles_;
    MetricsRegistry metrics_;
    TraceRecorder trace_;
        mutable std::atomic<std::uint64_t> next_dynamic_session_id_{kFirstDynamicSessionId};
};

}  // namespace fastfix::runtime
