#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <nimblefix/codec.hpp>
#include <nimblefix/message.hpp>

namespace nimblefix {

struct DynamicMessageLimits {
    std::size_t max_fields{64};
    std::size_t max_value_bytes{4096};
    std::size_t max_msg_type_bytes{16};
};

// Explicit runtime-tag message builder. Compile-time tag users should use
// message<MsgType>().set<Tag>(); this class is for tags chosen at runtime.
class DynamicMessage {
public:
    explicit DynamicMessage(std::string_view msg_type, DynamicMessageLimits limits = {}) noexcept
        : limits_(limits) {
        if (msg_type.empty() || msg_type.size() > limits_.max_msg_type_bytes ||
            !detail::is_valid_text(msg_type)) {
            status_ = {ErrorCode::invalid_value, 35};
            return;
        }
        try {
            msg_type_.assign(msg_type.data(), msg_type.size());
        } catch (...) {
            status_ = {ErrorCode::capacity_exceeded};
        }
    }

    DynamicMessage(const DynamicMessage&) = delete;
    DynamicMessage& operator=(const DynamicMessage&) = delete;
    DynamicMessage(DynamicMessage&&) noexcept = default;
    DynamicMessage& operator=(DynamicMessage&&) noexcept = default;

    [[nodiscard]] Status status() const noexcept { return status_; }
    [[nodiscard]] std::size_t field_count() const noexcept { return plan_.size(); }
    [[nodiscard]] std::size_t value_byte_count() const noexcept { return value_bytes_.size(); }
    [[nodiscard]] WireHints wire_hints() const noexcept { return {length_data_pairs_}; }

    template <class V>
        requires (detail::supported_value_v<V>)
    [[nodiscard]] Status set(std::uint32_t tag, V&& value) noexcept {
        if (!status_.ok()) return status_;
        const auto tag_status = validate_tag(tag);
        if (!tag_status.ok()) return tag_status;
        if (std::any_of(plan_.begin(), plan_.end(), [tag](const auto& field) { return field.tag == tag; }))
            return {ErrorCode::duplicate_field, static_cast<int>(tag)};
        if (plan_.size() == limits_.max_fields || plan_.size() >= std::numeric_limits<std::uint32_t>::max())
            return {ErrorCode::capacity_exceeded, static_cast<int>(tag), plan_.size()};

        detail::SlotValue slot{};
        detail::PlanField field{};
        field.tag = tag;
        field.slot_index = static_cast<std::uint32_t>(slots_.size());
        field.kind = detail::value_kind_v<V>;
        field.wire_type = wire_type_for(field.kind);
        make_prefix(tag, field.tag_prefix, field.prefix_size);

        const auto old_bytes = value_bytes_.size();
        Status stored{};
        if constexpr (detail::is_string_v<V>) {
            const auto text = detail::as_string_view(std::forward<V>(value));
            if (!detail::is_valid_text(text)) return {ErrorCode::invalid_value, static_cast<int>(tag)};
            constexpr auto max_slot_bytes = static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
            if (old_bytes > limits_.max_value_bytes || text.size() > limits_.max_value_bytes - old_bytes ||
                old_bytes > max_slot_bytes || text.size() > max_slot_bytes - old_bytes)
                return {ErrorCode::capacity_exceeded, static_cast<int>(tag), old_bytes};
            slot.offset = static_cast<std::uint32_t>(old_bytes);
            slot.size = static_cast<std::uint32_t>(text.size());
            slot.scalar = text.size();
            try {
                value_bytes_.insert(value_bytes_.end(), text.begin(), text.end());
            } catch (...) {
                return {ErrorCode::capacity_exceeded, static_cast<int>(tag)};
            }
        } else {
            auto byte_count = old_bytes;
            char* bytes = value_bytes_.empty() ? nullptr : value_bytes_.data();
            stored = detail::store_typed_slot_value<WireType::unspecified>(slot,
                bytes, byte_count, limits_.max_value_bytes, std::forward<V>(value));
            if (!stored.ok()) return {stored.code, static_cast<int>(tag), stored.offset};
        }
        try {
            slots_.push_back(slot);
            plan_.push_back(field);
        } catch (...) {
            if (slots_.size() > plan_.size()) slots_.pop_back();
            value_bytes_.resize(old_bytes);
            return {ErrorCode::capacity_exceeded, static_cast<int>(tag)};
        }
        return Status::success();
    }

    // Adds a runtime Length/Data pair. The builder owns all bytes and permits
    // embedded SOH/NUL only in the explicit Data value.
    [[nodiscard]] Status set_data(std::uint32_t length_tag, std::uint32_t data_tag,
        std::string_view bytes) noexcept {
        if (!status_.ok()) return status_;
        if (const auto status = validate_tag(length_tag); !status.ok()) return status;
        if (const auto status = validate_tag(data_tag); !status.ok()) return status;
        if (length_tag == data_tag || contains_tag(length_tag) || contains_tag(data_tag))
            return {ErrorCode::duplicate_field, static_cast<int>(data_tag)};
        if (bytes.size() != 0 && bytes.data() == nullptr)
            return {ErrorCode::invalid_value, static_cast<int>(data_tag)};
        if (length_data_pairs_.size() == max_wire_hint_pairs)
            return {ErrorCode::capacity_exceeded, static_cast<int>(data_tag), length_data_pairs_.size()};
        constexpr auto max_slot_bytes = static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max());
        if (value_bytes_.size() > limits_.max_value_bytes ||
            bytes.size() > limits_.max_value_bytes - value_bytes_.size() ||
            value_bytes_.size() > max_slot_bytes || bytes.size() > max_slot_bytes - value_bytes_.size())
            return {ErrorCode::capacity_exceeded, static_cast<int>(data_tag), value_bytes_.size()};
        if (limits_.max_fields < 2 || plan_.size() > limits_.max_fields - 2)
            return {ErrorCode::capacity_exceeded, static_cast<int>(data_tag), plan_.size()};
        if (plan_.size() > std::numeric_limits<std::uint32_t>::max() - 2)
            return {ErrorCode::capacity_exceeded, static_cast<int>(data_tag)};

