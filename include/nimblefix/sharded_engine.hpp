#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nimblefix/engine.hpp>

namespace nimblefix {

struct ShardedEngineConfig {
    EngineConfig base{};
    // Number of independent IoBackend reactor shards. 0 = auto (hardware_concurrency, clamped to [1, 64]).
    std::size_t shard_count{0};
    // Optional per-shard CPU core affinity list (e.g., {0, 1, 2, 3}). Empty disables core pinning;
    // if shorter than shard_count, remaining shards stay unpinned.
    std::vector<int> cpu_cores{};
};

// Multi-core Sharded Reactor FIX Engine.
// Runs K completely independent IoBackend reactor loops (epoll / busy_poll / io_uring) with
// static session-to-shard pinning, preserving strict single-writer lock-free SessionCore and
// segmented mmap Journal ownership within every shard.
//
// Shared Acceptor listeners bind once on Shard 0; incoming TCP/TLS connections perform Logon
// routing on Shard 0 and hand off the accepted socket directly to the owning shard's reactor
// thread so 100% of steady-state order processing runs lock-free on that shard's pinned CPU core.
template <class Schema = FlatSchema, class Policy = Inline>
class ShardedEngine {
    static_assert(std::is_same_v<Schema, FlatSchema> || requires {
        { Schema::descriptor() } -> std::same_as<const SchemaDescriptor&>;
        { Schema::find_message(std::string_view{}) } -> std::same_as<const MessageDescriptor*>;
        { Schema::wire_hints() } -> std::same_as<WireHints>;
    }, "typed ShardedEngine Schema must expose descriptor(), find_message(), and wire_hints()");
    static_assert(std::is_same_v<Policy, Inline> || std::is_same_v<Policy, Queued>,
        "ShardedEngine Policy must be nimblefix::Inline or nimblefix::Queued");

public:
    static constexpr std::size_t auto_shard = std::numeric_limits<std::size_t>::max();
    static constexpr std::size_t max_shards = 64;

    ShardedEngine(const ShardedEngine&) = delete;
    ShardedEngine& operator=(const ShardedEngine&) = delete;
    ShardedEngine(ShardedEngine&&) noexcept = default;
    ShardedEngine& operator=(ShardedEngine&&) noexcept = default;

    [[nodiscard]] static Result<ShardedEngine> create(ShardedEngineConfig config) noexcept {
        try {
            std::size_t count = config.shard_count;
            if (count == 0) {
                const unsigned hw = std::thread::hardware_concurrency();
                count = hw == 0 ? 1U : std::min<std::size_t>(hw, max_shards);
            }
            if (count == 0 || count > max_shards) return Status{ErrorCode::invalid_config};
            if (config.cpu_cores.size() > max_shards) return Status{ErrorCode::invalid_config};

            const SchemaDescriptor* descriptor = nullptr;
            WireHints schema_wire_hints{};
            if constexpr (!std::is_same_v<Schema, FlatSchema>) {
                descriptor = &Schema::descriptor();
                schema_wire_hints = Schema::wire_hints();
            }

            std::shared_ptr<TelemetryWriter> shared_tel = config.base.shared_telemetry;
            if (!shared_tel && !config.base.telemetry_path.empty()) {
                const std::size_t cap = std::min<std::size_t>(1024, std::max<std::size_t>(64, config.base.max_sessions * count));
                auto created_tel = TelemetryWriter::create(config.base.telemetry_path, cap);
                if (!created_tel.ok()) return created_tel.status();
                shared_tel = std::move(created_tel).value();
            }

            std::vector<std::unique_ptr<detail::EngineRuntime>> shards;
            shards.reserve(count);
            for (std::size_t i = 0; i < count; ++i) {
                EngineConfig shard_cfg = config.base;
                shard_cfg.shared_telemetry = shared_tel;
                shard_cfg.telemetry_path.clear();
                if (i < config.cpu_cores.size()) {
                    shard_cfg.cpu_core = config.cpu_cores[i];
                } else {
                    shard_cfg.cpu_core = -1;
                }
                auto created = detail::EngineRuntime::create(std::move(shard_cfg),
                    std::is_same_v<Policy, Queued>, descriptor, schema_wire_hints);
                if (!created.ok()) return created.status();
                shards.push_back(std::move(created).value());
            }
            return ShardedEngine{std::move(shards)};
        } catch (...) {
            return Status{ErrorCode::capacity_exceeded};
        }
    }

