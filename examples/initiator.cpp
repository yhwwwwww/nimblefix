#include "endpoint.hpp"
#include "order_entry.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <thread>

namespace {
static_assert(std::atomic<bool>::is_always_lock_free);
std::atomic<bool> stop_signal{false};
}

extern "C" void handle_signal(int) {
    stop_signal.store(true, std::memory_order_relaxed);
}

int main(int argc, char** argv) {
    using namespace nimblefix_example::order_entry;
    demo::Options options;
    if (!demo::parse_args(argc, argv, true, options)) {
        demo::print_usage("initiator-example", true);
        return 2;
    }
    if (options.help) {
        demo::print_usage("initiator-example", true);
        return 0;
    }

    nf::EngineConfig config;
    config.storage.directory = options.state;
    config.logon_timeout_ms = options.timeout_ms;
    config.connect_timeout_ms = options.timeout_ms;
    config.logout_timeout_ms = options.timeout_ms;
    auto created = ExampleEngine::create(std::move(config));
    if (!created.ok()) {
        std::cerr << "Engine create failed: " << created.status().message() << '\n';
        return 1;
    }
    auto engine = std::move(created).value();

    ReportQueue report_events;
    DiagnosticQueue diagnostic_events;
#if defined(NIMBLEFIX_BUILTIN_FIX44) && NIMBLEFIX_BUILTIN_FIX44
    const auto callback_status = engine.on<nf::fix44::ExecutionReport>(
        [&report_events](nf::SessionContext&, const nf::fix44::ExecutionReport::view_type& report) {
            if (!report_events.push(make_report_event(report.flat())))
                return nf::Status{nf::ErrorCode::capacity_exceeded};
            return nf::Status::success();
        });
#else
    const auto callback_status = engine.on_message(
        [&report_events](nf::SessionContext&, const nf::FlatMessageView& message) {
            if (!demo::is_message(message, "8")) return nf::Status::success();
            if (!report_events.push(make_report_event(message)))
                return nf::Status{nf::ErrorCode::capacity_exceeded};
            return nf::Status::success();
        });
#endif
    if (!callback_status.ok()) {
        std::cerr << "ExecutionReport callback registration failed: "
            << callback_status.message() << '\n';
        return 1;
    }
    const auto error_status = engine.on_error(
        [&diagnostic_events](const nf::ErrorEvent& error) { push_diagnostic(diagnostic_events, error); });
    if (!error_status.ok()) {
        std::cerr << "on_error registration failed: " << error_status.message() << '\n';
        return 1;
    }

    const auto session_result = engine.add_initiator({
        .name = "demo-initiator",
        .sender_comp_id = "BUY",
        .target_comp_id = "SELL",
        .endpoint = {options.host, options.port},
    });
    if (!session_result.ok()) {
        std::cerr << "add_initiator failed: " << session_result.status().message() << '\n';
        return 1;
    }
    auto session = std::move(session_result).value();
    auto sender = session.sender<ExampleSchema>();
    const auto validation = engine.validate();
    if (!validation.ok()) {
        std::cerr << "Engine config invalid: " << validation.message() << '\n';
        return 1;
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    nf::Status run_status{};
    std::atomic<bool> run_done{false};
    std::thread owner([&] {
        run_status = engine.run();
        run_done.store(true, std::memory_order_release);
    });

    std::array<TrackedOrder, max_tracked_orders> orders{};
    std::size_t order_count = 0;
    bool application_error = false;
    bool timed_out = false;
    std::uint64_t dropped_reports = 0;
    std::uint64_t dropped_diagnostics = 0;
    bool session_active = false;
    const auto startup_deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(options.timeout_ms);
    const auto connection_active = [&] {
        const auto snapshot = engine.snapshot("demo-initiator");
        return snapshot.ok() && snapshot.value().state == nf::SessionState::active;
    };

    while (!run_done.load(std::memory_order_acquire) &&
        !stop_signal.load(std::memory_order_relaxed) &&
        !(sender.can_send() && connection_active())) {
        print_diagnostics(diagnostic_events, dropped_diagnostics);
        print_report_events(report_events, orders, order_count, application_error, dropped_reports);
        if (std::chrono::steady_clock::now() >= startup_deadline) {
            timed_out = true;
            std::cerr << "Timed out waiting for SESSION_ACTIVE after " << options.timeout_ms << " ms\n" << std::flush;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!run_done.load(std::memory_order_acquire) && sender.can_send() && connection_active() &&
        !stop_signal.load(std::memory_order_relaxed) && !timed_out) {
        session_active = true;
        std::cout << "SESSION_ACTIVE\n" << std::flush;
    }

    bool stop_input = !session_active;
    std::array<char, 512> input_line{};
    std::size_t input_size = 0;
    bool input_overflow = false;
    bool input_eof = false;
    if (session_active) {
        while (!run_done.load(std::memory_order_acquire)) {
            const bool active_now = connection_active();
            if (active_now != session_active) {
                std::cout << (active_now ? "SESSION_ACTIVE\n" : "SESSION_INACTIVE\n") << std::flush;
                session_active = active_now;
            }
            print_diagnostics(diagnostic_events, dropped_diagnostics);
            print_report_events(report_events, orders, order_count, application_error, dropped_reports);
            update_receipts(sender, orders, order_count);

            if (stop_signal.load(std::memory_order_relaxed)) stop_input = true;
            report_expired_orders(orders, order_count, std::chrono::steady_clock::now(), timed_out);
            if (timed_out) stop_input = true;
            if (stop_input && all_orders_settled(orders, order_count, std::chrono::steady_clock::now()))
                break;

            if (stop_input || input_eof) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            pollfd input_descriptor{STDIN_FILENO, POLLIN, 0};
            const int ready = ::poll(&input_descriptor, 1, 50);
            if (ready < 0) {
                if (errno == EINTR) continue;
                std::cerr << "ERROR poll(stdin): " << errno << '\n' << std::flush;
                application_error = true;
                stop_input = true;
                continue;
            }
            if (ready == 0) continue;
            if ((input_descriptor.revents & (POLLIN | POLLHUP)) == 0) {
                if ((input_descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
                    std::cerr << "ERROR stdin poll reported a terminal error\n" << std::flush;
                    application_error = true;
                    stop_input = true;
                }
                continue;
            }

            std::array<char, 1024> bytes{};
            const auto received = ::read(STDIN_FILENO, bytes.data(), bytes.size());
            if (received < 0) {
                if (errno == EINTR || errno == EAGAIN) continue;
                std::cerr << "ERROR read(stdin): " << errno << '\n' << std::flush;
                application_error = true;
                stop_input = true;
                continue;
            }
            if (received == 0) {
                input_eof = true;
                if (input_overflow) {
                    std::cerr << "ERROR command line exceeded " << input_line.size() << " bytes\n" << std::flush;
                    input_overflow = false;
                    input_size = 0;
                } else if (input_size != 0) {
                    handle_line({input_line.data(), input_size}, sender, engine, orders,
                        order_count, options.timeout_ms, stop_input);
                    input_size = 0;
                }
                stop_input = true;
                continue;
            }

            for (ssize_t i = 0; i < received && !stop_input; ++i) {
                const char byte = bytes[static_cast<std::size_t>(i)];
                if (byte == '\n') {
                    if (input_overflow) {
                        std::cerr << "ERROR command line exceeded " << input_line.size() << " bytes\n" << std::flush;
                        input_overflow = false;
                        input_size = 0;
                        continue;
                    }
                    handle_line({input_line.data(), input_size}, sender, engine, orders,
                        order_count, options.timeout_ms, stop_input);
                    input_size = 0;
                    continue;
                }
                if (input_overflow) continue;
                if (input_size == input_line.size()) {
                    input_overflow = true;
                    continue;
                }
                input_line[input_size++] = byte;
            }
        }
    }

    if (!run_done.load(std::memory_order_acquire)) {
        const int stop_result = finish_engine(engine, owner, run_done, run_status, sender,
            orders, order_count, report_events, diagnostic_events, application_error,
            dropped_reports, dropped_diagnostics, options.timeout_ms);
        return stop_result != 0 || timed_out ? 1 : 0;
    }

    print_diagnostics(diagnostic_events, dropped_diagnostics);
    print_report_events(report_events, orders, order_count, application_error, dropped_reports);
    if (owner.joinable()) owner.join();
    if (!run_status.ok()) {
        std::cerr << "Initiator stopped: " << run_status.message() << '\n' << std::flush;
        return 1;
    }
    if (timed_out || application_error) return 1;
    std::cerr << "Engine stopped before the user requested quit\n" << std::flush;
    return 1;
}
