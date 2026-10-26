#include "endpoint.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

#include <nimblefix/decimal.hpp>
#include <nimblefix/message.hpp>

namespace nf = nimblefix;
namespace demo = nimblefix_example;
using namespace nf::literals;

namespace {

static_assert(std::atomic<bool>::is_always_lock_free);
std::atomic<bool> stop_signal{false};

extern "C" void handle_signal(int) {
    stop_signal.store(true, std::memory_order_relaxed);
}

std::string_view field_value(const nf::FlatMessageView& message, std::uint32_t tag,
    nf::Status& status) noexcept {
    const auto found = message.find_unique(tag);
    if (!found.ok()) {
        status = found.status();
        return {};
    }
    return found.value().value;
}

bool is_expected_report(const nf::FlatMessageView& message,
    std::string_view cl_ord_id) noexcept {
    nf::Status status{};
    const auto response_id = field_value(message, 11, status);
    if (!status.ok() || response_id != cl_ord_id) return false;
    const auto side = field_value(message, 54, status);
    if (!status.ok() || side != "1") return false;
    const auto leaves = field_value(message, 151, status);
    if (!status.ok()) return false;
    const auto cumulative = field_value(message, 14, status);
    if (!status.ok()) return false;
    const auto average = field_value(message, 6, status);
    if (!status.ok()) return false;
    const auto symbol = field_value(message, 55, status);
    if (!status.ok() || symbol != "AAPL") return false;
    const auto exec_type = field_value(message, 150, status);
    if (!status.ok() || exec_type != "0") return false;
    const auto order_status = field_value(message, 39, status);
    if (!status.ok() || order_status != "0") return false;
    const auto order_id = field_value(message, 37, status);
    if (!status.ok() || !order_id.starts_with("ORD-")) return false;
    const auto exec_id = field_value(message, 17, status);
    if (!status.ok() || !exec_id.starts_with("EXEC-")) return false;
    const auto transact_time = field_value(message, 60, status);
    if (!status.ok() || !demo::timestamp_shaped(transact_time)) return false;

    const auto leaves_value = nf::Decimal::parse(leaves);
    const auto cumulative_value = nf::Decimal::parse(cumulative);
    const auto average_value = nf::Decimal::parse(average);
    const auto expected_leaves = nf::Decimal::parse("100");
    const auto zero = nf::Decimal::parse("0");
    return leaves_value.ok() && expected_leaves.ok() &&
        leaves_value.value() == expected_leaves.value() &&
        cumulative_value.ok() && zero.ok() && cumulative_value.value() == zero.value() &&
        average_value.ok() && average_value.value() == zero.value();
}

void publish_sent(demo::ConsoleEvents& events, std::string_view cl_ord_id,
    const nf::SendReceipt& receipt) noexcept {
    std::array<char, 192> text{};
    std::size_t used = 0;
    const auto append = [&](std::string_view part) {
        const auto count = std::min(part.size(), text.size() - used);
        if (count != 0) std::copy_n(part.data(), count, text.data() + used);
        used += count;
    };
    append("accepted ClOrdID ");
    append(cl_ord_id);
    append(" at sequence ");
    const auto sequence = std::to_chars(text.data() + used, text.data() + text.size(), receipt.msg_seq_num);
    if (sequence.ec == std::errc{}) used = static_cast<std::size_t>(sequence.ptr - text.data());
    append(" (receipt ");
    const auto id = std::to_chars(text.data() + used, text.data() + text.size(), receipt.id);
    if (id.ec == std::errc{}) used = static_cast<std::size_t>(id.ptr - text.data());
    append(")");
    events.push(demo::ConsoleEvent::Kind::message, {text.data(), used});
}

std::string make_default_cl_ord_id(std::string_view timestamp) {
    std::string id{"DEMO-"};
    id.reserve(64);
    id.append(timestamp);
    id.push_back('-');
    id.append(std::to_string(static_cast<unsigned long long>(::getpid())));
    return id;
}

} // namespace

