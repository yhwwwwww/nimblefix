#include "common.hpp"

namespace nf = nimblefix;
using namespace nf::literals;

int main(int argc, char** argv) try {
    const auto options = example::options(argc, argv, false);
    if (options.help) { example::usage("acceptor-example", false); return 0; }
    example::install_signals();

    auto engine = example::take(nf::Engine<nf::FlatSchema, nf::Queued>::create(example::config(options)));
    example::check(engine.on_message([](nf::SessionContext& context, const nf::FlatMessageView& order) {
        if (example::field(order, 35) != "D") return nf::Status::success();
        const auto quantity = example::take(nf::Decimal::parse(example::field(order, 38)));
        const auto side = example::field(order, 54);
        if (side.size() != 1) return nf::Status{nf::ErrorCode::invalid_value, 54};
        const bool accepted = quantity.coefficient() > 0;
        const auto record = std::to_string(context.inbound_record_id());
        auto report = nf::message<"8">()
            .set<37>("ORD-" + record).set<17>("EXEC-" + record).set<11>(example::field(order, 11))
            .set<150>(accepted ? '0' : '8').set<39>(accepted ? '0' : '8')
            .set<55>(example::field(order, 55)).set<54>(side.front())
            .set<151>(accepted ? quantity : "0"_dec).set<14>("0"_dec).set<6>("0"_dec)
            .set<60>(example::field(order, 60));
        return context.send(report).status();
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
