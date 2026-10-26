#include <nimblefix/encoder.hpp>

#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <system_error>

namespace nimblefix {
namespace {

constexpr std::string_view begin_string = "8=FIX.4.4\x01";
constexpr std::size_t checksum_suffix_size = 7;

template <class Unsigned>
[[nodiscard]] std::size_t decimal_digits(Unsigned value) noexcept {
    std::size_t digits = 1;
    while (value >= 10) {
        value /= 10;
        ++digits;
    }
    return digits;
}

[[nodiscard]] std::size_t signed_digits(std::int64_t value) noexcept {
    const bool negative = value < 0;
    const auto bits = std::bit_cast<std::uint64_t>(value);
    const auto magnitude = negative ? std::uint64_t{0} - bits : bits;
    return decimal_digits(magnitude) + (negative ? 1 : 0);
}

[[nodiscard]] bool valid_text(std::string_view value) noexcept {
    return detail::is_valid_text(value);
}

[[nodiscard]] Status field_value_text(const detail::MessageView& message,
    const detail::PlanField& field, std::array<char, 32>& scratch, std::string_view& output) noexcept {
    const auto& slot = message.slots[detail::physical_slot_index(message, field.slot_index)];
    switch (field.kind) {
    case FieldKind::character:
        scratch[0] = static_cast<char>(slot.scalar);
        output = {scratch.data(), 1};
        return Status::success();
    case FieldKind::signed_integer: {
        const auto result = std::to_chars(scratch.data(), scratch.data() + scratch.size(),
            std::bit_cast<std::int64_t>(slot.scalar));
        if (result.ec != std::errc{}) return Status{ErrorCode::invalid_value, static_cast<int>(field.tag)};
        output = {scratch.data(), static_cast<std::size_t>(result.ptr - scratch.data())};
        return Status::success();
    }
    case FieldKind::unsigned_integer: {
        const auto result = std::to_chars(scratch.data(), scratch.data() + scratch.size(), slot.scalar);
        if (result.ec != std::errc{}) return Status{ErrorCode::invalid_value, static_cast<int>(field.tag)};
        output = {scratch.data(), static_cast<std::size_t>(result.ptr - scratch.data())};
        return Status::success();
    }
    case FieldKind::decimal: {
        const Decimal value{std::bit_cast<std::int64_t>(slot.scalar), static_cast<std::uint8_t>(slot.size)};
        std::size_t size = 0;
        const auto status = value.format(scratch, size);
        if (!status.ok()) return status;
        output = {scratch.data(), size};
        return Status::success();
    }
    case FieldKind::string:
        output = {message.value_bytes == nullptr ? "" : message.value_bytes + slot.offset, slot.size};
        return Status::success();
    }
    return Status{ErrorCode::invalid_value, static_cast<int>(field.tag)};
}

[[nodiscard]] Status validate_message(const detail::MessageView& message) noexcept {
    if (!message.status.ok()) return message.status;
    if (const auto schema_status = detail::validate_schema_presence(message); !schema_status.ok()) return schema_status;
    if (message.msg_type.empty() || !valid_text(message.msg_type)) return Status{ErrorCode::invalid_value, 35};
    if (message.field_count != 0 && (message.plan == nullptr || message.slots == nullptr)) {
        return Status{ErrorCode::invalid_value};
    }
    if (message.value_byte_count != 0 && message.value_bytes == nullptr) return Status{ErrorCode::invalid_value};

    for (std::size_t i = 0; i < message.field_count; ++i) {
        const auto& field = message.plan[i];
        if (field.slot_index >= message.field_count) return Status{ErrorCode::invalid_value, static_cast<int>(field.tag)};
        if (!detail::is_present(message, field.slot_index)) continue;
        const auto& slot = message.slots[detail::physical_slot_index(message, field.slot_index)];
        switch (field.kind) {
        case FieldKind::character: {
            if (slot.scalar > 0xff || slot.scalar < 0x20 || slot.scalar == 0x7f) {
                return Status{ErrorCode::invalid_value, static_cast<int>(field.tag)};
            }
            break;
        }
        case FieldKind::signed_integer:
        case FieldKind::unsigned_integer:
            break;
        case FieldKind::decimal:
            if (slot.size > 18) return Status{ErrorCode::invalid_decimal, static_cast<int>(field.tag)};
            break;
        case FieldKind::string:
            if (slot.offset > message.value_byte_count ||
                slot.size > message.value_byte_count - slot.offset ||
                (slot.size != 0 && message.value_bytes == nullptr)) {
                return Status{ErrorCode::invalid_value, static_cast<int>(field.tag)};
            }
            if (!valid_text({message.value_bytes == nullptr ? "" : message.value_bytes + slot.offset, slot.size})) {
                return Status{ErrorCode::invalid_value, static_cast<int>(field.tag)};
            }
            break;
        }
        if (field.wire_type == WireType::utc_timestamp) {
            std::array<char, 32> scratch{};
            std::string_view value;
            if (const auto text_status = field_value_text(message, field, scratch, value); !text_status.ok()) {
                return text_status;
            }
            if (!detail::is_valid_utc_timestamp(value)) {
                return Status{ErrorCode::invalid_timestamp, static_cast<int>(field.tag)};
            }
        }
        if (message.enum_value_allowed != nullptr) {
            std::array<char, 32> scratch{};
            std::string_view value;
            if (const auto text_status = field_value_text(message, field, scratch, value); !text_status.ok()) {
                return text_status;
            }
            if (!message.enum_value_allowed(field.tag, value)) {
                return Status{ErrorCode::invalid_value, static_cast<int>(field.tag)};
            }
        }
    }
    return Status::success();
}

[[nodiscard]] std::size_t field_value_size(const detail::MessageView& message,
    const detail::PlanField& field) noexcept {
    const auto& slot = message.slots[detail::physical_slot_index(message, field.slot_index)];
    switch (field.kind) {
    case FieldKind::character:
        return 1;
    case FieldKind::signed_integer: {
        return signed_digits(std::bit_cast<std::int64_t>(slot.scalar));
    }
    case FieldKind::unsigned_integer: {
        return decimal_digits(slot.scalar);
    }
    case FieldKind::decimal: {
        const Decimal value{std::bit_cast<std::int64_t>(slot.scalar), static_cast<std::uint8_t>(slot.size)};
        return value.formatted_size();
    }
    case FieldKind::string:
        return slot.size;
    }
    return 0;
}

class Writer {
public:
    explicit Writer(std::span<char> output) noexcept : output_(output) {}

