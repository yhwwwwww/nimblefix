#include <nimblefix/engine.hpp>
#include <nimblefix/message.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace nf = nimblefix;
using namespace std::chrono_literals;

namespace {

int failures = 0;

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #condition << '\n'; \
    ++failures; \
} } while (false)

std::filesystem::path make_directory(std::string_view suffix) {
    std::string pattern = "/tmp/nimblefix-engine-" + std::string{suffix} + "-XXXXXX";
    std::vector<char> path(pattern.begin(), pattern.end());
    path.push_back('\0');
    const auto* created = ::mkdtemp(path.data());
    return created == nullptr ? std::filesystem::path{} : std::filesystem::path{created};
}

std::string sending_time() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto millisecond_part = duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    const auto time = system_clock::to_time_t(now);
    std::tm utc{};
    if (::gmtime_r(&time, &utc) == nullptr) return "20261031-12:00:00.000";
    std::array<char, 32> result{};
    const auto used = std::strftime(result.data(), result.size(), "%Y%m%d-%H:%M:%S", &utc);
    if (used == 0) return "20261031-12:00:00.000";
    char fractional[8]{};
    std::snprintf(fractional, sizeof(fractional), ".%03lld",
        static_cast<long long>(millisecond_part));
    return std::string{result.data(), used} + fractional;
}

template <class Builder>
std::string encode(Builder&& builder, std::string_view sender, std::string_view target,
    std::uint64_t sequence) {
    std::array<char, 2048> bytes{};
    const auto timestamp = sending_time();
    const nf::SessionHeader header{sender, target, sequence, timestamp};
    const auto encoded = nf::encode_fix44(std::forward<Builder>(builder), header, bytes);
    if (!encoded.ok()) return {};
    return {bytes.data(), encoded.value()};
}

std::string logon(std::string_view sender, std::string_view target, std::uint64_t sequence,
    std::string_view username = {}, std::string_view password = {}) {
    if (!username.empty()) {
        auto message = nf::message<"A">().set<98>(0).set<108>(30)
            .set<553>(username).set<554>(password);
        return encode(std::move(message), sender, target, sequence);
    }
    return encode(nf::message<"A">().set<98>(0).set<108>(30), sender, target, sequence);
}

std::string business(std::string_view sender, std::string_view target,
    std::uint64_t sequence) {
    return encode(nf::message<"D">().set<11>("ENGINE-ORDER").set<55>("AAPL"),
        sender, target, sequence);
}

std::string logout(std::string_view sender, std::string_view target, std::uint64_t sequence) {
    return encode(nf::message<"5">(), sender, target, sequence);
}

