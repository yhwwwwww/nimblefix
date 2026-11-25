#pragma once

#include <cstddef>
#include <concepts>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <nimblefix/codec.hpp>
#include <nimblefix/connection_strategy.hpp>
#include <nimblefix/execution.hpp>
#include <nimblefix/io_backend.hpp>
#include <nimblefix/schema.hpp>
#include <nimblefix/sender.hpp>
#include <nimblefix/session.hpp>
#include <nimblefix/settings.hpp>
#include <nimblefix/status.hpp>
#include <nimblefix/telemetry.hpp>
#include <nimblefix/tls.hpp>

namespace nimblefix {

struct StorageConfig {
    std::filesystem::path directory;
    std::size_t journal_capacity_bytes{16U * 1024U * 1024U};
    std::size_t journal_max_records{65'536};
    std::size_t segment_bytes{1024U * 1024U};
    std::size_t max_segments{};
    std::size_t retain_outbound_messages{8192};
};

struct EngineConfig {
    StorageConfig storage;
    std::uint32_t heartbeat_ms{30'000};
    std::uint32_t connect_timeout_ms{10'000};
    std::uint32_t reconnect_delay_ms{250};
    std::uint32_t logon_timeout_ms{10'000};
    std::uint32_t write_stall_ms{30'000};
    std::uint32_t logout_timeout_ms{5'000};
    std::uint32_t max_pending_age_ms{30'000}; // Zero explicitly disables oldest-message expiry.
    std::size_t max_frame_bytes{default_max_frame_bytes};
    std::size_t max_sessions{64};
    std::size_t max_pending_connections{32};
    std::size_t max_pending_messages{256};
    std::size_t max_pending_bytes{4U * 1024U * 1024U};
    std::size_t max_pending_inbound_messages{128};
    std::size_t max_pending_inbound_bytes{4U * 1024U * 1024U};
    std::size_t max_pending_sender_messages{256};
    std::size_t max_pending_sender_bytes{4U * 1024U * 1024U};
    std::size_t sender_receipt_capacity{1024};
    // Zero disables the explicit SPSC path; nonzero values are capped at 64.
    std::size_t max_producer_lanes_per_session{8};
    QueuedConfig queued{};
    IoBackendConfig io_backend{};
    std::size_t diagnostic_capacity{256};
    WireHints wire_hints{};
    std::function<Status(const FlatMessageView&)> message_validator{};
    // Optional Linux CPU core pinning for this Engine's reactor owner thread (-1 = unpinned).
    int cpu_core{-1};
    // Optional mmap-backed telemetry region path (empty = disabled, zero overhead).
    std::filesystem::path telemetry_path{};
    bool telemetry_stage_probes{false};
    // Internal shared writer used by ShardedEngine so all shards publish into one mmap file.
    std::shared_ptr<TelemetryWriter> shared_telemetry{};
};

struct InitiatorConfig {
    std::string name;
    std::string sender_comp_id;
    std::string target_comp_id;
    TcpEndpoint endpoint;
    SessionSettings settings{};
    TlsConfig tls{};
    // Optional ordered failover endpoint list (up to 16 entries). When non-empty,
    // overrides the single `endpoint` field and uses `connection_strategy`.
    std::vector<TcpEndpoint> endpoints{};
    ConnectionStrategy connection_strategy{};
};

struct ListenerConfig {
    std::string name;
    TcpEndpoint endpoint;
    std::size_t backlog{128};
    TlsConfig tls{};
};

struct AcceptorConfig {
    std::string name;
    std::string listener;
    std::string local_comp_id;
    std::string remote_comp_id;
    SessionSettings settings{};
};

class SessionHandle {
public:
    std::string name;

    template <class Schema = FlatSchema>
    [[nodiscard]] Sender<Schema> sender() const noexcept {
        std::string_view fingerprint{};
        if constexpr (!std::is_same_v<Schema, FlatSchema>) {
            static_assert(requires { { Schema::fingerprint } -> std::convertible_to<std::string_view>; },
                "Sender schema must expose a fingerprint");
            fingerprint = Schema::fingerprint;
        }
        return Sender<Schema>{sender_state_, fingerprint};
    }

    template <class Schema = FlatSchema>
    [[nodiscard]] Result<ProducerLane<Schema>> producer_lane(
        ProducerLaneConfig config = {}) const noexcept {
        std::string_view fingerprint{};
        if constexpr (!std::is_same_v<Schema, FlatSchema>) {
            static_assert(requires { { Schema::fingerprint } -> std::convertible_to<std::string_view>; },
                "ProducerLane schema must expose a fingerprint");
            fingerprint = Schema::fingerprint;
        }
        auto created = detail::create_producer_lane(sender_state_, config, fingerprint);
        if (!created.ok()) return created.status();
        return ProducerLane<Schema>{std::move(created).value(), fingerprint};
    }

private:
    friend class detail::EngineRuntime;
    SessionHandle(std::string value, std::shared_ptr<detail::SenderSharedState> sender) noexcept
        : name(std::move(value)), sender_state_(std::move(sender)) {}

    std::shared_ptr<detail::SenderSharedState> sender_state_{};
};

struct ListenerHandle {
    std::string name;
};

struct SessionSnapshot {
    std::string name;
    SessionState state{SessionState::disconnected};
    Status last_error{};
    std::uint64_t generation{};
    std::uint64_t next_inbound_seq{1};
    std::uint64_t next_outbound_seq{1};
    std::uint64_t received_frames{};
    std::uint64_t received_bytes{};
    std::uint64_t written_bytes{};
    std::uint64_t business_deliveries{};
    std::size_t pending_messages{};
    std::size_t pending_bytes{};
    std::size_t pending_inbound_messages{};
    std::size_t pending_inbound_bytes{};
    bool read_paused{};
    std::size_t active_endpoint_index{};
};

struct ErrorEvent {
    std::string session_name;
    Status status{};
    std::string_view operation{"session"};

    [[nodiscard]] std::string format() const;
};

namespace detail {

template <class Callback>
[[nodiscard]] bool callback_is_present(const Callback& callback) {
    if constexpr (requires { static_cast<bool>(callback); })
        return static_cast<bool>(callback);
    else
        return true;
}

using EngineMessageCallback = std::function<Status(
    SessionContext&, const FlatMessageView&, const DeliveryInfo&)>;
using EngineActiveCallback = std::function<Status(SessionContext&)>;
using EngineErrorCallback = std::function<void(const ErrorEvent&)>;

class EngineRuntime {
public:
    EngineRuntime(const EngineRuntime&) = delete;
    EngineRuntime& operator=(const EngineRuntime&) = delete;
    ~EngineRuntime();

    [[nodiscard]] static Result<std::unique_ptr<EngineRuntime>> create(EngineConfig config,
        bool queued = false, const SchemaDescriptor* schema = nullptr,
        WireHints wire_hints = {}) noexcept;
    [[nodiscard]] Status on_message(EngineMessageCallback callback) noexcept;
    [[nodiscard]] Status on_typed_message(std::string_view msg_type,
        EngineMessageCallback callback) noexcept;
    [[nodiscard]] Status ignore_message(std::string_view msg_type,
        std::string_view reason) noexcept;
    [[nodiscard]] Status on_session_active(EngineActiveCallback callback) noexcept;
    [[nodiscard]] Status on_error(EngineErrorCallback callback) noexcept;
    [[nodiscard]] Result<SessionHandle> add_initiator(InitiatorConfig config) noexcept;
    [[nodiscard]] Result<ListenerHandle> add_listener(ListenerConfig config) noexcept;
    [[nodiscard]] Result<SessionHandle> add_acceptor(AcceptorConfig config) noexcept;
    // Internal sharded-reactor hooks: add a non-binding virtual listener on a non-zero shard
    // and register a Logon-routing rule on the listener-owning shard to hand accepted sockets
    // directly to the owning shard's reactor loop.
    [[nodiscard]] Result<ListenerHandle> add_handoff_listener(ListenerConfig config) noexcept;
    [[nodiscard]] Status register_remote_acceptor_route(std::string_view listener_name,
        FixVersion version, std::string_view local_comp_id, std::string_view remote_comp_id,
        EngineRuntime* target_shard) noexcept;
    [[nodiscard]] Status validate() const noexcept;
    [[nodiscard]] Status run() noexcept;
    [[nodiscard]] Status start() noexcept;
    [[nodiscard]] Status poll_once(std::uint32_t max_wait_ms = 0) noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] Result<SessionSnapshot> snapshot(std::string_view session_name) const noexcept;
    [[nodiscard]] std::uint64_t dropped_diagnostics() const noexcept;
    void request_stop() noexcept;
    [[nodiscard]] Result<TcpEndpoint> listener_endpoint(std::string_view name) const noexcept;

private:
    struct Impl;
    explicit EngineRuntime(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_{};
};

} // namespace detail

// One session owner runs protocol state for both policies. Queued controls only
// where business delivery runs; both policies keep the same Journal/driver.
template <class Schema = FlatSchema, class Policy = Inline>
class Engine {
    static_assert(std::is_same_v<Schema, FlatSchema> || requires {
        { Schema::descriptor() } -> std::same_as<const SchemaDescriptor&>;
        { Schema::find_message(std::string_view{}) } -> std::same_as<const MessageDescriptor*>;
        { Schema::wire_hints() } -> std::same_as<WireHints>;
    }, "typed Engine Schema must expose descriptor() and find_message(msg_type)");
    static_assert(std::is_same_v<Policy, Inline> || std::is_same_v<Policy, Queued>,
        "Engine Policy must be nimblefix::Inline or nimblefix::Queued");

public:
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) noexcept = default;
    Engine& operator=(Engine&&) noexcept = default;

    [[nodiscard]] static Result<Engine> create(EngineConfig config) noexcept {
        const SchemaDescriptor* descriptor = nullptr;
        WireHints schema_wire_hints{};
        if constexpr (!std::is_same_v<Schema, FlatSchema>) {
            static_assert(requires {
                { Schema::descriptor() } -> std::same_as<const SchemaDescriptor&>;
                { Schema::find_message(std::string_view{}) } -> std::same_as<const MessageDescriptor*>;
                { Schema::wire_hints() } -> std::same_as<WireHints>;
            }, "typed Engine Schema must expose descriptor() and find_message(msg_type)");
            descriptor = &Schema::descriptor();
            schema_wire_hints = Schema::wire_hints();
        }
        auto created = detail::EngineRuntime::create(std::move(config),
            std::is_same_v<Policy, Queued>, descriptor, schema_wire_hints);
        if (!created.ok()) return created.status();
        return Engine{std::move(created).value()};
    }

    // Callback registration is single-assignment before run(); null/empty
    // callbacks and duplicate registrations return an error.
    template <class Callback>
        requires std::is_same_v<Schema, FlatSchema> &&
            std::is_invocable_r_v<Status, Callback&, SessionContext&, const FlatMessageView&>
    [[nodiscard]] Status on_message(Callback&& callback) noexcept {
        try {
            if (!detail::callback_is_present(callback)) return {ErrorCode::invalid_config};
            return runtime_->on_message(
                [fn = std::forward<Callback>(callback)](SessionContext& context,
                    const FlatMessageView& message, const DeliveryInfo&) mutable {
                    return std::invoke(fn, context, message);
                });
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
            return runtime_->on_message(std::forward<Callback>(callback));
        } catch (...) {
            return Status{ErrorCode::capacity_exceeded};
        }
    }

    template <class Message, class Callback>
        requires (!std::is_same_v<Schema, FlatSchema>) &&
            requires { typename Message::schema_type; typename Message::view_type; } &&
            std::is_same_v<Schema, typename Message::schema_type> &&
            (std::is_invocable_r_v<Status, Callback&, SessionContext&, typename Message::view_type> ||
             std::is_invocable_r_v<Status, Callback&, SessionContext&,
                const typename Message::view_type&>)
    [[nodiscard]] Status on(Callback&& callback) noexcept {
        try {
            if (!detail::callback_is_present(callback)) return {ErrorCode::invalid_config};
            if (Schema::find_message(Message::msg_type) == nullptr)
                return {ErrorCode::schema_mismatch, 35};
            using View = typename Message::view_type;
            return runtime_->on_typed_message(Message::msg_type,
                [fn = std::forward<Callback>(callback)](SessionContext& context,
                    const FlatMessageView& flat, const DeliveryInfo&) mutable -> Status {
                    View view{flat};
                    if constexpr (std::is_invocable_r_v<Status, Callback&, SessionContext&, View>)
                        return std::invoke(fn, context, std::move(view));
                    else
                        return std::invoke(fn, context, std::as_const(view));
                });
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
            return {ErrorCode::schema_mismatch, 35};
        return runtime_->ignore_message(Message::msg_type, reason);
    }

    template <class Callback>
        requires std::is_invocable_r_v<Status, Callback&, SessionContext&>
    [[nodiscard]] Status on_session_active(Callback&& callback) noexcept {
        try {
            if (!detail::callback_is_present(callback)) return {ErrorCode::invalid_config};
            return runtime_->on_session_active(std::forward<Callback>(callback));
        } catch (...) {
            return Status{ErrorCode::capacity_exceeded};
        }
    }

    template <class Callback>
        requires std::is_invocable_r_v<void, Callback&, const ErrorEvent&>
    [[nodiscard]] Status on_error(Callback&& callback) noexcept {
        try {
            if (!detail::callback_is_present(callback)) return {ErrorCode::invalid_config};
            return runtime_->on_error(std::forward<Callback>(callback));
        } catch (...) {
            return Status{ErrorCode::capacity_exceeded};
        }
    }

    [[nodiscard]] Result<SessionHandle> add_initiator(InitiatorConfig config) noexcept {
        return runtime_->add_initiator(std::move(config));
    }

    [[nodiscard]] Result<ListenerHandle> add_listener(ListenerConfig config) noexcept {
        return runtime_->add_listener(std::move(config));
    }

    [[nodiscard]] Result<SessionHandle> add_acceptor(AcceptorConfig config) noexcept {
        return runtime_->add_acceptor(std::move(config));
    }

    [[nodiscard]] Status ignore_message(std::string_view msg_type,
        std::string_view reason) noexcept requires std::is_same_v<Schema, FlatSchema> {
        return runtime_->ignore_message(msg_type, reason);
    }

    [[nodiscard]] Status validate() const noexcept { return runtime_->validate(); }
    [[nodiscard]] Status run() noexcept { return runtime_->run(); }
    // start/poll_once use the same runtime as run(). The calling thread owns
    // all protocol work; only Queued callbacks and diagnostics use workers.
    [[nodiscard]] Status start() noexcept { return runtime_->start(); }
    [[nodiscard]] Status poll_once(std::uint32_t max_wait_ms = 0) noexcept {
        return runtime_->poll_once(max_wait_ms);
    }
    [[nodiscard]] bool running() const noexcept { return runtime_->running(); }
    [[nodiscard]] Result<SessionSnapshot> snapshot(std::string_view name) const noexcept {
        return runtime_->snapshot(name);
    }
    [[nodiscard]] std::uint64_t dropped_diagnostics() const noexcept {
        return runtime_->dropped_diagnostics();
    }
    void request_stop() noexcept { runtime_->request_stop(); }

    // Returns need_more until run() has bound the listener and published the
    // OS-selected address, which lets tests and embedders use port 0 safely.
    [[nodiscard]] Result<TcpEndpoint> listener_endpoint(std::string_view name) const noexcept {
        return runtime_->listener_endpoint(name);
    }

private:
    explicit Engine(std::unique_ptr<detail::EngineRuntime> runtime) noexcept
        : runtime_(std::move(runtime)) {}

    std::unique_ptr<detail::EngineRuntime> runtime_{};
};

} // namespace nimblefix
