#include <nimblefix/engine.hpp>
#include <nimblefix/fix44.hpp>

#include <chrono>
#include <filesystem>

namespace nf = nimblefix;
using namespace nf::literals;

int main() {
    if (!nf::tls_available()) return 1;

    using TypedQueuedEngine = nf::Engine<nf::fix44::Schema, nf::Queued>;
    nf::EngineConfig config;
    config.storage.directory = std::filesystem::current_path() / "typed-queued-state";
    config.queued.worker_threads = 1;
    config.queued.max_pending_callbacks = 8;
    auto created = TypedQueuedEngine::create(std::move(config));
    if (!created.ok()) return 2;
    auto engine = std::move(created).value();

    if (!engine.on<nf::fix44::NewOrderSingle>(
            [](nf::SessionContext&, nf::fix44::NewOrderSingleView message) {
                return message.cl_ord_id().ok() ? nf::Status::success()
                                                : nf::Status{nf::ErrorCode::invalid_value, 11};
            }).ok()) return 3;
    if (!engine.on_session_active([](nf::SessionContext&) { return nf::Status::success(); }).ok()) return 4;

    const auto listener = engine.add_listener({
        .name = "installed-manual-listener",
        .endpoint = {"127.0.0.1", 0},
    });
    if (!listener.ok()) return 5;
    const auto acceptor = engine.add_acceptor({
        .name = "installed-fix44-session",
        .listener = listener.value().name,
        .local_comp_id = "SELL",
        .remote_comp_id = "BUY",
    });
    if (!acceptor.ok()) return 6;

    auto write_order = [](auto& w) {
        w.cl_ord_id("INSTALLED-ORDER")
         .handl_inst('1')
         .symbol("AAPL")
         .side('1')
         .transact_time("20261031-12:00:00.000")
         .order_qty("1"_dec)
         .ord_type('1');
    };
    auto sender = acceptor.value().sender<nf::fix44::Schema>();
    auto lane = acceptor.value().producer_lane<nf::fix44::Schema>();
    if (!lane.ok()) return 13;
    if (lane.value().send_stream<nf::fix44::NewOrderSingle>(write_order).status().code != nf::ErrorCode::not_ready) return 14;
    const auto not_active_yet = sender.send_stream<nf::fix44::NewOrderSingle>(write_order);
    if (not_active_yet.ok() || not_active_yet.status().code != nf::ErrorCode::not_ready) return 7;

    if (!engine.validate().ok() || !engine.start().ok()) return 8;
    const auto endpoint = engine.listener_endpoint(listener.value().name);
    if (!endpoint.ok() || endpoint.value().port == 0) return 9;
    if (!engine.poll_once(0).ok()) return 10;

    engine.request_stop();
    const auto stop_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (engine.running() && std::chrono::steady_clock::now() < stop_deadline) {
        const auto status = engine.poll_once(10);
        if (!status.ok()) return 11;
    }
    return engine.running() ? 12 : 0;
}
