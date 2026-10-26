#include <array>
#include <cstdlib>
#include <iostream>
#include <string>

#include <quickfix/DataDictionary.h>
#include <quickfix/Message.h>
#include <nimblefix/codec.hpp>

#define CHECK(x) do { if (!(x)) { std::cerr << "QuickFIX oracle: " #x << " line " << __LINE__ << '\n'; std::exit(1); } } while (false)

int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "usage: quickfix-test path/to/FIX44.xml\n"; return 2; }
    try {
        FIX::DataDictionary dictionary(argv[1]);
        for (int quantity = 1; quantity <= 1000; ++quantity) {
            const auto id = "ORACLE-" + std::to_string(quantity);
            auto order = nimblefix::message<"D">().set<11>(id).set<21>('1')
                .set<55>("AAPL").set<54>('1').set<60>("20261026-12:00:00.000")
                .set<38>(quantity).set<40>('2').set<44>(nimblefix::Decimal{15025, 2});
            std::array<char, 1024> buffer;
            const auto encoded = nimblefix::encode_fix44(order,
                {"BUY", "SELL", static_cast<std::uint64_t>(quantity), "20261026-12:00:00.000"}, buffer);
            CHECK(encoded.ok());
            const std::string bytes(buffer.data(), encoded.value());
            FIX::Message independent(bytes, dictionary, true);
            dictionary.validate(independent);
            CHECK(independent.getField(11) == id);
            CHECK(independent.getField(38) == std::to_string(quantity));
            CHECK(independent.getField(44) == "150.25");
            // QuickFIX chooses its own body ordering and recomputes 9/10.
            // Parse that independently encoded frame with NimbleFIX as well.
            const std::string reverse = independent.toString();
            nimblefix::FrameView frame;
            CHECK(nimblefix::inspect_frame(reverse, frame).ok());
            nimblefix::FlatMessageView view;
            std::array<nimblefix::FieldView, 32> scratch;
            CHECK(nimblefix::parse_flat(frame, scratch, view).ok());
            CHECK(view.find_unique(11).value().value == id);
            CHECK(view.find_unique(38).value().value == std::to_string(quantity));
            CHECK(view.find_unique(44).value().value == "150.25");
            std::string corrupted = bytes;
            corrupted[corrupted.size() - 3] = corrupted[corrupted.size() - 3] == '9' ? '0' : '9';
            bool rejected = false;
            try { FIX::Message invalid(corrupted, true); } catch (const FIX::InvalidMessage&) { rejected = true; }
            CHECK(rejected);
        }
        std::cout << "QuickFIX 1.16.0 independent dictionary/wire checks passed: 1000 bidirectional frames\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
