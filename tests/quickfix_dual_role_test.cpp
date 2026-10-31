#include <nimblefix/engine.hpp>
#include <nimblefix/fix44.hpp>

#include <atomic>
#include <array>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#include <quickfix/Application.h>
#include <quickfix/FileStore.h>
#include <quickfix/Session.h>
#include <quickfix/SessionSettings.h>
#include <quickfix/SocketAcceptor.h>
#include <quickfix/fix44/ExecutionReport.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

namespace nf = nimblefix;
using namespace std::chrono_literals;

namespace {
struct Peer final : FIX::Application {
    std::string expected;
    std::atomic<unsigned> orders{};
    std::atomic<bool> logged_out{};
    explicit Peer(std::string identifier) : expected(std::move(identifier)) {}
    void onCreate(const FIX::SessionID&) override {}
    void onLogon(const FIX::SessionID&) override {}
    void onLogout(const FIX::SessionID&) override { logged_out = true; }
    void toAdmin(FIX::Message&, const FIX::SessionID&) override {}
    void toApp(FIX::Message&, const FIX::SessionID&) override {}
    void fromAdmin(const FIX::Message&, const FIX::SessionID&) override {}
    void fromApp(const FIX::Message& message, const FIX::SessionID& session) override {
        if (message.getHeader().getField(35) != "D" || message.getField(11) != expected) return;
        FIX44::ExecutionReport report(FIX::OrderID(expected), FIX::ExecID(expected), FIX::ExecType('0'),
            FIX::OrdStatus('0'), FIX::Side('1'), FIX::LeavesQty(100), FIX::CumQty(0), FIX::AvgPx(0));
        report.set(FIX::ClOrdID(expected));
        report.set(FIX::Symbol(message.getField(55)));
        FIX::Session::sendToTarget(report, session);
        ++orders;
    }
};

std::uint16_t available_port() {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) throw std::runtime_error("port probe socket failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd); throw std::runtime_error("port probe bind failed");
    }
    socklen_t size = sizeof(address);
    const auto status = ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size);
    ::close(fd);
    if (status != 0) throw std::runtime_error("port probe endpoint failed");
    return ntohs(address.sin_port);
}

