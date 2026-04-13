#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "fastfix/base/result.h"
#include "fastfix/codec/fix_codec.h"

namespace fastfix::codec {

struct RawPassThroughView {
    // Session-layer fields (parsed)
    std::string_view begin_string;
    std::string_view msg_type;
    std::uint32_t msg_seq_num{0};
    std::string_view sender_comp_id;
    std::string_view target_comp_id;
    std::string_view sending_time;

    // Raw message body (unparsed application fields)
    // Points into the original receive buffer
    std::span<const std::byte> raw_body;

    // Full raw message for reference
    std::span<const std::byte> raw_message;

    bool valid{false};
};

struct ForwardingOptions {
    std::string_view sender_comp_id;      // downstream sender
    std::string_view target_comp_id;      // downstream target
    std::uint32_t msg_seq_num{0};         // downstream seq num
    std::string_view sending_time;        // current timestamp
    std::string_view begin_string;        // downstream BeginString (empty = use original)

    // Optional routing fields
    std::string_view on_behalf_of_comp_id;   // tag 115, empty = omit
    std::string_view deliver_to_comp_id;     // tag 128, empty = omit
};

struct ReplayOptions {
    std::string_view sender_comp_id;
    std::string_view target_comp_id;
    std::string_view begin_string;
    std::string_view default_appl_ver_id;   // tag 1128, empty = omit
    std::uint32_t msg_seq_num{0};
    std::string_view sending_time;          // new sending time
    std::string_view orig_sending_time;     // original sending time from stored frame
};

auto DecodeRawPassThrough(
    std::span<const std::byte> data,
    char delimiter = kFixSoh,
    bool verify_checksum = true) -> base::Result<RawPassThroughView>;

auto EncodeForwarded(
    const RawPassThroughView& inbound,
    const ForwardingOptions& options,
    EncodeBuffer* buffer) -> base::Status;

auto EncodeReplay(
    const RawPassThroughView& stored,
    const ReplayOptions& options,
    EncodeBuffer* buffer) -> base::Status;

}  // namespace fastfix::codec
