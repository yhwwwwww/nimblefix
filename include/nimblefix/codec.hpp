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
    bool verify_checksum{true};
};

struct LengthDataPair {
    std::uint32_t length_tag{};
    std::uint32_t data_tag{};
};

// Additional wire-level Length/Data pairs for a counterparty's extensions.
// Standard FIX Length/Data pairs are always recognized as well.
struct WireHints {
    std::span<const LengthDataPair> length_data_pairs{};
    bool prevalidated{false};
    bool builtin_only{false};
};

struct FrameView {
    // Exactly one complete frame; coalesced input after it is not included.
    std::string_view bytes{};
    std::size_t consumed{};
    FixVersion version{FixVersion::fix44};
    std::uint32_t body_start_offset{};
    std::uint32_t checksum_offset{};
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

namespace detail {

// Generation-stamped O(1) top-level field index populated during single-pass
// schema validation. Zero memset on begin(); touches only present fields.
struct ParsedFieldIndex {
    static constexpr std::size_t max_members = 768;
    std::array<std::uint16_t, max_members> wire_idx{};
    std::array<std::uint16_t, max_members> group_end{};
    std::array<std::uint32_t, max_members> stamp{};
    std::uint32_t generation{1};
    std::uint32_t msg_index{0xFFFF'FFFFu};
    const void* schema_id{nullptr};

    void begin(const void* schema_ptr, std::uint32_t message_idx) noexcept {
        schema_id = schema_ptr;
        msg_index = message_idx;
        if (++generation == 0) {
            stamp.fill(0);
            generation = 1;
        }
    }

    void invalidate() noexcept {
        schema_id = nullptr;
        msg_index = 0xFFFF'FFFFu;
    }

    void record(std::uint32_t member_idx, std::size_t field_idx, std::size_t end_idx = 0) noexcept {
        if (member_idx < max_members && field_idx < 0xFFFFu) {
            wire_idx[member_idx] = static_cast<std::uint16_t>(field_idx);
            group_end[member_idx] = end_idx < 0xFFFFu ? static_cast<std::uint16_t>(end_idx) : 0u;
            stamp[member_idx] = generation;
        }
    }

    [[nodiscard]] bool matches(const void* schema_ptr, std::uint32_t message_idx) const noexcept {
        return schema_id == schema_ptr && msg_index == message_idx;
    }

    [[nodiscard]] int lookup(std::uint32_t member_idx) const noexcept {
        if (member_idx < max_members && stamp[member_idx] == generation) {
            return static_cast<int>(wire_idx[member_idx]);
        }
        return -1;
    }
};

} // namespace detail

class FlatMessageView {
public:
    [[nodiscard]] std::string_view frame() const noexcept { return frame_; }
    [[nodiscard]] std::span<const FieldView> fields() const noexcept { return fields_; }
    [[nodiscard]] Result<FieldView> find_unique(std::uint32_t tag) const noexcept;
    void bind_index(detail::ParsedFieldIndex* idx) const noexcept { parsed_index_ = idx; }
    [[nodiscard]] detail::ParsedFieldIndex* parsed_index() const noexcept { return parsed_index_; }
    [[nodiscard]] std::string_view msg_type() const noexcept {
        if (fields_.size() >= 3 && fields_[2].tag == tag::msg_type) return fields_[2].value;
        const auto found = find_unique(tag::msg_type);
        return found.ok() ? found.value().value : std::string_view{};
    }
    template <class Message>
        requires requires { { Message::msg_type } -> std::convertible_to<std::string_view>; }
    [[nodiscard]] bool is() const noexcept {
        return msg_type() == Message::msg_type;
    }
    template <class Message>
        requires requires { typename Message::view_type; } &&
            std::constructible_from<typename Message::view_type, const FlatMessageView&>
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
    mutable detail::ParsedFieldIndex* parsed_index_{nullptr};
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

#include <nimblefix/stream_encoder.hpp>