    void append(std::string_view value) noexcept {
        for (const char ch : value) append(ch);
    }

    void append(char value) noexcept {
        output_[position_++] = value;
        checksum_ = (checksum_ + static_cast<unsigned char>(value)) & 0xffU;
    }

    void append_unsigned(std::uint64_t value) noexcept {
        const auto result = std::to_chars(output_.data() + position_, output_.data() + output_.size(), value);
        update_checksum(static_cast<std::size_t>(result.ptr - (output_.data() + position_)));
    }

    void append_signed(std::int64_t value) noexcept {
        const auto result = std::to_chars(output_.data() + position_, output_.data() + output_.size(), value);
        update_checksum(static_cast<std::size_t>(result.ptr - (output_.data() + position_)));
    }

    void append_decimal(const detail::SlotValue& slot) noexcept {
        std::size_t size = 0;
        const Decimal value{std::bit_cast<std::int64_t>(slot.scalar), static_cast<std::uint8_t>(slot.size)};
        (void)value.format(output_.subspan(position_), size);
        update_checksum(size);
    }

    void append_value(const detail::MessageView& message, const detail::PlanField& field) noexcept {
        const auto& slot = message.slots[detail::physical_slot_index(message, field.slot_index)];
        switch (field.kind) {
        case FieldKind::character:
            append(static_cast<char>(slot.scalar));
            break;
        case FieldKind::signed_integer:
            append_signed(std::bit_cast<std::int64_t>(slot.scalar));
            break;
        case FieldKind::unsigned_integer:
            append_unsigned(slot.scalar);
            break;
        case FieldKind::decimal:
            append_decimal(slot);
            break;
        case FieldKind::string:
            append(std::string_view{message.value_bytes == nullptr ? "" : message.value_bytes + slot.offset, slot.size});
            break;
        }
    }

