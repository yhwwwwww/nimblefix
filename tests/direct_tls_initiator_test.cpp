#include "test_process_utils.hpp"
#include <nimblefix/direct_initiator.hpp>
#include <nimblefix/direct_tls_transport.hpp>
#include <nimblefix/fix44_sample.hpp>
#include <nimblefix/message.hpp>

#include "../src/transport/tls.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace nf = nimblefix;
namespace fix = nimblefix::fix44_sample;

namespace {

int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::cerr << __FILE__ << ":" << __LINE__ << ": failed: " #cond "\n"; ++g_failures; } } while (0)

struct TempDir {
    std::string path;
    TempDir() {
        char pat[] = "/tmp/nimblefix-direct-tls-XXXXXX";
        char* p = ::mkdtemp(pat);
        if (!p) std::abort();
        path = p;
    }
    ~TempDir() { std::filesystem::remove_all(path); }
};

using OrderShape = nf::Fields<
    nf::Field<11, nf::FieldKind::string>,
    nf::Field<55, nf::FieldKind::string>,
    nf::Field<54, nf::FieldKind::character>,
    nf::Field<60, nf::FieldKind::string>,
    nf::Field<38, nf::FieldKind::decimal>,
    nf::Field<40, nf::FieldKind::character>>;

using ErShape = nf::Fields<
    nf::Field<37, nf::FieldKind::string>,
    nf::Field<17, nf::FieldKind::string>,
    nf::Field<11, nf::FieldKind::string>,
    nf::Field<150, nf::FieldKind::character>,
    nf::Field<39, nf::FieldKind::character>,
    nf::Field<55, nf::FieldKind::string>,
    nf::Field<54, nf::FieldKind::character>,
    nf::Field<151, nf::FieldKind::decimal>,
    nf::Field<14, nf::FieldKind::decimal>,
    nf::Field<6, nf::FieldKind::decimal>>;

struct TlsFixPeer {
    int listen_fd{-1};
    std::uint16_t port{};
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> out_seq{1};
    std::atomic<int> accepted_connections{0};
    std::atomic<int> orders_received{0};
    std::atomic<std::uint16_t> last_peer_port{0};
    std::thread worker;
    nf::TlsConfig server_tls{};

