#include <quickfix/Application.h>
#include <quickfix/FileStore.h>
#include <quickfix/FixFields.h>
#include <quickfix/Message.h>
#include <quickfix/Session.h>
#include <quickfix/SessionSettings.h>
#include <quickfix/SocketAcceptor.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

static_assert(std::atomic<bool>::is_always_lock_free);
std::atomic<bool> stopping{false};
std::atomic<std::uint64_t> delivered{0};
std::atomic<std::uint64_t> errors{0};

void request_stop(int) noexcept {
    stopping.store(true, std::memory_order_relaxed);
}

bool reserve_loopback_port(std::uint16_t& port) noexcept {
    const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
    if (descriptor < 0) return false;

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (::bind(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(descriptor);
        return false;
    }

    socklen_t address_size = sizeof(address);
    const bool good = ::getsockname(descriptor, reinterpret_cast<sockaddr*>(&address),
        &address_size) == 0;
    if (good) port = ntohs(address.sin_port);
    ::close(descriptor);
    return good && port != 0;
}

bool acceptor_is_listening(std::uint16_t port) noexcept {
    const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
    if (descriptor < 0) return false;

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    const bool listening = ::connect(descriptor,
        reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
    ::close(descriptor);
    return listening;
}

class ExecutionReport final : public FIX::Message {
public:
    explicit ExecutionReport(const std::string& cl_ord_id)
        : FIX::Message(FIX::BeginString{"FIX.4.4"}, FIX::MsgType{"8"}) {
        setField(FIX::ClOrdID{cl_ord_id});
    }
};

class BenchmarkApplication final : public FIX::Application {
public:
    void onCreate(const FIX::SessionID&) override {}
    void onLogon(const FIX::SessionID&) override {}
    void onLogout(const FIX::SessionID&) override {}
    void toAdmin(FIX::Message&, const FIX::SessionID&) override {}
    void toApp(FIX::Message&, const FIX::SessionID&) override {}
    void fromAdmin(const FIX::Message&, const FIX::SessionID&) override {}

    void fromApp(const FIX::Message& request, const FIX::SessionID& session_id) override {
        try {
            const auto& type = request.getHeader().getField<FIX::MsgType>().getValue();
            if (type != "D") {
                errors.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            const auto& cl_ord_id = request.getField<FIX::ClOrdID>().getValue();
            if (cl_ord_id.empty()) {
                errors.fetch_add(1, std::memory_order_relaxed);
                return;
            }

            ExecutionReport response{cl_ord_id};
            if (!FIX::Session::sendToTarget(response, session_id)) {
                errors.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            delivered.fetch_add(1, std::memory_order_relaxed);
        } catch (...) {
            errors.fetch_add(1, std::memory_order_relaxed);
        }
    }
};

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: quickfix-tcp-server <FileStore-directory>\n";
        return 2;
    }

    std::uint16_t port{};
    if (!reserve_loopback_port(port)) {
        std::cerr << "could not reserve a loopback TCP port\n";
        return 1;
    }

    try {
        std::filesystem::create_directories(argv[1]);

        FIX::Dictionary defaults;
        defaults.setString(FIX::CONNECTION_TYPE, "acceptor");
        defaults.setString(FIX::START_TIME, "00:00:00");
        defaults.setString(FIX::END_TIME, "23:59:59");
        defaults.setInt(FIX::HEARTBTINT, 30);
        defaults.setInt(FIX::SOCKET_ACCEPT_PORT, port);
        defaults.setString(FIX::SOCKET_NODELAY, "Y");
        defaults.setString(FIX::USE_DATA_DICTIONARY, "N");
        defaults.setString(FIX::RESET_ON_LOGON, "N");
        defaults.setString(FIX::RESET_ON_LOGOUT, "N");
        defaults.setString(FIX::RESET_ON_DISCONNECT, "N");
        defaults.setString(FIX::PERSIST_MESSAGES, "Y");
        defaults.setString(FIX::FILE_STORE_PATH, argv[1]);

        FIX::SessionSettings settings;
        settings.set(defaults);
        const FIX::SessionID session_id{"FIX.4.4", "SELL", "BUY"};
        FIX::Dictionary session;
        session.setString(FIX::BEGINSTRING, "FIX.4.4");
        session.setString(FIX::SENDERCOMPID, "SELL");
        session.setString(FIX::TARGETCOMPID, "BUY");
        settings.set(session_id, session);

        FIX::FileStoreFactory store_factory{settings};
        BenchmarkApplication application;
        FIX::SocketAcceptor acceptor{application, store_factory, settings};
        std::signal(SIGTERM, request_stop);
        std::signal(SIGINT, request_stop);
        acceptor.start();

        const auto listen_deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds{5};
        bool listening = false;
        while (!stopping.load(std::memory_order_relaxed) &&
            std::chrono::steady_clock::now() < listen_deadline) {
            if (acceptor_is_listening(port)) {
                listening = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        if (!listening) {
            stopping.store(true, std::memory_order_relaxed);
            acceptor.stop(true);
            std::cerr << "QuickFIX acceptor did not become ready\n";
            return 1;
        }

        // tcp_run.py reads this line as the selected loopback port.
        std::cout << port << std::endl;
        while (!stopping.load(std::memory_order_relaxed))
            std::this_thread::sleep_for(std::chrono::milliseconds{5});

        acceptor.stop();
        std::cout << "{\"delivered\":" << delivered.load(std::memory_order_relaxed)
            << ",\"errors\":" << errors.load(std::memory_order_relaxed)
            << ",\"peer\":\"QuickFIX\",\"quickfix_version\":\"1.16.0\""
            << ",\"store_backend\":\"FileStore\""
            << ",\"store_flush_semantics\":\"FileStore::set fflushes body and header; sequence counters fflush seqnums; no fsync or fdatasync\""
            << ",\"power_loss_durability\":false}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "QuickFIX server failed: " << error.what() << '\n';
    } catch (...) {
        std::cerr << "QuickFIX server failed with an unknown exception\n";
    }
    return 1;
}