bool send_all(int fd, std::string_view bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::send(fd, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

int connect_loopback(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    timeval timeout{2, 0};
    (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool receive_frame(int fd, std::string& buffer, std::string& output,
    std::chrono::milliseconds timeout = 3s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::array<char, 4096> bytes{};
    while (std::chrono::steady_clock::now() < deadline) {
        nf::FrameView frame{};
        const auto parsed = nf::inspect_frame(buffer, frame);
        if (parsed.ok()) {
            output.assign(frame.bytes);
            buffer.erase(0, frame.consumed);
            return true;
        }
        if (parsed.code != nf::ErrorCode::need_more) return false;
        pollfd descriptor{fd, POLLIN, 0};
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        const auto ready = ::poll(&descriptor, 1,
            static_cast<int>(std::max<std::int64_t>(1, left.count())));
        if (ready <= 0) return false;
        const auto count = ::recv(fd, bytes.data(), bytes.size(), 0);
        if (count <= 0) return false;
        buffer.append(bytes.data(), static_cast<std::size_t>(count));
    }
    return false;
}

bool is_type(std::string_view bytes, std::string_view type) {
    return bytes.find(std::string{"35="} + std::string{type} + '\x01') != std::string_view::npos;
}

bool poll_until_stopped(auto& engine, nf::Status& final_status,
    std::chrono::milliseconds timeout = 3s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (engine.running() && std::chrono::steady_clock::now() < deadline) {
        final_status = engine.poll_once(10);
        if (!final_status.ok() && final_status.code != nf::ErrorCode::callback_failed) return false;
    }
    return !engine.running();
}

template <class Policy>
void test_manual_owner_and_callback_threads(std::string_view directory_name,
    bool callbacks_on_worker) {
    const auto root = make_directory(directory_name);
    CHECK(!root.empty());
    if (root.empty()) return;

    auto created = nf::Engine<nf::FlatSchema, Policy>::create({
        .storage = {.directory = root / "journal"}, .logout_timeout_ms = 700});
    CHECK(created.ok());
    if (!created.ok()) return;
    auto engine = std::move(created).value();
    const auto owner_id = std::this_thread::get_id();

    std::mutex id_mutex;
    std::thread::id active_id{}, message_id{};
    std::atomic<bool> active_called{false}, message_called{false};
    std::atomic<int> peer_stage{0};
    CHECK(engine.on_session_active([&](nf::SessionContext&) {
        {
            std::lock_guard lock(id_mutex);
            active_id = std::this_thread::get_id();
        }
        active_called.store(true, std::memory_order_release);
        return nf::Status::success();
    }).ok());
    CHECK(engine.on_message([&](nf::SessionContext&, const nf::FlatMessageView& view) {
        const auto type = view.find_unique(35);
        if (!type.ok() || type.value().value != "D") return nf::Status::success();
        {
            std::lock_guard lock(id_mutex);
            message_id = std::this_thread::get_id();
        }
        message_called.store(true, std::memory_order_release);
        return nf::Status::success();
    }).ok());
    CHECK(engine.on_error([&](const nf::ErrorEvent& error) {
        if (error.status.code != nf::ErrorCode::connection_closed)
            std::cerr << "engine " << directory_name << " error=" << static_cast<int>(error.status.code)
                      << " tag=" << error.status.tag << '\n';
    }).ok());

    auto listener = engine.add_listener({.name = "listener", .endpoint = {"127.0.0.1", 0}});
    CHECK(listener.ok());
    auto session = engine.add_acceptor({.name = "acceptor", .listener = "listener",
        .local_comp_id = "SERVER", .remote_comp_id = "CLIENT"});
    CHECK(session.ok());
    if (!listener.ok() || !session.ok()) return;
    auto before = engine.snapshot("acceptor");
    CHECK(before.ok() && before.value().state == nf::SessionState::disconnected);
    CHECK(engine.validate().ok());
    CHECK(engine.start().ok());
    CHECK(engine.running());

    nf::Status wrong_thread{};
    std::thread wrong_thread_probe([&] { wrong_thread = engine.poll_once(0); });
    wrong_thread_probe.join();
    CHECK(wrong_thread.code == nf::ErrorCode::wrong_thread);

    const auto endpoint = engine.listener_endpoint("listener");
    CHECK(endpoint.ok());
    if (!endpoint.ok()) {
        engine.request_stop();
        nf::Status stopped{};
        (void)poll_until_stopped(engine, stopped);
        return;
    }

    std::atomic<bool> peer_ok{true};
    std::thread peer([&] {
        const int fd = connect_loopback(endpoint.value().port);
        if (fd < 0) { peer_stage.store(1); peer_ok.store(false, std::memory_order_release); return; }
        std::string buffered, wire;
        const auto logon_wire = logon("CLIENT", "SERVER", 1);
        if (logon_wire.empty() || !send_all(fd, logon_wire)) {
            peer_stage.store(2);
            peer_ok.store(false, std::memory_order_release);
            ::close(fd);
            return;
        }
        if (!receive_frame(fd, buffered, wire) || !is_type(wire, "A")) {
            peer_stage.store(3);
            peer_ok.store(false, std::memory_order_release);
            ::close(fd);
            return;
        }
        const auto business_wire = business("CLIENT", "SERVER", 2);
        if (business_wire.empty() || !send_all(fd, business_wire)) {
            peer_stage.store(4);
            peer_ok.store(false, std::memory_order_release);
            ::close(fd);
            return;
        }
        if (!receive_frame(fd, buffered, wire, 4s) || !is_type(wire, "5") ||
            !send_all(fd, logout("CLIENT", "SERVER", 3))) {
            peer_stage.store(5);
            peer_ok.store(false, std::memory_order_release);
        }
        ::close(fd);
    });

    const auto delivery_deadline = std::chrono::steady_clock::now() + 4s;
    while (!message_called.load(std::memory_order_acquire) &&
        std::chrono::steady_clock::now() < delivery_deadline) {
        const auto status = engine.poll_once(10);
        CHECK(status.ok());
        if (!status.ok()) break;
    }
    CHECK(message_called.load(std::memory_order_acquire));
    const auto active_deadline = std::chrono::steady_clock::now() + 2s;
    while (!active_called.load(std::memory_order_acquire) &&
        std::chrono::steady_clock::now() < active_deadline) {
        const auto status = engine.poll_once(5);
        CHECK(status.ok());
        if (!status.ok()) break;
    }
    CHECK(active_called.load(std::memory_order_acquire));

    auto snapshot = engine.snapshot("acceptor");
    const auto snapshot_deadline = std::chrono::steady_clock::now() + 1s;
    while (snapshot.ok() && (snapshot.value().business_deliveries != 1 ||
            snapshot.value().next_inbound_seq < 3) &&
        std::chrono::steady_clock::now() < snapshot_deadline) {
        const auto status = engine.poll_once(5);
        CHECK(status.ok());
        snapshot = engine.snapshot("acceptor");
    }
    CHECK(snapshot.ok());
    if (snapshot.ok()) {
        CHECK(snapshot.value().state == nf::SessionState::active);
        CHECK(snapshot.value().received_frames >= 1);
        CHECK(snapshot.value().business_deliveries == 1);
        CHECK(snapshot.value().next_inbound_seq >= 3);
    }
    {
        std::lock_guard lock(id_mutex);
        CHECK((active_id != owner_id) == callbacks_on_worker);
        CHECK((message_id != owner_id) == callbacks_on_worker);
    }

    engine.request_stop();
    nf::Status final_status{};
    CHECK(poll_until_stopped(engine, final_status));
    CHECK(final_status.ok());
    peer.join();
    CHECK(peer_ok.load(std::memory_order_acquire));
    if (!peer_ok.load(std::memory_order_acquire))
        std::cerr << "engine " << directory_name << " client peer stage=" << peer_stage.load() << '\n';
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

std::uint8_t weekday_bit_not_today() {
    using namespace std::chrono;
    const auto day = floor<days>(system_clock::now());
    const auto today = weekday(day).c_encoding();
    return static_cast<std::uint8_t>(1U << ((today + 1U) % 7U));
}

void test_auth_schedule_reject_before_store() {
    const auto root = make_directory("auth-schedule");
    CHECK(!root.empty());
    if (root.empty()) return;
    const auto storage = root / "journal";
    auto created = nf::Engine<>::create({.storage = {.directory = storage},
        .logout_timeout_ms = 400, .diagnostic_capacity = 8});
    CHECK(created.ok());
    if (!created.ok()) return;
    auto engine = std::move(created).value();
    std::atomic<bool> auth_error{false}, schedule_error{false};
    CHECK(engine.on_error([&](const nf::ErrorEvent& event) {
        if (event.status.code == nf::ErrorCode::authentication_failed)
            auth_error.store(true, std::memory_order_release);
        if (event.status.code == nf::ErrorCode::schedule_closed)
            schedule_error.store(true, std::memory_order_release);
    }).ok());
    CHECK(engine.add_listener({.name = "listener", .endpoint = {"127.0.0.1", 0}}).ok());

    nf::AcceptorConfig authenticated{.name = "auth", .listener = "listener",
        .local_comp_id = "AUTH", .remote_comp_id = "CLIENT"};
    authenticated.settings.authentication.method = nf::AuthenticationMethod::credentials;
    authenticated.settings.authentication.username = "alice";
    authenticated.settings.authentication.password = "correct horse";
    authenticated.settings.authentication.require_tls = false;
    CHECK(engine.add_acceptor(std::move(authenticated)).ok());

    nf::AcceptorConfig closed{.name = "closed", .listener = "listener",
        .local_comp_id = "SCHEDULE", .remote_comp_id = "CLIENT2"};
    closed.settings.schedule.enabled = true;
    closed.settings.schedule.weekdays = weekday_bit_not_today();
    closed.settings.schedule.start_second_utc = 0;
    closed.settings.schedule.end_second_utc = 86'400;
    CHECK(!closed.settings.schedule.contains(std::chrono::system_clock::now()));
    CHECK(engine.add_acceptor(std::move(closed)).ok());
    CHECK(engine.start().ok());
    const auto endpoint = engine.listener_endpoint("listener");
    CHECK(endpoint.ok());
    if (!endpoint.ok()) return;

    const int bad_auth = connect_loopback(endpoint.value().port);
    CHECK(bad_auth >= 0);
    if (bad_auth >= 0) {
        CHECK(send_all(bad_auth, logon("CLIENT", "AUTH", 1, "alice", "wrong")));
        ::close(bad_auth);
    }
    const int closed_schedule = connect_loopback(endpoint.value().port);
    CHECK(closed_schedule >= 0);
    if (closed_schedule >= 0) {
        CHECK(send_all(closed_schedule, logon("CLIENT2", "SCHEDULE", 1)));
        ::close(closed_schedule);
    }

    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while ((!auth_error.load(std::memory_order_acquire) ||
            !schedule_error.load(std::memory_order_acquire)) &&
        std::chrono::steady_clock::now() < deadline) {
        const auto status = engine.poll_once(10);
        CHECK(status.ok());
        if (!status.ok()) break;
    }
    CHECK(auth_error.load(std::memory_order_acquire));
    CHECK(schedule_error.load(std::memory_order_acquire));
    CHECK(std::filesystem::is_empty(storage));
    const auto auth_snapshot = engine.snapshot("auth");
    const auto schedule_snapshot = engine.snapshot("closed");
    CHECK(auth_snapshot.ok() && auth_snapshot.value().state == nf::SessionState::disconnected);
    CHECK(schedule_snapshot.ok() && schedule_snapshot.value().state == nf::SessionState::disconnected);

    engine.request_stop();
    nf::Status final_status{};
    CHECK(poll_until_stopped(engine, final_status));
    CHECK(final_status.ok());
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

void test_observer_thread_overflow_and_exception() {
    {
        const auto root = make_directory("observer-overflow");
        auto created = nf::Engine<>::create({.storage = {.directory = root / "journal"},
            .logout_timeout_ms = 300, .diagnostic_capacity = 1});
        CHECK(created.ok());
        if (created.ok()) {
            auto engine = std::move(created).value();
            std::mutex mutex;
            std::condition_variable changed;
            bool entered = false, release = false;
            std::thread::id observer_id{};
            CHECK(engine.on_error([&](const nf::ErrorEvent&) {
                std::unique_lock lock(mutex);
                observer_id = std::this_thread::get_id();
                if (!entered) {
                    entered = true;
                    changed.notify_all();
                    changed.wait(lock, [&] { return release; });
                }
            }).ok());
            CHECK(engine.add_listener({.name = "listener", .endpoint = {"127.0.0.1", 0}}).ok());
            CHECK(engine.add_acceptor({.name = "peer", .listener = "listener",
                .local_comp_id = "SERVER", .remote_comp_id = "CLIENT"}).ok());
            CHECK(engine.start().ok());
            const auto endpoint = engine.listener_endpoint("listener");
            CHECK(endpoint.ok());
            if (endpoint.ok()) {
                const int first = connect_loopback(endpoint.value().port);
                CHECK(first >= 0);
                if (first >= 0) {
                    CHECK(send_all(first, business("CLIENT", "SERVER", 1)));
                    ::close(first);
                }
                const auto observer_deadline = std::chrono::steady_clock::now() + 2s;
                while (true) {
                    {
                        std::lock_guard lock(mutex);
                        if (entered) break;
                    }
                    if (std::chrono::steady_clock::now() >= observer_deadline) break;
                    const auto status = engine.poll_once(5);
                    CHECK(status.ok());
                    if (!status.ok()) break;
                }
                {
                    std::lock_guard lock(mutex);
                    CHECK(entered);
                }
                for (unsigned i = 0; i < 24; ++i) {
                    const int fd = connect_loopback(endpoint.value().port);
                    CHECK(fd >= 0);
                    if (fd >= 0) {
                        CHECK(send_all(fd, business("CLIENT", "SERVER", i + 2)));
                        ::close(fd);
                    }
                }
                for (unsigned i = 0; i < 64 && engine.dropped_diagnostics() == 0; ++i) {
                    const auto status = engine.poll_once(5);
                    CHECK(status.ok());
                }
                CHECK(engine.dropped_diagnostics() > 0);
            }
            {
                std::lock_guard lock(mutex);
                release = true;
            }
            changed.notify_all();
            engine.request_stop();
            nf::Status final_status{};
            CHECK(poll_until_stopped(engine, final_status));
            CHECK(final_status.ok());
            CHECK(observer_id != std::this_thread::get_id());
        }
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    {
        const auto root = make_directory("observer-throws");
        auto created = nf::Engine<>::create({.storage = {.directory = root / "journal"},
            .logout_timeout_ms = 300, .diagnostic_capacity = 4});
        CHECK(created.ok());
        if (created.ok()) {
            auto engine = std::move(created).value();
            const auto owner_id = std::this_thread::get_id();
            std::atomic<bool> observer_called{false};
            std::thread::id observer_id{};
            std::mutex mutex;
            CHECK(engine.on_error([&](const nf::ErrorEvent&) {
                {
                    std::lock_guard lock(mutex);
                    observer_id = std::this_thread::get_id();
                }
                observer_called.store(true, std::memory_order_release);
                throw 7;
            }).ok());
            CHECK(engine.add_listener({.name = "listener", .endpoint = {"127.0.0.1", 0}}).ok());
            CHECK(engine.add_acceptor({.name = "peer", .listener = "listener",
                .local_comp_id = "SERVER", .remote_comp_id = "CLIENT"}).ok());
            CHECK(engine.start().ok());
            const auto endpoint = engine.listener_endpoint("listener");
            CHECK(endpoint.ok());
            if (endpoint.ok()) {
                const int fd = connect_loopback(endpoint.value().port);
                CHECK(fd >= 0);
                if (fd >= 0) {
                    CHECK(send_all(fd, business("CLIENT", "SERVER", 1)));
                    ::close(fd);
                }
                nf::Status last{};
                const auto deadline = std::chrono::steady_clock::now() + 3s;
                while (engine.running() && std::chrono::steady_clock::now() < deadline) {
                    last = engine.poll_once(10);
                    if (!last.ok()) break;
                }
                CHECK(last.code == nf::ErrorCode::callback_failed);
                CHECK(observer_called.load(std::memory_order_acquire));
                {
                    std::lock_guard lock(mutex);
                    CHECK(observer_id != owner_id);
                }
            }
        }
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
}

void test_qualified_session_identities() {
    const auto root = std::filesystem::temp_directory_path() / "nimblefix-qualified-identities";
    auto opened = nf::Engine<>::create({.storage={.directory=root}});
    CHECK(opened.ok());
    if (!opened.ok()) return;
    auto engine = std::move(opened).value();
    CHECK(engine.add_listener({.name="one",.endpoint={"127.0.0.1",0}}).ok());
    CHECK(engine.add_listener({.name="two",.endpoint={"127.0.0.1",0}}).ok());
    nf::SessionSettings first, second;
    first.qualifier="one";
    second.qualifier="two";
    CHECK(engine.add_acceptor({.name="first",.listener="one",.local_comp_id="SELL",.remote_comp_id="BUY",.settings=first}).ok());
    CHECK(engine.add_acceptor({.name="second",.listener="two",.local_comp_id="SELL",.remote_comp_id="BUY",.settings=second}).ok());
    // Same socket/transport/CompIDs cannot route by a local-only qualifier.
    second.qualifier="ambiguous";
    CHECK(!engine.add_acceptor({.name="ambiguous",.listener="one",.local_comp_id="SELL",.remote_comp_id="BUY",.settings=second}).ok());
    // A repeated persistent identity remains invalid on a different socket.
    CHECK(!engine.add_acceptor({.name="duplicate",.listener="two",.local_comp_id="SELL",.remote_comp_id="BUY",.settings=first}).ok());
    second.version=nf::FixVersion::fix42;
    second.qualifier="one";
    CHECK(engine.add_acceptor({.name="other-version",.listener="one",.local_comp_id="SELL",.remote_comp_id="BUY",.settings=second}).ok());
    CHECK(engine.validate().ok());
}

} // namespace

int main() {
    test_manual_owner_and_callback_threads<nf::Inline>("inline", false);
    test_manual_owner_and_callback_threads<nf::Queued>("queued", true);
    test_auth_schedule_reject_before_store();
    test_qualified_session_identities();
    test_observer_thread_overflow_and_exception();
    if (failures == 0)
        std::cout << "Engine manual ownership, callback policies, snapshots, diagnostics, auth and schedules passed\n";
    return failures == 0 ? 0 : 1;
}
