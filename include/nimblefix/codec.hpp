#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include <nimblefix/message.hpp>
#include "nimblefix/status.hpp"

namespace nimblefix {

inline constexpr char soh = '\x01';
inline constexpr std::size_t default_max_frame_bytes = 65'536;
inline constexpr std::size_t max_wire_hint_pairs = 64;

struct CodecLimits {
    std::size_t max_frame_bytes{default_max_frame_bytes};
};

struct LengthDataPair {
    std::uint32_t length_tag{};
    std::uint32_t data_tag{};
};

// Additional wire-level Length/Data pairs for a counterparty's extensions.
// FIX 4.4 standard pairs are always recognized as well.
struct WireHints {
    std::span<const LengthDataPair> length_data_pairs{};
};

struct FrameView {
    // Exactly one complete frame; coalesced input after it is not included.
    std::string_view bytes{};
    std::size_t consumed{};
};

// On success, output.bytes is the first validated frame and output.consumed is
// its size. need_more means input could still grow into a valid frame.
[[nodiscard]] Status inspect_frame(std::string_view input,
                                   FrameView& output,
                                   WireHints hints = {},
                                   CodecLimits limits = {}) noexcept;

struct FieldView {
    std::uint32_t tag{};
    std::string_view value{};
    // Byte offset of the tag's first digit relative to the start of the frame.
    std::size_t offset{};
};

class FlatMessageView {
public:
    [[nodiscard]] std::string_view frame() const noexcept { return frame_; }
    [[nodiscard]] std::span<const FieldView> fields() const noexcept { return fields_; }
    [[nodiscard]] Result<FieldView> find_unique(std::uint32_t tag) const noexcept;

private:
    friend Status parse_flat(FrameView,
                             std::span<FieldView>,
                             FlatMessageView&,
                             WireHints,
                             CodecLimits) noexcept;

    std::string_view frame_{};
    std::span<const FieldView> fields_{};
};

// Revalidates the frame, then fills caller-owned scratch in wire order. The
// resulting view borrows both the frame bytes and the populated scratch.
[[nodiscard]] Status parse_flat(FrameView frame,
                                std::span<FieldView> scratch,
                                FlatMessageView& output,
                                WireHints hints = {},
                                CodecLimits limits = {}) noexcept;

// Runtime FIX session fields are deliberately separate from the message builder.
// The codec has no dependency on a Session or transport implementation.
struct SessionHeader {
    std::string_view sender_comp_id{};
    std::string_view target_comp_id{};
    std::uint64_t msg_seq_num{};
    std::string_view sending_time{};
};

// Encode one complete FIX 4.4 frame into caller-owned storage. On any error the
// destination span is left unchanged. The result is the number of bytes written.
// The destination must not overlap message or header input storage.
[[nodiscard]] Result<std::size_t> encode_fix44(detail::MessageView message,
    SessionHeader header, std::span<char> output) noexcept;

template <class Message>
    requires requires(const Message& message) { message.view(); }
[[nodiscard]] Result<std::size_t> encode_fix44(const Message& message,
    SessionHeader header, std::span<char> output) noexcept {
    return encode_fix44(message.view(), header, output);
}

} // namespace nimblefix
