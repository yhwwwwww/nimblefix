#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include <nimblefix/message.hpp>
#include <nimblefix/protocol.hpp>
#include "nimblefix/status.hpp"

namespace nimblefix {

inline constexpr char soh = '\x01';
inline constexpr std::size_t default_max_frame_bytes = 65'536;
inline constexpr std::size_t max_wire_hint_pairs = 128;

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
    FixVersion version{FixVersion::fix44};
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
    [[nodiscard]] std::string_view msg_type() const noexcept {
        if (fields_.size() >= 3 && fields_[2].tag == 35) return fields_[2].value;
        const auto found = find_unique(35);
        return found.ok() ? found.value().value : std::string_view{};
    }
    template <class Message>
        requires requires { { Message::msg_type } -> std::convertible_to<std::string_view>; }
    [[nodiscard]] bool is() const noexcept {
        return msg_type() == Message::msg_type;
    }
    template <class Message>
        requires requires(const FlatMessageView& v) { typename Message::view_type(v); }
    [[nodiscard]] typename Message::view_type as() const noexcept {
        return typename Message::view_type{*this};
    }

private:
    friend Status parse_flat(FrameView,
                             std::span<FieldView>,
                             FlatMessageView&,
                             WireHints,
                             CodecLimits) noexcept;
    friend Status parse_flat_verified(FrameView,
                                      std::span<FieldView>,
                                      FlatMessageView&,
                                      WireHints) noexcept;

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

// Fast-path flat parser for frames already validated by inspect_frame() with
// matching hints/limits. Skips the redundant framing & checksum pass.
[[nodiscard]] Status parse_flat_verified(FrameView frame,
                                         std::span<FieldView> scratch,
                                         FlatMessageView& output,
                                         WireHints hints = {}) noexcept;

// Runtime FIX session fields are deliberately separate from the message builder.
// The codec has no dependency on a Session or transport implementation.
struct SessionHeader {
    std::string_view sender_comp_id{};
    std::string_view target_comp_id{};
    std::uint64_t msg_seq_num{};
    std::string_view sending_time{};
    bool poss_dup{};
    std::string_view orig_sending_time{};
    FixVersion version{FixVersion::fix44};
    std::optional<std::uint64_t> last_msg_seq_num_processed{};
};

// Measure the complete frame without writing bytes. This runs the same input
// validation and checked size arithmetic as encode_fix, for accepted-before-encode
// reservation by callers with bounded output storage.
[[nodiscard]] Result<std::size_t> measure_fix(detail::MessageView message,
    SessionHeader header) noexcept;

[[nodiscard]] inline Result<std::size_t> encoded_size(detail::MessageView message,
    SessionHeader header) noexcept {
    return measure_fix(message, header);
}

// Encode one complete FIX frame into caller-owned storage. On any error the
// destination span is left unchanged. The destination must not overlap input storage.
[[nodiscard]] Result<std::size_t> encode_fix(detail::MessageView message,
    SessionHeader header, std::span<char> output) noexcept;

// Validate and encode only the application body fields (without session header/trailer).
// Useful for lock-free producer-to-I/O queueing where MsgSeqNum/SendingTime are stamped by the I/O thread.
[[nodiscard]] Result<std::size_t> encode_fix_body(detail::MessageView message,
    std::span<char> output) noexcept;

// Wrap a pre-encoded body (from encode_fix_body) with session header, BodyLength and CheckSum.
[[nodiscard]] Result<std::size_t> encode_fix_from_body(std::string_view msg_type,
    std::string_view prebuilt_body, SessionHeader header, std::span<char> output) noexcept;

// Re-encode a parsed frame for replay. BeginString remains the original version.
[[nodiscard]] Result<std::size_t> encode_fix_replay(const FlatMessageView& original,
    SessionHeader header, std::span<char> output) noexcept;

// FIX 4.4 compatibility spellings. They use the same version-aware encoder.
[[nodiscard]] Result<std::size_t> encode_fix44(detail::MessageView message,
    SessionHeader header, std::span<char> output) noexcept;

// Re-encodes a previously parsed FIX 4.4 message for replay. It retains the
// original MsgSeqNum and OrigSendingTime, sets PossDupFlag=Y and uses the new
// SendingTime supplied in header. Non-managed body fields are copied verbatim
// from the original frame, including Length/Data fields.
[[nodiscard]] Result<std::size_t> encode_fix44_replay(const FlatMessageView& original,
    SessionHeader header, std::span<char> output) noexcept;

template <class Message>
    requires requires(const Message& message) { message.view(); }
[[nodiscard]] Result<std::size_t> encode_fix(const Message& message,
    SessionHeader header, std::span<char> output) noexcept {
    return encode_fix(message.view(), header, output);
}

template <class Message>
    requires requires(const Message& message) { message.view(); }
[[nodiscard]] Result<std::size_t> encode_fix44(const Message& message,
    SessionHeader header, std::span<char> output) noexcept {
    return encode_fix44(message.view(), header, output);
}

} // namespace nimblefix
