#include "fastfix/message/fixed_layout_writer.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "fastfix/codec/fix_codec.h"

namespace fastfix::message {

namespace {

inline constexpr std::size_t kIntBufSize =
    static_cast<std::size_t>(std::numeric_limits<std::int64_t>::digits10) + 3U;
inline constexpr std::size_t kUintBufSize =
    static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::digits10) + 3U;

auto IsEncodeManagedTag(std::uint32_t tag) -> bool {
    switch (tag) {
        case 8U: case 9U: case 10U:
        case 34U: case 35U: case 43U:
        case 49U: case 52U: case 56U:
            return true;
        default:
            return false;
    }
}

void SetEncodeStepPrefix(FixedLayout::EncodeStep& step, std::uint32_t tag) {
    auto s = std::to_string(tag);
    s.push_back('=');
    const auto len = std::min(s.size(), step.prefix_data.size());
    std::memcpy(step.prefix_data.data(), s.data(), len);
    step.prefix_length = static_cast<std::uint8_t>(len);
}

void AppendTagToBuffer(std::string& buf, std::uint32_t tag) {
    std::array<char, kUintBufSize> tmp{};
    const auto [ptr, ec] = std::to_chars(tmp.data(), tmp.data() + tmp.size(), tag);
    if (ec == std::errc()) {
        buf.append(tmp.data(), static_cast<std::size_t>(ptr - tmp.data()));
    }
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
        if (rule.tag == 35U) {
            continue;  // MsgType is implicit
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
    for (const auto& rule : rules) {
        if (IsEncodeManagedTag(rule.tag)) {
            continue;
        }
        EncodeStep step{};
        step.tag = rule.tag;
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
    layout.msg_type_fragment_.append("35=");
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
    std::array<char, kIntBufSize> buf{};
    const auto [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value);
    if (ec == std::errc()) {
        buffer_->append(buf.data(), static_cast<std::size_t>(ptr - buf.data()));
    }
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

    frag.header_prefix.append("8=");
    frag.header_prefix.append(begin_string);
    frag.header_prefix.push_back(codec::kFixSoh);
    frag.header_prefix.append("9=");
    frag.body_length_offset = frag.header_prefix.size();
    frag.header_prefix.append("0000000000", kBLPlaceholderWidth);
    frag.header_prefix.push_back(codec::kFixSoh);
    frag.body_start_offset = frag.header_prefix.size();
    frag.header_prefix.append(layout_->msg_type_fragment_);

    // Build sender_target: "49={sender}\x01 56={target}\x01"
    frag.sender_target.clear();
    frag.sender_target.reserve(
        3U + sender_comp_id.size() + 1U +
        3U + target_comp_id.size() + 1U);
    frag.sender_target.append("49=");
    frag.sender_target.append(sender_comp_id);
    frag.sender_target.push_back(codec::kFixSoh);
    frag.sender_target.append("56=");
    frag.sender_target.append(target_comp_id);
    frag.sender_target.push_back(codec::kFixSoh);

    // Pre-compute checksum for all static bytes.
    frag.static_checksum = 0;
    for (const auto ch : frag.header_prefix) {
        frag.static_checksum += static_cast<unsigned char>(ch);
    }
    for (const auto ch : frag.sender_target) {
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
        std::array<char, kIntBufSize> buf{};
        const auto [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), value);
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
    std::uint32_t checksum = 0;

    // -- Helpers (capture out & checksum by reference) --
    auto at = [&](std::string_view text) {
        out.append(text);
        for (const auto ch : text) checksum += static_cast<unsigned char>(ch);
    };
    auto ac = [&](char ch) {
        out.push_back(ch);
        checksum += static_cast<unsigned char>(ch);
    };

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
        checksum = frag.static_checksum;  // includes header_prefix + sender_target

        // 4. MsgSeqNum (dynamic)
        {
            const auto seq = options.msg_seq_num == 0U ? 1U : options.msg_seq_num;
            std::array<char, kUintBufSize> buf{};
            const auto [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), seq);
            at("34=");
            if (ec == std::errc()) at(std::string_view(buf.data(), static_cast<std::size_t>(ptr - buf.data())));
            ac(delimiter);
        }

        // 5-6. SenderCompID + TargetCompID — memcpy from pre-built fragment
        //      (checksum already included via static_checksum)
        out.append(frag.sender_target);
    } else {
        // --- Slow path: format each header field individually ---

        // 1. BeginString: 8={begin_string}\x01  9=
        at("8=");
        at(options.begin_string);
        ac(delimiter);
        at("9=");

        // 2. BodyLength placeholder (10 chars reserved)
        bl_offset = out.size();
        at(std::string_view("0000000000", kBLPlaceholderWidth));
        ac(delimiter);
        body_start = out.size();

        // 3. MsgType
        at(layout_->msg_type_fragment_);

        // 4. MsgSeqNum
        {
            const auto seq = options.msg_seq_num == 0U ? 1U : options.msg_seq_num;
            std::array<char, kUintBufSize> buf{};
            const auto [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), seq);
            at("34=");
            if (ec == std::errc()) at(std::string_view(buf.data(), static_cast<std::size_t>(ptr - buf.data())));
            ac(delimiter);
        }

        // 5. SenderCompID
        at("49=");
        at(options.sender_comp_id);
        ac(delimiter);

        // 6. TargetCompID
        at("56=");
        at(options.target_comp_id);
        ac(delimiter);
    }

