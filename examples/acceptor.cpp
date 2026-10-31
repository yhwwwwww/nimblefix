#include "endpoint.hpp"

#if defined(NIMBLEFIX_BUILTIN_FIX44)
#include <nimblefix/fix44.hpp>
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <thread>

namespace nf = nimblefix;
namespace demo = nimblefix_example;
using namespace nf::literals;
#if defined(NIMBLEFIX_BUILTIN_FIX44)
using Schema = nf::fix44::Schema;
#else
using Schema = nf::FlatSchema;
#endif

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

bool valid_transact_time(std::string_view value) noexcept {
    if (!demo::timestamp_shaped(value)) return false;
    constexpr std::array<unsigned, 12> month_days{
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const auto two_digits = [value](std::size_t position) noexcept {
        return static_cast<unsigned>((value[position] - '0') * 10 + (value[position + 1] - '0'));
    };
    const auto month = two_digits(4);
    const auto day = two_digits(6);
    const auto hour = two_digits(9);
    const auto minute = two_digits(12);
    const auto second = two_digits(15);
    if (month < 1 || month > 12 || hour > 23 || minute > 59 || second > 60) return false;
    const auto year = static_cast<unsigned>((value[0] - '0') * 1000 +
        (value[1] - '0') * 100 + (value[2] - '0') * 10 + (value[3] - '0'));
    auto max_day = month_days[month - 1];
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) ++max_day;
    return day >= 1 && day <= max_day;
}

std::string_view make_id(std::array<char, 40>& buffer, std::string_view prefix,
    std::uint64_t record_id) noexcept {
    if (prefix.size() >= buffer.size()) return {};
    std::copy(prefix.begin(), prefix.end(), buffer.begin());
    const auto converted = std::to_chars(buffer.data() + prefix.size(),
        buffer.data() + buffer.size(), record_id);
    if (converted.ec != std::errc{}) return {};
    return {buffer.data(), static_cast<std::size_t>(converted.ptr - buffer.data())};
}

void publish_accepted(demo::ConsoleEvents& events, std::string_view cl_ord_id,
    const nf::SendReceipt& receipt) noexcept {
    std::array<char, 192> text{};
    std::size_t used = 0;
    const auto append = [&](std::string_view part) {
        const auto count = std::min(part.size(), text.size() - used);
        if (count != 0) std::copy_n(part.data(), count, text.data() + used);
        used += count;
    };
    append("accepted order ");
    append(cl_ord_id);
    append("; outbound sequence ");
    const auto sequence = std::to_chars(text.data() + used, text.data() + text.size(), receipt.msg_seq_num);
    if (sequence.ec == std::errc{}) used = static_cast<std::size_t>(sequence.ptr - text.data());
    events.push(demo::ConsoleEvent::Kind::message, {text.data(), used});
}

} // namespace

