#include "common.hpp"
#include "orders.hpp"

namespace nf = nimblefix;
using Schema = nf::FlatSchema;

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
    example::check(engine.on_message([&](nf::SessionContext&, const nf::FlatMessageView& report) {
        if (example::field(report, 35) != "8") return nf::Status::success();
        const auto id = example::field(report, 11);
        const auto side = example::field(report, 54);
        example::print("EXECUTION_REPORT ClOrdID=", id, " Symbol=", example::field(report, 55),
            " Side=", side.empty() ? "unknown" : example::side_name(side.front()),
            " ExecType=", example::field(report, 150), " OrdStatus=", example::field(report, 39));
        pending.received(id);
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
            auto message = nf::message<"D">()
                .set<11>(order.id).set<21>('1').set<55>(order.symbol).set<54>(order.side)
                .set<60>(example::utc_now()).set<38>(order.quantity).set<40>(order.market ? '1' : '2');
            const auto sent = order.market ? sender.send(message)
                : sender.send(std::move(message).set<44>(order.price));
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
