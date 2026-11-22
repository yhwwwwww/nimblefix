#include <nimblefix/encoder.hpp>

#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <system_error>

namespace nimblefix {
namespace {

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

[[nodiscard]] const detail::RepeatingGroupView* find_group(
    const detail::MessageView& message, std::uint32_t count_tag) noexcept {
    if (message.groups == nullptr) return nullptr;
    for (std::size_t i = 0; i < message.group_count; ++i)
        if (message.groups[i].count_tag == count_tag) return &message.groups[i];
    return nullptr;
}

[[nodiscard]] std::size_t group_tag_count(const detail::MessageView& message,
    std::uint32_t count_tag) noexcept {
    if (message.groups == nullptr) return 0;
    std::size_t count = 0;
    for (std::size_t i = 0; i < message.group_count; ++i)
        if (message.groups[i].count_tag == count_tag) ++count;
    return count;
}

[[nodiscard]] Status group_count_value(const detail::MessageView& message,
    const detail::PlanField& field, std::uint64_t& value) noexcept {
    if (field.slot_index >= message.field_count || message.slots == nullptr)
        return Status{ErrorCode::invalid_value, static_cast<int>(field.tag)};
    const auto& slot = message.slots[detail::physical_slot_index(message, field.slot_index)];
    if (field.kind == FieldKind::unsigned_integer) {
        value = slot.scalar;
        return Status::success();
    }
    if (field.kind == FieldKind::signed_integer) {
        const auto signed_value = std::bit_cast<std::int64_t>(slot.scalar);
        if (signed_value < 0) return Status{ErrorCode::invalid_value, static_cast<int>(field.tag)};
        value = static_cast<std::uint64_t>(signed_value);
        return Status::success();
    }
    return Status{ErrorCode::invalid_value, static_cast<int>(field.tag)};
}

[[nodiscard]] const detail::PlanField* find_plan_field(
    const detail::MessageView& message, std::uint32_t tag) noexcept {
    for (std::size_t i = 0; i < message.field_count; ++i)
        if (message.plan[i].tag == tag) return &message.plan[i];
    return nullptr;
}

[[nodiscard]] std::size_t binary_data_size(const detail::MessageView& message,
    const detail::PlanField& field) noexcept {
    return message.slots[detail::physical_slot_index(message, field.slot_index)].size;
}

[[nodiscard]] Status validate_message_impl(const detail::MessageView& message, std::size_t depth) noexcept {
    if (depth > 64) return Status{ErrorCode::invalid_value};
    if (!message.status.ok()) return message.status;
    if (!message.presence_validated) {
        if (const auto schema_status = detail::validate_schema_presence(message); !schema_status.ok())
            return schema_status;
    }
    if (message.msg_type.empty() || (!message.prevalidated_scalars && !valid_text(message.msg_type)))
        return Status{ErrorCode::invalid_value, 35};
    if (message.field_count != 0 && (message.plan == nullptr || message.slots == nullptr)) {
        return Status{ErrorCode::invalid_value};
    }
    if (message.value_byte_count != 0 && message.value_bytes == nullptr) return Status{ErrorCode::invalid_value};
    if (message.group_count != 0 && message.groups == nullptr) return Status{ErrorCode::invalid_value};

    for (std::size_t i = 0; i < message.field_count; ++i) {
        const auto& field = message.plan[i];
        if (!message.presence_validated && field.slot_index >= message.field_count)
            return Status{ErrorCode::invalid_value, static_cast<int>(field.tag)};
        if (!detail::is_present(message, field.slot_index)) continue;
        const auto& slot = message.slots[detail::physical_slot_index(message, field.slot_index)];
        if (!message.prevalidated_scalars) {
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
                if (field.wire_type != WireType::binary_data &&
                    !valid_text({message.value_bytes == nullptr ? "" : message.value_bytes + slot.offset, slot.size})) {
                    return Status{ErrorCode::invalid_value, static_cast<int>(field.tag)};
                }
                break;
            }
        }
        if (field.wire_type == WireType::binary_data) {
            // FIX TagValue requires Length > 0 for binary DATA fields.
            if (slot.size == 0 || field.pair_tag == 0 || field.pair_prefix_size == 0)
                return Status{ErrorCode::invalid_value, static_cast<int>(field.tag)};
            const auto* length_field = find_plan_field(message, field.pair_tag);
            if (length_field != nullptr && detail::is_present(message, length_field->slot_index)) {
                std::uint64_t length{};
                if (const auto status = group_count_value(message, *length_field, length); !status.ok())
                    return Status{ErrorCode::invalid_value, static_cast<int>(field.pair_tag)};
                if (length == 0 || length != slot.size)
                    return Status{ErrorCode::invalid_value, static_cast<int>(field.pair_tag)};
            }
        } else if (field.pair_tag != 0) {
            const auto* data_field = find_plan_field(message, field.pair_tag);
            std::uint64_t length{};
            if (const auto status = group_count_value(message, field, length); !status.ok()) return status;
            if (data_field != nullptr && detail::is_present(message, data_field->slot_index)) {
                if (data_field->wire_type != WireType::binary_data || length == 0 ||
                    length != binary_data_size(message, *data_field))
                    return Status{ErrorCode::invalid_value, static_cast<int>(field.tag)};
            } else {
                return Status{ErrorCode::invalid_value, static_cast<int>(field.tag)};
            }
        }
        if (!message.prevalidated_scalars && field.wire_type == WireType::utc_timestamp) {
            std::array<char, 32> scratch{};
            std::string_view value;
            if (const auto text_status = field_value_text(message, field, scratch, value); !text_status.ok()) {
                return text_status;
            }
            if (!detail::is_valid_utc_timestamp(value)) {
                return Status{ErrorCode::invalid_timestamp, static_cast<int>(field.tag)};
            }
        }
        if (field.needs_policy_check && message.enum_value_allowed != nullptr) {
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
    for (std::size_t i = 0; i < message.group_count; ++i) {
        const auto& group = message.groups[i];
        if (group.count_tag == 0 || group.delimiter_tag == 0 ||
            (group.entry_count != 0 && group.entries == nullptr) ||
            group_tag_count(message, group.count_tag) != 1) {
            return Status{ErrorCode::invalid_value, static_cast<int>(group.count_tag)};
        }
        const detail::PlanField* count_field = nullptr;
        for (std::size_t field_index = 0; field_index < message.field_count; ++field_index) {
            const auto& field = message.plan[field_index];
            if (field.group_counter && field.tag == group.count_tag) { count_field = &field; break; }
        }
        if (count_field == nullptr || !detail::is_present(message, count_field->slot_index))
            return Status{ErrorCode::invalid_value, static_cast<int>(group.count_tag)};
        std::uint64_t stored_count{};
        if (const auto count_status = group_count_value(message, *count_field, stored_count); !count_status.ok())
            return count_status;
        if (stored_count != group.entry_count)
            return Status{ErrorCode::invalid_value, static_cast<int>(group.count_tag)};
        for (std::size_t entry_index = 0; entry_index < group.entry_count; ++entry_index) {
            const auto& entry = group.entries[entry_index];
            if (entry.field_count == 0 || entry.plan == nullptr || entry.plan[0].tag != group.delimiter_tag ||
                !detail::is_present(entry, entry.plan[0].slot_index)) {
                return Status{ErrorCode::invalid_value, static_cast<int>(group.delimiter_tag)};
            }
            if (const auto entry_status = validate_message_impl(entry, depth + 1); !entry_status.ok()) return entry_status;
        }
    }
    for (std::size_t i = 0; i < message.field_count; ++i) {
        const auto& field = message.plan[i];
        if (!field.group_counter || !detail::is_present(message, field.slot_index)) continue;
        const auto* group = find_group(message, field.tag);
        if (group != nullptr) continue;
        std::uint64_t count{};
        if (const auto count_status = group_count_value(message, field, count); !count_status.ok()) return count_status;
        if (count != 0) return Status{ErrorCode::invalid_value, static_cast<int>(field.tag)};
    }
    return Status::success();
}

[[nodiscard]] Status validate_message(const detail::MessageView& message) noexcept {
    return validate_message_impl(message, 0);
}

[[nodiscard]] std::size_t field_value_size(const detail::MessageView& message,
    const detail::PlanField& field) noexcept {
    if (field.group_counter) {
        if (const auto* group = find_group(message, field.tag)) return decimal_digits(group->entry_count);
    }
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
        if (value.empty()) return;
        std::memcpy(output_.data() + position_, value.data(), value.size());
        update_checksum(value.size());
    }

    void append(char value) noexcept {
        output_[position_++] = value;
        checksum_ += static_cast<unsigned char>(value);
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
        if (field.group_counter) {
            if (const auto* group = find_group(message, field.tag)) {
                append_unsigned(group->entry_count);
                return;
            }
        }
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
        const auto checksum = static_cast<unsigned>(checksum_ & 0xffU);
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
        const auto* raw = reinterpret_cast<const unsigned char*>(output_.data() + position_);
        std::uint64_t sum = checksum_;
        for (std::size_t i = 0; i < size; ++i) sum += raw[i];
        checksum_ = sum;
        position_ += size;
    }

    std::span<char> output_;
    std::size_t position_{};
    std::uint64_t checksum_{};
};

[[nodiscard]] Status measure_body_fields(const detail::MessageView& message,
    std::size_t& body_length, std::size_t depth) noexcept {
    if (depth > 64) return Status{ErrorCode::invalid_value};
    for (std::size_t i = 0; i < message.field_count; ++i) {
        const auto& field = message.plan[i];
        if (!detail::is_present(message, field.slot_index)) continue;
        if (field.wire_type == WireType::binary_data) {
            const auto* length_field = find_plan_field(message, field.pair_tag);
            if (length_field == nullptr || !detail::is_present(message, length_field->slot_index)) {
                const auto length_size = decimal_digits(binary_data_size(message, field));
                if (field.pair_prefix_size + length_size + 1 >
                    std::numeric_limits<std::size_t>::max() - body_length)
                    return Status{ErrorCode::too_large, static_cast<int>(field.pair_tag)};
                body_length += field.pair_prefix_size + length_size + 1;
            }
        }
        const auto value_size = field_value_size(message, field);
        if (value_size > std::numeric_limits<std::size_t>::max() - field.prefix_size - 1 ||
            field.prefix_size + value_size + 1 > std::numeric_limits<std::size_t>::max() - body_length)
            return Status{ErrorCode::too_large, static_cast<int>(field.tag)};
        body_length += field.prefix_size + value_size + 1;
        if (!field.group_counter) continue;
        const auto* group = find_group(message, field.tag);
        if (group == nullptr) continue;
        for (std::size_t entry = 0; entry < group->entry_count; ++entry) {
            const auto status = measure_body_fields(group->entries[entry], body_length, depth + 1);
            if (!status.ok()) return status;
        }
    }
    return Status::success();
}

void append_body_fields(const detail::MessageView& message, Writer& writer) noexcept {
    for (std::size_t i = 0; i < message.field_count; ++i) {
        const auto& field = message.plan[i];
        if (!detail::is_present(message, field.slot_index)) continue;
        if (field.wire_type == WireType::binary_data) {
            const auto* length_field = find_plan_field(message, field.pair_tag);
            if (length_field == nullptr || !detail::is_present(message, length_field->slot_index)) {
                writer.append(std::string_view{field.pair_tag_prefix.data(), field.pair_prefix_size});
                writer.append_unsigned(binary_data_size(message, field));
                writer.append(soh);
            }
        }
        writer.append(std::string_view{field.tag_prefix.data(), field.prefix_size});
        writer.append_value(message, field);
        writer.append(soh);
        if (!field.group_counter) continue;
        const auto* group = find_group(message, field.tag);
        if (group == nullptr) continue;
        for (std::size_t entry = 0; entry < group->entry_count; ++entry)
            append_body_fields(group->entries[entry], writer);
    }
}

[[nodiscard]] Status validate_header(const detail::MessageView& message, const SessionHeader& header) noexcept {
    if (!valid_text(header.sender_comp_id) || !valid_text(header.target_comp_id) || header.msg_seq_num == 0) {
        return Status{ErrorCode::invalid_header};
    }
    if (!valid_text(header.sending_time) || !detail::is_valid_utc_timestamp(header.sending_time)) {
        return Status{ErrorCode::invalid_timestamp, 52};
    }
    if (header.poss_dup && (!valid_text(header.orig_sending_time) ||
        !detail::is_valid_utc_timestamp(header.orig_sending_time) ||
        detail::compare_utc_timestamps(header.orig_sending_time, header.sending_time) > 0)) {
        return Status{ErrorCode::invalid_timestamp, 122};
    }
    if (!header.poss_dup && !header.orig_sending_time.empty()) return Status{ErrorCode::invalid_header, 122};
    (void)message;
    return Status::success();
}

[[nodiscard]] Status measure_encoded_frame(detail::MessageView message, SessionHeader header,
    std::size_t& body_length, std::size_t& total_size) noexcept {
    const auto begin = begin_string(header.version);
    if (begin.empty()) return Status{ErrorCode::invalid_header, 8};
    if (const auto status = validate_message(message); !status.ok()) return status;
    if (const auto status = validate_header(message, header); !status.ok()) return status;

    body_length = 0;
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
    if (!add_header_field(message.msg_type.size()) ||
        !add_header_field(header.sender_comp_id.size()) ||
        !add_header_field(header.target_comp_id.size()) ||
        !add_header_field(decimal_digits(header.msg_seq_num)) ||
        !add_header_field(header.sending_time.size()) ||
        (header.last_msg_seq_num_processed &&
            !add_header_field(decimal_digits(*header.last_msg_seq_num_processed), 5))) {
        return Status{ErrorCode::too_large};
    }
    if ((header.poss_dup && !add_header_field(1)) ||
        (header.poss_dup && !add_header_field(header.orig_sending_time.size(), 5))) {
        return Status{ErrorCode::too_large};
    }
    if (const auto status = measure_body_fields(message, body_length, 0); !status.ok()) return status;

    const auto body_length_digits = decimal_digits(body_length);
    constexpr std::size_t body_length_prefix_size = 2;
    constexpr std::size_t begin_prefix_and_delimiter = 3; // "8=" and SOH
    constexpr std::size_t body_prefix_and_delimiter = 3; // "9=" and SOH
    const std::size_t fixed_size = begin_prefix_and_delimiter + begin.size() + body_prefix_and_delimiter;
    if (body_length_digits > std::numeric_limits<std::size_t>::max() - fixed_size) {
        return Status{ErrorCode::too_large};
    }
    (void)body_length_prefix_size;
    const auto header_size = fixed_size + body_length_digits;
    if (body_length > std::numeric_limits<std::size_t>::max() - header_size) return Status{ErrorCode::too_large};
    const auto total_without_checksum = header_size + body_length;
    if (total_without_checksum > std::numeric_limits<std::size_t>::max() - checksum_suffix_size) {
        return Status{ErrorCode::too_large};
    }
    total_size = total_without_checksum + checksum_suffix_size;
    return Status::success();
}

class FastSinglePassWriter {
public:
    FastSinglePassWriter(char* data, std::size_t start, std::size_t limit) noexcept
        : data_(data), pos_(start), limit_(limit) {}

