#include "common.hpp"
#include "orders.hpp"
#include <nimblefix/fix44.hpp>

namespace nf = nimblefix;
using Schema = nf::fix44::Schema;

int main(int argc, char** argv) try {
    const auto options = example::options(argc, argv, true);
    if (options.help) { example::usage("initiator-example", true); return 0; }
    example::install_signals();

    auto engine = example::take(nf::Engine<Schema, nf::Queued>::create(example::config(options)));
    example::PendingRequests pending;
    example::check(engine.on_session_active([](nf::SessionContext&) {
        example::print("SESSION_ACTIVE");
        return nf::Status::success();
    }));
    example::check(engine.on<nf::fix44::ExecutionReport>(
        [&](nf::SessionContext&, const nf::fix44::ExecutionReportView& report) {
            const auto id = report.cl_ord_id();
            if (!id.ok()) return nf::Status::success(); // Unsolicited report without a client ID.
            const auto symbol = report.symbol();
            const auto side = example::take(report.side());
            example::print("EXECUTION_REPORT ClOrdID=", id.value(),
                " Symbol=", symbol.ok() ? symbol.value() : std::string_view{},
                " Side=", example::side_name(side),
                " ExecType=", example::take(report.exec_type()),
                " OrdStatus=", example::take(report.ord_status()));
            pending.received(id.value());
            return nf::Status::success();
        }));
    example::check(engine.on_error([](const nf::ErrorEvent& event) {
        example::error("ERROR ", event.format());
    }));

    const auto session = example::take(engine.add_initiator({
        .name="venue", .sender_comp_id="BUY", .target_comp_id="SELL",
        .endpoint={options.host, static_cast<std::uint16_t>(options.port)}}));
    auto sender = session.sender<Schema>();
    nf::Status run_status{};
    std::atomic<bool> finished{false};
    std::jthread protocol([&](std::stop_token stop) {
        std::stop_callback cleanup(stop, [&] { engine.request_stop(); });
        run_status = engine.run();
        finished.store(true);
    });
    if (!example::wait_until([&] { return sender.can_send(); }, options.timeout_ms, finished)) {
        example::error("Session did not become ready before the connection deadline");
        return example::stopping.load() ? 0 : 1;
    }

    // This is the business thread. Only explicit commands create orders.
    std::string line;
    while (example::read_line(line, finished)) {
        if (line == "quit") break;
        if (line == "status") {
            example::print("STATUS can_send=", sender.can_send() ? "yes" : "no");
            pending.print_receipts(sender);
            continue;
        }
        if (line.empty()) continue;
        try {
            const auto order = example::parse_order(line);
            pending.reserve(order.id);
            const auto sent = sender.send_stream<nf::fix44::NewOrderSingle>([&](auto& w) {
                w.cl_ord_id(order.id).handl_inst('1').symbol(order.symbol).side(order.side)
                 .transact_time(example::utc_now()).order_qty(order.quantity)
                 .ord_type(order.market ? '1' : '2');
                if (!order.market) w.price(order.price);
            });
            if (!sent.ok()) {
                pending.discard(order.id);
                example::error("REJECTED ClOrdID=", order.id, " reason=", sent.status().message());
                continue;
            }
            pending.accepted(order.id, sent.value().id);
            example::print("ACCEPTED ClOrdID=", order.id, " receipt=", sent.value().id);
        } catch (const std::invalid_argument& error) {
            example::error("ERROR ", error.what());
        }
    }
    const bool drained = pending.drain(sender, options.timeout_ms, finished);
    engine.request_stop();
    protocol.join();
    example::check(run_status);
    return drained ? 0 : 1;
} catch (const std::invalid_argument& error) {
    example::error(error.what());
    return 2;
} catch (const std::exception& error) {
    example::error(error.what());
    return 1;
}
