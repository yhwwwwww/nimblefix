#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#include <arpa/inet.h>
#include <unistd.h>

#include <nimblefix/engine.hpp>

namespace nimblefix_example {

struct Options {
    std::filesystem::path state;
    std::string host{"127.0.0.1"};
    std::uint16_t port{};
    std::uint32_t timeout_ms{10'000};
    bool port_set{};
    bool help{};
};
inline bool parse_unsigned(std::string_view text, std::uint64_t& value) noexcept {
    if (text.empty()) return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}
inline bool numeric_host(std::string_view host) noexcept {
    if (host.empty() || host.size() >= INET6_ADDRSTRLEN) return false;
    std::array<char, INET6_ADDRSTRLEN> buffer{};
    std::copy(host.begin(), host.end(), buffer.begin());
    in_addr ipv4{};
    in6_addr ipv6{};
    return ::inet_pton(AF_INET, buffer.data(), &ipv4) == 1 ||
        ::inet_pton(AF_INET6, buffer.data(), &ipv6) == 1;
}
inline bool valid_text(std::string_view text) noexcept {
    if (text.empty()) return false;
    for (const unsigned char ch : text) if (ch < 0x20 || ch == 0x7f) return false;
    return true;
}
inline bool parse_args(int argc, char** argv, bool initiator, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--help" || argument == "-h") { options.help = true; return true; }
        if (index + 1 >= argc) { std::cerr << "missing value for " << argument << '\n'; return false; }
        const std::string_view value{argv[++index]};
        if (argument == "--state") {
            if (value.empty()) return false;
            options.state = std::filesystem::path{value};
        } else if (argument == "--host") {
            if (!numeric_host(value)) { std::cerr << "--host must be a numeric IPv4 or IPv6 address\n"; return false; }
            options.host.assign(value);
        } else if (argument == "--port") {
            std::uint64_t port{};
            if (!parse_unsigned(value, port) || port > 65'535) {
                std::cerr << "--port must be an integer in 0..65535\n"; return false;
            }
            options.port = static_cast<std::uint16_t>(port);
            options.port_set = true;
        } else if (argument == "--timeout-ms") {
            std::uint64_t timeout{};
            if (!parse_unsigned(value, timeout) || timeout == 0 || timeout > 3'600'000) {
                std::cerr << "--timeout-ms must be an integer in 1..3600000\n"; return false;
            }
            options.timeout_ms = static_cast<std::uint32_t>(timeout);
        } else {
            std::cerr << "unknown option: " << argument << '\n'; return false;
        }
    }
    if (!options.state.empty() && options.port_set) {
        if (initiator && options.port == 0) {
            std::cerr << "initiator --port must be in 1..65535\n";
            return false;
        }
        return true;
    }
    std::cerr << "--state and --port are required\n";
    return false;
}
inline void print_usage(std::string_view program, bool initiator) {
    std::cerr << "Usage: " << program << " --state PATH --port PORT [--host NUMERIC] [--timeout-ms 10000]";
    std::cerr << '\n';
    if (initiator) std::cerr << "Commands: order ID SYMBOL buy|sell QTY PRICE|market; status; quit\n";
}
inline std::string_view utc_timestamp(std::array<char, 22>& buffer) noexcept {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto whole = time_point_cast<seconds>(now);
    const auto millis = duration_cast<milliseconds>(now - whole).count();
    const auto time_value = system_clock::to_time_t(now);
    std::tm utc{};
    if (::gmtime_r(&time_value, &utc) == nullptr) return {};
    const auto date_size = ::strftime(buffer.data(), buffer.size(), "%Y%m%d-%H:%M:%S", &utc);
    if (date_size != 17) return {};
    if (::snprintf(buffer.data() + date_size, buffer.size() - date_size,
            ".%03u", static_cast<unsigned>(millis)) != 4) return {};
    return {buffer.data(), 21};
}

struct ConsoleEvent {
    enum class Kind : std::uint8_t { message, error } kind{Kind::message};
    nimblefix::ErrorCode code{nimblefix::ErrorCode::none};
    std::array<char, 192> text{};
    std::uint16_t size{};
};
class ConsoleEvents {
public:
    // One producer and one consumer. The owner callback and diagnostic
    // callback run on different threads and must use separate instances.
    static constexpr std::size_t capacity = 64;
    void push(ConsoleEvent::Kind kind, std::string_view text,
        nimblefix::ErrorCode code = nimblefix::ErrorCode::none) noexcept {
        const auto write = write_.load(std::memory_order_relaxed);
        const auto read = read_.load(std::memory_order_acquire);
        if (write - read == capacity) { dropped_.fetch_add(1, std::memory_order_relaxed); return; }
        auto& event = entries_[write % capacity];
        event.kind = kind;
        event.code = code;
        const auto size = std::min(text.size(), event.text.size());
        if (size != 0) std::copy_n(text.data(), size, event.text.data());
        event.size = static_cast<std::uint16_t>(size);
        write_.store(write + 1, std::memory_order_release);
    }
    void push_error(const nimblefix::ErrorEvent& error) noexcept {
        std::array<char, 192> text{};
        std::size_t used = 0;
        const auto append = [&](std::string_view part) {
            const auto count = std::min(part.size(), text.size() - used);
            if (count != 0) std::copy_n(part.data(), count, text.data() + used);
            used += count;
        };
        append(error.session_name);
        if (used != 0) append(": ");
        append(error.status.message());
        push(ConsoleEvent::Kind::error, {text.data(), used}, error.status.code);
    }
    bool pop(ConsoleEvent& event) noexcept {
        const auto read = read_.load(std::memory_order_relaxed);
        if (read == write_.load(std::memory_order_acquire)) return false;
        event = entries_[read % capacity];
        read_.store(read + 1, std::memory_order_release);
        return true;
    }
    [[nodiscard]] std::uint64_t dropped() const noexcept { return dropped_.load(std::memory_order_relaxed); }
private:
    std::array<ConsoleEvent, capacity> entries_{};
    std::atomic<std::size_t> write_{};
    std::atomic<std::size_t> read_{};
    std::atomic<std::uint64_t> dropped_{};
};
inline void print_events(ConsoleEvents& events, std::uint64_t& previous_dropped) {
    ConsoleEvent event;
    while (events.pop(event)) {
        const std::string_view text{event.text.data(), event.size};
        if (event.kind == ConsoleEvent::Kind::error)
            std::cerr << "ERROR [" << static_cast<int>(event.code) << "] " << text << '\n';
        else std::cout << text << '\n';
    }
    const auto dropped = events.dropped();
    if (dropped != previous_dropped) {
        std::cerr << "WARNING: " << (dropped - previous_dropped) << " console events were dropped\n";
        previous_dropped = dropped;
    }
}
inline void push_status(ConsoleEvents& events, std::string_view operation,
    nimblefix::Status status) noexcept {
    std::array<char, 192> text{};
    std::size_t used = std::min(operation.size(), text.size());
    if (used != 0) std::copy_n(operation.data(), used, text.data());
    if (used < text.size()) text[used++] = ':';
    if (used < text.size()) text[used++] = ' ';
    const auto message = status.message();
    const auto size = std::min(message.size(), text.size() - used);
    if (size != 0) std::copy_n(message.data(), size, text.data() + used);
    used += size;
    events.push(ConsoleEvent::Kind::error, {text.data(), used}, status.code);
}
inline bool is_message(const nimblefix::FlatMessageView& message, std::string_view type) noexcept {
    const auto found = message.find_unique(35);
    return found.ok() && found.value().value == type;
}
inline bool timestamp_shaped(std::string_view value) noexcept {
    if (value.size() != 17 && value.size() != 21) return false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8) { if (value[index] != '-') return false; }
        else if (index == 11 || index == 14) { if (value[index] != ':') return false; }
        else if (index == 17) { if (value[index] != '.') return false; }
        else if (value[index] < '0' || value[index] > '9') return false;
    }
    return true;
}
} // namespace nimblefix_example
