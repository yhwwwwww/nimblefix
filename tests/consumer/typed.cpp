#include <array>
#include <venue.hpp>
#include <nimblefix/codec.hpp>

int main() {
    std::array<char, 256> buffer;
    const auto encoded = nimblefix::encode_fix44_stream<venue::CustomOrder>(
        {"OUTSIDE", "VENUE", 1, "20261026-12:00:00.000"}, buffer,
        [](auto& w) { w.external_id("external"); });
    return encoded.ok() ? 0 : 1;
}