    void append_checksum_suffix() noexcept {
        const auto checksum = static_cast<unsigned>(checksum_);
        output_[position_++] = '1';
        output_[position_++] = '0';
        output_[position_++] = '=';
        output_[position_++] = static_cast<char>('0' + (checksum / 100) % 10);
        output_[position_++] = static_cast<char>('0' + (checksum / 10) % 10);
        output_[position_++] = static_cast<char>('0' + checksum % 10);
        output_[position_++] = nimblefix::soh;
    }

private:
    void update_checksum(std::size_t size) noexcept {
        for (std::size_t i = 0; i < size; ++i) checksum_ = (checksum_ + static_cast<unsigned char>(output_[position_ + i])) & 0xffU;
        position_ += size;
    }

    std::span<char> output_;
    std::size_t position_{};
    unsigned checksum_{};
};

[[nodiscard]] Status validate_header(const detail::MessageView& message, const SessionHeader& header) noexcept {
    if (!valid_text(header.sender_comp_id) || !valid_text(header.target_comp_id) || header.msg_seq_num == 0) {
        return Status{ErrorCode::invalid_header};
    }
    if (!valid_text(header.sending_time) || !detail::is_valid_utc_timestamp(header.sending_time)) {
        return Status{ErrorCode::invalid_timestamp, 52};
    }
    if (header.poss_dup && (!valid_text(header.orig_sending_time) ||
        !detail::is_valid_utc_timestamp(header.orig_sending_time) ||
        header.orig_sending_time > header.sending_time)) {
        return Status{ErrorCode::invalid_timestamp, 122};
    }
    if (!header.poss_dup && !header.orig_sending_time.empty()) return Status{ErrorCode::invalid_header, 122};
    (void)message;
    return Status::success();
}

} // namespace

Result<std::size_t> encode_fix44(detail::MessageView message, SessionHeader header,
    std::span<char> output) noexcept {
    if (const auto status = validate_message(message); !status.ok()) return status;
    if (const auto status = validate_header(message, header); !status.ok()) return status;

    std::size_t body_length = 0;
    const auto add_size = [&body_length](std::size_t amount) noexcept {
        if (amount > std::numeric_limits<std::size_t>::max() - body_length) return false;
        body_length += amount;
        return true;
    };
    const auto add_header_field = [&add_size](std::size_t value_size,
        std::size_t prefix_and_delimiter = 4) noexcept {
        if (value_size > std::numeric_limits<std::size_t>::max() - prefix_and_delimiter) return false;
        return add_size(prefix_and_delimiter + value_size);
    };
    // MsgType, SenderCompID, TargetCompID, MsgSeqNum, SendingTime.
    if (!add_header_field(message.msg_type.size()) ||
        !add_header_field(header.sender_comp_id.size()) ||
        !add_header_field(header.target_comp_id.size()) ||
        !add_header_field(decimal_digits(header.msg_seq_num)) ||
        !add_header_field(header.sending_time.size())) {
        return Status{ErrorCode::too_large};
    }
    if ((header.poss_dup && !add_header_field(1)) ||
        (header.poss_dup && !add_header_field(header.orig_sending_time.size(), 5))) {
        return Status{ErrorCode::too_large};
    }
    for (std::size_t i = 0; i < message.field_count; ++i) {
        const auto& field = message.plan[i];
        if (!detail::is_present(message, field.slot_index)) continue;
        const auto value_size = field_value_size(message, field);
        if (value_size > std::numeric_limits<std::size_t>::max() - field.prefix_size - 1 ||
            !add_size(field.prefix_size + value_size + 1)) {
            return Status{ErrorCode::too_large, static_cast<int>(field.tag)};
        }
    }

    const auto body_length_digits = decimal_digits(body_length);
    constexpr std::size_t body_length_prefix_size = 2; // "9="
    constexpr auto fixed_size = begin_string.size() + body_length_prefix_size + 1;
    if (body_length_digits > std::numeric_limits<std::size_t>::max() - fixed_size) {
        return Status{ErrorCode::too_large};
    }
    const auto header_size = fixed_size + body_length_digits;
    if (body_length > std::numeric_limits<std::size_t>::max() - header_size) {
        return Status{ErrorCode::too_large};
    }
    const auto total_without_checksum = header_size + body_length;
    if (total_without_checksum > std::numeric_limits<std::size_t>::max() - checksum_suffix_size) {
        return Status{ErrorCode::too_large};
    }
    const auto total_size = total_without_checksum + checksum_suffix_size;
    if (output.size() < total_size) return Status{ErrorCode::capacity_exceeded};

    Writer writer{output};
    writer.append(begin_string);
    writer.append("9=");
    writer.append_unsigned(body_length);
    writer.append(soh);
    writer.append("35=");
    writer.append(message.msg_type);
    writer.append(soh);
    writer.append("49=");
    writer.append(header.sender_comp_id);
    writer.append(soh);
    writer.append("56=");
    writer.append(header.target_comp_id);
    writer.append(soh);
    writer.append("34=");
    writer.append_unsigned(header.msg_seq_num);
    writer.append(soh);
    if (header.poss_dup) {
        writer.append("43=Y");
        writer.append(soh);
    }
    writer.append("52=");
    writer.append(header.sending_time);
    writer.append(soh);
    if (header.poss_dup) {
        writer.append("122=");
        writer.append(header.orig_sending_time);
        writer.append(soh);
    }
    for (std::size_t i = 0; i < message.field_count; ++i) {
        const auto& field = message.plan[i];
        if (!detail::is_present(message, field.slot_index)) continue;
        writer.append(std::string_view{field.tag_prefix.data(), field.prefix_size});
        writer.append_value(message, field);
        writer.append(soh);
    }
    writer.append_checksum_suffix();
    return total_size;
}

Result<std::size_t> encode_fix44_replay(const FlatMessageView& original,
    SessionHeader header, std::span<char> output) noexcept {
    const auto fields = original.fields();
    const auto frame = original.frame();
    if (fields.empty() || frame.empty() || frame.size() < checksum_suffix_size ||
        !valid_text(header.sender_comp_id) || !valid_text(header.target_comp_id) ||
        !detail::is_valid_utc_timestamp(header.sending_time)) {
        return Status{ErrorCode::invalid_header};
    }

    std::uint32_t duplicate_header_tag = 0;
    const auto find_optional = [&fields, &duplicate_header_tag](std::uint32_t tag) noexcept -> const FieldView* {
        const FieldView* found = nullptr;
        for (const auto& field : fields) {
            if (field.tag != tag) continue;
            if (found != nullptr) {
                duplicate_header_tag = tag;
                return nullptr;
            }
            found = &field;
        }
        return found;
    };
    const auto* msg_type_field = find_optional(35);
    const auto* seq_field = find_optional(34);
    const auto* sending_time_field = find_optional(52);
    const auto* original_time_field = find_optional(122);
    const auto* poss_dup_field = find_optional(43);
    if (duplicate_header_tag != 0)
        return Status{ErrorCode::duplicate_field, static_cast<int>(duplicate_header_tag)};
    if (msg_type_field == nullptr || seq_field == nullptr || sending_time_field == nullptr) {
        return Status{ErrorCode::invalid_header};
    }
    const bool was_replay = poss_dup_field != nullptr && poss_dup_field->value == "Y";
    if (poss_dup_field != nullptr && poss_dup_field->value != "Y" && poss_dup_field->value != "N")
        return Status{ErrorCode::invalid_value, 43};
    if (was_replay != (original_time_field != nullptr))
        return Status{ErrorCode::invalid_header, 122};
    std::uint64_t sequence{};
    const auto seq_parse = std::from_chars(seq_field->value.data(),
        seq_field->value.data() + seq_field->value.size(), sequence);
    if (seq_parse.ec != std::errc{} || seq_parse.ptr != seq_field->value.data() + seq_field->value.size() ||
        sequence == 0) {
        return Status{ErrorCode::invalid_value, 34, seq_field->offset};
    }
    if (!detail::is_valid_utc_timestamp(sending_time_field->value)) {
        return Status{ErrorCode::invalid_timestamp, 52, sending_time_field->offset};
    }
    const auto original_sending_time = original_time_field == nullptr
        ? sending_time_field->value : original_time_field->value;
    if (!detail::is_valid_utc_timestamp(original_sending_time)) {
        return Status{ErrorCode::invalid_timestamp, 122,
            original_time_field == nullptr ? sending_time_field->offset : original_time_field->offset};
    }
    if (!valid_text(msg_type_field->value)) return Status{ErrorCode::invalid_value, 35};
    if (original_sending_time > header.sending_time) return Status{ErrorCode::invalid_timestamp, 122};
    if (header.msg_seq_num != 0 && header.msg_seq_num != sequence) {
        return Status{ErrorCode::invalid_value, 34, seq_field->offset};
    }

    const auto checksum_offset = frame.size() - checksum_suffix_size;
    std::size_t body_length = 0;
    const auto add_size = [&body_length](std::size_t amount) noexcept {
        if (amount > std::numeric_limits<std::size_t>::max() - body_length) return false;
        body_length += amount;
        return true;
    };
    const auto add_header_field = [&add_size](std::size_t value_size,
        std::size_t prefix_and_delimiter = 4) noexcept {
        return value_size <= std::numeric_limits<std::size_t>::max() - prefix_and_delimiter &&
            add_size(prefix_and_delimiter + value_size);
    };
    if (!add_header_field(msg_type_field->value.size()) ||
        !add_header_field(header.sender_comp_id.size()) ||
        !add_header_field(header.target_comp_id.size()) ||
        !add_header_field(decimal_digits(sequence)) ||
        !add_header_field(1) || // PossDupFlag=Y
        !add_header_field(header.sending_time.size()) ||
        !add_header_field(original_sending_time.size(), 5)) {
        return Status{ErrorCode::too_large};
    }

    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto& field = fields[i];
        if (field.tag == 8 || field.tag == 9 || field.tag == 10 || field.tag == 35 ||
            field.tag == 34 || field.tag == 43 || field.tag == 49 || field.tag == 52 ||
            field.tag == 56 || field.tag == 122) {
            continue;
        }
        const auto start = field.offset;
        const auto end = i + 1 < fields.size() ? fields[i + 1].offset : checksum_offset;
        if (start >= end || end > checksum_offset) {
            return Status{ErrorCode::invalid_frame, static_cast<int>(field.tag), start};
        }
        if (!add_size(end - start)) return Status{ErrorCode::too_large, static_cast<int>(field.tag)};
    }