    [[nodiscard]] std::size_t shard_count() const noexcept { return shards_.size(); }

    [[nodiscard]] std::size_t session_shard(std::string_view session_name) const noexcept {
        const auto it = session_to_shard_.find(std::string{session_name});
        return it == session_to_shard_.end() ? auto_shard : it->second;
    }

    // Callback registrations are replicated across all shards. Individual sessions are strictly
    // single-threaded on their pinned shard; shared state touched across different sessions must
    // be synchronized or thread_local.
    template <class Callback>
        requires std::is_same_v<Schema, FlatSchema> &&
            std::is_invocable_r_v<Status, Callback&, SessionContext&, const FlatMessageView&>
    [[nodiscard]] Status on_message(Callback&& callback) noexcept {
        try {
            if (!detail::callback_is_present(callback)) return {ErrorCode::invalid_config};
            auto fn = std::forward<Callback>(callback);
            for (auto& shard : shards_) {
                const auto st = shard->on_message(
                    [cb = fn](SessionContext& ctx, const FlatMessageView& msg, const DeliveryInfo&) mutable {
                        return std::invoke(cb, ctx, msg);
                    });
                if (!st.ok()) return st;
            }
            return Status::success();
        } catch (...) {
            return Status{ErrorCode::capacity_exceeded};
        }
    }

    template <class Callback>
        requires std::is_same_v<Schema, FlatSchema> &&
            std::is_invocable_r_v<Status, Callback&, SessionContext&,
                const FlatMessageView&, const DeliveryInfo&>
    [[nodiscard]] Status on_message(Callback&& callback) noexcept {
        try {
            if (!detail::callback_is_present(callback)) return {ErrorCode::invalid_config};
            auto fn = std::forward<Callback>(callback);
            for (auto& shard : shards_) {
                const auto st = shard->on_message(fn);
                if (!st.ok()) return st;
            }
            return Status::success();
        } catch (...) {
            return Status{ErrorCode::capacity_exceeded};
        }
    }

    template <class Message, class Callback>
        requires (!std::is_same_v<Schema, FlatSchema>) &&
            requires { typename Message::schema_type; typename Message::view_type; } &&
            std::is_same_v<Schema, typename Message::schema_type> &&
            (std::is_invocable_r_v<Status, Callback&, SessionContext&, typename Message::view_type> ||
             std::is_invocable_r_v<Status, Callback&, SessionContext&, const typename Message::view_type&> ||
             std::is_invocable_r_v<Status, Callback&, SessionContext&,
                typename Message::view_type, const DeliveryInfo&> ||
             std::is_invocable_r_v<Status, Callback&, SessionContext&,
                const typename Message::view_type&, const DeliveryInfo&>)
    [[nodiscard]] Status on(Callback&& callback) noexcept {
        try {
            if (!detail::callback_is_present(callback)) return {ErrorCode::invalid_config};
            if (Schema::find_message(Message::msg_type) == nullptr)
                return {ErrorCode::schema_mismatch, tag::msg_type};
            using View = typename Message::view_type;
            auto fn = std::forward<Callback>(callback);
            for (auto& shard : shards_) {
                const auto st = shard->on_typed_message(Message::msg_type,
                    [cb = fn](SessionContext& ctx, const FlatMessageView& flat,
                              const DeliveryInfo& info) mutable -> Status {
                        View view{flat};
                        if constexpr (std::is_invocable_r_v<Status, Callback&,
                                          SessionContext&, View, const DeliveryInfo&>)
                            return std::invoke(cb, ctx, std::move(view), info);
                        else if constexpr (std::is_invocable_r_v<Status, Callback&,
                                               SessionContext&, const View&, const DeliveryInfo&>)
                            return std::invoke(cb, ctx, std::as_const(view), info);
                        else if constexpr (std::is_invocable_r_v<Status, Callback&, SessionContext&, View>)
                            return std::invoke(cb, ctx, std::move(view));
                        else
                            return std::invoke(cb, ctx, std::as_const(view));
                    });
                if (!st.ok()) return st;
            }
            return Status::success();
        } catch (...) {
            return Status{ErrorCode::capacity_exceeded};
        }
    }

    template <class Message>
        requires (!std::is_same_v<Schema, FlatSchema>) &&
            requires { typename Message::schema_type; } &&
            std::is_same_v<Schema, typename Message::schema_type>
    [[nodiscard]] Status ignore(std::string_view reason) noexcept {
        if (Schema::find_message(Message::msg_type) == nullptr)
            return {ErrorCode::schema_mismatch, tag::msg_type};
        for (auto& shard : shards_) {
            if (const auto st = shard->ignore_message(Message::msg_type, reason); !st.ok()) return st;
        }
        return Status::success();
    }

