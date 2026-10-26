#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <thread>
#include <nimblefix/engine.hpp>

namespace {
static_assert(std::atomic<bool>::is_always_lock_free);
std::atomic<bool> stopping{false};
void stop(int) { stopping.store(true, std::memory_order_relaxed); }
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    std::filesystem::create_directories(argv[1]);
    auto result = nimblefix::Engine<>::create({.storage = {.directory = argv[1]}});
    if (!result.ok()) return 1;
    auto engine = std::move(result).value();
    std::atomic<unsigned> delivered{0}, errors{0};
    if (!engine.on_message([&](auto& context, const auto& message) {
        const auto id = message.find_unique(11);
        if (!id.ok()) return id.status();
        auto reply = nimblefix::message<"8">().set<11>(id.value().value);
        const auto sent = context.send(reply);
        if (sent.ok()) ++delivered;
        return sent.status();
    }).ok()) return 1;
    if (!engine.on_error([&](const auto&) { ++errors; }).ok()) return 1;
    if (!engine.add_listener({.name="bench", .endpoint={"127.0.0.1",0}}).ok()) return 1;
    if (!engine.add_acceptor({.name="peer", .listener="bench", .local_comp_id="SELL", .remote_comp_id="BUY"}).ok()) return 1;
    std::signal(SIGTERM, stop);
    std::signal(SIGINT, stop);
    nimblefix::Status run_status;
    std::atomic<bool> owner_done{false};
    std::thread owner([&] { run_status = engine.run(); owner_done.store(true); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool ready = false;
    while (!stopping.load(std::memory_order_relaxed) && !owner_done.load() && std::chrono::steady_clock::now() < deadline) {
        const auto endpoint = engine.listener_endpoint("bench");
        if (endpoint.ok()) { std::cout << endpoint.value().port << std::endl; ready = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!ready) stopping.store(true, std::memory_order_relaxed);
    while (!stopping.load(std::memory_order_relaxed) && !owner_done.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    engine.request_stop(); owner.join();
    std::cout << "{\"delivered\":" << delivered.load() << ",\"errors\":" << errors.load() << "}\n";
    return ready && run_status.ok() ? 0 : 1;
}
