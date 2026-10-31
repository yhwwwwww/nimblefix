#pragma once

#include <arpa/inet.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <syncstream>
#include <thread>

#include <nimblefix/engine.hpp>

namespace example {
namespace nf = nimblefix;
using Clock = std::chrono::steady_clock;
inline std::atomic<bool> stopping{false};
static_assert(std::atomic<bool>::is_always_lock_free);
extern "C" inline void interrupt(int) { stopping.store(true); }
inline void install_signals() {
    std::signal(SIGINT, interrupt);
    std::signal(SIGTERM, interrupt);
}

template<class... Values> void print(Values&&... values) {
    std::osyncstream out(std::cout);
    (out << ... << values);
    out << std::endl;
}
template<class... Values> void error(Values&&... values) {
    std::osyncstream out(std::cerr);
    (out << ... << values);
    out << std::endl;
}
inline void check(nf::Status status) {
    if (!status.ok()) throw std::runtime_error(std::string(status.message()));
}
template<class T> T take(nf::Result<T> result) {
    check(result.status());
    return std::move(result).value();
}

struct Options {
    std::filesystem::path state;
    std::string host{"127.0.0.1"};
    unsigned port{};
    unsigned timeout_ms{10'000};
    bool help{};
};
inline unsigned number(std::string_view text, unsigned limit) {
    unsigned value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value > limit)
        throw std::invalid_argument("invalid numeric option");
    return value;
}
inline Options options(int argc, char** argv, bool initiator) {
    Options result;
    bool port_set = false;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--help" || key == "-h") { result.help = true; return result; }
        if (++i == argc) throw std::invalid_argument("missing option value");
        const std::string value = argv[i];
        if (key == "--state") result.state = value;
        else if (key == "--host") result.host = value;
        else if (key == "--port") { result.port = number(value, 65535); port_set = true; }
        else if (key == "--timeout-ms") result.timeout_ms = number(value, 3'600'000);
        else throw std::invalid_argument("unknown option: " + key);
    }
    in_addr ipv4{};
    in6_addr ipv6{};
    if (result.state.empty() || !port_set || (initiator && result.port == 0) || result.timeout_ms == 0)
        throw std::invalid_argument("--state and --port are required; timeout must be positive");
    if (::inet_pton(AF_INET, result.host.c_str(), &ipv4) != 1 &&
        ::inet_pton(AF_INET6, result.host.c_str(), &ipv6) != 1)
        throw std::invalid_argument("--host must be a numeric IPv4 or IPv6 address");
    return result;
}
inline void usage(std::string_view program, bool initiator) {
    print("Usage: ", program, " --state PATH --port PORT [--host NUMERIC] [--timeout-ms 10000]");
    if (initiator) print("Commands: order ID SYMBOL buy|sell QTY PRICE|market; status; quit");
}
inline nf::EngineConfig config(const Options& options) {
    nf::EngineConfig result;
    result.storage.directory = options.state;
    result.connect_timeout_ms = result.logon_timeout_ms = result.logout_timeout_ms = options.timeout_ms;
    return result;
}
inline std::string utc_now() {
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    if (::gmtime_r(&seconds, &utc) == nullptr) throw std::runtime_error("UTC clock failed");
    char text[32]{};
    if (::strftime(text, sizeof(text), "%Y%m%d-%H:%M:%S", &utc) == 0)
        throw std::runtime_error("UTC formatting failed");
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::snprintf(text + 17, sizeof(text) - 17, ".%03d", static_cast<int>(millis));
    return text;
}

template<class Ready> bool wait_until(Ready ready, unsigned timeout_ms, const std::atomic<bool>& done) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!ready()) {
        if (done.load() || stopping.load() || Clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

// Read stdin without blocking signal handling or protocol shutdown. Reading
// directly also avoids mixing poll() with an iostream's buffered input.
inline bool read_line(std::string& line, const std::atomic<bool>& done) {
    line.clear();
    bool too_long = false;
    while (!stopping.load() && !done.load()) {
        pollfd input{STDIN_FILENO, POLLIN, 0};
        const int ready = ::poll(&input, 1, 50);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) throw std::runtime_error("poll(stdin) failed");
        if (ready == 0) continue;
        char byte{};
        const auto count = ::read(STDIN_FILENO, &byte, 1);
        if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (count < 0) throw std::runtime_error("read(stdin) failed");
        if (count == 0 || byte == '\n') {
            if (too_long) { error("ERROR command exceeds 512 bytes"); line.clear(); }
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return count != 0 || !line.empty();
        }
        if (line.size() == 512) too_long = true;
        if (!too_long) line += byte;
    }
    return false;
}

inline std::string_view field(const nf::FlatMessageView& message, unsigned tag) {
    const auto found = message.find_unique(tag);
    return found.ok() ? found.value().value : std::string_view{};
}
inline std::string_view side_name(char side) {
    return side == '1' ? "buy" : side == '2' ? "sell" : "unknown";
}
inline std::string_view receipt_name(nf::ReceiptState state) {
    switch (state) {
    case nf::ReceiptState::accepted: return "accepted";
    case nf::ReceiptState::appended: return "appended";
    case nf::ReceiptState::written: return "written";
    case nf::ReceiptState::failed: return "failed";
    case nf::ReceiptState::expired: return "expired";
    }
    return "unknown";
}
} // namespace example