    // 7. SendingTime
    codec::UtcTimestampBuffer ts_buf;
    const auto sending_time = options.sending_time.empty()
        ? codec::CurrentUtcTimestamp(&ts_buf)
        : options.sending_time;
    at("52=");
    at(sending_time);
    ac(delimiter);

    // 8. DefaultApplVerID (optional)
    if (!options.default_appl_ver_id.empty()) {
        at("1128=");
        at(options.default_appl_ver_id);
        ac(delimiter);
    }

    // 9. PossDup (optional)
    if (options.poss_dup) {
        at("43=Y");
        ac(delimiter);
    }

    // 10. OrigSendingTime (optional)
    if (!options.orig_sending_time.empty()) {
        at("122=");
        at(options.orig_sending_time);
        ac(delimiter);
    }

    // 11. Body fields + groups from encode_order_
    for (const auto& step : layout_->encode_order_) {
        if (step.kind == FixedLayout::EncodeStep::Kind::kField) {
            const auto& range = slot_ranges_[step.slot_index];
            if (range.length > 0) {
                at(step.prefix());
                at(std::string_view(slot_buffer_.data() + range.offset, range.length));
                ac(delimiter);
            }
        } else {
            // Group: find in groups_ and encode pre-formatted entry bytes.
            for (const auto& g : groups_) {
                if (g.count_tag == step.tag) {
                    // Write "TAG=count\x01"
                    at(step.prefix());
                    std::array<char, kUintBufSize> count_buf{};
                    const auto [count_ptr, count_ec] = std::to_chars(
                        count_buf.data(), count_buf.data() + count_buf.size(), g.active_count);
                    if (count_ec == std::errc()) {
                        at(std::string_view(count_buf.data(),
                            static_cast<std::size_t>(count_ptr - count_buf.data())));
                    }
                    ac(delimiter);
                    // Write each entry's pre-formatted bytes.
                    for (std::size_t ei = 0; ei < g.active_count; ++ei) {
                        const auto& entry = g.entries[ei];
                        if (delimiter == codec::kFixSoh) {
                            at(entry.field_bytes);
                        } else {
                            for (const auto ch : entry.field_bytes) {
                                if (ch == codec::kFixSoh) {
                                    ac(delimiter);
                                } else {
                                    ac(ch);
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
        std::array<char, kUintBufSize> buf{};
        const auto [ptr, ec] = std::to_chars(buf.data(), buf.data() + buf.size(), body_length);
        if (ec != std::errc()) {
            return base::Status::FormatError("BodyLength conversion failed");
        }
        const auto len = static_cast<std::size_t>(ptr - buf.data());
        if (len > kBLPlaceholderWidth) {
            return base::Status::FormatError(
                "encoded body length exceeds BodyLength placeholder width");
        }
        // Adjust checksum: subtract placeholder, add real value.
        for (std::size_t i = 0; i < kBLPlaceholderWidth; ++i) {
            checksum -= static_cast<unsigned char>(out[bl_offset + i]);
        }
        for (std::size_t i = 0; i < len; ++i) {
            checksum += static_cast<unsigned char>(buf[i]);
        }
        out.replace(bl_offset, kBLPlaceholderWidth, buf.data(), len);
    }

    // 13. Trailer: 10=NNN\x01  (NOT included in checksum)
    checksum %= 256U;
    out.append("10=");
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

}  // namespace fastfix::message
