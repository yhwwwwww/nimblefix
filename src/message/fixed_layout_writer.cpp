#include "fastfix/message/fixed_layout_writer.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "fastfix/codec/fast_int_format.h"
#include "fastfix/codec/fix_codec.h"
#include "fastfix/codec/fix_tags.h"
#include "fastfix/codec/simd_scan.h"

namespace fastfix::message {

namespace {

auto IsEncodeManagedTag(std::uint32_t tag) -> bool {
    return fastfix::codec::tags::IsEncodeManagedTag(tag);
}

void SetEncodeStepPrefix(FixedLayout::EncodeStep& step, std::uint32_t tag) {
    auto s = std::to_string(tag);
    s.push_back('=');
    const auto len = std::min(s.size(), step.prefix_data.size());
    std::memcpy(step.prefix_data.data(), s.data(), len);
    step.prefix_length = static_cast<std::uint8_t>(len);
}

void AppendTagToBuffer(std::string& buf, std::uint32_t tag) {
    char tmp[10];
    const auto len = codec::FormatUint32(tmp, tag);
    buf.append(tmp, len);
}

}  // namespace

// ---------------------------------------------------------------------------
// FixedLayout
// ---------------------------------------------------------------------------

auto FixedLayout::Build(
    const profile::NormalizedDictionaryView& dictionary,
    std::string_view msg_type) -> base::Result<FixedLayout> {
    const auto* message_def = dictionary.find_message(msg_type);
    if (message_def == nullptr) {
        return base::Status::NotFound("FixedLayout::Build: unknown message type");
    }

    FixedLayout layout;
    layout.msg_type_ = std::string(msg_type);

    const auto rules = dictionary.message_field_rules(*message_def);
    std::uint32_t field_slot = 0;
    std::uint32_t group_slot = 0;

    // First pass: assign slot indices.
    for (const auto& rule : rules) {
        if (IsEncodeManagedTag(rule.tag)) {
            continue;
        }
        if (dictionary.find_group(rule.tag) != nullptr) {
            layout.group_slots_.push_back({rule.tag, group_slot++});
        } else {
            layout.field_slots_.push_back({rule.tag, field_slot++});
        }
    }

    layout.total_field_slots_ = field_slot;
    layout.total_group_slots_ = group_slot;

    // Build encode_order_ BEFORE sorting field_slots_ / group_slots_.
    for (std::size_t rule_index = 0; rule_index < rules.size(); ++rule_index) {
        const auto& rule = rules[rule_index];
        if (IsEncodeManagedTag(rule.tag)) {
            continue;
        }
        EncodeStep step{};
        step.tag = rule.tag;
        step.rule_index = static_cast<std::uint32_t>(rule_index);
        SetEncodeStepPrefix(step, rule.tag);
        if (dictionary.find_group(rule.tag) != nullptr) {
            step.kind = EncodeStep::Kind::kGroup;
            for (const auto& gs : layout.group_slots_) {
                if (gs.count_tag == rule.tag) {
                    step.slot_index = gs.slot_index;
                    break;
                }
            }
        } else {
            step.kind = EncodeStep::Kind::kField;
            for (const auto& fs : layout.field_slots_) {
                if (fs.tag == rule.tag) {
                    step.slot_index = fs.slot_index;
                    break;
                }
            }
        }
        layout.encode_order_.push_back(std::move(step));
    }

    // Pre-compute msg_type_fragment_: "35={msg_type}\x01"
    layout.msg_type_fragment_.reserve(3U + msg_type.size() + 1U);
    layout.msg_type_fragment_.append(codec::tags::kMsgTypePrefix);
    layout.msg_type_fragment_.append(msg_type);
    layout.msg_type_fragment_.push_back(codec::kFixSoh);

    std::sort(layout.field_slots_.begin(), layout.field_slots_.end(),
        [](const auto& a, const auto& b) { return a.tag < b.tag; });
    std::sort(layout.group_slots_.begin(), layout.group_slots_.end(),
        [](const auto& a, const auto& b) { return a.count_tag < b.count_tag; });

    // Build direct tag -> slot lookup for O(1) access.
    if (!layout.field_slots_.empty()) {
        const auto max_tag = layout.field_slots_.back().tag;  // sorted, so last is max
        layout.tag_to_slot_.assign(max_tag + 1U, -1);
        for (const auto& fs : layout.field_slots_) {
            layout.tag_to_slot_[fs.tag] = static_cast<int>(fs.slot_index);
        }
    }

    return layout;
}

auto FixedLayout::slot_index(std::uint32_t tag) const -> int {
    if (tag < tag_to_slot_.size()) {
        return tag_to_slot_[tag];
    }
    return -1;
}

auto FixedLayout::group_slot_index(std::uint32_t count_tag) const -> int {
    auto it = std::lower_bound(
        group_slots_.begin(), group_slots_.end(), count_tag,
        [](const GroupSlot& slot, std::uint32_t t) { return slot.count_tag < t; });
    if (it != group_slots_.end() && it->count_tag == count_tag) {
        return static_cast<int>(it->slot_index);
    }
    return -1;
}

// ---------------------------------------------------------------------------
// FixedGroupEntryBuilder
// ---------------------------------------------------------------------------

auto FixedGroupEntryBuilder::set_string(std::uint32_t tag, std::string_view value) -> FixedGroupEntryBuilder& {
    AppendTagToBuffer(*buffer_, tag);
    buffer_->push_back('=');
    buffer_->append(value);
    buffer_->push_back(codec::kFixSoh);
    return *this;
}

auto FixedGroupEntryBuilder::set_int(std::uint32_t tag, std::int64_t value) -> FixedGroupEntryBuilder& {
    AppendTagToBuffer(*buffer_, tag);
    buffer_->push_back('=');
    char buf[20];
    const auto len = codec::FormatInt64(buf, value);
    buffer_->append(buf, len);
    buffer_->push_back(codec::kFixSoh);
    return *this;
}

auto FixedGroupEntryBuilder::set_char(std::uint32_t tag, char value) -> FixedGroupEntryBuilder& {
    AppendTagToBuffer(*buffer_, tag);
    buffer_->push_back('=');
    buffer_->push_back(value);
    buffer_->push_back(codec::kFixSoh);
    return *this;
}

auto FixedGroupEntryBuilder::set_float(std::uint32_t tag, double value) -> FixedGroupEntryBuilder& {
    AppendTagToBuffer(*buffer_, tag);
    buffer_->push_back('=');
    std::array<char, 32> buf{};
    const auto [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value,
                                          std::chars_format::general, 12);
    if (ec == std::errc()) {
        buffer_->append(buf.data(), static_cast<std::size_t>(ptr - buf.data()));
    }
    buffer_->push_back(codec::kFixSoh);
    return *this;
}

auto FixedGroupEntryBuilder::set_boolean(std::uint32_t tag, bool value) -> FixedGroupEntryBuilder& {
    AppendTagToBuffer(*buffer_, tag);
    buffer_->push_back('=');
    buffer_->push_back(value ? 'Y' : 'N');
    buffer_->push_back(codec::kFixSoh);
    return *this;
}

// ---------------------------------------------------------------------------
// FixedLayoutWriter
// ---------------------------------------------------------------------------

FixedLayoutWriter::FixedLayoutWriter(const FixedLayout& layout)
    : layout_(&layout) {
    for (std::size_t i = 0; i < layout.total_field_slots_; ++i) {
        slot_ranges_.push_back(SlotRange{});
    }
}

auto FixedLayoutWriter::clear() -> void {
    // Reset slot ranges: keep capacity, set all lengths to 0.
    for (std::size_t i = 0; i < slot_ranges_.size(); ++i) {
        slot_ranges_[i] = SlotRange{};
    }
    slot_buffer_.clear();
    for (auto& g : groups_) {
        // Clear entry strings but preserve their heap capacity for reuse.
        for (std::size_t i = 0; i < g.active_count; ++i) {
            g.entries[i].field_bytes.clear();
        }
        g.active_count = 0;
    }
}

auto FixedLayoutWriter::bind_session(
    std::string_view begin_string,
    std::string_view sender_comp_id,
    std::string_view target_comp_id) -> void {
    session_bound_ = true;
    auto& frag = session_header_;
    frag.static_checksum = 0;

    constexpr std::size_t kBLPlaceholderWidth = 7U;

    // Build header_prefix: "8={bs}\x01 9=0000000\x01 35={mt}\x01"
    frag.header_prefix.clear();
    frag.header_prefix.reserve(
        2U + begin_string.size() + 1U +  // "8={bs}\x01"
        2U + kBLPlaceholderWidth + 1U +   // "9=0000000000\x01"
        layout_->msg_type_fragment_.size());

    frag.header_prefix.append(codec::tags::kBeginStringPrefix);
    frag.header_prefix.append(begin_string);
    frag.header_prefix.push_back(codec::kFixSoh);
    frag.header_prefix.append(codec::tags::kBodyLengthPrefix);
    frag.body_length_offset = frag.header_prefix.size();
    frag.header_prefix.append("0000000000", kBLPlaceholderWidth);
    frag.header_prefix.push_back(codec::kFixSoh);
    frag.body_start_offset = frag.header_prefix.size();
    frag.header_prefix.append(layout_->msg_type_fragment_);

    frag.sender_fragment.clear();
    frag.sender_fragment.reserve(3U + sender_comp_id.size() + 1U);
    frag.sender_fragment.append(codec::tags::kSenderCompIDPrefix);
    frag.sender_fragment.append(sender_comp_id);
    frag.sender_fragment.push_back(codec::kFixSoh);

    frag.target_fragment.clear();
    frag.target_fragment.reserve(3U + target_comp_id.size() + 1U);
    frag.target_fragment.append(codec::tags::kTargetCompIDPrefix);
    frag.target_fragment.append(target_comp_id);
    frag.target_fragment.push_back(codec::kFixSoh);

    // Pre-compute checksum for all static bytes.
    frag.static_checksum = 0;
    for (const auto ch : frag.header_prefix) {
        frag.static_checksum += static_cast<unsigned char>(ch);
    }
    for (const auto ch : frag.sender_fragment) {
        frag.static_checksum += static_cast<unsigned char>(ch);
    }
    for (const auto ch : frag.target_fragment) {
        frag.static_checksum += static_cast<unsigned char>(ch);
    }
}

auto FixedLayoutWriter::set_string(std::uint32_t tag, std::string_view value) -> FixedLayoutWriter& {
    const auto idx = layout_->slot_index(tag);
    if (idx >= 0) {
        const auto slot = static_cast<std::size_t>(idx);
        slot_ranges_[slot] = SlotRange{
            static_cast<std::uint32_t>(slot_buffer_.size()),
            static_cast<std::uint32_t>(value.size())};
        slot_buffer_.append(value);
    }
    return *this;
}

auto FixedLayoutWriter::set_int(std::uint32_t tag, std::int64_t value) -> FixedLayoutWriter& {
    const auto idx = layout_->slot_index(tag);
    if (idx >= 0) {
        const auto slot = static_cast<std::size_t>(idx);
        char buf[20];
        const auto len = codec::FormatInt64(buf, value);
        slot_ranges_[slot] = SlotRange{
            static_cast<std::uint32_t>(slot_buffer_.size()),
            static_cast<std::uint32_t>(len)};
        slot_buffer_.append(buf, len);
    }
    return *this;
}

auto FixedLayoutWriter::set_char(std::uint32_t tag, char value) -> FixedLayoutWriter& {
    const auto idx = layout_->slot_index(tag);
    if (idx >= 0) {
        const auto slot = static_cast<std::size_t>(idx);
        slot_ranges_[slot] = SlotRange{
            static_cast<std::uint32_t>(slot_buffer_.size()), 1};
        slot_buffer_.push_back(value);
    }
    return *this;
}

auto FixedLayoutWriter::set_float(std::uint32_t tag, double value) -> FixedLayoutWriter& {
    const auto idx = layout_->slot_index(tag);
    if (idx >= 0) {
        const auto slot = static_cast<std::size_t>(idx);
        std::array<char, 32> buf{};
        const auto [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value,
                                              std::chars_format::general, 12);
        if (ec == std::errc()) {
            const auto len = static_cast<std::size_t>(ptr - buf.data());
            slot_ranges_[slot] = SlotRange{
                static_cast<std::uint32_t>(slot_buffer_.size()),
                static_cast<std::uint32_t>(len)};
            slot_buffer_.append(buf.data(), len);
        }
    }
    return *this;
}

auto FixedLayoutWriter::set_boolean(std::uint32_t tag, bool value) -> FixedLayoutWriter& {
    const auto idx = layout_->slot_index(tag);
    if (idx >= 0) {
        const auto slot = static_cast<std::size_t>(idx);
        slot_ranges_[slot] = SlotRange{
            static_cast<std::uint32_t>(slot_buffer_.size()), 1};
        slot_buffer_.push_back(value ? 'Y' : 'N');
    }
    return *this;
}

auto FixedLayoutWriter::add_group_entry(std::uint32_t count_tag) -> FixedGroupEntryBuilder {
    for (auto& g : groups_) {
        if (g.count_tag == count_tag) {
            if (g.active_count < g.entries.size()) {
                // Reuse existing entry (preserves string capacity).
                auto& entry = g.entries[g.active_count++];
                return FixedGroupEntryBuilder(&entry.field_bytes);
            }
            g.entries.push_back(GroupEntryData{});
            ++g.active_count;
            return FixedGroupEntryBuilder(&g.entries.back().field_bytes);
        }
    }
    groups_.push_back(GroupEncodeData{.count_tag = count_tag});
    groups_.back().entries.push_back(GroupEntryData{});
    groups_.back().active_count = 1;
    return FixedGroupEntryBuilder(&groups_.back().entries.back().field_bytes);
}

auto FixedLayoutWriter::reserve_group_entries(std::uint32_t count_tag, std::size_t count) -> void {
    for (auto& g : groups_) {
        if (g.count_tag == count_tag) {
            g.entries.reserve(count);
            return;
        }
    }
    groups_.push_back(GroupEncodeData{.count_tag = count_tag, .entries = {}, .active_count = 0});
    groups_.back().entries.reserve(count);
}

auto FixedLayoutWriter::encode_to_buffer(
    const profile::NormalizedDictionaryView& /*dictionary*/,
    const codec::EncodeOptions& options,
    codec::EncodeBuffer* buffer) const -> base::Status {
    if (buffer == nullptr) {
        return base::Status::InvalidArgument("encode buffer is null");
    }

    const char delimiter = (options.delimiter == '\0') ? codec::kFixSoh : options.delimiter;
    auto& out = buffer->storage;
    out.clear();

    constexpr std::size_t kBLPlaceholderWidth = 7U;
    std::size_t bl_offset = 0;
    std::size_t body_start = 0;

    if (session_bound_ && delimiter == codec::kFixSoh) {
        // --- Fast path: use pre-built session header fragments ---
        const auto& frag = session_header_;

        // 1-3. Header prefix (BeginString + BodyLength placeholder + MsgType) — memcpy
        out.append(frag.header_prefix);
        bl_offset = frag.body_length_offset;
        body_start = frag.body_start_offset;

        // 4. MsgSeqNum (dynamic)
        {
            const auto seq = options.msg_seq_num == 0U ? 1U : options.msg_seq_num;
            char buf[10];
            const auto len = codec::FormatUint32(buf, seq);
            out.append(codec::tags::kMsgSeqNumPrefix);
            out.append(buf, len);
            out.push_back(delimiter);
        }

        // 5-8. Pre-built SenderCompID / TargetCompID plus optional sub IDs.
        out.append(frag.sender_fragment);
        if (!options.sender_sub_id.empty()) {
            out.append(codec::tags::kSenderSubIDPrefix);
            out.append(options.sender_sub_id);
            out.push_back(delimiter);
        }
        out.append(frag.target_fragment);
        if (!options.target_sub_id.empty()) {
            out.append(codec::tags::kTargetSubIDPrefix);
            out.append(options.target_sub_id);
            out.push_back(delimiter);
        }
    } else {
        // --- Slow path: format each header field individually ---

        // 1. BeginString: 8={begin_string}\x01  9=
        out.append(codec::tags::kBeginStringPrefix);
        out.append(options.begin_string);
        out.push_back(delimiter);
        out.append(codec::tags::kBodyLengthPrefix);

        // 2. BodyLength placeholder (7 chars reserved)
        bl_offset = out.size();
        out.append("0000000", kBLPlaceholderWidth);
        out.push_back(delimiter);
        body_start = out.size();

        // 3. MsgType
        out.append(layout_->msg_type_fragment_);

        // 4. MsgSeqNum
        {
            const auto seq = options.msg_seq_num == 0U ? 1U : options.msg_seq_num;
            char buf[10];
            const auto len = codec::FormatUint32(buf, seq);
            out.append(codec::tags::kMsgSeqNumPrefix);
            out.append(buf, len);
            out.push_back(delimiter);
        }

        // 5. SenderCompID
        out.append(codec::tags::kSenderCompIDPrefix);
        out.append(options.sender_comp_id);
        out.push_back(delimiter);

        // 6. SenderSubID (optional)
        if (!options.sender_sub_id.empty()) {
            out.append(codec::tags::kSenderSubIDPrefix);
            out.append(options.sender_sub_id);
            out.push_back(delimiter);
        }

        // 7. TargetCompID
        out.append(codec::tags::kTargetCompIDPrefix);
        out.append(options.target_comp_id);
        out.push_back(delimiter);

        // 8. TargetSubID (optional)
        if (!options.target_sub_id.empty()) {
            out.append(codec::tags::kTargetSubIDPrefix);
            out.append(options.target_sub_id);
            out.push_back(delimiter);
        }
    }

    // 7. SendingTime
    codec::UtcTimestampBuffer ts_buf;
    const auto sending_time = options.sending_time.empty()
        ? codec::CurrentUtcTimestamp(&ts_buf)
        : options.sending_time;
    out.append(codec::tags::kSendingTimePrefix);
    out.append(sending_time);
    out.push_back(delimiter);

    // 8. DefaultApplVerID (Logon only)
    if (layout_->msg_type() == "A" && !options.default_appl_ver_id.empty()) {
        out.append(codec::tags::kDefaultApplVerIDPrefix);
        out.append(options.default_appl_ver_id);
        out.push_back(delimiter);
    }

    // 9. PossDup (optional)
    if (options.poss_dup) {
        out.append(codec::tags::kPossDupFlagYesField);
        out.push_back(delimiter);
    }

    // 10. PossResend (optional)
    if (options.poss_resend) {
        out.append(codec::tags::kPossResendYesField);
        out.push_back(delimiter);
    }

    // 11. OrigSendingTime (optional)
    if (!options.orig_sending_time.empty()) {
        out.append(codec::tags::kOrigSendingTimePrefix);
        out.append(options.orig_sending_time);
        out.push_back(delimiter);
    }

    // 12. Routing headers (optional)
    if (!options.on_behalf_of_comp_id.empty()) {
        out.append(codec::tags::kOnBehalfOfCompIDPrefix);
        out.append(options.on_behalf_of_comp_id);
        out.push_back(delimiter);
    }
    if (!options.deliver_to_comp_id.empty()) {
        out.append(codec::tags::kDeliverToCompIDPrefix);
        out.append(options.deliver_to_comp_id);
        out.push_back(delimiter);
    }

    // 13. Body fields + groups from encode_order_
    for (const auto& step : layout_->encode_order_) {
        if (step.kind == FixedLayout::EncodeStep::Kind::kField) {
            const auto& range = slot_ranges_[step.slot_index];
            if (range.length > 0) {
                out.append(step.prefix());
                out.append(slot_buffer_.data() + range.offset, range.length);
                out.push_back(delimiter);
            }
        } else {
            // Group: find in groups_ and encode pre-formatted entry bytes.
            for (const auto& g : groups_) {
                if (g.count_tag == step.tag) {
                    // Write "TAG=count\x01"
                    out.append(step.prefix());
                    char count_buf[10];
                    const auto count_len = codec::FormatUint32(count_buf,
                        static_cast<std::uint32_t>(g.active_count));
                    out.append(count_buf, count_len);
                    out.push_back(delimiter);
                    // Write each entry's pre-formatted bytes.
                    for (std::size_t ei = 0; ei < g.active_count; ++ei) {
                        const auto& entry = g.entries[ei];
                        if (delimiter == codec::kFixSoh) {
                            out.append(entry.field_bytes);
                        } else {
                            for (const auto ch : entry.field_bytes) {
                                if (ch == codec::kFixSoh) {
                                    out.push_back(delimiter);
                                } else {
                                    out.push_back(ch);
                                }
                            }
                        }
                    }
                    break;
                }
            }
        }
    }

    // 12. Replace BodyLength placeholder
    {
        const auto body_length = static_cast<std::uint32_t>(out.size() - body_start);
        char buf[10];
        const auto len = codec::FormatUint32(buf, body_length);
        if (len > kBLPlaceholderWidth) {
            return base::Status::FormatError(
                "encoded body length exceeds BodyLength placeholder width");
        }
        out.replace(bl_offset, kBLPlaceholderWidth, buf, len);
    }

    // 13. SIMD checksum over the complete buffer (before trailer).
    const auto checksum = codec::ComputeChecksumSIMD(out.data(), out.size()) % 256U;

    // 14. Trailer: 10=NNN\x01  (NOT included in checksum)
    out.append(codec::tags::kCheckSumPrefix);
    std::array<char, 3> ck{};
    ck[0] = static_cast<char>('0' + ((checksum / 100U) % 10U));
    ck[1] = static_cast<char>('0' + ((checksum / 10U) % 10U));
    ck[2] = static_cast<char>('0' + (checksum % 10U));
    out.append(ck.data(), 3U);
    out.push_back(delimiter);

    return base::Status::Ok();
}

auto FixedLayoutWriter::encode_to_buffer(
    const profile::NormalizedDictionaryView& /*dictionary*/,
    const codec::EncodeOptions& options,
    codec::EncodedOutboundExtrasView extras,
    codec::EncodeBuffer* buffer) const -> base::Status {
    if (buffer == nullptr) {
        return base::Status::InvalidArgument("encode buffer is null");
    }

    const char delimiter = (options.delimiter == '\0') ? codec::kFixSoh : options.delimiter;
    auto& out = buffer->storage;
    out.clear();

    constexpr std::size_t kBLPlaceholderWidth = 7U;
    std::size_t bl_offset = 0;
    std::size_t body_start = 0;

    if (session_bound_ && delimiter == codec::kFixSoh) {
        const auto& frag = session_header_;

        out.append(frag.header_prefix);
        bl_offset = frag.body_length_offset;
        body_start = frag.body_start_offset;

        {
            const auto seq = options.msg_seq_num == 0U ? 1U : options.msg_seq_num;
            char buf[10];
            const auto len = codec::FormatUint32(buf, seq);
            out.append(codec::tags::kMsgSeqNumPrefix);
            out.append(buf, len);
            out.push_back(delimiter);
        }

        out.append(frag.sender_fragment);
        if (!options.sender_sub_id.empty()) {
            out.append(codec::tags::kSenderSubIDPrefix);
            out.append(options.sender_sub_id);
            out.push_back(delimiter);
        }
        out.append(frag.target_fragment);
        if (!options.target_sub_id.empty()) {
            out.append(codec::tags::kTargetSubIDPrefix);
            out.append(options.target_sub_id);
            out.push_back(delimiter);
        }
    } else {
        const auto begin_string = options.begin_string.empty()
            ? std::string_view("FIX.4.4")
            : std::string_view(options.begin_string);

        out.append(codec::tags::kBeginStringPrefix);
        out.append(begin_string);
        out.push_back(delimiter);
        out.append(codec::tags::kBodyLengthPrefix);

        bl_offset = out.size();
        out.append("0000000", kBLPlaceholderWidth);
        out.push_back(delimiter);
        body_start = out.size();

        out.append(layout_->msg_type_fragment_);

        {
            const auto seq = options.msg_seq_num == 0U ? 1U : options.msg_seq_num;
            char buf[10];
            const auto len = codec::FormatUint32(buf, seq);
            out.append(codec::tags::kMsgSeqNumPrefix);
            out.append(buf, len);
            out.push_back(delimiter);
        }

        out.append(codec::tags::kSenderCompIDPrefix);
        out.append(options.sender_comp_id);
        out.push_back(delimiter);
        if (!options.sender_sub_id.empty()) {
            out.append(codec::tags::kSenderSubIDPrefix);
            out.append(options.sender_sub_id);
            out.push_back(delimiter);
        }
        out.append(codec::tags::kTargetCompIDPrefix);
        out.append(options.target_comp_id);
        out.push_back(delimiter);
        if (!options.target_sub_id.empty()) {
            out.append(codec::tags::kTargetSubIDPrefix);
            out.append(options.target_sub_id);
            out.push_back(delimiter);
        }
    }

    codec::UtcTimestampBuffer ts_buf;
    const auto sending_time = options.sending_time.empty()
        ? codec::CurrentUtcTimestamp(&ts_buf)
        : options.sending_time;
    out.append(codec::tags::kSendingTimePrefix);
    out.append(sending_time);
    out.push_back(delimiter);

    if (layout_->msg_type() == "A" && !options.default_appl_ver_id.empty()) {
        out.append(codec::tags::kDefaultApplVerIDPrefix);
        out.append(options.default_appl_ver_id);
        out.push_back(delimiter);
    }
    if (options.poss_dup) {
        out.append(codec::tags::kPossDupFlagYesField);
        out.push_back(delimiter);
    }
    if (options.poss_resend) {
        out.append(codec::tags::kPossResendYesField);
        out.push_back(delimiter);
    }
    if (!options.orig_sending_time.empty()) {
        out.append(codec::tags::kOrigSendingTimePrefix);
        out.append(options.orig_sending_time);
        out.push_back(delimiter);
    }
    if (!options.on_behalf_of_comp_id.empty()) {
        out.append(codec::tags::kOnBehalfOfCompIDPrefix);
        out.append(options.on_behalf_of_comp_id);
        out.push_back(delimiter);
    }
    if (!options.deliver_to_comp_id.empty()) {
        out.append(codec::tags::kDeliverToCompIDPrefix);
        out.append(options.deliver_to_comp_id);
        out.push_back(delimiter);
    }
    if (!extras.header_fragment.empty()) {
        out.append(extras.header_fragment);
    }

    for (const auto& step : layout_->encode_order_) {
        if (step.kind == FixedLayout::EncodeStep::Kind::kField) {
            const auto& range = slot_ranges_[step.slot_index];
            if (range.length > 0) {
                out.append(step.prefix());
                out.append(slot_buffer_.data() + range.offset, range.length);
                out.push_back(delimiter);
            }
        } else {
            for (const auto& g : groups_) {
                if (g.count_tag == step.tag) {
                    out.append(step.prefix());
                    char count_buf[10];
                    const auto count_len = codec::FormatUint32(
                        count_buf,
                        static_cast<std::uint32_t>(g.active_count));
                    out.append(count_buf, count_len);
                    out.push_back(delimiter);
                    for (std::size_t ei = 0; ei < g.active_count; ++ei) {
                        const auto& entry = g.entries[ei];
                        if (delimiter == codec::kFixSoh) {
                            out.append(entry.field_bytes);
                        } else {
                            for (const auto ch : entry.field_bytes) {
                                if (ch == codec::kFixSoh) {
                                    out.push_back(delimiter);
                                } else {
                                    out.push_back(ch);
                                }
                            }
                        }
                    }
                    break;
                }
            }
        }
    }
    if (!extras.body_fragment.empty()) {
        out.append(extras.body_fragment);
    }

    {
        const auto body_length = static_cast<std::uint32_t>(out.size() - body_start);
        char buf[10];
        const auto len = codec::FormatUint32(buf, body_length);
        if (len > kBLPlaceholderWidth) {
            return base::Status::FormatError(
                "encoded body length exceeds BodyLength placeholder width");
        }
        out.replace(bl_offset, kBLPlaceholderWidth, buf, len);
    }

    const auto checksum = codec::ComputeChecksumSIMD(out.data(), out.size()) % 256U;

    out.append(codec::tags::kCheckSumPrefix);
    std::array<char, 3> ck{};
    ck[0] = static_cast<char>('0' + ((checksum / 100U) % 10U));
    ck[1] = static_cast<char>('0' + ((checksum / 10U) % 10U));
    ck[2] = static_cast<char>('0' + (checksum % 10U));
    out.append(ck.data(), 3U);
    out.push_back(delimiter);

    return base::Status::Ok();
}

auto FixedLayoutWriter::encode_to_buffer(
    const profile::NormalizedDictionaryView& dictionary,
    const codec::EncodeOptions& options,
    codec::EncodeBuffer* buffer,
    const codec::PrecompiledTemplateTable* /*precompiled*/) const -> base::Status {
    return encode_to_buffer(dictionary, options, buffer);
}

auto FixedLayoutWriter::encode_to_buffer(
    const profile::NormalizedDictionaryView& dictionary,
    const codec::EncodeOptions& options,
    codec::EncodedOutboundExtrasView extras,
    codec::EncodeBuffer* buffer,
    const codec::PrecompiledTemplateTable* /*precompiled*/) const -> base::Status {
    return encode_to_buffer(dictionary, options, extras, buffer);
}

auto FixedLayoutWriter::encode(
    const profile::NormalizedDictionaryView& dictionary,
    const codec::EncodeOptions& options) const -> base::Result<std::vector<std::byte>> {
    codec::EncodeBuffer buf;
    auto status = encode_to_buffer(dictionary, options, &buf);
    if (!status.ok()) return status;
    std::vector<std::byte> bytes(buf.size());
    if (!bytes.empty()) {
        std::memcpy(bytes.data(), buf.storage.data(), buf.size());
    }
    return bytes;
}

auto FixedLayoutWriter::encode(
    const profile::NormalizedDictionaryView& dictionary,
    const codec::EncodeOptions& options,
    codec::EncodedOutboundExtrasView extras) const -> base::Result<std::vector<std::byte>> {
    codec::EncodeBuffer buf;
    auto status = encode_to_buffer(dictionary, options, extras, &buf);
    if (!status.ok()) {
        return status;
    }
    std::vector<std::byte> bytes(buf.size());
    if (!bytes.empty()) {
        std::memcpy(bytes.data(), buf.storage.data(), buf.size());
    }
    return bytes;
}

}  // namespace fastfix::message