template<class Policy>
bool exchange(const std::filesystem::path& root, const std::string& dictionary, unsigned pass) {
    std::filesystem::create_directories(root / "quickfix");
    const auto identifier = "INDEPENDENT-" + std::to_string(pass);
    const auto requested_port = available_port();
    std::ostringstream config;
    config << "[DEFAULT]\nConnectionType=acceptor\nStartTime=00:00:00\nEndTime=00:00:00\n"
        << "SocketAcceptPort=" << requested_port << "\nSocketNodelay=Y\nHeartBtInt=30\nUseDataDictionary=Y\n"
        "ResetOnLogon=N\nResetOnLogout=N\nResetOnDisconnect=N\n"
        "DataDictionary=" << dictionary << "\nFileStorePath=" << (root / "quickfix").string()
        << "\n[SESSION]\nBeginString=FIX.4.4\nSenderCompID=SELL\nTargetCompID=BUY\n";
    std::istringstream input{config.str()};
    FIX::SessionSettings settings{input};
    FIX::FileStoreFactory stores{settings};
    Peer peer{identifier};
    FIX::SocketAcceptor server{peer, stores, settings};
    server.start();
    if (server.sessionToPort().empty()) { server.stop(); return false; }
    const auto port = server.sessionToPort().begin()->second;

    std::atomic<bool> received{}, correct_thread{};
    std::atomic<unsigned> errors{};
    std::thread::id owner_id;
    auto opened = nf::Engine<nf::fix44::Schema, Policy>::create({
        .storage={.directory=root / "nimblefix"}, .logout_timeout_ms=1000});
    if (!opened.ok()) { server.stop(); return false; }
    auto engine = std::move(opened).value();
    if (!engine.template on<nf::fix44::ExecutionReport>(
        [&](nf::SessionContext&, const nf::fix44::ExecutionReport::view_type& report) {
            correct_thread = std::is_same_v<Policy, nf::Inline>
                ? std::this_thread::get_id() == owner_id : std::this_thread::get_id() != owner_id;
            const auto id = report.template get<11>();
            const auto leaves = report.template get<151>();
            if (!id.ok() || id.value() != identifier || !leaves.ok() || leaves.value() != nf::Decimal{100})
                return nf::Status{nf::ErrorCode::invalid_value};
            received = true;
            return nf::Status::success();
        }).ok()) { server.stop(); return false; }
    std::array<char, 32> timestamp_storage{};
    std::time_t now = std::time(nullptr);
    std::tm utc{};
    if (::gmtime_r(&now, &utc) == nullptr || ::strftime(timestamp_storage.data(), timestamp_storage.size(), "%Y%m%d-%H:%M:%S", &utc) != 17) {
        server.stop(); return false;
    }
    const std::string timestamp{timestamp_storage.data()};
    if (!engine.on_session_active([&](nf::SessionContext& context) {
        auto order = nf::fix44::NewOrderSingle::make().cl_ord_id(identifier).handl_inst('1')
            .symbol("AAPL").side('1').transact_time(timestamp).order_qty(100).ord_type('2').price(nf::Decimal{15025,2});
        return context.send(order).status();
    }).ok() || !engine.on_error([&](const auto& error) {
        ++errors; std::cerr << error.format() << '\n';
    }).ok() || !engine.add_initiator({.name="quickfix",.sender_comp_id="BUY",.target_comp_id="SELL",
        .endpoint={"127.0.0.1", port}}).ok()) { server.stop(); return false; }
    nf::Status status;
    std::thread owner([&] { owner_id=std::this_thread::get_id(); status=engine.run(); });
    const auto deadline=std::chrono::steady_clock::now()+10s;
    while (!received && std::chrono::steady_clock::now()<deadline) std::this_thread::sleep_for(2ms);
    engine.request_stop();
    owner.join();
    // Socket write completion can precede QuickFIX's fromApp/onLogout callback
    // bookkeeping on its own thread. Wait for those observable completions.
    const auto peer_deadline=std::chrono::steady_clock::now()+2s;
    while ((!peer.logged_out || peer.orders!=1) && std::chrono::steady_clock::now()<peer_deadline)
        std::this_thread::sleep_for(1ms);
    server.stop();
    if (!(received && correct_thread && peer.orders==1 && peer.logged_out && errors==0 && status.ok())) {
        const auto snapshot = engine.snapshot("quickfix");
        std::cerr << "pass=" << pass << " port=" << port << " received=" << received << " thread=" << correct_thread
            << " orders=" << peer.orders << " logout=" << peer.logged_out << " errors=" << errors
            << " status=" << status.message();
        if (snapshot.ok()) std::cerr << " state=" << static_cast<unsigned>(snapshot.value().state)
            << " nextIn=" << snapshot.value().next_inbound_seq << " nextOut=" << snapshot.value().next_outbound_seq;
        std::cerr << '\n';
    }
    return received && correct_thread && peer.orders==1 && peer.logged_out && errors==0 && status.ok();
}
}

int main(int argc,char** argv) {
    if(argc!=2)return 2;
    char pattern[]="/tmp/nimblefix-qf-dual-XXXXXX";
    const auto* directory=::mkdtemp(pattern);
    if(directory==nullptr)return 1;
    bool passed=false;
    try {
        const std::filesystem::path root{directory};
        passed=exchange<nf::Inline>(root/"inline",argv[1],1) && exchange<nf::Inline>(root/"inline",argv[1],2)
            && exchange<nf::Queued>(root/"queued",argv[1],3) && exchange<nf::Queued>(root/"queued",argv[1],4);
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';}
    std::filesystem::remove_all(directory);
    if(!passed){std::cerr<<"independent QuickFIX acceptor / typed NimbleFIX initiator failed\n";return 1;}
    std::cout<<"typed inline/queued initiator against independent QuickFIX acceptor and restart passed\n";
}
