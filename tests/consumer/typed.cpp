#include <array>
#include <venue.hpp>
#include <nimblefix/codec.hpp>

int main() {
    auto message = venue::CustomOrder::make().external_id("external");
    std::array<char, 256> buffer;
    const auto encoded = nimblefix::encode_fix44(message,
        {"OUTSIDE", "VENUE", 1, "20261026-12:00:00.000"}, buffer);
    return encoded.ok() ? 0 : 1;
}
