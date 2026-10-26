#include <array>
#include <nimblefix/codec.hpp>
#include <nimblefix/engine.hpp>

int main() {
    auto message = nimblefix::message<"Z">().set<900001>("external");
    std::array<char, 256> buffer;
    const auto encoded = nimblefix::encode_fix44(message,
        {"OUTSIDE", "VENUE", 1, "20261026-12:00:00.000"}, buffer);
    if (!encoded.ok()) return 1;
    auto created = nimblefix::Engine<>::create({.storage={.directory="."}});
    if (!created.ok()) return 1;
    auto engine = std::move(created).value();
    if (!engine.on_message([](auto&,const auto&) { return nimblefix::Status::success(); }).ok()) return 1;
    if (!engine.add_listener({.name="local",.endpoint={"127.0.0.1",0}}).ok()) return 1;
    if (!engine.add_acceptor({.name="peer",.listener="local",.local_comp_id="SELL",.remote_comp_id="BUY"}).ok()) return 1;
    if (!engine.validate().ok()) return 1;
    // Cold registration and validation must not bind a listener.
    return engine.listener_endpoint("local").ok() ? 1 : 0;
}