    [[nodiscard]] bool overflow() const noexcept { return overflow_; }
    [[nodiscard]] std::size_t pos() const noexcept { return pos_; }
    [[nodiscard]] std::uint64_t checksum() const noexcept { return checksum_; }
    [[nodiscard]] const char* data() const noexcept { return data_; }

    void append(std::string_view value) noexcept {
        const std::size_t n = value.size();
        if (n == 0) return;
        if (n > limit_ - pos_) { overflow_ = true; return; }
        std::memcpy(data_ + pos_, value.data(), n);
        add_checksum(data_ + pos_, n);
        pos_ += n;
    }

    void append(char value) noexcept {
        if (pos_ >= limit_) { overflow_ = true; return; }
        data_[pos_++] = value;
        checksum_ += static_cast<unsigned char>(value);
    }

    void append_unsigned(std::uint64_t value) noexcept {
        if (pos_ >= limit_) { overflow_ = true; return; }
        const auto res = std::to_chars(data_ + pos_, data_ + limit_, value);
        if (res.ec != std::errc{}) { overflow_ = true; return; }
        const auto n = static_cast<std::size_t>(res.ptr - (data_ + pos_));
        add_checksum(data_ + pos_, n);
        pos_ += n;
    }

    void append_signed(std::int64_t value) noexcept {
        if (pos_ >= limit_) { overflow_ = true; return; }
        const auto res = std::to_chars(data_ + pos_, data_ + limit_, value);
        if (res.ec != std::errc{}) { overflow_ = true; return; }
        const auto n = static_cast<std::size_t>(res.ptr - (data_ + pos_));
        add_checksum(data_ + pos_, n);
        pos_ += n;
    }

