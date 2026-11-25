#include "common.hpp"
#include <nimblefix/fix44.hpp>

namespace nf = nimblefix;
using namespace nf::literals;

int main(int argc, char** argv) try {
    const auto options = example::options(argc, argv, false);
    if (options.help) { example::usage("acceptor-example", false); return 0; }
    example::install_signals();

    auto engine = example::take(nf::Engine<nf::fix44::Schema, nf::Queued>::create(example::config(options)));
    example::check(engine.on<nf::fix44::NewOrderSingle>(
        [](nf::SessionContext& context, const nf::fix44::NewOrderSingleView& order) {
            const auto id = example::take(order.cl_ord_id());
            const auto quantity = example::take(order.order_qty());
            const bool accepted = quantity.coefficient() > 0;
            // A simulated broker acknowledges the request. IDs and payload are
            // deterministic so interrupted callback delivery can recover.
            const auto record = std::to_string(context.inbound_record_id());
            return context.send_stream<nf::fix44::ExecutionReport>([&](auto& w) {
                w.order_id("ORD-" + record).exec_id("EXEC-" + record).cl_ord_id(id)
                 .exec_type(accepted ? '0' : '8').ord_status(accepted ? '0' : '8')
                 .symbol(example::take(order.symbol())).side(example::take(order.side()))
                 .leaves_qty(accepted ? quantity : "0"_dec).cum_qty("0"_dec).avg_px("0"_dec)
                 .transact_time(example::take(order.transact_time()));
            }).status();
        }));
    example::check(engine.on_error([](const nf::ErrorEvent& event) {
        example::error("ERROR ", event.format());
    }));
    example::take(engine.add_listener({.name="orders",
        .endpoint={options.host, static_cast<std::uint16_t>(options.port)}}));
    example::take(engine.add_acceptor({.name="client", .listener="orders",
        .local_comp_id="SELL", .remote_comp_id="BUY"}));

    // The main thread owns protocol polling; Queued callbacks handle business
    // messages on workers. Polling also lets SIGINT/SIGTERM request Logout.
    example::check(engine.start());
    const auto endpoint = example::take(engine.listener_endpoint("orders"));
    example::print("LISTENING ", endpoint.host.find(':') == std::string::npos
        ? endpoint.host : "[" + endpoint.host + "]", ":", endpoint.port);
    while (!example::stopping.load() && engine.running()) example::check(engine.poll_once(100));
    engine.request_stop();
    while (engine.running()) example::check(engine.poll_once(100));
    return 0;
} catch (const std::invalid_argument& error) {
    example::error(error.what());
    return 2;
} catch (const std::exception& error) {
    example::error(error.what());
    return 1;
}
