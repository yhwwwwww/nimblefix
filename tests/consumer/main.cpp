#include <array>
#include <nimblefix/codec.hpp>

int main() {
    auto message = nimblefix::message<"Z">().set<900001>("external");
    std::array<char, 256> buffer;
    const auto encoded = nimblefix::encode_fix44(message,
        {"OUTSIDE", "VENUE", 1, "20261026-12:00:00.000"}, buffer);
    return encoded.ok() ? 0 : 1;
}
