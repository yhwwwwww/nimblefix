#include <nimblefix/direct_initiator.hpp>
#include <nimblefix/fix44_sample.hpp>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace nf = nimblefix;
namespace fix = nimblefix::fix44_sample;

namespace {

struct OrderHandler {
    std::uint64_t exec_reports{};
    bool active{};

    [[nodiscard]] nf::Status on_active() noexcept {
        active = true;
        return nf::Status::success();
    }

    [[nodiscard]] nf::Status on_message(const nf::FlatMessageView& msg,
                                        const nf::DeliveryInfo& info) noexcept {
        const auto mt = msg.find_unique(35);
        if (mt.ok() && mt.value().value == "8") ++exec_reports;
        (void)info;
        return nf::Status::success();
    }
};

} // namespace

int main(int argc, char** argv) {
    const std::string host = argc > 1 ? argv[1] : "127.0.0.1";
    const std::uint16_t port = argc > 2 ? static_cast<std::uint16_t>(std::stoi(argv[2])) : 9876;
    const std::string store_dir = argc > 3 ? argv[3] : "/tmp/nimblefix-direct-initiator-example";

    nf::DirectInitiatorConfig cfg{};
    cfg.host = host;
    cfg.port = port;
    cfg.storage_directory = store_dir;
    cfg.local_comp_id = "BUY";
    cfg.remote_comp_id = "SELL";
    cfg.heartbeat_ms = 30'000;

    auto opened = nf::DirectInitiator<fix::Schema, OrderHandler>::open(std::move(cfg));
    if (!opened.ok()) {
        std::cerr << "open failed: " << opened.status().message() << '\n';
        return 1;
    }
    auto initiator = std::move(opened).value();
    if (const auto st = initiator.connect(); !st.ok()) {
        std::cerr << "connect failed: " << st.message() << '\n';
        return 1;
    }

    // Reusable stack-allocated FixedMessage: zero heap allocations per order.
    using OrderShape = nf::Fields<
        nf::Field<11, nf::FieldKind::string>,
        nf::Field<55, nf::FieldKind::string>,
        nf::Field<54, nf::FieldKind::character>,
        nf::Field<60, nf::FieldKind::string>,
        nf::Field<38, nf::FieldKind::decimal>,
        nf::Field<40, nf::FieldKind::character>>;
    fix::NewOrderSingleFixed<OrderShape, 128> order;
    for (int step = 0; step < 200 && !initiator.is_active(); ++step) {
        if (const auto st = initiator.poll_once(); !st.ok()) {
            std::cerr << "poll failed: " << st.message() << '\n';
            return 1;
        }
        ::usleep(1000);
    }
    if (!initiator.is_active()) {
        std::cerr << "did not become active\n";
        return 1;
    }

    order.reset();
    (void)order.cl_ord_id("ORD-0001");
    (void)order.symbol("AAPL");
    (void)order.side('1');
    (void)order.transact_time("20261123-00:00:00.000");
    (void)order.order_qty(nf::Decimal{100, 0});
    (void)order.ord_type('1');

    const auto seq = initiator.send(order);
    if (!seq.ok()) {
        std::cerr << "send failed: " << seq.status().message() << '\n';
        return 1;
    }
    std::cout << "sent order seq=" << seq.value() << '\n';
    (void)initiator.logout("example done");
    for (int step = 0; step < 50 && initiator.valid_transport(); ++step) {
        (void)initiator.poll_once();
        ::usleep(1000);
    }
    return 0;
}
