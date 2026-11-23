#include <nimblefix/direct_initiator.hpp>
#include <nimblefix/fix44_sample.hpp>

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
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
        if (msg.is<fix::ExecutionReport>()) {
            const auto er = msg.as<fix::ExecutionReport>();
            if (er.exec_id().ok()) ++exec_reports;
        }
        (void)info;
        return nf::Status::success();
    }
};

} // namespace

int main(int argc, char** argv) {
    const std::string host = argc > 1 ? argv[1] : "127.0.0.1";
    const std::uint16_t port = argc > 2 ? static_cast<std::uint16_t>(std::stoi(argv[2])) : 9876;
    const std::string store_dir = argc > 3 ? argv[3] : "/tmp/nimblefix-direct-initiator-example";

    auto opened = nf::DirectInitiator<fix::Schema, OrderHandler>::open({
        .host = host,
        .port = port,
        .storage_directory = store_dir,
        .local_comp_id = "BUY",
        .remote_comp_id = "SELL",
    });
    if (!opened.ok()) {
        std::cerr << "open failed: " << opened.status().message() << '\n';
        return 1;
    }
    auto initiator = std::move(opened).value();
    if (const auto st = initiator.connect(); !st.ok()) {
        std::cerr << "connect failed: " << st.message() << '\n';
        return 1;
    }

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

    // 零声明 + 0 堆分配（SSO 256B，超长自动溢到堆）+ 支持原生 if：
    fix::NewOrderSingle::Draft order;
    const bool is_limit = false;
    order.cl_ord_id("ORD-0001")
         .symbol("AAPL")
         .side('1')
         .transact_time("20261123-00:00:00.000")
         .order_qty(nf::Decimal{100, 0})
         .ord_type(is_limit ? '2' : '1');
    if (is_limit) order.price(nf::Decimal{15025, 2});

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
