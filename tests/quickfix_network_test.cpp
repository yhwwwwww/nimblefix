#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <thread>
#include <unistd.h>

#include <quickfix/Application.h>
#include <quickfix/FileStore.h>
#include <quickfix/Session.h>
#include <quickfix/SessionSettings.h>
#include <quickfix/SocketInitiator.h>
#include <quickfix/fix44/NewOrderSingle.h>
#include <nimblefix/engine.hpp>

using namespace std::chrono_literals;
namespace nf = nimblefix;

struct Peer final : FIX::Application {
    std::string identifier;
    std::atomic<bool> received{false};
    std::atomic<bool> logged_out{false};
    explicit Peer(std::string id) : identifier(std::move(id)) {}
    void onCreate(const FIX::SessionID&) override {}
    void onLogout(const FIX::SessionID&) override { logged_out = true; }
    void onLogon(const FIX::SessionID& id) override {
        FIX44::NewOrderSingle order(FIX::ClOrdID(identifier), FIX::Side('1'),
            FIX::TransactTime(FIX::UtcTimeStamp::now()), FIX::OrdType('2'));
        order.set(FIX::Symbol("AAPL")); order.set(FIX::HandlInst('1'));
        order.set(FIX::OrderQty(100)); order.set(FIX::Price(150.25));
        FIX::Session::sendToTarget(order, id);
    }
    void toAdmin(FIX::Message&, const FIX::SessionID&) override {}
    void toApp(FIX::Message&, const FIX::SessionID&) override {}
    void fromAdmin(const FIX::Message&, const FIX::SessionID&) override {}
    void fromApp(const FIX::Message& message, const FIX::SessionID&) override {
        if (message.getHeader().getField(35) == "8" && message.getField(11) == identifier &&
            message.getField(39) == "0" && message.getField(151) == "100") received = true;
    }
};

bool exchange(const std::filesystem::path& directory, const std::string& dictionary, unsigned pass) {
    auto created = nf::Engine<>::create({.storage={.directory=directory/"nimble"}, .logout_timeout_ms=1000});
    if (!created.ok()) return false;
    auto engine = std::move(created).value();
    std::atomic<unsigned> delivered{0};
    if (!engine.on_message([&](nf::SessionContext& context, const nf::FlatMessageView& message) {
        const auto id = message.find_unique(11);
        if (!id.ok()) return id.status();
        auto reply = nf::message<"8">().set<37>("ORDER-1").set<17>("EXEC-1")
            .set<11>(id.value().value).set<150>('0').set<39>('0').set<55>("AAPL")
            .set<54>('1').set<151>(100).set<14>(0).set<6>(nf::Decimal{0});
        const auto sent = context.send(reply);
        if (sent.ok()) ++delivered;
        return sent.status();
    }).ok()) return false;
    if (!engine.on_error([](const auto& error) { std::cerr << "NimbleFIX: " << error.format() << '\n'; }).ok()) return false;
    if (!engine.add_listener({.name="main",.endpoint={"127.0.0.1",0}}).ok()) return false;
    if (!engine.add_acceptor({.name="qf",.listener="main",.local_comp_id="SELL",.remote_comp_id="BUY"}).ok()) return false;
    nf::Status run_status;
    std::thread owner([&] { run_status = engine.run(); });
    auto shutdown = [&] { engine.request_stop(); if (owner.joinable()) owner.join(); };
    try {
        std::uint16_t port = 0;
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!port && std::chrono::steady_clock::now() < deadline) {
            const auto endpoint = engine.listener_endpoint("main");
            if (endpoint.ok()) port = endpoint.value().port;
            else std::this_thread::sleep_for(1ms);
        }
        if (!port) { shutdown(); return false; }
        std::ostringstream configuration;
        configuration << "[DEFAULT]\nConnectionType=initiator\nStartTime=00:00:00\nEndTime=00:00:00\n"
            "HeartBtInt=30\nReconnectInterval=1\nSocketConnectHost=127.0.0.1\nSocketNodelay=Y\n"
            "UseDataDictionary=Y\nResetOnLogon=N\nResetOnLogout=N\nResetOnDisconnect=N\n"
            "FileStorePath=" << (directory/"quickfix").string() << "\nDataDictionary=" << dictionary <<
            "\n[SESSION]\nBeginString=FIX.4.4\nSenderCompID=BUY\nTargetCompID=SELL\nSocketConnectPort=" << port << "\n";
        std::istringstream input(configuration.str());
        FIX::SessionSettings settings(input);
        FIX::FileStoreFactory stores(settings);
        Peer application("QF-" + std::to_string(pass));
        FIX::SocketInitiator initiator(application, stores, settings);
        initiator.start();
        const auto complete_by = std::chrono::steady_clock::now() + 10s;
        while (!application.received && std::chrono::steady_clock::now() < complete_by)
            std::this_thread::sleep_for(2ms);
        initiator.stop();
        const bool passed = application.received && application.logged_out && delivered == 1;
        shutdown();
        return passed && run_status.ok();
    } catch (...) { shutdown(); throw; }
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    char pattern[] = "/tmp/nimblefix-qf-network-XXXXXX";
    const auto directory = ::mkdtemp(pattern);
    if (!directory) return 1;
    bool passed = false;
    try { passed = exchange(directory, argv[1], 1) && exchange(directory, argv[1], 2); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    std::filesystem::remove_all(directory);
    if (!passed) { std::cerr << "QuickFIX TCP/restart interoperability failed\n"; return 1; }
    std::cout << "QuickFIX TCP Logon/order/report/Logout and persistent restart passed\n";
}
