#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <nimblefix/codec.hpp>
#include <nimblefix/session.hpp>
#include <nimblefix/status.hpp>

namespace nimblefix {

struct FlatSchema {};

struct TcpEndpoint {
    // Stage 3 accepts numeric IPv4/IPv6 addresses. An empty listener host binds
    // all interfaces; initiator hosts must be nonempty numeric addresses.
    std::string host;
    std::uint16_t port{};
};

struct StorageConfig {
    std::filesystem::path directory;
    std::size_t journal_capacity_bytes{16U * 1024U * 1024U};
    std::size_t journal_max_records{65'536};
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
};

struct InitiatorConfig {
    std::string name;
    std::string sender_comp_id;
    std::string target_comp_id;
    TcpEndpoint endpoint;
};

struct ListenerConfig {
    std::string name;
    TcpEndpoint endpoint;
    std::size_t backlog{128};
};

struct AcceptorConfig {
    std::string name;
    std::string listener;
    std::string local_comp_id;
    std::string remote_comp_id;
};

struct SessionHandle {
    std::string name;
};

struct ListenerHandle {
    std::string name;
};

struct ErrorEvent {
    std::string session_name;
    Status status{};

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

    [[nodiscard]] static Result<std::unique_ptr<EngineRuntime>> create(EngineConfig config) noexcept;
    [[nodiscard]] Status on_message(EngineMessageCallback callback) noexcept;
    [[nodiscard]] Status on_session_active(EngineActiveCallback callback) noexcept;
    [[nodiscard]] Status on_error(EngineErrorCallback callback) noexcept;
    [[nodiscard]] Result<SessionHandle> add_initiator(InitiatorConfig config) noexcept;
    [[nodiscard]] Result<ListenerHandle> add_listener(ListenerConfig config) noexcept;
    [[nodiscard]] Result<SessionHandle> add_acceptor(AcceptorConfig config) noexcept;
    [[nodiscard]] Status validate() const noexcept;
    [[nodiscard]] Status run() noexcept;
    void request_stop() noexcept;
    [[nodiscard]] Result<TcpEndpoint> listener_endpoint(std::string_view name) const noexcept;

private:
    struct Impl;
    explicit EngineRuntime(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_{};
};

} // namespace detail

// The runtime is single-owner and inline. Engine is movable and deliberately
// noncopyable so each live session has exactly one protocol owner.
// Schema-specific receive dispatch is not implemented yet. Generated typed
// message builders can still be sent through Engine<> and receive callbacks
// currently use FlatMessageView; claiming Engine<GeneratedSchema> would not
// validate or project inbound messages, so only FlatSchema is accepted here.
template <class Schema = FlatSchema>
class Engine {
    static_assert(std::is_same_v<Schema, FlatSchema>,
        "typed Engine<Schema> dispatch is not implemented; use Engine<> with generated send builders");

public:
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) noexcept = default;
    Engine& operator=(Engine&&) noexcept = default;

    [[nodiscard]] static Result<Engine> create(EngineConfig config) noexcept {
        auto created = detail::EngineRuntime::create(std::move(config));
        if (!created.ok()) return created.status();
        return Engine{std::move(created).value()};
    }

    // Callback registration is single-assignment before run(); null/empty
    // callbacks and duplicate registrations return an error.
    template <class Callback>
        requires std::is_invocable_r_v<Status, Callback&, SessionContext&, const FlatMessageView&>
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
        requires std::is_invocable_r_v<Status, Callback&, SessionContext&,
            const FlatMessageView&, const DeliveryInfo&>
    [[nodiscard]] Status on_message(Callback&& callback) noexcept {
        try {
            if (!detail::callback_is_present(callback)) return {ErrorCode::invalid_config};
            return runtime_->on_message(std::forward<Callback>(callback));
        } catch (...) {
            return Status{ErrorCode::capacity_exceeded};
        }
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

    [[nodiscard]] Status validate() const noexcept { return runtime_->validate(); }
    [[nodiscard]] Status run() noexcept { return runtime_->run(); }
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
