#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "nimblefix/base/result.h"
#include "nimblefix/codec/fix_codec.h"
#include "nimblefix/session/encoded_frame.h"

namespace nimble::codec {

struct RawPassThroughView
{
  // Session-layer fields (parsed)
  std::string_view begin_string;
  std::string_view msg_type;
  std::uint32_t msg_seq_num{ 0 };
  std::string_view sender_comp_id;
  std::string_view sender_sub_id;
  std::string_view target_comp_id;
  std::string_view target_sub_id;
  std::string_view on_behalf_of_comp_id;
  std::string_view deliver_to_comp_id;
  std::string_view default_appl_ver_id;
  std::string_view sending_time;
  std::string_view orig_sending_time;
  bool poss_dup{ false };
  bool poss_resend{ false };

  // Raw message body (unparsed application fields)
  // Points into the original receive buffer
  std::span<const std::byte> raw_body;
  std::uint32_t body_start_offset{ 0 };

  // Full raw message for reference
  std::span<const std::byte> raw_message;

  bool valid{ false };
};

struct ForwardingOptions
{
  std::string_view sender_comp_id; // downstream sender
  std::string_view sender_sub_id;  // downstream sender sub id
  std::string_view target_comp_id; // downstream target
  std::string_view target_sub_id;  // downstream target sub id
  std::uint32_t msg_seq_num{ 0 };  // downstream seq num
  std::string_view sending_time;   // current timestamp
  std::string_view begin_string;   // downstream BeginString (empty = use original)

  // Optional routing fields
  std::string_view on_behalf_of_comp_id; // OnBehalfOfCompID, empty = omit
  std::string_view deliver_to_comp_id;   // DeliverToCompID, empty = omit

  // PossDupFlag / OrigSendingTime for forwarded retransmissions
  bool poss_dup{ false };             // PossDupFlag
  bool poss_resend{ false };          // PossResend
  std::string_view orig_sending_time; // OrigSendingTime, empty = omit

  // Delimiter (default SOH)
  char delimiter{ kFixSoh };
};

struct ReplayOptions
{
  std::string_view sender_comp_id;
  std::string_view sender_sub_id;
  std::string_view target_comp_id;
  std::string_view target_sub_id;
  std::string_view begin_string;
  std::string_view default_appl_ver_id;  // DefaultApplVerID, empty = omit
  std::string_view on_behalf_of_comp_id; // OnBehalfOfCompID, empty = preserve stored
  std::string_view deliver_to_comp_id;   // DeliverToCompID, empty = preserve stored
  std::uint32_t msg_seq_num{ 0 };
  std::string_view sending_time;      // new sending time
  std::string_view orig_sending_time; // original sending time from stored frame
  bool poss_resend{ false };          // PossResend

  // Delimiter (default SOH)
  char delimiter{ kFixSoh };

  // When true, body bytes are referenced via external_body (zero-copy
  // scatter-gather). The caller must ensure the body memory outlives the send.
  // Suitable for mmap-backed stores.
  bool zero_copy_body{ false };
};

auto
DecodeRawPassThrough(std::span<const std::byte> data, char delimiter = kFixSoh, bool verify_checksum = true)
  -> base::Result<RawPassThroughView>;

auto
DecodeRawPassThrough(std::span<const std::byte> data,
                     std::uint32_t body_start_offset,
                     char delimiter = kFixSoh,
                     bool verify_checksum = true) -> base::Result<RawPassThroughView>;

auto
EncodeForwarded(const RawPassThroughView& inbound, const ForwardingOptions& options, EncodeBuffer* buffer)
  -> base::Status;

auto
EncodeReplay(const RawPassThroughView& stored, const ReplayOptions& options, EncodeBuffer* buffer) -> base::Status;

auto
EncodeReplayInto(const RawPassThroughView& stored, const ReplayOptions& options, session::EncodedFrameBytes* out)
  -> base::Status;

} // namespace nimble::codec
