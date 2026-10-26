#include <array>
#include <iostream>
#include <string_view>

#include <nimblefix/codec.hpp>

namespace nf = nimblefix;
using namespace nf::literals;

int main(int argc, char** argv) {
    if (argc > 2 || (argc == 2 && std::string_view{argv[1]} != "--self-test")) {
        std::cerr << "usage: codec-demo [--self-test]\n";
        return 2;
    }
    // A codec example: explicit header values, no network or FIX session.
    auto order = nf::message<"D">()
        .set<11>("DEMO-001")
        .set<55>("AAPL")
        .set<54>('1')
        .set<60>("20261026-12:00:00.000")
        .set<38>(100)
        .set<40>('2')
        .set<44>("150.25"_dec);

    std::array<char, 1024> buffer;
    const auto encoded = nf::encode_fix44(order,
        {"BUY", "SELL", 1, "20261026-12:00:00.000"}, buffer);
    if (!encoded.ok()) {
        std::cerr << encoded.error().message() << '\n';
        return 1;
    }
    nf::FrameView frame;
    auto status = nf::inspect_frame({buffer.data(), encoded.value()}, frame);
    std::array<nf::FieldView, 32> scratch;
    nf::FlatMessageView view;
    if (status.ok()) status = nf::parse_flat(frame, scratch, view);
    if (!status.ok()) {
        std::cerr << status.message() << " at byte " << status.offset << '\n';
        return 1;
    }
    const auto symbol = view.find_unique(55);
    const auto price = view.find_unique(44);
    if (!symbol.ok() || !price.ok() || symbol.value().value != "AAPL" || price.value().value != "150.25") {
        std::cerr << "codec round trip failed\n";
        return 1;
    }
    if (argc == 1) {
        for (char ch : frame.bytes) std::cout << (ch == nf::soh ? '|' : ch);
        std::cout << '\n';
    }
    std::cout << "codec self-test passed (FIX 4.4, " << encoded.value() << " bytes)\n";
}
