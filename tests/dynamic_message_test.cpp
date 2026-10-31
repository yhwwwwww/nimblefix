#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <nimblefix/dynamic_message.hpp>

namespace {

int failures = 0;

#define CHECK(condition) do { if (!(condition)) ++failures; } while (false)

void test_runtime_tags_and_owned_values() {
    using namespace nimblefix;
    std::string msg_type = "Z";
    std::string value = "temporary-value";
    DynamicMessage message{msg_type};
    msg_type.assign("X");
    CHECK(message.set(900001, std::string_view{value}).ok());
    value.assign("mutated");
    CHECK(message.set(900002, std::uint64_t{42}).ok());
    CHECK(message.view().msg_type == "Z");
    CHECK(message.set(900001, "duplicate").code == ErrorCode::duplicate_field);

    const std::array<char, 5> binary{'A', '\x01', 'B', '\0', 'C'};
    CHECK(message.set_data(900010, 900011, {binary.data(), binary.size()}).ok());
    const auto view = message.view();
    SessionHeader header{"BUY", "SELL", 1, "20261031-12:00:00.123456789"};
    std::array<char, 512> wire{};
    const auto measured = measure_fix(view, header);
    const auto encoded = encode_fix(view, header, wire);
    CHECK(measured.ok() && encoded.ok());
    if (!measured.ok() || !encoded.ok()) return;
    CHECK(measured.value() == encoded.value());

    FrameView frame{};
    const std::string_view bytes{wire.data(), encoded.value()};
    CHECK(inspect_frame(bytes, frame, message.wire_hints()).ok());
    std::array<FieldView, 24> scratch{};
    FlatMessageView parsed{};
    CHECK(parse_flat(frame, scratch, parsed, message.wire_hints()).ok());
    CHECK(parsed.find_unique(900001).value().value == "temporary-value");
    CHECK(parsed.find_unique(900002).value().value == "42");
    CHECK(parsed.find_unique(900010).value().value == "5");
    CHECK((parsed.find_unique(900011).value().value == std::string_view{binary.data(), binary.size()}));
}

void test_limits_and_invalid_runtime_tags() {
    using namespace nimblefix;
    DynamicMessage too_small{"Z", {.max_fields = 1, .max_value_bytes = 3, .max_msg_type_bytes = 1}};
    CHECK(too_small.set(58, "four").code == ErrorCode::capacity_exceeded);
    CHECK(too_small.set(59, "x").ok());
    CHECK(too_small.set(60, "y").code == ErrorCode::capacity_exceeded);
    CHECK(too_small.set(34, 1).code == ErrorCode::managed_field);
    CHECK(too_small.set(0, 1).code == ErrorCode::invalid_value);

    DynamicMessage bad_data{"Z"};
    const std::string_view invalid_source{nullptr, 1};
    CHECK(bad_data.set_data(900020, 900021, invalid_source).code == ErrorCode::invalid_value);
    CHECK(bad_data.set_data(900022, 900023, {}).ok());
}

} // namespace

int main() {
    test_runtime_tags_and_owned_values();
    test_limits_and_invalid_runtime_tags();
    return failures == 0 ? 0 : 1;
}
