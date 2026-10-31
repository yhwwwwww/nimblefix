#include <nimblefix/engine.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <thread>

namespace nf = nimblefix;
using namespace std::chrono_literals;
namespace {
std::atomic<bool> stopped{false};
void stop(int) { stopped.store(true, std::memory_order_relaxed); }
}
int main(int argc, char** argv) {
    if (argc != 4) return 2;
    const bool first = std::string_view{argv[2]} == "first";
    nf::EngineConfig configuration;
    configuration.storage.directory = argv[1];
    configuration.logout_timeout_ms = 1000;
    configuration.queued.worker_threads = 1;
    configuration.queued.delivery_strategy = std::string_view{argv[3]} == "journal"
        ? nf::QueuedDeliveryStrategy::journal_cursor : nf::QueuedDeliveryStrategy::descriptor_ring;
    auto created = nf::Engine<nf::FlatSchema, nf::Queued>::create(configuration);
    if (!created.ok()) return 3;
    auto engine = std::move(created).value();
    std::atomic<unsigned> callbacks{0}, errors{0};
    std::atomic<bool> done{false};
    nf::RecordId input_id{};
    if (!engine.on_message([&](nf::SessionContext& context, const nf::FlatMessageView& message,
            const nf::DeliveryInfo& info) {
        const auto id = message.find_unique(11);
        if (!id.ok()) return id.status();
        input_id = info.record_id;
        ++callbacks;
        auto reply = nf::message<"8">().set<11>(id.value().value).set<17>("DURABLE-REPLY");
        const auto sent = context.send(reply);
        if (!sent.ok()) return sent.status();
        std::cout << "{\"event\":\"callback\",\"input_id\":" << info.record_id
            << ",\"sequence\":" << info.msg_seq_num << ",\"recovery\":" << (info.is_recovery ? "true" : "false") << "}" << std::endl;
        if (first) while (!stopped.load(std::memory_order_relaxed)) std::this_thread::sleep_for(1ms);
        return nf::Status::success();
    }).ok() || !engine.on_error([&](const auto& error) { ++errors; std::cerr << error.format() << '\n'; }).ok() ||
        !engine.add_listener({.name="main", .endpoint={"127.0.0.1",0}}).ok() ||
        !engine.add_acceptor({.name="peer",.listener="main",.local_comp_id="SELL",.remote_comp_id="BUY"}).ok()) return 4;
    std::signal(SIGTERM, stop);
    std::signal(SIGINT, stop);
    nf::Status result;
    std::thread owner([&] { result=engine.run(); done.store(true, std::memory_order_release); });
    while (!done.load(std::memory_order_acquire)) {
        const auto endpoint = engine.listener_endpoint("main");
        if (endpoint.ok()) { std::cout << "{\"event\":\"listening\",\"port\":" << endpoint.value().port << "}" << std::endl; break; }
        std::this_thread::sleep_for(1ms);
    }
    while (!done.load(std::memory_order_acquire) && !stopped.load(std::memory_order_relaxed)) std::this_thread::sleep_for(1ms);
    engine.request_stop(); owner.join();
    if (!result.ok() || errors != 0 || callbacks != 1 || first) return 5;
    std::string path;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(argv[1]))
        if (entry.path().filename() == "session.nfxj") path=entry.path().string();
    if (path.empty()) return 6;
    nf::JournalConfig store{.path=path,.session_key="FIX.4.4|4:SELL|3:BUY",.segment_bytes=1024U*1024U};
    auto opened=nf::Journal::open(store);
    if (!opened.ok()) return 7;
    unsigned replies=0;
    for (const auto& record : opened.value().records()) if (record.kind==nf::RecordKind::outbound && record.dependency==input_id) ++replies;
    const bool delivered=opened.value().has_delivery(input_id);
    std::cout << "{\"event\":\"verified\",\"replies\":" << replies << ",\"delivered\":" << (delivered?"true":"false") << "}" << std::endl;
    return replies==1 && delivered ? 0 : 8;
}