    void append_decimal(const detail::SlotValue& slot) noexcept {
        if (pos_ >= limit_) { overflow_ = true; return; }
        std::size_t sz = 0;
        const Decimal value{std::bit_cast<std::int64_t>(slot.scalar), static_cast<std::uint8_t>(slot.size)};
        if (!value.format(std::span<char>{data_ + pos_, limit_ - pos_}, sz).ok()) {
            overflow_ = true;
            return;
        }
        add_checksum(data_ + pos_, sz);
        pos_ += sz;
    }

private:
    void add_checksum(const char* ptr, std::size_t n) noexcept {
        const auto* raw = reinterpret_cast<const unsigned char*>(ptr);
        std::uint64_t sum = checksum_;
        for (std::size_t i = 0; i < n; ++i) sum += raw[i];
        checksum_ = sum;
    }

    char* data_;
    std::size_t pos_;
    std::size_t limit_;
    std::uint64_t checksum_{};
    bool overflow_{false};
};

[[nodiscard]] Status encode_body_single_pass(const detail::MessageView& message,
    FastSinglePassWriter& writer, std::size_t depth) noexcept {
    if (depth > 64) return Status{ErrorCode::invalid_value};
    if (!message.status.ok()) return message.status;
    if (!message.presence_validated) {
        if (const auto schema_status = detail::validate_schema_presence(message); !schema_status.ok())
            return schema_status;
    }
    if (message.msg_type.empty() || (!message.prevalidated_scalars && !valid_text(message.msg_type)))
        return Status{ErrorCode::invalid_value, 35};
    if (message.field_count != 0 && (message.plan == nullptr || message.slots == nullptr))
        return Status{ErrorCode::invalid_value};
    if (message.value_byte_count != 0 && message.value_bytes == nullptr) return Status{ErrorCode::invalid_value};
    if (message.group_count != 0 && message.groups == nullptr) return Status{ErrorCode::invalid_value};

    std::size_t matched_groups = 0;
    for (std::size_t i = 0; i < message.field_count; ++i) {
        const auto& field = message.plan[i];
        if (!message.presence_validated && field.slot_index >= message.field_count)
            return Status{ErrorCode::invalid_value, static_cast<int>(field.tag)};
        if (!detail::is_present(message, field.slot_index)) continue;
        const auto& slot = message.slots[detail::physical_slot_index(message, field.slot_index)];

        if (!message.prevalidated_scalars) {
            switch (field.kind) {
            case FieldKind::character:
                if (slot.scalar > 0xff || slot.scalar < 0x20 || slot.scalar == 0x7f)
                    return Status{ErrorCode::invalid_value, static_cast<int>(field.tag)};
                break;
            case FieldKind::signed_integer:
            case FieldKind::unsigned_integer:
                break;
            case FieldKind::decimal:
                if (slot.size > 18) return Status{ErrorCode::invalid_decimal, static_cast<int>(field.tag)};
                break;
            case FieldKind::string:
                if (slot.offset > message.value_byte_count ||
                    slot.size > message.value_byte_count - slot.offset ||
                    (slot.size != 0 && message.value_bytes == nullptr))
                    return Status{ErrorCode::invalid_value, static_cast<int>(field.tag)};
                if (field.wire_type != WireType::binary_data &&
                    !valid_text({message.value_bytes == nullptr ? "" : message.value_bytes + slot.offset, slot.size}))
                    return Status{ErrorCode::invalid_value, static_cast<int>(field.tag)};
                break;
            }
        }

        if (field.wire_type == WireType::binary_data) {
            if (slot.size == 0 || field.pair_tag == 0 || field.pair_prefix_size == 0)
                return Status{ErrorCode::invalid_value, static_cast<int>(field.tag)};
            const auto* length_field = find_plan_field(message, field.pair_tag);
            if (length_field != nullptr && detail::is_present(message, length_field->slot_index)) {
                std::uint64_t length{};
                if (const auto st = group_count_value(message, *length_field, length); !st.ok())
                    return Status{ErrorCode::invalid_value, static_cast<int>(field.pair_tag)};
                if (length == 0 || length != slot.size)
                    return Status{ErrorCode::invalid_value, static_cast<int>(field.pair_tag)};
            } else {
                writer.append(std::string_view{field.pair_tag_prefix.data(), field.pair_prefix_size});
                writer.append_unsigned(slot.size);
                writer.append(soh);
                if (writer.overflow()) return Status::success();
            }
        } else if (field.pair_tag != 0) {
            const auto* data_field = find_plan_field(message, field.pair_tag);
            std::uint64_t length{};
            if (const auto st = group_count_value(message, field, length); !st.ok()) return st;
            if (data_field != nullptr && detail::is_present(message, data_field->slot_index)) {
                if (data_field->wire_type != WireType::binary_data || length == 0 ||
                    length != binary_data_size(message, *data_field))
                    return Status{ErrorCode::invalid_value, static_cast<int>(field.tag)};
            } else {
                return Status{ErrorCode::invalid_value, static_cast<int>(field.tag)};
            }
        }

        const detail::RepeatingGroupView* group = nullptr;
        if (field.group_counter) {
            group = find_group(message, field.tag);
            std::uint64_t stored_count{};
            if (const auto st = group_count_value(message, field, stored_count); !st.ok()) return st;
            if (group != nullptr) {
                if (group->count_tag == 0 || group->delimiter_tag == 0 ||
                    (group->entry_count != 0 && group->entries == nullptr) ||
                    group_tag_count(message, group->count_tag) != 1 ||
                    stored_count != group->entry_count) {
                    return Status{ErrorCode::invalid_value, static_cast<int>(field.tag)};
                }
                ++matched_groups;
            } else if (stored_count != 0) {
                return Status{ErrorCode::invalid_value, static_cast<int>(field.tag)};
            }
        }

        writer.append(std::string_view{field.tag_prefix.data(), field.prefix_size});
        const std::size_t val_start = writer.pos();
        if (group != nullptr) {
            writer.append_unsigned(group->entry_count);
        } else {
            switch (field.kind) {
            case FieldKind::character:
                writer.append(static_cast<char>(slot.scalar));
                break;
            case FieldKind::signed_integer:
                writer.append_signed(std::bit_cast<std::int64_t>(slot.scalar));
                break;
            case FieldKind::unsigned_integer:
                writer.append_unsigned(slot.scalar);
                break;
            case FieldKind::decimal:
                writer.append_decimal(slot);
                break;
            case FieldKind::string:
                writer.append(std::string_view{message.value_bytes == nullptr ? "" : message.value_bytes + slot.offset, slot.size});
                break;
            }
        }
        if (writer.overflow()) return Status::success();

        const std::string_view written_val{writer.data() + val_start, writer.pos() - val_start};
        if (!message.prevalidated_scalars && field.wire_type == WireType::utc_timestamp) {
            if (!detail::is_valid_utc_timestamp(written_val))
                return Status{ErrorCode::invalid_timestamp, static_cast<int>(field.tag)};
        }
        if (field.needs_policy_check && message.enum_value_allowed != nullptr) {
            if (!message.enum_value_allowed(field.tag, written_val))
                return Status{ErrorCode::invalid_value, static_cast<int>(field.tag)};
        }
        writer.append(soh);
        if (writer.overflow()) return Status::success();

        if (group != nullptr) {
            for (std::size_t entry_index = 0; entry_index < group->entry_count; ++entry_index) {
                const auto& entry = group->entries[entry_index];
                if (entry.field_count == 0 || entry.plan == nullptr || entry.plan[0].tag != group->delimiter_tag ||
                    !detail::is_present(entry, entry.plan[0].slot_index)) {
                    return Status{ErrorCode::invalid_value, static_cast<int>(group->delimiter_tag)};
                }
                if (const auto st = encode_body_single_pass(entry, writer, depth + 1); !st.ok()) return st;
                if (writer.overflow()) return Status::success();
            }
        }
    }

    if (matched_groups != message.group_count) {
        return validate_message_impl(message, depth);
    }
    return Status::success();
}

[[nodiscard]] bool estimate_max_body_bytes(const detail::MessageView& message,
    std::size_t& total, std::size_t depth) noexcept {
    if (depth > 64) return false;
    constexpr std::size_t max_bytes_per_field = 40;
    if (message.field_count > std::numeric_limits<std::size_t>::max() / max_bytes_per_field) return false;
    const std::size_t field_bound = message.field_count * max_bytes_per_field;
    if (field_bound > std::numeric_limits<std::size_t>::max() - total) return false;
    total += field_bound;
    if (message.value_byte_count > std::numeric_limits<std::size_t>::max() - total) return false;
    total += message.value_byte_count;
    if (message.group_count != 0) {
        if (message.groups == nullptr) return false;
        for (std::size_t g = 0; g < message.group_count; ++g) {
            const auto& group = message.groups[g];
            if (group.entry_count != 0 && group.entries == nullptr) return false;
            for (std::size_t e = 0; e < group.entry_count; ++e) {
                if (!estimate_max_body_bytes(group.entries[e], total, depth + 1)) return false;
            }
        }
    }
    return true;
}

} // namespace

Result<std::size_t> measure_fix(detail::MessageView message, SessionHeader header) noexcept {
    std::size_t body_length{};
    std::size_t total_size{};
    if (const auto status = measure_encoded_frame(message, header, body_length, total_size); !status.ok()) return status;
    return total_size;
}

Result<std::size_t> encode_fix(detail::MessageView message, SessionHeader header,
    std::span<char> output) noexcept {
    const auto begin = begin_string(header.version);
    if (begin.empty()) return Status{ErrorCode::invalid_header, 8};

    // Never touch output before verifying basic message & header status.
    if (!message.status.ok()) return message.status;
    if (!message.presence_validated) {
        if (const auto schema_status = detail::validate_schema_presence(message); !schema_status.ok())
            return schema_status;
        message.presence_validated = true;
    }
    if (message.msg_type.empty() || (!message.prevalidated_scalars && !valid_text(message.msg_type)))
        return Status{ErrorCode::invalid_value, 35};

    // Validate header first on the happy path; if invalid, run full message validation
    // to preserve message-before-header error precedence without touching output.
    if (const auto hs = validate_header(message, header); !hs.ok()) {
        if (const auto ms = validate_message(message); !ms.ok()) return ms;
        return hs;
    }

    // Compute O(1) safe upper bound so single-pass direct-write NEVER writes to output
    // if capacity_exceeded is even theoretically possible.
    std::size_t max_frame_bound = 128 + begin.size() + message.msg_type.size() +
        header.sender_comp_id.size() + header.target_comp_id.size() +
        header.sending_time.size() + header.orig_sending_time.size();
    const bool bound_ok = estimate_max_body_bytes(message, max_frame_bound, 0);

    // Single-pass direct-write + BodyLength backpatch fast path.
    // Guesses 3-digit BodyLength (100..999 bytes, zero memmove for normal FIX orders/reports).
    const std::size_t guessed_body_start = 3 + begin.size() + 6; // "8=" + begin + SOH + "9=000" + SOH
    if (bound_ok && output.size() >= max_frame_bound && output.size() > guessed_body_start + checksum_suffix_size) {
        FastSinglePassWriter writer{output.data(), guessed_body_start, output.size() - checksum_suffix_size};
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
        if (header.last_msg_seq_num_processed) {
            writer.append("369=");
            writer.append_unsigned(*header.last_msg_seq_num_processed);
            writer.append(soh);
        }
        if (header.poss_dup) {
            writer.append("122=");
            writer.append(header.orig_sending_time);
            writer.append(soh);
        }

        if (!writer.overflow()) {
            if (const auto st = encode_body_single_pass(message, writer, 0); !st.ok()) return st;
        }

        if (!writer.overflow()) {
            const std::size_t body_length = writer.pos() - guessed_body_start;
            const std::size_t body_digits = decimal_digits(body_length);
            const std::size_t actual_body_start = 3 + begin.size() + 3 + body_digits;
            const std::size_t total_size = actual_body_start + body_length + checksum_suffix_size;
            if (total_size <= output.size()) {
                if (actual_body_start != guessed_body_start) {
                    std::memmove(output.data() + actual_body_start,
                                 output.data() + guessed_body_start, body_length);
                }
                char* p = output.data();
                p[0] = '8';
                p[1] = '=';
                std::memcpy(p + 2, begin.data(), begin.size());
                std::size_t off = 2 + begin.size();
                p[off++] = soh;
                p[off++] = '9';
                p[off++] = '=';
                const auto len_res = std::to_chars(p + off, p + actual_body_start, body_length);
                off = static_cast<std::size_t>(len_res.ptr - p);
                p[off++] = soh;

                std::uint64_t total_sum = writer.checksum();
                const auto* prefix_raw = reinterpret_cast<const unsigned char*>(p);
                for (std::size_t i = 0; i < actual_body_start; ++i) total_sum += prefix_raw[i];
                const auto checksum = static_cast<unsigned>(total_sum & 0xffU);
                char* tail = p + actual_body_start + body_length;
                tail[0] = '1';
                tail[1] = '0';
                tail[2] = '=';
                tail[3] = static_cast<char>('0' + (checksum / 100U) % 10U);
                tail[4] = static_cast<char>('0' + (checksum / 10U) % 10U);
                tail[5] = static_cast<char>('0' + checksum % 10U);
                tail[6] = soh;
                return total_size;
            }
        }
    }

    // Tight-buffer fallback: preserves exact validation-vs-capacity error precedence.
    std::size_t body_length{};
    std::size_t total_size{};
    if (const auto status = measure_encoded_frame(message, header, body_length, total_size); !status.ok()) return status;
    if (output.size() < total_size) return Status{ErrorCode::capacity_exceeded};

    Writer writer{output};
    writer.append("8=");
    writer.append(begin);
    writer.append(soh);
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
    if (header.last_msg_seq_num_processed) {
        writer.append("369=");
        writer.append_unsigned(*header.last_msg_seq_num_processed);
        writer.append(soh);
    }
    if (header.poss_dup) {
        writer.append("122=");
        writer.append(header.orig_sending_time);
        writer.append(soh);
    }
    append_body_fields(message, writer);
    writer.append_checksum_suffix();
    return total_size;
}

Result<std::size_t> encode_fix44(detail::MessageView message, SessionHeader header,
    std::span<char> output) noexcept {
    header.version = FixVersion::fix44;
    return encode_fix(message, header, output);
}

Result<std::size_t> encode_fix_replay(const FlatMessageView& original,
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
    const auto* begin_string_field = find_optional(8);
    const auto* msg_type_field = find_optional(35);
    const auto* seq_field = find_optional(34);
    const auto* sending_time_field = find_optional(52);
    const auto* original_time_field = find_optional(122);
    const auto* poss_dup_field = find_optional(43);
    (void)find_optional(49);
    (void)find_optional(56);
    (void)find_optional(369); // Reject a duplicate even though the old value is replaced.
    if (duplicate_header_tag != 0)
        return Status{ErrorCode::duplicate_field, static_cast<int>(duplicate_header_tag)};
    if (begin_string_field == nullptr || msg_type_field == nullptr || seq_field == nullptr || sending_time_field == nullptr) {
        return Status{ErrorCode::invalid_header};
    }
    const auto original_version = parse_fix_version(begin_string_field->value);
    if (!original_version.ok() || header.version != original_version.value())
        return Status{ErrorCode::invalid_header, 8, begin_string_field->offset};
    const auto begin = begin_string(header.version);
    if (begin.empty()) return Status{ErrorCode::invalid_header, 8};
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
    if (detail::compare_utc_timestamps(original_sending_time, header.sending_time) > 0)
        return Status{ErrorCode::invalid_timestamp, 122};
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
        (header.last_msg_seq_num_processed &&
            !add_header_field(decimal_digits(*header.last_msg_seq_num_processed), 5)) ||
        !add_header_field(original_sending_time.size(), 5)) {
        return Status{ErrorCode::too_large};
    }

    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto& field = fields[i];
        if (field.tag == 8 || field.tag == 9 || field.tag == 10 || field.tag == 35 ||
            field.tag == 34 || field.tag == 43 || field.tag == 49 || field.tag == 52 ||
            field.tag == 56 || field.tag == 122 || field.tag == 369) {
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
    constexpr std::size_t begin_prefix_and_delimiter = 3;
    constexpr std::size_t body_prefix_and_delimiter = 3;
    const std::size_t fixed_size = begin_prefix_and_delimiter + begin.size() + body_prefix_and_delimiter;
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
    writer.append("8=");
    writer.append(begin);
    writer.append(soh);
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
    if (header.last_msg_seq_num_processed) {
        writer.append("369=");
        writer.append_unsigned(*header.last_msg_seq_num_processed);
        writer.append(soh);
    }
    writer.append("122=");
    writer.append(original_sending_time);
    writer.append(soh);
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto& field = fields[i];
        if (field.tag == 8 || field.tag == 9 || field.tag == 10 || field.tag == 35 ||
            field.tag == 34 || field.tag == 43 || field.tag == 49 || field.tag == 52 ||
            field.tag == 56 || field.tag == 122 || field.tag == 369) {
            continue;
        }
        const auto start = field.offset;
        const auto end = i + 1 < fields.size() ? fields[i + 1].offset : checksum_offset;
        writer.append(frame.substr(start, end - start));
    }
    writer.append_checksum_suffix();
    return total_size;
}

Result<std::size_t> encode_fix44_replay(const FlatMessageView& original,
    SessionHeader header, std::span<char> output) noexcept {
    header.version = FixVersion::fix44;
    return encode_fix_replay(original, header, output);
}

} // namespace nimblefix