int main(int argc, char** argv) {
    demo::Options options;
    if (!demo::parse_args(argc, argv, false, options)) {
        demo::print_usage("acceptor-example", false);
        return 2;
    }
    if (options.help) {
        demo::print_usage("acceptor-example", false);
        return 0;
    }

    nf::EngineConfig config;
    config.storage.directory = options.state;
    config.logon_timeout_ms = options.timeout_ms;
    config.logout_timeout_ms = options.timeout_ms;
    auto created = nf::Engine<Schema>::create(std::move(config));
    if (!created.ok()) {
        std::cerr << "Engine create failed: " << created.status().message() << '\n';
        return 1;
    }
    auto engine = std::move(created).value();
    demo::ConsoleEvents events;
    demo::ConsoleEvents errors;
    auto handle_order = [&events](nf::SessionContext& context,
        const nf::FlatMessageView& incoming) -> nf::Status {
            if (!demo::is_message(incoming, "D")) return nf::Status::success();

            nf::Status status{};
            const auto cl_ord_id = field_value(incoming, 11, status);
            if (!status.ok()) return {nf::ErrorCode::invalid_value, 11};
            const auto symbol = field_value(incoming, 55, status);
            if (!status.ok()) return {nf::ErrorCode::invalid_value, 55};
            const auto side_text = field_value(incoming, 54, status);
            if (!status.ok() || side_text.size() != 1 ||
                (side_text.front() != '1' && side_text.front() != '2')) {
                return {nf::ErrorCode::invalid_value, 54};
            }
            const auto transact_time = field_value(incoming, 60, status);
            if (!status.ok() || !valid_transact_time(transact_time)) {
                return {nf::ErrorCode::invalid_timestamp, 60};
            }
            const auto quantity_text = field_value(incoming, 38, status);
            if (!status.ok()) return {nf::ErrorCode::invalid_value, 38};
            const auto quantity = nf::Decimal::parse(quantity_text);
            if (!quantity.ok() || quantity.value().coefficient() <= 0) {
                return {nf::ErrorCode::invalid_decimal, 38};
            }

            std::array<char, 40> order_id_storage{};
            std::array<char, 40> exec_id_storage{};
            const auto order_id = make_id(order_id_storage, "ORD-", context.inbound_record_id());
            const auto exec_id = make_id(exec_id_storage, "EXEC-", context.inbound_record_id());
            if (order_id.empty() || exec_id.empty()) return {nf::ErrorCode::too_large};

#if defined(NIMBLEFIX_BUILTIN_FIX44)
            auto report = nf::fix44::ExecutionReport::make()
                .order_id(order_id).exec_id(exec_id).cl_ord_id(cl_ord_id)
                .exec_type('0').ord_status('0').symbol(symbol).side(side_text.front())
                .leaves_qty(quantity.value()).cum_qty("0"_dec).avg_px("0"_dec)
                .transact_time(transact_time);
#else
            auto report = nf::message<"8">()
                .set<37>(order_id)
                .set<17>(exec_id)
                .set<11>(cl_ord_id)
                .set<150>('0')
                .set<39>('0')
                .set<55>(symbol)
                .set<54>(side_text.front())
                .set<151>(quantity.value())
                .set<14>("0"_dec)
                .set<6>("0"_dec)
                .set<60>(transact_time);
#endif
            const auto sent = context.send(report);
            if (!sent.ok()) return sent.status();
            publish_accepted(events, cl_ord_id, sent.value());
            return nf::Status::success();
        };
#if defined(NIMBLEFIX_BUILTIN_FIX44)
    auto message_callback = engine.on<nf::fix44::NewOrderSingle>(
        [handle_order](nf::SessionContext& context, const nf::fix44::NewOrderSingleView& order) {
            return handle_order(context, order.flat());
        });
#else
    auto message_callback = engine.on_message(handle_order);
#endif
    if (!message_callback.ok()) {
        std::cerr << "on_message failed: " << message_callback.message() << '\n';
        return 1;
    }
    auto error_callback = engine.on_error([&errors](const nf::ErrorEvent& error) {
        errors.push_error(error);
    });
    if (!error_callback.ok()) {
        std::cerr << "on_error failed: " << error_callback.message() << '\n';
        return 1;
    }
    const auto listener = engine.add_listener({
        .name = "demo-listener",
        .endpoint = {options.host, options.port},
    });
    if (!listener.ok()) {
        std::cerr << "add_listener failed: " << listener.status().message() << '\n';
        return 1;
    }
    const auto acceptor = engine.add_acceptor({
        .name = "demo-acceptor",
        .listener = "demo-listener",
        .local_comp_id = "SELL",
        .remote_comp_id = "BUY",
    });
    if (!acceptor.ok()) {
        std::cerr << "add_acceptor failed: " << acceptor.status().message() << '\n';
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

    bool endpoint_printed = false;
    bool stop_requested = false;
    std::uint64_t dropped_events = 0;
    std::uint64_t dropped_errors = 0;
    while (!run_done.load(std::memory_order_acquire) &&
        !stop_signal.load(std::memory_order_relaxed)) {
        if (!endpoint_printed) {
            const auto endpoint = engine.listener_endpoint("demo-listener");
            if (endpoint.ok()) {
                if (endpoint.value().host.find(':') != std::string::npos)
                    std::cout << "LISTENING [" << endpoint.value().host << "]:" << endpoint.value().port << '\n';
                else
                    std::cout << "LISTENING " << endpoint.value().host << ':' << endpoint.value().port << '\n';
                std::cout.flush();
                endpoint_printed = true;
            } else if (endpoint.status().code != nf::ErrorCode::need_more) {
                std::cerr << "listener endpoint failed: " << endpoint.status().message() << '\n';
                engine.request_stop();
                stop_requested = true;
                break;
            }
        }
        demo::print_events(events, dropped_events);
        demo::print_events(errors, dropped_errors);
        std::cout.flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (!run_done.load(std::memory_order_acquire)) {
        engine.request_stop();
        stop_requested = true;
    }
    owner.join();
    demo::print_events(events, dropped_events);
    demo::print_events(errors, dropped_errors);
    if (!run_status.ok()) {
        std::cerr << "Acceptor stopped: " << run_status.message() << '\n';
        return 1;
    }
    if (stop_requested) std::cout << "Acceptor stopped gracefully.\n";
    return 0;
}