    [[nodiscard]] Status ignore_message(std::string_view msg_type,
        std::string_view reason) noexcept requires std::is_same_v<Schema, FlatSchema> {
        for (auto& shard : shards_) {
            if (const auto st = shard->ignore_message(msg_type, reason); !st.ok()) return st;
        }
        return Status::success();
    }

    template <class Callback>
        requires std::is_invocable_r_v<Status, Callback&, SessionContext&>
    [[nodiscard]] Status on_session_active(Callback&& callback) noexcept {
        try {
            if (!detail::callback_is_present(callback)) return {ErrorCode::invalid_config};
            auto fn = std::forward<Callback>(callback);
            for (auto& shard : shards_) {
                if (const auto st = shard->on_session_active(fn); !st.ok()) return st;
            }
            return Status::success();
        } catch (...) {
            return Status{ErrorCode::capacity_exceeded};
        }
    }

    template <class Callback>
        requires std::is_invocable_r_v<void, Callback&, const ErrorEvent&>
    [[nodiscard]] Status on_error(Callback&& callback) noexcept {
        try {
            if (!detail::callback_is_present(callback)) return {ErrorCode::invalid_config};
            auto fn = std::forward<Callback>(callback);
            for (auto& shard : shards_) {
                if (const auto st = shard->on_error(fn); !st.ok()) return st;
            }
            return Status::success();
        } catch (...) {
            return Status{ErrorCode::capacity_exceeded};
        }
    }

    // Bind a shared listening socket on Shard 0 and register handoff listeners on Shards 1..K-1.
    [[nodiscard]] Result<ListenerHandle> add_listener(ListenerConfig config) noexcept {
        if (shards_.empty()) return Status{ErrorCode::invalid_config};
        try {
            auto primary = shards_[0]->add_listener(config);
            if (!primary.ok()) return primary.status();
            for (std::size_t i = 1; i < shards_.size(); ++i) {
                const auto handoff = shards_[i]->add_handoff_listener(config);
                if (!handoff.ok()) return handoff.status();
            }
            return primary;
        } catch (...) {
            return Status{ErrorCode::capacity_exceeded};
        }
    }

    [[nodiscard]] Result<SessionHandle> add_initiator(InitiatorConfig config,
        std::size_t shard_index = auto_shard) noexcept {
        if (shards_.empty()) return Status{ErrorCode::invalid_config};
        const std::size_t target = (shard_index == auto_shard)
            ? (next_rr_shard_++ % shards_.size()) : shard_index;
        if (target >= shards_.size()) return Status{ErrorCode::invalid_config};
        try {
            if (session_to_shard_.contains(config.name)) return Status{ErrorCode::invalid_config};
            const std::string name = config.name;
            auto handle = shards_[target]->add_initiator(std::move(config));
            if (!handle.ok()) return handle.status();
            session_to_shard_.emplace(name, target);
            return handle;
        } catch (...) {
            return Status{ErrorCode::capacity_exceeded};
        }
    }

    [[nodiscard]] Result<SessionHandle> add_acceptor(AcceptorConfig config,
        std::size_t shard_index = auto_shard) noexcept {
        if (shards_.empty()) return Status{ErrorCode::invalid_config};
        const std::size_t target = (shard_index == auto_shard)
            ? (next_rr_shard_++ % shards_.size()) : shard_index;
        if (target >= shards_.size()) return Status{ErrorCode::invalid_config};
        try {
            if (session_to_shard_.contains(config.name)) return Status{ErrorCode::invalid_config};
            const std::string name = config.name;
            const std::string listener_name = config.listener;
            const FixVersion version = config.settings.version;
            const std::string local_comp = config.local_comp_id;
            const std::string remote_comp = config.remote_comp_id;
            const bool verify_checksum = config.settings.verify_checksum;
            const WireHints route_wire_hints = config.settings.schema_wire_hints;
            auto handle = shards_[target]->add_acceptor(std::move(config));
            if (!handle.ok()) return handle.status();
            if (target != 0) {
                const auto routed = shards_[0]->register_remote_acceptor_route(
                    listener_name, version, local_comp, remote_comp, shards_[target].get(),
                    verify_checksum, route_wire_hints);
                if (!routed.ok()) return routed;
            }
            session_to_shard_.emplace(name, target);
            return handle;
        } catch (...) {
            return Status{ErrorCode::capacity_exceeded};
        }
    }