    bool start(nf::TlsConfig tls_cfg) {
        server_tls = std::move(tls_cfg);
        listen_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (listen_fd < 0) return false;
        int one = 1;
        ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;
        if (::listen(listen_fd, 4) != 0) return false;
        socklen_t len = sizeof(addr);
        if (::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return false;
        port = ntohs(addr.sin_port);
        worker = std::thread([this] { run(); });
        return true;
    }

    void finish() {
        stop.store(true, std::memory_order_release);
        if (worker.joinable()) worker.join();
        if (listen_fd >= 0) {
            ::close(listen_fd);
            listen_fd = -1;
        }
    }

    void run() {
        auto ctx_res = nf::detail::TlsContext::create(server_tls, true);
        if (!ctx_res.ok()) return;
        auto ctx = std::move(ctx_res).value();

        while (!stop.load(std::memory_order_acquire)) {
            pollfd pfd{listen_fd, POLLIN, 0};
            if (::poll(&pfd, 1, 20) <= 0) continue;
            sockaddr_in peer_addr{};
            socklen_t peer_len = sizeof(peer_addr);
            int fd = ::accept4(listen_fd, reinterpret_cast<sockaddr*>(&peer_addr), &peer_len,
                SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0) continue;
            last_peer_port.store(ntohs(peer_addr.sin_port), std::memory_order_release);
            int one = 1;
            ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            accepted_connections.fetch_add(1, std::memory_order_relaxed);
            serve_client(ctx, fd);
            ::close(fd);
        }
    }

    static bool tls_write_all(nf::detail::TlsStream& tls, int fd, std::span<const char> data,
                              std::atomic<bool>& stop) {
        std::size_t off = 0;
        while (off < data.size() && !stop.load(std::memory_order_acquire)) {
            const auto io = tls.write(data.subspan(off));
            if (!io.status.ok() || io.peer_closed) return false;
            off += io.bytes;
            if (off < data.size() && io.would_block) {
                pollfd pfd{fd, static_cast<short>((tls.wants_read() ? POLLIN : 0) | POLLOUT), 0};
                (void)::poll(&pfd, 1, 20);
            }
        }
        return off == data.size();
    }

    void serve_client(const nf::detail::TlsContext& ctx, int fd) {
        auto stream_res = nf::detail::TlsStream::create(ctx, fd);
        if (!stream_res.ok()) return;
        auto tls = std::move(stream_res).value();

        // Nonblocking TLS server handshake.
        while (!tls.handshake_complete() && !stop.load(std::memory_order_acquire)) {
            const auto hs = tls.handshake();
            if (!hs.status.ok() || hs.peer_closed) return;
            if (hs.would_block) {
                pollfd pfd{fd, static_cast<short>((tls.wants_read() ? POLLIN : 0) |
                                                  (tls.wants_write() ? POLLOUT : 0)), 0};
                (void)::poll(&pfd, 1, 20);
            }
        }
        if (!tls.handshake_complete()) return;

        std::vector<char> rx(65536);
        std::size_t head = 0, tail = 0;
        std::array<char, 4096> tx_buf{};
        std::array<nf::FieldView, 64> fields{};
        constexpr std::string_view wt{"20261123-00:00:00.000"};

        fix::ExecutionReportFixed<ErShape, 128> er;
        (void)er.order_id("ORD-TLS");
        (void)er.exec_id("EX-TLS");
        (void)er.cl_ord_id("TLS-1");
        (void)er.exec_type('0');
        (void)er.ord_status('0');
        (void)er.symbol("AAPL");
        (void)er.side('1');
        (void)er.leaves_qty(nf::Decimal{100, 0});
        (void)er.cum_qty(nf::Decimal{0, 0});
        (void)er.avg_px(nf::Decimal{0, 0});

        while (!stop.load(std::memory_order_acquire)) {
            if (tail == rx.size()) {
                const std::size_t rem = tail - head;
                std::memmove(rx.data(), rx.data() + head, rem);
                head = 0;
                tail = rem;
            }
            const auto io = tls.read(std::span<char>{rx.data() + tail, rx.size() - tail});
            if (io.peer_closed || !io.status.ok()) break;
            if (io.would_block && io.bytes == 0) {
                pollfd pfd{fd, static_cast<short>(POLLIN | (tls.wants_write() ? POLLOUT : 0)), 0};
                (void)::poll(&pfd, 1, 20);
                continue;
            }
            tail += io.bytes;
            while (head < tail) {
                nf::FrameView fv{};
                const auto st = nf::inspect_frame({rx.data() + head, tail - head}, fv);
                if (st.code == nf::ErrorCode::need_more) break;
                if (!st.ok()) { tls.close(); return; }
                head += fv.consumed;
                nf::FlatMessageView msg{};
                if (!nf::parse_flat_verified(fv, fields, msg).ok()) { tls.close(); return; }
                const auto mt = msg.find_unique(35);
                if (!mt.ok()) continue;

                nf::SessionHeader h{"SELL", "BUY", out_seq.fetch_add(1, std::memory_order_relaxed), wt};
                std::size_t out_n = 0;
                if (mt.value().value == "A") {
                    auto l = nf::message<"A">().set<98>(0).set<108>(30);
                    auto r = nf::encode_fix(l, h, tx_buf);
                    if (r.ok()) out_n = r.value();
                } else if (mt.value().value == "D") {
                    orders_received.fetch_add(1, std::memory_order_relaxed);
                    if (const auto cid = msg.find_unique(11); cid.ok()) (void)er.cl_ord_id(cid.value().value);
                    auto r = nf::encode_fix(er, h, tx_buf);
                    if (r.ok()) out_n = r.value();
                } else if (mt.value().value == "5") {
                    auto l = nf::message<"5">();
                    auto r = nf::encode_fix(l, h, tx_buf);
                    if (r.ok()) (void)tls_write_all(tls, fd, {tx_buf.data(), r.value()}, stop);
                    tls.close();
                    return;
                }
                if (out_n != 0 && !tls_write_all(tls, fd, {tx_buf.data(), out_n}, stop)) {
                    tls.close();
                    return;
                }
            }
            if (head == tail) head = tail = 0;
        }
        tls.close();
    }
};

struct TestHandler {
    std::uint64_t received{};
    std::string last_cl_ord_id{};
    [[nodiscard]] nf::Status on_message(const nf::FlatMessageView& msg, const nf::DeliveryInfo&) noexcept {
        ++received;
        if (const auto f = msg.find_unique(11); f.ok()) {
            try { last_cl_ord_id = std::string{f.value().value}; } catch (...) {}
        }
        return nf::Status::success();
    }
};

nf::DirectInitiatorConfig make_cfg(const std::string& dir, std::uint16_t port) {
    nf::DirectInitiatorConfig cfg{};
    cfg.host = "127.0.0.1";
    cfg.port = port;
    cfg.storage_directory = dir;
    cfg.session_key = "FIX.4.4/BUY/SELL";
    cfg.local_comp_id = "BUY";
    cfg.remote_comp_id = "SELL";
    cfg.heartbeat_ms = 30'000;
    cfg.journal_capacity_bytes = 2U * 1024U * 1024U;
    cfg.journal_segment_bytes = 256U * 1024U;
    cfg.journal_max_records = 4096;
    return cfg;
}

template <class Init>
bool wait_active(Init& init, int max_ms = 3000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(max_ms);
    while (!init.is_active() && std::chrono::steady_clock::now() < deadline) {
        if (!init.poll_once().ok()) return false;
        if (!init.is_active()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return init.is_active();
}

} // namespace

int main(int argc, char** argv) {
    if (!nf::tls_available()) {
        nf::TlsConfig cfg{};
        cfg.enabled = true;
        const auto res = nf::DirectTlsTransport::create(cfg);
        CHECK(!res.ok() && res.status().code == nf::ErrorCode::unsupported);
        std::cout << "DirectTlsTransport unavailable-mode contract passed\n";
        return g_failures == 0 ? 0 : 1;
    }

    std::optional<nimblefix::test_util::TempDir> auto_dir;
    std::string ca_file, cert_file, key_file;
    if (argc >= 4) {
        ca_file = argv[1];
        cert_file = argv[2];
        key_file = argv[3];
    } else {
        auto_dir.emplace("nimblefix-direct-tls-");
        std::filesystem::path cp, kp;
        CHECK(nimblefix::test_util::generate_self_signed_cert(auto_dir->path, cp, kp));
        ca_file = cp.string();
        cert_file = cp.string();
        key_file = kp.string();
    }

    // 1. TLS 1.3 + mTLS full FIX 4.4 round-trips + disconnect/reconnect + restart recovery.
    {
        TempDir dir;
        nf::TlsConfig server_tls{};
        server_tls.enabled = true;
        server_tls.ca_file = ca_file;
        server_tls.certificate_file = cert_file;
        server_tls.private_key_file = key_file;
        server_tls.require_client_certificate = true;
        server_tls.minimum_version = 13;

        TlsFixPeer peer;
        CHECK(peer.start(server_tls));

        nf::TlsConfig client_tls{};
        client_tls.enabled = true;
        client_tls.ca_file = ca_file;
        client_tls.certificate_file = cert_file;
        client_tls.private_key_file = key_file;
        client_tls.server_name = "localhost";
        client_tls.minimum_version = 13;

        std::uint64_t saved_out = 0;
        std::uint64_t saved_in = 0;
        {
            auto transport = nf::DirectTlsTransport::create(client_tls);
            CHECK(transport.ok());
            auto opened = nf::DirectInitiator<fix::Schema, TestHandler, nf::DirectTlsTransport>::open(
                make_cfg(dir.path, peer.port), TestHandler{}, std::move(transport).value());
            CHECK(opened.ok());
            auto init = std::move(opened).value();
            CHECK(init.connect().ok());
            CHECK(wait_active(init));
            CHECK(init.transport().handshake_complete());

            fix::NewOrderSingleFixed<OrderShape, 128> order;
            (void)order.symbol("AAPL");
            (void)order.side('1');
            (void)order.transact_time("20261123-00:00:00.000");
            (void)order.order_qty(nf::Decimal{100, 0});
            (void)order.ord_type('1');

            for (int i = 1; i <= 25; ++i) {
                const std::string id = "TLS-ORD-" + std::to_string(i);
                CHECK(order.cl_ord_id(id).ok());
                const auto seq = init.send(order);
                CHECK(seq.ok());
                const auto target = init.handler().received + 1;
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                while (init.handler().received < target && std::chrono::steady_clock::now() < deadline) {
                    CHECK(init.poll_once().ok());
                }
                CHECK(init.handler().received == target);
                CHECK(init.handler().last_cl_ord_id == id);
            }

            // Disconnect and reconnect using the SAME DirectTlsTransport instance (verifies TlsContext reuse).
            init.disconnect();
            CHECK(!init.valid_transport());
            CHECK(init.connect().ok());
            CHECK(wait_active(init));
            CHECK(peer.accepted_connections.load() == 2);

            CHECK(order.cl_ord_id("TLS-RECONN-1").ok());
            CHECK(init.send(order).ok());
            const auto target2 = init.handler().received + 1;
            const auto deadline2 = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (init.handler().received < target2 && std::chrono::steady_clock::now() < deadline2) {
                CHECK(init.poll_once().ok());
            }
            CHECK(init.handler().received == target2);
            CHECK(init.handler().last_cl_ord_id == "TLS-RECONN-1");

            saved_out = init.next_outbound_seq();
            saved_in = init.next_inbound_seq();
            CHECK(init.logout("tls done").ok());
            for (int i = 0; i < 200 && init.valid_transport(); ++i) {
                (void)init.poll_once();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }

        // 2. Full process-restart recovery over TLS with sequence continuity!
        {
            fix::NewOrderSingleFixed<OrderShape, 128> order;
            (void)order.symbol("AAPL");
            (void)order.side('1');
            (void)order.transact_time("20261123-00:00:00.000");
            (void)order.order_qty(nf::Decimal{100, 0});
            (void)order.ord_type('1');

            auto t2 = nf::DirectTlsTransport::create(client_tls);
            CHECK(t2.ok());
            auto reopened = nf::DirectInitiator<fix::Schema, TestHandler, nf::DirectTlsTransport>::open(
                make_cfg(dir.path, peer.port), TestHandler{}, std::move(t2).value());
            CHECK(reopened.ok());
            auto init2 = std::move(reopened).value();
            CHECK(init2.next_outbound_seq() >= saved_out);
            CHECK(init2.next_inbound_seq() >= saved_in);
            CHECK(init2.connect().ok());
            CHECK(wait_active(init2));
            CHECK(order.cl_ord_id("TLS-RESTART-1").ok());
            CHECK(init2.send(order).ok());
            const auto d3 = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (init2.handler().received < 1 && std::chrono::steady_clock::now() < d3) {
                CHECK(init2.poll_once().ok());
            }
            CHECK(init2.handler().received == 1);
            CHECK(init2.handler().last_cl_ord_id == "TLS-RESTART-1");
            (void)init2.logout("done");
        }

        // 3. Wrong hostname must fail TLS certificate verification with certificate_error!
        {
            TempDir bad_dir;
            nf::TlsConfig bad_tls = client_tls;
            bad_tls.server_name = "wrong.example.invalid";
            auto bad_t = nf::DirectTlsTransport::create(bad_tls);
            CHECK(bad_t.ok());
            auto bad_open = nf::DirectInitiator<fix::Schema, TestHandler, nf::DirectTlsTransport>::open(
                make_cfg(bad_dir.path, peer.port), TestHandler{}, std::move(bad_t).value());
            CHECK(bad_open.ok());
            auto bad_init = std::move(bad_open).value();
            CHECK(bad_init.connect().ok());
            nf::Status err{};
            const auto d4 = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < d4) {
                err = bad_init.poll_once();
                if (!err.ok()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            CHECK(!err.ok());
            CHECK(err.code == nf::ErrorCode::certificate_error || err.code == nf::ErrorCode::tls_error);
        }

        // 4. DirectTlsTransport respects SocketOptions (local source IP/port binding & tcp_quickack).
        {
            TempDir bind_dir;
            int probe_fd = ::socket(AF_INET, SOCK_STREAM, 0);
            CHECK(probe_fd >= 0);
            sockaddr_in paddr{};
            paddr.sin_family = AF_INET;
            paddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            paddr.sin_port = 0;
            CHECK(::bind(probe_fd, reinterpret_cast<sockaddr*>(&paddr), sizeof(paddr)) == 0);
            socklen_t plen = sizeof(paddr);
            CHECK(::getsockname(probe_fd, reinterpret_cast<sockaddr*>(&paddr), &plen) == 0);
            const std::uint16_t chosen_src_port = ntohs(paddr.sin_port);
            ::close(probe_fd);

            peer.out_seq.store(1, std::memory_order_release);
            auto bound_cfg = make_cfg(bind_dir.path, peer.port);
            bound_cfg.session_key = "FIX.4.4/BUY/SELL/BOUND";
            bound_cfg.socket_options.local_bind_ip = "127.0.0.1";
            bound_cfg.socket_options.local_bind_port = chosen_src_port;
            bound_cfg.socket_options.tcp_quickack = true;

            auto bound_t = nf::DirectTlsTransport::create(client_tls);
            CHECK(bound_t.ok());
            auto bound_open = nf::DirectInitiator<fix::Schema, TestHandler, nf::DirectTlsTransport>::open(
                bound_cfg, TestHandler{}, std::move(bound_t).value());
            CHECK(bound_open.ok());
            auto bound_init = std::move(bound_open).value();
            CHECK(bound_init.connect().ok());
            CHECK(wait_active(bound_init));
            CHECK(peer.last_peer_port.load(std::memory_order_acquire) == chosen_src_port);
            bound_init.disconnect();
        }

        peer.finish();
    }

    if (g_failures == 0) {
        std::cout << "DirectInitiator TLS 1.3, mTLS, reconnect, restart and cert-mismatch tests passed\n";
    }
    return g_failures == 0 ? 0 : 1;
}