int main(int argc, char** argv) {
    demo::Options options;
    if (!demo::parse_args(argc, argv, true, options)) {
        demo::print_usage("initiator-example", true);
        return 2;
    }
    if (options.help) {
        demo::print_usage("initiator-example", true);
        return 0;
    }

    std::array<char, 22> time_storage{};
    const auto initial_time = demo::utc_timestamp(time_storage);
    if (initial_time.empty()) {
        std::cerr << "could not read UTC clock\n";
        return 1;
    }
    const std::string transact_time{initial_time};
    if (options.cl_ord_id.empty()) options.cl_ord_id = make_default_cl_ord_id(transact_time);

    nf::EngineConfig config;
    config.storage.directory = options.state;
    config.logon_timeout_ms = options.timeout_ms;
    config.logout_timeout_ms = 10'000;
    config.connect_timeout_ms = options.timeout_ms;
    auto created = nf::Engine<>::create(std::move(config));
    if (!created.ok()) {
        std::cerr << "Engine create failed: " << created.status().message() << '\n';
        return 1;
    }
    auto engine = std::move(created).value();
    demo::ConsoleEvents events;
    std::atomic<bool> order_accepted{false};
    std::atomic<bool> report_received{false};
    std::atomic<bool> invalid_report{false};

    auto active_callback = engine.on_session_active(
        [&events, &order_accepted, &options, &transact_time](nf::SessionContext& context) -> nf::Status {
            if (order_accepted.load(std::memory_order_acquire)) return nf::Status::success();
            auto new_order = nf::message<"D">()
                .set<11>(options.cl_ord_id)
                .set<55>("AAPL")
                .set<54>('1')
                .set<60>(transact_time)
                .set<38>("100"_dec)
                .set<40>('2')
                .set<44>("150.25"_dec);
            const auto sent = context.send(new_order);
            if (!sent.ok()) {
                demo::push_status(events, "send NewOrderSingle", sent.status());
                return sent.status();
            }
            order_accepted.store(true, std::memory_order_release);
            publish_sent(events, options.cl_ord_id, sent.value());
            return nf::Status::success();
        });
    if (!active_callback.ok()) {
        std::cerr << "on_session_active failed: " << active_callback.message() << '\n';
        return 1;
    }

    auto message_callback = engine.on_message(
        [&events, &options, &report_received, &invalid_report](nf::SessionContext&,
            const nf::FlatMessageView& message) -> nf::Status {
            if (!demo::is_message(message, "8")) return nf::Status::success();
            nf::Status status{};
            const auto response_id = field_value(message, 11, status);
            if (!status.ok() || response_id != options.cl_ord_id) return nf::Status::success();
            if (!is_expected_report(message, options.cl_ord_id)) {
                invalid_report.store(true, std::memory_order_release);
                events.push(demo::ConsoleEvent::Kind::error,
                    "matching ExecutionReport did not contain expected New acknowledgement",
                    nf::ErrorCode::invalid_value);
                return nf::Status::success();
            }
            if (!report_received.exchange(true, std::memory_order_acq_rel)) {
                events.push(demo::ConsoleEvent::Kind::message,
                    "received matching New ExecutionReport");
            }
            return nf::Status::success();
        });
    if (!message_callback.ok()) {
        std::cerr << "on_message failed: " << message_callback.message() << '\n';
        return 1;
    }
    auto error_callback = engine.on_error([&events](const nf::ErrorEvent& error) {
        events.push_error(error);
    });
    if (!error_callback.ok()) {
        std::cerr << "on_error failed: " << error_callback.message() << '\n';
        return 1;
    }
    const auto session = engine.add_initiator({
        .name = "demo-initiator",
        .sender_comp_id = "BUY",
        .target_comp_id = "SELL",
        .endpoint = {options.host, options.port},
    });
    if (!session.ok()) {
        std::cerr << "add_initiator failed: " << session.status().message() << '\n';
        return 1;
    }
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

    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(options.timeout_ms);
    bool stop_requested = false;
    bool timed_out = false;
    std::uint64_t dropped_events = 0;
    while (!run_done.load(std::memory_order_acquire) &&
        !stop_signal.load(std::memory_order_relaxed) &&
        !report_received.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() >= deadline) {
            timed_out = true;
            break;
        }
        demo::print_events(events, dropped_events);
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (!run_done.load(std::memory_order_acquire)) {
        engine.request_stop();
        stop_requested = true;
    }
    while (!run_done.load(std::memory_order_acquire)) {
        demo::print_events(events, dropped_events);
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    owner.join();
    demo::print_events(events, dropped_events);
    if (!run_status.ok()) {
        std::cerr << "Initiator stopped: " << run_status.message() << '\n';
        return 1;
    }
    if (timed_out && !report_received.load(std::memory_order_acquire)) {
        std::cerr << "Timed out waiting for the matching ExecutionReport after "
            << options.timeout_ms << " ms\n";
        return 1;
    }
    if (stop_signal.load(std::memory_order_relaxed) &&
        !report_received.load(std::memory_order_acquire)) {
        std::cerr << "Stopped by signal before the order was acknowledged\n";
        return 130;
    }
    if (!report_received.load(std::memory_order_acquire)) {
        std::cerr << "Engine stopped before a matching ExecutionReport arrived\n";
        return 1;
    }
    if (stop_requested) std::cout << "Initiator stopped gracefully.\n";
    return invalid_report.load(std::memory_order_acquire) ? 1 : 0;
}
