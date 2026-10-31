#include <nimblefix/codec.hpp>
#include <nimblefix/fix44.hpp>
#include <nimblefix/schema.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > nimblefix::default_max_frame_bytes) return 0;
    const std::string_view input{reinterpret_cast<const char*>(data), size};
    const auto hints = nimblefix::fix44::Schema::wire_hints();
    nimblefix::FrameView frame;
    if (!nimblefix::inspect_frame(input, frame, hints).ok()) return 0;
    std::array<nimblefix::FieldView, 4096> fields;
    nimblefix::FlatMessageView message;
    if (!nimblefix::parse_flat(frame, fields, message, hints).ok()) return 0;
    (void)nimblefix::validate_schema_message(nimblefix::fix44::Schema::descriptor(), message,
        nimblefix::ValidationLevel::strict);
    return 0;
}
