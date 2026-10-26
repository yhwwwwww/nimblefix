#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include <nimblefix/codec.hpp>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view input{reinterpret_cast<const char*>(data), size};
    nimblefix::FrameView frame;
    const auto inspected = nimblefix::inspect_frame(input, frame);
    if (!inspected.ok()) return 0;
    if (frame.consumed > size || frame.bytes.size() != frame.consumed) std::abort();
    std::array<nimblefix::FieldView, 256> scratch;
    nimblefix::FlatMessageView view;
    const auto parsed = nimblefix::parse_flat(frame, scratch, view);
    if (parsed.ok()) {
        std::size_t previous = 0;
        for (const auto& field : view.fields()) {
            if (field.offset < previous || field.offset >= frame.bytes.size() ||
                field.value.data() < frame.bytes.data() ||
                field.value.data() + field.value.size() > frame.bytes.data() + frame.bytes.size()) std::abort();
            previous = field.offset;
        }
    }
    return 0;
}
