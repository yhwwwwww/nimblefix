#include <nimblefix/engine.hpp>
#include <nimblefix/message.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

namespace nf = nimblefix;

namespace {

static_assert(std::atomic<bool>::is_always_lock_free);
std::atomic<bool> stopping{false};

void request_stop(int) noexcept {
    stopping.store(true, std::memory_order_relaxed);
}

template <class Integer>
void update_max(std::atomic<Integer>& target, Integer value) noexcept {
    auto previous = target.load(std::memory_order_relaxed);
    while (previous < value && !target.compare_exchange_weak(previous, value,
        std::memory_order_relaxed)) {}
}

const char* state_name(nf::SessionState state) noexcept {
    switch (state) {
    case nf::SessionState::disconnected: return "disconnected";
    case nf::SessionState::awaiting_logon: return "awaiting_logon";
    case nf::SessionState::active: return "active";
    case nf::SessionState::logout_pending: return "logout_pending";
    case nf::SessionState::closed: return "closed";
    case nf::SessionState::failed: return "failed";
    }
    return "unknown";
}

const char* error_name(nf::ErrorCode code) noexcept {
    switch (code) {
    case nf::ErrorCode::none: return "none";
    case nf::ErrorCode::queue_full: return "queue_full";
    case nf::ErrorCode::write_stalled: return "write_stalled";
    case nf::ErrorCode::backlog_timeout: return "backlog_timeout";
    case nf::ErrorCode::connection_closed: return "connection_closed";
    case nf::ErrorCode::io_error: return "io_error";
    default: return "other";
    }
}

struct Metrics {
    std::atomic<std::uint64_t> healthy_inbound{0};
    std::atomic<std::uint64_t> healthy_replies{0};
    std::atomic<std::uint64_t> healthy_during_slow_backpressure{0};
    std::atomic<std::uint64_t> healthy_send_failures{0};
    std::atomic<std::uint64_t> slow_inbound{0};
    std::atomic<std::uint64_t> slow_replies{0};
    std::atomic<std::uint64_t> slow_queue_full{0};
    std::atomic<std::uint64_t> slow_other_send_failures{0};
    std::atomic<std::uint64_t> error_events{0};
    std::atomic<std::uint64_t> healthy_error_events{0};
    std::atomic<std::uint64_t> slow_error_events{0};
    std::atomic<int> healthy_last_error{static_cast<int>(nf::ErrorCode::none)};
    std::atomic<int> slow_last_error{static_cast<int>(nf::ErrorCode::none)};
    std::atomic<std::size_t> slow_pending_messages_high_water{0};
    std::atomic<std::size_t> slow_pending_bytes_high_water{0};
    std::atomic<bool> healthy_active_observed{false};
};

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: isolation-server <state-directory>\n";
        return 2;
    }

    try {
        std::filesystem::create_directories(argv[1]);
    } catch (const std::exception& error) {
        std::cerr << "state directory: " << error.what() << '\n';
        return 1;
    }

    constexpr std::size_t slow_payload_bytes = 50U * 1024U;
    const std::string slow_payload(slow_payload_bytes, 'S');
    Metrics metrics;

    nf::EngineConfig config;
    config.storage.directory = argv[1];
    config.storage.journal_capacity_bytes = 16U * 1024U * 1024U;
    config.storage.journal_max_records = 100'000;
    config.max_frame_bytes = 64U * 1024U;
    config.max_sessions = 4;
    config.max_pending_connections = 8;
    config.max_pending_messages = 64;
    config.max_pending_bytes = 4U * 1024U * 1024U;
    config.max_pending_inbound_messages = 256;
    config.max_pending_inbound_bytes = 4U * 1024U * 1024U;
    config.write_stall_ms = 2'500;
    config.logout_timeout_ms = 750;
    config.max_pending_age_ms = 0;

    auto created = nf::Engine<>::create(std::move(config));
    if (!created.ok()) {
        std::cerr << "engine creation failed: " << created.status().message() << '\n';
        return 1;
    }
    auto engine = std::move(created).value();

    if (!engine.on_message([&](nf::SessionContext& context,
            const nf::FlatMessageView& message) -> nf::Status {
            const auto type = message.find_unique(35);
            const auto cl_ord_id = message.find_unique(11);
            if (!type.ok() || type.value().value != "D" || !cl_ord_id.ok() ||
                cl_ord_id.value().value.empty())
                return {nf::ErrorCode::invalid_value, 11};

            if (context.remote_comp_id() == "BUY") {
                metrics.healthy_inbound.fetch_add(1, std::memory_order_relaxed);
                if (metrics.slow_queue_full.load(std::memory_order_relaxed) != 0)
                    metrics.healthy_during_slow_backpressure.fetch_add(1,
                        std::memory_order_relaxed);
                auto reply = nf::message<"8">().set<11>(cl_ord_id.value().value);
                const auto sent = context.send(reply);
                if (!sent.ok()) {
                    metrics.healthy_send_failures.fetch_add(1, std::memory_order_relaxed);
                    return sent.status();
                }
                metrics.healthy_replies.fetch_add(1, std::memory_order_relaxed);
                return nf::Status::success();
            }

            if (context.remote_comp_id() == "LAG") {
                metrics.slow_inbound.fetch_add(1, std::memory_order_relaxed);
                auto reply = nf::message<"8">(nf::MessageLimits{8, 52U * 1024U})
                    .set<11>(cl_ord_id.value().value)
                    .set<58>(std::string_view{slow_payload});
                const auto sent = context.send(reply);
                if (sent.ok()) {
                    metrics.slow_replies.fetch_add(1, std::memory_order_relaxed);
                } else if (sent.status().code == nf::ErrorCode::queue_full) {
                    metrics.slow_queue_full.fetch_add(1, std::memory_order_relaxed);
                    // Keep the slow session alive so we can observe its bounded
                    // queue rejecting replies while healthy traffic continues.
                } else {
                    metrics.slow_other_send_failures.fetch_add(1, std::memory_order_relaxed);
                }
                return nf::Status::success();
            }

            return {nf::ErrorCode::invalid_header, 49};
        }).ok()) {
        std::cerr << "could not install message callback\n";
        return 1;
    }
    if (!engine.on_error([&](const nf::ErrorEvent& event) {
            metrics.error_events.fetch_add(1, std::memory_order_relaxed);
            if (event.session_name == "healthy") {
                metrics.healthy_error_events.fetch_add(1, std::memory_order_relaxed);
                metrics.healthy_last_error.store(static_cast<int>(event.status.code),
                    std::memory_order_relaxed);
            } else if (event.session_name == "lag") {
                metrics.slow_error_events.fetch_add(1, std::memory_order_relaxed);
                metrics.slow_last_error.store(static_cast<int>(event.status.code),
                    std::memory_order_relaxed);
            }
        }).ok()) {
        std::cerr << "could not install error callback\n";
        return 1;
    }
    if (!engine.add_listener({.name = "isolation", .endpoint = {"127.0.0.1", 0}}).ok()) {
        std::cerr << "could not add listener\n";
        return 1;
    }
    if (!engine.add_acceptor({.name = "healthy", .listener = "isolation",
            .local_comp_id = "SELL", .remote_comp_id = "BUY"}).ok()) {
        std::cerr << "could not add healthy acceptor\n";
        return 1;
    }

    nf::SessionSettings lag_settings;
    nf::FlowControlConfig lag_flow;
    lag_flow.max_pending_messages = 12;
    lag_flow.max_pending_bytes = 512U * 1024U;
    lag_flow.max_pending_inbound_messages = 128;
    lag_flow.max_pending_inbound_bytes = 1024U * 1024U;
    lag_flow.max_pending_age_ms = 0;
    lag_flow.max_delivery_age_ms = 30'000;
    lag_flow.receive_pause_timeout_ms = 30'000;
    lag_flow.write_stall_ms = 2'500;
    lag_settings.flow_control = lag_flow;
    if (!engine.add_acceptor({.name = "lag", .listener = "isolation",
            .local_comp_id = "SELL", .remote_comp_id = "LAG",
            .settings = std::move(lag_settings)}).ok()) {
        std::cerr << "could not add slow acceptor\n";
        return 1;
    }

    std::signal(SIGTERM, request_stop);
    std::signal(SIGINT, request_stop);
    nf::Status run_status{};
    std::atomic<bool> owner_done{false};
    std::thread owner([&] {
        run_status = engine.run();
        owner_done.store(true, std::memory_order_release);
    });

    const auto ready_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    bool ready = false;
    std::uint16_t bound_port{};
    while (!stopping.load(std::memory_order_relaxed) &&
        !owner_done.load(std::memory_order_acquire) &&
        std::chrono::steady_clock::now() < ready_deadline) {
        const auto endpoint = engine.listener_endpoint("isolation");
        if (endpoint.ok()) {
            bound_port = endpoint.value().port;
            std::cout << bound_port << std::endl;
            ready = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }

    if (!ready) stopping.store(true, std::memory_order_relaxed);
    while (ready && !stopping.load(std::memory_order_relaxed) &&
        !owner_done.load(std::memory_order_acquire)) {
        const auto snapshot = engine.snapshot("lag");
        if (snapshot.ok()) {
            update_max(metrics.slow_pending_messages_high_water, snapshot.value().pending_messages);
            update_max(metrics.slow_pending_bytes_high_water, snapshot.value().pending_bytes);
        }
        const auto healthy_snapshot = engine.snapshot("healthy");
        if (healthy_snapshot.ok() && healthy_snapshot.value().state == nf::SessionState::active)
            metrics.healthy_active_observed.store(true, std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }

    engine.request_stop();
    if (owner.joinable()) owner.join();

    const auto slow_snapshot = engine.snapshot("lag");
    const auto healthy_snapshot = engine.snapshot("healthy");
    const auto slow_state = slow_snapshot.ok() ? state_name(slow_snapshot.value().state) : "missing";
    const auto healthy_state = healthy_snapshot.ok() ? state_name(healthy_snapshot.value().state) : "missing";
    const auto slow_written_bytes_total = slow_snapshot.ok() ? slow_snapshot.value().written_bytes : 0;
    const auto slow_received_frames_total = slow_snapshot.ok() ? slow_snapshot.value().received_frames : 0;
    const auto slow_last_error = static_cast<nf::ErrorCode>(
        metrics.slow_last_error.load(std::memory_order_relaxed));
    const auto healthy_last_error = static_cast<nf::ErrorCode>(
        metrics.healthy_last_error.load(std::memory_order_relaxed));
    const auto slow_pending_high = metrics.slow_pending_bytes_high_water.load(std::memory_order_relaxed);
    const bool slow_pressure = metrics.slow_queue_full.load(std::memory_order_relaxed) != 0 ||
        slow_last_error == nf::ErrorCode::write_stalled ||
        slow_last_error == nf::ErrorCode::backlog_timeout || slow_pending_high != 0;

    std::cout << "{\"listen_port\":" << bound_port
        << ",\"healthy_inbound\":" << metrics.healthy_inbound.load()
        << ",\"healthy_replies\":" << metrics.healthy_replies.load()
        << ",\"healthy_during_slow_backpressure\":"
        << metrics.healthy_during_slow_backpressure.load()
        << ",\"healthy_active_observed\":"
        << (metrics.healthy_active_observed.load() ? "true" : "false")
        << ",\"healthy_send_failures\":" << metrics.healthy_send_failures.load()
        << ",\"healthy_error_events\":" << metrics.healthy_error_events.load()
        << ",\"healthy_last_error\":\"" << error_name(healthy_last_error) << "\""
        << ",\"healthy_last_error_code\":" << metrics.healthy_last_error.load()
        << ",\"healthy_state\":\"" << healthy_state << "\""
        << ",\"slow_inbound\":" << metrics.slow_inbound.load()
        << ",\"slow_replies\":" << metrics.slow_replies.load()
        << ",\"slow_queue_full\":" << metrics.slow_queue_full.load()
        << ",\"slow_other_send_failures\":" << metrics.slow_other_send_failures.load()
        << ",\"slow_error_events\":" << metrics.slow_error_events.load()
        << ",\"slow_last_error\":\"" << error_name(slow_last_error) << "\""
        << ",\"slow_last_error_code\":" << metrics.slow_last_error.load()
        << ",\"slow_state\":\"" << slow_state << "\""
        << ",\"slow_pending_messages_high_water\":"
        << metrics.slow_pending_messages_high_water.load()
        << ",\"slow_pending_bytes_high_water\":" << slow_pending_high
        << ",\"slow_written_bytes_total\":" << slow_written_bytes_total
        << ",\"slow_received_frames_total\":" << slow_received_frames_total
        << ",\"slow_payload_bytes\":" << slow_payload_bytes
        << ",\"slow_pressure_confirmed\":" << (slow_pressure ? "true" : "false")
        << ",\"error_events\":" << metrics.error_events.load()
        << ",\"engine_status\":\"" << error_name(run_status.code) << "\"}\n";
    return ready && run_status.ok() ? 0 : 1;
}