    [[nodiscard]] Status validate() const noexcept {
        for (const auto& shard : shards_) {
            if (const auto st = shard->validate(); !st.ok()) return st;
        }
        return Status::success();
    }

    // Runs Shards 1..K-1 on dedicated OS threads (pinned to cpu_cores[i] if configured)
    // and runs Shard 0 on the calling thread until request_stop() or a fatal shard error.
    [[nodiscard]] Status run() noexcept {
        if (shards_.empty()) return Status{ErrorCode::invalid_config};
        try {
            const std::size_t k = shards_.size();
            std::vector<Status> results(k, Status::success());
            std::vector<std::thread> workers;
            workers.reserve(k > 0 ? k - 1 : 0);

            for (std::size_t i = 1; i < k; ++i) {
                workers.emplace_back([this, i, &results] {
                    results[i] = shards_[i]->run();
                    if (!results[i].ok()) request_stop();
                });
            }
            results[0] = shards_[0]->run();
            if (!results[0].ok()) request_stop();
            for (auto& t : workers) {
                if (t.joinable()) t.join();
            }
            for (const auto& st : results) {
                if (!st.ok()) return st;
            }
            return Status::success();
        } catch (...) {
            request_stop();
            return Status{ErrorCode::capacity_exceeded};
        }
    }

    void request_stop() noexcept {
        for (auto& shard : shards_) {
            if (shard) shard->request_stop();
        }
    }

    [[nodiscard]] bool running() const noexcept {
        for (const auto& shard : shards_) {
            if (shard && shard->running()) return true;
        }
        return false;
    }

    [[nodiscard]] Result<SessionSnapshot> snapshot(std::string_view name) const noexcept {
        const std::size_t shard = session_shard(name);
        if (shard == auto_shard || shard >= shards_.size()) return Status{ErrorCode::invalid_value};
        return shards_[shard]->snapshot(name);
    }

    [[nodiscard]] Status request_logout(std::string_view session_name,
        std::string_view reason = {}) noexcept {
        const std::size_t shard = session_shard(session_name);
        if (shard == auto_shard || shard >= shards_.size()) return Status{ErrorCode::invalid_value};
        return shards_[shard]->request_logout(session_name, reason);
    }

    [[nodiscard]] Status request_reconnect(std::string_view session_name) noexcept {
        const std::size_t shard = session_shard(session_name);
        if (shard == auto_shard || shard >= shards_.size()) return Status{ErrorCode::invalid_value};
        return shards_[shard]->request_reconnect(session_name);
    }

    [[nodiscard]] Status request_sequence_reset(std::string_view session_name,
        std::string_view reason = "admin sequence reset") noexcept {
        const std::size_t shard = session_shard(session_name);
        if (shard == auto_shard || shard >= shards_.size()) return Status{ErrorCode::invalid_value};
        return shards_[shard]->request_sequence_reset(session_name, reason);
    }

    [[nodiscard]] Status adjust_sequences(std::string_view session_name,
        std::uint64_t next_inbound, std::uint64_t next_outbound,
        std::string_view reason = "admin sequence adjust") noexcept {
        const std::size_t shard = session_shard(session_name);
        if (shard == auto_shard || shard >= shards_.size()) return Status{ErrorCode::invalid_value};
        return shards_[shard]->adjust_sequences(session_name, next_inbound, next_outbound, reason);
    }

    [[nodiscard]] Result<TcpEndpoint> listener_endpoint(std::string_view name) const noexcept {
        if (shards_.empty()) return Status{ErrorCode::invalid_value};
        return shards_[0]->listener_endpoint(name);
    }

    [[nodiscard]] std::uint64_t dropped_diagnostics() const noexcept {
        std::uint64_t total = 0;
        for (const auto& shard : shards_) {
            if (shard) total += shard->dropped_diagnostics();
        }
        return total;
    }

private:
    explicit ShardedEngine(std::vector<std::unique_ptr<detail::EngineRuntime>> shards) noexcept
        : shards_(std::move(shards)) {}

    std::vector<std::unique_ptr<detail::EngineRuntime>> shards_{};
    std::unordered_map<std::string, std::size_t> session_to_shard_{};
    std::size_t next_rr_shard_{0};
};

} // namespace nimblefix