    const auto body_length_digits = decimal_digits(body_length);
    constexpr std::size_t body_length_prefix_size = 2;
    constexpr auto fixed_size = begin_string.size() + body_length_prefix_size + 1;
    if (body_length_digits > std::numeric_limits<std::size_t>::max() - fixed_size) {
        return Status{ErrorCode::too_large};
    }
    const auto header_size = fixed_size + body_length_digits;
    if (body_length > std::numeric_limits<std::size_t>::max() - header_size) return Status{ErrorCode::too_large};
    const auto total_without_checksum = header_size + body_length;
    if (total_without_checksum > std::numeric_limits<std::size_t>::max() - checksum_suffix_size) {
        return Status{ErrorCode::too_large};
    }
    const auto total_size = total_without_checksum + checksum_suffix_size;
    if (output.size() < total_size) return Status{ErrorCode::capacity_exceeded};

    Writer writer{output};
    writer.append(begin_string);
    writer.append("9=");
    writer.append_unsigned(body_length);
    writer.append(soh);
    writer.append("35=");
    writer.append(msg_type_field->value);
    writer.append(soh);
    writer.append("49=");
    writer.append(header.sender_comp_id);
    writer.append(soh);
    writer.append("56=");
    writer.append(header.target_comp_id);
    writer.append(soh);
    writer.append("34=");
    writer.append_unsigned(sequence);
    writer.append(soh);
    writer.append("43=Y");
    writer.append(soh);
    writer.append("52=");
    writer.append(header.sending_time);
    writer.append(soh);
    writer.append("122=");
    writer.append(original_sending_time);
    writer.append(soh);
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto& field = fields[i];
        if (field.tag == 8 || field.tag == 9 || field.tag == 10 || field.tag == 35 ||
            field.tag == 34 || field.tag == 43 || field.tag == 49 || field.tag == 52 ||
            field.tag == 56 || field.tag == 122) {
            continue;
        }
        const auto start = field.offset;
        const auto end = i + 1 < fields.size() ? fields[i + 1].offset : checksum_offset;
        writer.append(frame.substr(start, end - start));
    }
    writer.append_checksum_suffix();
    return total_size;
}

} // namespace nimblefix