        const auto old_fields = plan_.size();
        const auto old_slots = slots_.size();
        const auto old_bytes = value_bytes_.size();
        const auto old_pairs = length_data_pairs_.size();
        detail::PlanField length_field{};
        length_field.tag = length_tag;
        length_field.slot_index = static_cast<std::uint32_t>(old_slots);
        length_field.kind = FieldKind::signed_integer;
        length_field.wire_type = WireType::integer;
        length_field.pair_tag = data_tag;
        make_prefix(length_tag, length_field.tag_prefix, length_field.prefix_size);

        detail::SlotValue length_slot{};
        length_slot.scalar = bytes.size();
        detail::PlanField data_field{};
        data_field.tag = data_tag;
        data_field.slot_index = static_cast<std::uint32_t>(old_slots + 1);
        data_field.kind = FieldKind::string;
        data_field.wire_type = WireType::binary_data;
        data_field.pair_tag = length_tag;
        make_prefix(data_tag, data_field.tag_prefix, data_field.prefix_size);
        make_prefix(length_tag, data_field.pair_tag_prefix, data_field.pair_prefix_size);

        detail::SlotValue data_slot{};
        data_slot.offset = static_cast<std::uint32_t>(old_bytes);
        data_slot.size = static_cast<std::uint32_t>(bytes.size());
        data_slot.scalar = bytes.size();
        try {
            slots_.push_back(length_slot);
            slots_.push_back(data_slot);
            plan_.push_back(length_field);
            plan_.push_back(data_field);
            if (!bytes.empty()) value_bytes_.insert(value_bytes_.end(), bytes.begin(), bytes.end());
            length_data_pairs_.push_back({length_tag, data_tag});
        } catch (...) {
            slots_.resize(old_slots);
            plan_.resize(old_fields);
            value_bytes_.resize(old_bytes);
            length_data_pairs_.resize(old_pairs);
            return {ErrorCode::capacity_exceeded, static_cast<int>(data_tag)};
        }
        return Status::success();
    }

    [[nodiscard]] detail::MessageView view() const noexcept {
        detail::MessageView result{};
        result.slots = slots_.empty() ? nullptr : slots_.data();
        result.value_bytes = value_bytes_.empty() ? nullptr : value_bytes_.data();
        result.value_byte_count = value_bytes_.size();
        result.plan = plan_.empty() ? nullptr : plan_.data();
        result.field_count = plan_.size();
        result.msg_type = msg_type_;
        result.status = status_;
        return result;
    }

private:
    [[nodiscard]] static constexpr WireType wire_type_for(FieldKind kind) noexcept {
        switch (kind) {
        case FieldKind::character: return WireType::character;
        case FieldKind::signed_integer:
        case FieldKind::unsigned_integer: return WireType::integer;
        case FieldKind::decimal: return WireType::decimal;
        case FieldKind::string: return WireType::string;
        }
        return WireType::unspecified;
    }

    [[nodiscard]] Status validate_tag(std::uint32_t tag) const noexcept {
        if (tag == 0 || tag > detail::maximum_fix_tag)
            return {ErrorCode::invalid_value, static_cast<int>(tag)};
        if (detail::is_managed_tag(tag)) return {ErrorCode::managed_field, static_cast<int>(tag)};
        return Status::success();
    }

    [[nodiscard]] bool contains_tag(std::uint32_t tag) const noexcept {
        return std::any_of(plan_.begin(), plan_.end(), [tag](const auto& field) { return field.tag == tag; });
    }

    static void make_prefix(std::uint32_t tag, std::array<char, 10>& output,
        std::uint8_t& size) noexcept {
        std::array<char, 9> reverse{};
        std::size_t count = 0;
        do {
            reverse[count++] = static_cast<char>('0' + tag % 10);
            tag /= 10;
        } while (tag != 0);
        size = static_cast<std::uint8_t>(count + 1);
        for (std::size_t i = 0; i < count; ++i) output[i] = reverse[count - i - 1];
        output[count] = '=';
    }

    DynamicMessageLimits limits_{};
    std::string msg_type_{};
    std::vector<detail::SlotValue> slots_{};
    std::vector<char> value_bytes_{};
    std::vector<detail::PlanField> plan_{};
    std::vector<LengthDataPair> length_data_pairs_{};
    Status status_{};
};

[[nodiscard]] inline DynamicMessage dynamic_message(std::string_view msg_type,
    DynamicMessageLimits limits = {}) noexcept {
    return DynamicMessage{msg_type, limits};
}

} // namespace nimblefix
