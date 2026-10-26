#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <nimblefix/decimal.hpp>
#include <nimblefix/status.hpp>

namespace nimblefix {

template <std::size_t N>
struct FixedString {
    char value[N]{};

    consteval FixedString(const char (&text)[N]) {
        for (std::size_t i = 0; i < N; ++i) value[i] = text[i];
    }

    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return {value, N == 0 ? 0 : N - 1};
    }

    friend constexpr bool operator==(const FixedString&, const FixedString&) noexcept = default;
};

template <std::size_t N>
FixedString(const char (&)[N]) -> FixedString<N>;

enum class FieldKind : std::uint8_t {
    character,
    signed_integer,
    unsigned_integer,
    decimal,
    string,
};

enum class WireType : std::uint8_t {
    unspecified,
    string,
    character,
    integer,
    decimal,
    utc_timestamp,
};

inline constexpr std::size_t unranked_field_rank = std::numeric_limits<std::size_t>::max();

template <std::uint32_t Tag, FieldKind Kind, std::size_t Rank = unranked_field_rank,
    WireType Type = WireType::unspecified>
struct Field {
    static constexpr std::uint32_t tag = Tag;
    static constexpr FieldKind kind = Kind;
    static constexpr std::size_t rank = Rank;
    static constexpr WireType wire_type = Type;
};

template <class... FieldTypes>
struct FieldList {};

template <class... FieldTypes>
struct Fields {};

template <class Policy, std::uint32_t Tag>
struct WireRank : std::integral_constant<std::size_t, unranked_field_rank> {};

template <class Policy, std::uint32_t Tag>
struct FieldWireType : std::integral_constant<WireType, WireType::unspecified> {};

struct MessageLimits {
    std::size_t max_fields{64};
    std::size_t max_value_bytes{4096};
};

namespace detail {

inline constexpr std::uint32_t maximum_fix_tag = 999999999;

[[nodiscard]] constexpr bool is_managed_tag(std::uint32_t tag) noexcept {
    return tag == 8 || tag == 9 || tag == 10 || tag == 34 || tag == 35 ||
        tag == 49 || tag == 52 || tag == 56;
}

[[nodiscard]] constexpr bool is_valid_text(std::string_view value) noexcept {
    if (value.empty() || value.data() == nullptr) return false;
    for (const unsigned char ch : value) {
        if (ch < 0x20 || ch == 0x7f) return false;
    }
    return true;
}

[[nodiscard]] constexpr unsigned timestamp_two_digits(std::string_view value, std::size_t position) noexcept {
    return static_cast<unsigned>((value[position] - '0') * 10 + (value[position + 1] - '0'));
}

// FIX 4.4 UTCTimestamp: YYYYMMDD-HH:MM:SS or millisecond precision.
[[nodiscard]] constexpr bool is_valid_utc_timestamp(std::string_view value) noexcept {
    if (value.size() != 17 && value.size() != 21) return false;
    if (value.data() == nullptr) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i == 8) {
            if (value[i] != '-') return false;
        } else if (i == 11 || i == 14) {
            if (value[i] != ':') return false;
        } else if (i == 17) {
            if (value[i] != '.') return false;
        } else if (value[i] < '0' || value[i] > '9') {
            return false;
        }
    }
    constexpr std::array<unsigned, 12> month_days{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const auto month = timestamp_two_digits(value, 4);
    const auto day = timestamp_two_digits(value, 6);
    const auto hour = timestamp_two_digits(value, 9);
    const auto minute = timestamp_two_digits(value, 12);
    const auto second = timestamp_two_digits(value, 15);
    if (month < 1 || month > 12 || hour > 23 || minute > 59 || second > 60) return false;
    unsigned max_day = month_days[month - 1];
    const auto year = static_cast<unsigned>((value[0] - '0') * 1000 + (value[1] - '0') * 100 +
        (value[2] - '0') * 10 + (value[3] - '0'));
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) ++max_day;
    return day >= 1 && day <= max_day;
}

struct SlotValue {
    std::uint64_t scalar{};
    std::uint32_t offset{};
    std::uint32_t size{};
};
static_assert(sizeof(SlotValue) == 16);

struct PlanField {
    std::uint32_t tag{};
    std::uint32_t slot_index{};
    FieldKind kind{};
    WireType wire_type{};
    std::uint8_t prefix_size{};
    std::array<char, 10> tag_prefix{};
};

using EnumValueValidator = bool (*)(std::uint32_t, std::string_view) noexcept;

struct MessageView {
    const SlotValue* slots{};
    const char* value_bytes{};
    std::size_t value_byte_count{};
    const PlanField* plan{};
    const std::uint32_t* slot_map{}; // canonical plan slot -> physical payload slot
    std::size_t field_count{};
    const std::uint8_t* presence_bits{}; // null means every slot is present
    std::string_view msg_type{};
    Status status{};
    const std::uint32_t* required_tags{};
    std::size_t required_tag_count{};
    EnumValueValidator enum_value_allowed{};
};

[[nodiscard]] constexpr bool is_present(const MessageView& message, std::size_t slot) noexcept {
    const auto physical = message.slot_map == nullptr ? slot : message.slot_map[slot];
    return message.presence_bits == nullptr ||
        (message.presence_bits[physical / 8] & static_cast<std::uint8_t>(1U << (physical % 8))) != 0;
}

[[nodiscard]] constexpr std::size_t physical_slot_index(const MessageView& message,
    std::size_t canonical_slot) noexcept {
    return message.slot_map == nullptr ? canonical_slot : message.slot_map[canonical_slot];
}

struct StorageBlock {
    std::size_t field_count{};
    std::size_t slot_capacity{};
    std::size_t value_byte_count{};
    std::size_t value_byte_capacity{};
    std::size_t max_fields{};
    std::size_t max_value_bytes{};

    [[nodiscard]] SlotValue* slots() noexcept {
        return reinterpret_cast<SlotValue*>(reinterpret_cast<std::byte*>(this) + sizeof(StorageBlock));
    }
    [[nodiscard]] const SlotValue* slots() const noexcept {
        return reinterpret_cast<const SlotValue*>(reinterpret_cast<const std::byte*>(this) + sizeof(StorageBlock));
    }
    [[nodiscard]] char* value_bytes() noexcept {
        return reinterpret_cast<char*>(slots() + slot_capacity);
    }
    [[nodiscard]] const char* value_bytes() const noexcept {
        return reinterpret_cast<const char*>(slots() + slot_capacity);
    }
};
static_assert(sizeof(StorageBlock) % alignof(SlotValue) == 0);

struct AdoptStorage {};

template <class Policy>
inline void attach_policy_validation(MessageView& message) noexcept {
    if constexpr (requires { Policy::required_tags; }) {
        message.required_tags = Policy::required_tags.data();
        message.required_tag_count = Policy::required_tags.size();
    }
    if constexpr (requires { &Policy::enum_value_allowed; }) {
        message.enum_value_allowed = &Policy::enum_value_allowed;
    }
}

template <class Policy, std::uint32_t Tag>
[[nodiscard]] consteval bool policy_allows_tag() {
    if constexpr (requires { Policy::allowed_tags; }) {
        return WireRank<Policy, Tag>::value != unranked_field_rank;
    }
    return true;
}

template <class Policy, class Shape>
struct fixed_shape_allowed;

template <class Policy, class... F>
struct fixed_shape_allowed<Policy, Fields<F...>>
    : std::bool_constant<(policy_allows_tag<Policy, F::tag>() && ...)> {};

[[nodiscard]] inline Status validate_schema_presence(const MessageView& message) noexcept {
    if (message.field_count != 0 && message.plan == nullptr) return Status{ErrorCode::invalid_value};
    if (message.required_tag_count != 0 && message.required_tags == nullptr) return Status{ErrorCode::invalid_value};
    for (std::size_t i = 0; i < message.field_count; ++i) {
        if (message.plan[i].slot_index >= message.field_count) {
            return Status{ErrorCode::invalid_value, static_cast<int>(message.plan[i].tag)};
        }
        if (physical_slot_index(message, message.plan[i].slot_index) >= message.field_count) {
            return Status{ErrorCode::invalid_value, static_cast<int>(message.plan[i].tag)};
        }
    }
    // Generated required tags and their shape plan share canonical wire order,
    // so the subset check is linear in the shape and required set.
    std::size_t required_index = 0;
    for (std::size_t i = 0; i < message.field_count && required_index < message.required_tag_count; ++i) {
        const auto& field = message.plan[i];
        if (field.tag != message.required_tags[required_index]) continue;
        if (!is_present(message, field.slot_index)) {
            return Status{ErrorCode::required_field_missing, static_cast<int>(field.tag)};
        }
        ++required_index;
    }
    if (required_index < message.required_tag_count) {
        return Status{ErrorCode::required_field_missing,
            static_cast<int>(message.required_tags[required_index])};
    }
    return Status::success();
}

[[nodiscard]] inline std::size_t clamp_value_limit(std::size_t limit) noexcept {
    return limit > std::numeric_limits<std::uint32_t>::max()
        ? std::numeric_limits<std::uint32_t>::max()
        : limit;
}

[[nodiscard]] inline bool allocation_size(std::size_t slot_capacity, std::size_t byte_capacity,
    std::size_t& result) noexcept {
    constexpr auto max = std::numeric_limits<std::size_t>::max();
    if (slot_capacity > (max - sizeof(StorageBlock)) / sizeof(SlotValue)) return false;
    const auto slots_size = slot_capacity * sizeof(SlotValue);
    if (byte_capacity > max - sizeof(StorageBlock) - slots_size) return false;
    result = sizeof(StorageBlock) + slots_size + byte_capacity;
    return true;
}

[[nodiscard]] inline std::size_t grown_capacity(std::size_t current, std::size_t required,
    std::size_t maximum, std::size_t initial) noexcept {
    if (required > maximum) return maximum;
    if (current >= required) return current;
    std::size_t capacity = current == 0 ? (initial < maximum ? initial : maximum) : current;
    while (capacity < required) {
        const auto next = capacity > maximum / 2 ? maximum : capacity * 2;
        if (next <= capacity) return required;
        capacity = next;
    }
    return capacity;
}

[[nodiscard]] inline Status ensure_storage(StorageBlock*& block, const MessageLimits& requested_limits,
    std::size_t required_fields, std::size_t required_bytes) noexcept {
    const auto max_fields = requested_limits.max_fields;
    const auto max_bytes = clamp_value_limit(requested_limits.max_value_bytes);
    if (required_fields > max_fields || required_bytes > max_bytes) {
        return Status{ErrorCode::capacity_exceeded};
    }

    const auto old_slots = block == nullptr ? 0 : block->slot_capacity;
    const auto old_bytes = block == nullptr ? 0 : block->value_byte_capacity;
    const auto new_slots = grown_capacity(old_slots, required_fields, max_fields, 2);
    const auto new_bytes = grown_capacity(old_bytes, required_bytes, max_bytes, 64);
    if (block != nullptr && new_slots == old_slots && new_bytes == old_bytes) return Status::success();

    std::size_t bytes_to_allocate{};
    if (!allocation_size(new_slots, new_bytes, bytes_to_allocate)) return Status{ErrorCode::capacity_exceeded};
    auto* replacement = static_cast<StorageBlock*>(std::malloc(bytes_to_allocate));
    if (replacement == nullptr) return Status{ErrorCode::capacity_exceeded};
    replacement->field_count = block == nullptr ? 0 : block->field_count;
    replacement->slot_capacity = new_slots;
    replacement->value_byte_count = block == nullptr ? 0 : block->value_byte_count;
    replacement->value_byte_capacity = new_bytes;
    replacement->max_fields = max_fields;
    replacement->max_value_bytes = max_bytes;
    if (block != nullptr) {
        if (block->field_count != 0) {
            std::memcpy(replacement->slots(), block->slots(), block->field_count * sizeof(SlotValue));
        }
        if (block->value_byte_count != 0) {
            std::memcpy(replacement->value_bytes(), block->value_bytes(), block->value_byte_count);
        }
        std::free(block);
    }
    block = replacement;
    return Status::success();
}

template <class T>
inline constexpr bool is_character_v = std::is_same_v<std::remove_cvref_t<T>, char>;

template <class T>
inline constexpr bool is_decimal_v = std::is_same_v<std::remove_cvref_t<T>, Decimal>;

template <class T>
inline constexpr bool is_string_v = std::is_convertible_v<T, std::string_view> && !is_character_v<T>;

template <class T>
inline constexpr bool is_signed_integer_v = std::is_integral_v<std::remove_cvref_t<T>> &&
    std::is_signed_v<std::remove_cvref_t<T>> && !is_character_v<T>;

template <class T>
inline constexpr bool is_unsigned_integer_v = std::is_integral_v<std::remove_cvref_t<T>> &&
    std::is_unsigned_v<std::remove_cvref_t<T>>;

template <class T>
inline constexpr bool supported_value_v = is_character_v<T> || is_decimal_v<T> || is_string_v<T> ||
    is_signed_integer_v<T> || is_unsigned_integer_v<T>;

template <class T>
inline constexpr FieldKind value_kind_v = is_character_v<T> ? FieldKind::character :
    is_decimal_v<T> ? FieldKind::decimal : is_string_v<T> ? FieldKind::string :
    is_signed_integer_v<T> ? FieldKind::signed_integer : FieldKind::unsigned_integer;

template <WireType Type>
inline constexpr FieldKind wire_field_kind_v = Type == WireType::string || Type == WireType::utc_timestamp
    ? FieldKind::string : Type == WireType::character ? FieldKind::character :
    Type == WireType::decimal ? FieldKind::decimal : FieldKind::signed_integer;

template <WireType Type, class T>
inline constexpr bool wire_value_compatible_v = Type == WireType::unspecified ? supported_value_v<T> :
    Type == WireType::string || Type == WireType::utc_timestamp ? is_string_v<T> :
    Type == WireType::character ? is_character_v<T> :
    Type == WireType::integer ? (is_signed_integer_v<T> || is_unsigned_integer_v<T>) :
    Type == WireType::decimal ? (is_decimal_v<T> || is_signed_integer_v<T> || is_unsigned_integer_v<T>) : false;

template <class Policy, std::uint32_t Tag, class T>
inline constexpr FieldKind schema_field_kind_v = FieldWireType<Policy, Tag>::value == WireType::unspecified
    ? value_kind_v<T> : wire_field_kind_v<FieldWireType<Policy, Tag>::value>;

template <class Shape>
struct FieldListTraits;

template <class... FieldTypes>
struct FieldListTraits<FieldList<FieldTypes...>> {
    static constexpr std::size_t size = sizeof...(FieldTypes);
    template <class Policy, class NewField>
    using append_builder = typename Policy::template rebind<FieldTypes..., NewField>;
};

template <class Shape, std::uint32_t Tag>
struct contains_tag;

template <std::uint32_t Tag>
struct contains_tag<FieldList<>, Tag> : std::false_type {};

template <std::uint32_t Tag, class First, class... Rest>
struct contains_tag<FieldList<First, Rest...>, Tag>
    : std::bool_constant<(First::tag == Tag) || contains_tag<FieldList<Rest...>, Tag>::value> {};

template <class... FieldTypes>
consteval auto make_slot_map(FieldList<FieldTypes...>) {
    constexpr std::size_t count = sizeof...(FieldTypes);
    static_assert(count <= std::numeric_limits<std::uint32_t>::max());
    struct Sortable {
        std::size_t rank;
        std::uint32_t tag;
        std::uint32_t physical_index;
    };
    std::array<Sortable, count> sortable{};
    if constexpr (count != 0) {
        std::size_t index = 0;
        ((sortable[index] = {FieldTypes::rank == unranked_field_rank ? index : FieldTypes::rank,
            FieldTypes::tag, static_cast<std::uint32_t>(index)}, ++index), ...);
    }
    for (std::size_t i = 1; i < count; ++i) {
        const auto value = sortable[i];
        std::size_t j = i;
        while (j > 0 && (sortable[j - 1].rank > value.rank ||
            (sortable[j - 1].rank == value.rank && sortable[j - 1].tag > value.tag))) {
            sortable[j] = sortable[j - 1];
            --j;
        }
        sortable[j] = value;
    }
    std::array<std::uint32_t, count> result{};
    for (std::size_t i = 0; i < count; ++i) result[i] = sortable[i].physical_index;
    return result;
}

template <class... FieldTypes>
consteval auto make_plan(FieldList<FieldTypes...>) {
    constexpr std::size_t count = sizeof...(FieldTypes);
    static_assert(count <= std::numeric_limits<std::uint32_t>::max());
    constexpr auto slot_map = make_slot_map(FieldList<FieldTypes...>{});
    std::array<PlanField, count> physical_fields{};
    if constexpr (count != 0) {
        std::size_t physical_index = 0;
        const auto add = [&]<class F>() consteval {
            static_assert(F::tag > 0 && F::tag <= maximum_fix_tag, "FIX tags must be in 1..999999999");
            static_assert(!is_managed_tag(F::tag), "session and framing tags are engine managed");
            PlanField field{};
            field.tag = F::tag;
            field.slot_index = static_cast<std::uint32_t>(physical_index);
            field.kind = F::kind;
            field.wire_type = F::wire_type;
            std::array<char, 9> digits{};
            std::size_t digit_count = 0;
            auto tag = F::tag;
            do {
                digits[digit_count++] = static_cast<char>('0' + tag % 10);
                tag /= 10;
            } while (tag != 0);
            field.prefix_size = static_cast<std::uint8_t>(digit_count + 1);
            for (std::size_t i = 0; i < digit_count; ++i) {
                field.tag_prefix[i] = digits[digit_count - i - 1];
            }
            field.tag_prefix[digit_count] = '=';
            physical_fields[physical_index++] = field;
        };
        (add.template operator()<FieldTypes>(), ...);
    }

    std::array<PlanField, count> result{};
    for (std::size_t canonical_index = 0; canonical_index < count; ++canonical_index) {
        result[canonical_index] = physical_fields[slot_map[canonical_index]];
        result[canonical_index].slot_index = static_cast<std::uint32_t>(canonical_index);
    }
    return result;
}

template <class Left, class Right>
struct field_list_concat;

template <class... Left, class... Right>
struct field_list_concat<FieldList<Left...>, FieldList<Right...>> {
    using type = FieldList<Left..., Right...>;
};

template <class Policy, class Shape, bool SchemaOrdered = requires { Policy::allowed_tags; }>
struct canonical_shape { using type = Shape; };

template <class Sorted, class NewField>
struct insert_field;

template <class NewField>
struct insert_field<FieldList<>, NewField> { using type = FieldList<NewField>; };

template <class First, class... Rest, class NewField>
struct insert_field<FieldList<First, Rest...>, NewField> {
private:
    static constexpr bool comes_first = NewField::rank < First::rank ||
        (NewField::rank == First::rank && NewField::tag < First::tag);
    using tail_inserted = typename insert_field<FieldList<Rest...>, NewField>::type;

public:
    using type = std::conditional_t<comes_first, FieldList<NewField, First, Rest...>,
        typename field_list_concat<FieldList<First>, tail_inserted>::type>;
};

template <class Policy, class... FieldTypes>
struct canonical_shape<Policy, FieldList<FieldTypes...>, true> {
private:
    template <class Sorted, class... Remaining>
    struct sort_fields;
    template <class Sorted>
    struct sort_fields<Sorted> { using type = Sorted; };
    template <class Sorted, class First, class... Rest>
    struct sort_fields<Sorted, First, Rest...> {
        using inserted = typename insert_field<Sorted, First>::type;
        using type = typename sort_fields<inserted, Rest...>::type;
    };

public:
    using type = typename sort_fields<FieldList<>, FieldTypes...>::type;
};

template <class Policy, FixedString MsgType, class CanonicalShape>
struct EncodingPlan {
    inline static constexpr auto fields = make_plan(CanonicalShape{});
};

template <class Shape>
struct ShapeTraits;

template <class... F>
struct ShapeTraits<Fields<F...>> {
    static constexpr std::size_t size = sizeof...(F);
};

template <class Shape, std::uint32_t Tag>
struct field_index;

template <std::uint32_t Tag>
struct field_index<Fields<>, Tag> : std::integral_constant<std::size_t, 0> {};

template <std::uint32_t Tag, class First, class... Rest>
struct field_index<Fields<First, Rest...>, Tag> : std::integral_constant<std::size_t,
    First::tag == Tag ? 0 : 1 + field_index<Fields<Rest...>, Tag>::value> {};

template <class Shape, std::uint32_t Tag>
inline constexpr bool shape_contains_tag_v = field_index<Shape, Tag>::value < ShapeTraits<Shape>::size;

template <class Shape, std::uint32_t Tag>
struct shape_field_by_tag;

template <std::uint32_t Tag, class First, class... Rest>
struct shape_field_by_tag<Fields<First, Rest...>, Tag> : std::conditional_t<First::tag == Tag,
    std::type_identity<First>, shape_field_by_tag<Fields<Rest...>, Tag>> {};

template <std::uint32_t Tag>
struct shape_field_by_tag<Fields<>, Tag> {};

template <class Policy, class Shape>
struct fixed_shape_to_list_with_policy;

template <class Policy, class... F>
struct fixed_shape_to_list_with_policy<Policy, Fields<F...>> {
    using type = FieldList<Field<F::tag, F::kind,
        (F::rank == unranked_field_rank ? WireRank<Policy, F::tag>::value : F::rank),
        (F::wire_type == WireType::unspecified ? FieldWireType<Policy, F::tag>::value : F::wire_type)>...>;
};

template <class Policy, class Shape>
using fixed_shape_field_list_t = typename fixed_shape_to_list_with_policy<Policy, Shape>::type;

template <class Shape>
struct shape_unique;

template <>
struct shape_unique<Fields<>> : std::true_type {};

template <class First, class... Rest>
struct shape_unique<Fields<First, Rest...>> : std::bool_constant<
    !contains_tag<FieldList<Rest...>, First::tag>::value && shape_unique<Fields<Rest...>>::value> {};

template <class T>
[[nodiscard]] inline std::string_view as_string_view(T&& value) noexcept {
    if constexpr (std::is_same_v<std::remove_cvref_t<T>, std::string>) {
        return value;
    } else if constexpr (std::is_pointer_v<std::remove_cvref_t<T>>) {
        return value == nullptr ? std::string_view{} : std::string_view{value};
    } else {
        return std::string_view{std::forward<T>(value)};
    }
}

template <class T>
[[nodiscard]] inline Status store_slot_value(SlotValue& slot, char* bytes, std::size_t& byte_count,
    std::size_t byte_capacity, T&& value) noexcept {
    using Value = std::remove_cvref_t<T>;
    if constexpr (is_character_v<Value>) {
        const auto character = static_cast<unsigned char>(value);
        if (character < 0x20 || character == 0x7f) return Status{ErrorCode::invalid_value};
        slot.scalar = character;
        slot.offset = 0;
        slot.size = 0;
    } else if constexpr (is_decimal_v<Value>) {
        if (value.scale() > 18) return Status{ErrorCode::invalid_decimal};
        slot.scalar = std::bit_cast<std::uint64_t>(value.coefficient());
        slot.offset = 0;
        slot.size = value.scale();
    } else if constexpr (is_string_v<T>) {
        const auto text = as_string_view(std::forward<T>(value));
        if (!is_valid_text(text)) return Status{ErrorCode::invalid_value};
        if (text.size() > byte_capacity - byte_count) return Status{ErrorCode::capacity_exceeded};
        slot.offset = static_cast<std::uint32_t>(byte_count);
        slot.size = static_cast<std::uint32_t>(text.size());
        slot.scalar = 0;
        if (!text.empty()) std::memcpy(bytes + byte_count, text.data(), text.size());
        byte_count += text.size();
    } else if constexpr (is_signed_integer_v<Value>) {
        slot.scalar = std::bit_cast<std::uint64_t>(static_cast<std::int64_t>(value));
        slot.offset = 0;
        slot.size = 0;
    } else if constexpr (is_unsigned_integer_v<Value>) {
        slot.scalar = static_cast<std::uint64_t>(value);
        slot.offset = 0;
        slot.size = 0;
    }
    return Status::success();
}

template <WireType Type, class T>
[[nodiscard]] inline Status store_typed_slot_value(SlotValue& slot, char* bytes, std::size_t& byte_count,
    std::size_t byte_capacity, T&& value) noexcept {
    using Value = std::remove_cvref_t<T>;
    if constexpr (Type == WireType::decimal && is_signed_integer_v<Value>) {
        return store_slot_value(slot, bytes, byte_count, byte_capacity,
            Decimal{static_cast<std::int64_t>(value)});
    } else if constexpr (Type == WireType::decimal && is_unsigned_integer_v<Value>) {
        if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return Status{ErrorCode::invalid_value};
        }
        return store_slot_value(slot, bytes, byte_count, byte_capacity,
            Decimal{static_cast<std::int64_t>(value)});
    } else if constexpr (Type == WireType::integer && is_unsigned_integer_v<Value>) {
        if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return Status{ErrorCode::invalid_value};
        }
        return store_slot_value(slot, bytes, byte_count, byte_capacity,
            static_cast<std::int64_t>(value));
    } else {
        return store_slot_value(slot, bytes, byte_count, byte_capacity, std::forward<T>(value));
    }
}

} // namespace detail

// The declaration is completed below. Policies let generated names preserve their
// fluent wrapper type while the core builder owns all storage and encoding behavior.
template <class Policy, FixedString MsgType, class Shape>
class MessageBuilder;

template <FixedString MsgType>
struct FlatMessagePolicy {
    template <class... SlotTypes>
    using rebind = MessageBuilder<FlatMessagePolicy<MsgType>, MsgType, FieldList<SlotTypes...>>;
};

template <FixedString MsgType, class Shape = FieldList<>>
using StaticMessage = MessageBuilder<FlatMessagePolicy<MsgType>, MsgType, Shape>;

template <FixedString MsgType>
[[nodiscard]] auto message(MessageLimits limits = {}) noexcept {
    return StaticMessage<MsgType>{limits};
}

template <FixedString MsgType, class Shape, std::size_t ByteCapacity = 4096,
    class Policy = FlatMessagePolicy<MsgType>>
class FixedShapeMessage {
    static_assert(detail::ShapeTraits<Shape>::size <= std::numeric_limits<std::uint32_t>::max());
    static_assert(ByteCapacity <= std::numeric_limits<std::uint32_t>::max());
    static_assert(detail::shape_unique<Shape>::value, "Fields cannot declare a tag more than once");
    static_assert(detail::fixed_shape_allowed<Policy, Shape>::value,
        "Fields declares a tag that is not allowed by this generated message");
    using PhysicalShape = detail::fixed_shape_field_list_t<Policy, Shape>;
    using CanonicalShape = typename detail::canonical_shape<Policy, PhysicalShape>::type;
    using Plan = detail::EncodingPlan<Policy, MsgType, CanonicalShape>;
    static constexpr const auto& plan_ = Plan::fields;
    static constexpr auto slot_map_ = detail::make_slot_map(PhysicalShape{});
    static constexpr std::size_t field_count_ = detail::ShapeTraits<Shape>::size;
    static constexpr std::size_t presence_size_ = (field_count_ + 7) / 8;

public:
    using shape_type = Shape;
    using policy_type = Policy;
    using plan_type = Plan;
    static constexpr std::string_view msg_type = MsgType.view();
    static constexpr std::size_t slot_count = field_count_;
    static constexpr std::size_t slot_storage_bytes = field_count_ * sizeof(detail::SlotValue);
    static constexpr std::size_t presence_storage_bytes = presence_size_;
    static constexpr std::size_t value_storage_capacity = ByteCapacity;
    static constexpr const auto& encoding_plan = Plan::fields;
    static constexpr const auto& physical_slot_map = slot_map_;

    FixedShapeMessage() noexcept {}
    FixedShapeMessage(const FixedShapeMessage&) = delete;
    FixedShapeMessage& operator=(const FixedShapeMessage&) = delete;
    FixedShapeMessage(FixedShapeMessage&& other) noexcept { move_from(other); }
    FixedShapeMessage& operator=(FixedShapeMessage&& other) noexcept {
        if (this != &other) move_from(other);
        return *this;
    }

    [[nodiscard]] Status status() const noexcept { return view().status; }

    void reset() noexcept {
        for (auto& bits : presence_) bits = 0;
        if (byte_count_ != 0) std::memset(bytes_.data(), 0, byte_count_);
        byte_count_ = 0;
        status_ = Status::success();
    }

    template <std::uint32_t Tag, class V>
    [[nodiscard]] Status set(V&& value) noexcept {
        static_assert(Tag > 0 && Tag <= detail::maximum_fix_tag, "FIX tags must be in 1..999999999");
        static_assert(!detail::is_managed_tag(Tag), "session and framing tags are engine managed");
        static_assert(detail::shape_contains_tag_v<Shape, Tag>, "set<Tag> must name a field in Fields");
        static_assert(detail::policy_allows_tag<Policy, Tag>(), "tag is not allowed by this generated message");
        using Declared = typename detail::shape_field_by_tag<Shape, Tag>::type;
        static_assert(detail::supported_value_v<V>, "unsupported static FIX field value type");
        constexpr auto policy_wire_type = FieldWireType<Policy, Tag>::value;
        constexpr auto declared_wire_type = policy_wire_type == WireType::unspecified
            ? Declared::wire_type : policy_wire_type;
        static_assert(detail::wire_value_compatible_v<declared_wire_type, V>,
            "value type does not match the declared Fields wire type");
        static_assert((declared_wire_type == WireType::unspecified && detail::value_kind_v<V> == Declared::kind) ||
            (declared_wire_type != WireType::unspecified &&
                detail::wire_field_kind_v<declared_wire_type> == Declared::kind),
            "value type does not match the declared Fields kind or XML wire type");
        if (!status_.ok()) return status_;
        if constexpr (detail::is_string_v<V>) {
            const auto text = detail::as_string_view(std::forward<V>(value));
            if (!detail::is_valid_text(text)) {
                status_ = Status{ErrorCode::invalid_value, static_cast<int>(Tag)};
                return status_;
            }
            if constexpr (declared_wire_type == WireType::utc_timestamp) {
                if (!detail::is_valid_utc_timestamp(text)) {
                    status_ = Status{ErrorCode::invalid_timestamp, static_cast<int>(Tag)};
                    return status_;
                }
            }
        }
        constexpr auto index = detail::field_index<Shape, Tag>::value;
        auto& slot = slots_[index];
        std::size_t byte_count = byte_count_;
        Status result{};
        if constexpr (detail::is_string_v<V>) {
            const auto text = detail::as_string_view(std::forward<V>(value));
            const bool was_present = (presence_[index / 8] & static_cast<std::uint8_t>(1U << (index % 8))) != 0;
            if (was_present) {
                std::array<char, ByteCapacity> aliased_input;
                const char* external_source = text.data();
                const auto old_offset = static_cast<std::size_t>(slot.offset);
                const auto old_reserved = static_cast<std::size_t>(slot.scalar);
                if (old_offset > byte_count_ || old_reserved > byte_count_ - old_offset) {
                    result = Status{ErrorCode::invalid_value};
                } else {
                    const auto old_end = old_offset + old_reserved;
                    const auto retained_count = byte_count_ - old_reserved;
                    if (retained_count > ByteCapacity || text.size() > ByteCapacity - retained_count) {
                        result = Status{ErrorCode::capacity_exceeded};
                    } else {
                        const auto new_count = retained_count + text.size();
                        bool input_in_arena = false;
                        std::size_t input_offset = 0;
                        if constexpr (ByteCapacity != 0) {
                            const auto source_address = reinterpret_cast<std::uintptr_t>(text.data());
                            const auto arena_address = reinterpret_cast<std::uintptr_t>(bytes_.data());
                            if (source_address >= arena_address && source_address - arena_address <= ByteCapacity) {
                                input_offset = static_cast<std::size_t>(source_address - arena_address);
                                if (input_offset > byte_count_ || text.size() > byte_count_ - input_offset) {
                                    result = Status{ErrorCode::invalid_value};
                                } else {
                                    input_in_arena = true;
                                }
                            }
                        }

                        const auto size_delta = text.size() > old_reserved
                            ? text.size() - old_reserved : old_reserved - text.size();
                        const bool growing = text.size() > old_reserved;
                        const bool aliases_shifted_tail = input_in_arena && input_offset >= old_end;
                        const bool crosses_shift_boundary = input_in_arena && input_offset < old_end &&
                            text.size() > old_end - input_offset;
                        if (result.ok() && crosses_shift_boundary) {
                            // A borrowed substring may span both the replaced
                            // value and the tail being moved. Preserve just its
                            // used bytes before either region changes.
                            std::memcpy(aliased_input.data(), text.data(), text.size());
                            external_source = aliased_input.data();
                            input_in_arena = false;
                        }
                        if (result.ok() && size_delta != 0) {
                            const auto tail_size = byte_count_ - old_end;
                            if (growing) {
                                std::memmove(bytes_.data() + old_end + size_delta,
                                    bytes_.data() + old_end, tail_size);
                            } else {
                                std::memmove(bytes_.data() + old_offset + text.size(),
                                    bytes_.data() + old_end, tail_size);
                            }
                            for (std::size_t plan_index = 0; plan_index < plan_.size(); ++plan_index) {
                                if (plan_[plan_index].kind != FieldKind::string) continue;
                                const auto physical = slot_map_[plan_index];
                                if (physical == index ||
                                    (presence_[physical / 8] & static_cast<std::uint8_t>(1U << (physical % 8))) == 0) {
                                    continue;
                                }
                                auto& other = slots_[physical];
                                if (other.offset >= old_end) {
                                    other.offset = static_cast<std::uint32_t>(growing
                                        ? static_cast<std::size_t>(other.offset) + size_delta
                                        : static_cast<std::size_t>(other.offset) - size_delta);
                                }
                            }
                            if (aliases_shifted_tail) {
                                input_offset = growing ? input_offset + size_delta : input_offset - size_delta;
                            }
                            byte_count = new_count;
                        }
                        if (result.ok()) {
                            const auto* source = input_in_arena ? bytes_.data() + input_offset : external_source;
                            if (!text.empty()) std::memmove(bytes_.data() + old_offset, source, text.size());
                            slot.size = static_cast<std::uint32_t>(text.size());
                            slot.scalar = text.size();
                        }
                    }
                }
            } else {
                if (text.size() > ByteCapacity - byte_count_) {
                    result = Status{ErrorCode::capacity_exceeded};
                } else {
                    slot.offset = static_cast<std::uint32_t>(byte_count_);
                    slot.size = static_cast<std::uint32_t>(text.size());
                    slot.scalar = text.size();
                    if (!text.empty()) std::memmove(bytes_.data() + byte_count_, text.data(), text.size());
                    byte_count += text.size();
                }
            }
        } else {
            result = detail::store_typed_slot_value<declared_wire_type>(slot, bytes_.data(), byte_count,
                ByteCapacity, std::forward<V>(value));
        }
        if (!result.ok()) {
            status_ = Status{result.code, static_cast<int>(Tag)};
            return status_;
        }
        byte_count_ = byte_count;
        presence_[index / 8] |= static_cast<std::uint8_t>(1U << (index % 8));
        return Status::success();
    }

    [[nodiscard]] detail::MessageView view() const noexcept {
        detail::MessageView result{slots_.data(), bytes_.data(), byte_count_, plan_.data(), slot_map_.data(),
            plan_.size(), presence_.data(), msg_type, status_};
        detail::attach_policy_validation<Policy>(result);
        if (result.status.ok()) result.status = detail::validate_schema_presence(result);
        return result;
    }

private:
    void move_from(FixedShapeMessage& other) noexcept {
        slots_ = other.slots_;
        presence_ = other.presence_;
        byte_count_ = other.byte_count_;
        status_ = other.status_;
        if (byte_count_ != 0) std::memcpy(bytes_.data(), other.bytes_.data(), byte_count_);
        other.reset();
    }

    std::array<detail::SlotValue, field_count_> slots_;
    std::array<std::uint8_t, presence_size_> presence_{};
    std::array<char, ByteCapacity> bytes_;
    std::size_t byte_count_{};
    Status status_{};
};

template <FixedString MsgType, class Shape, std::size_t ByteCapacity = 4096,
    class Policy = FlatMessagePolicy<MsgType>>
using FixedMessage = FixedShapeMessage<MsgType, Shape, ByteCapacity, Policy>;

template <class Policy, FixedString MsgType, class Shape>
class MessageBuilder {
    template <class P, FixedString M, class S>
    friend class MessageBuilder;
    using ShapeInfo = detail::FieldListTraits<Shape>;
    using CanonicalShape = typename detail::canonical_shape<Policy, Shape>::type;
    using Plan = detail::EncodingPlan<Policy, MsgType, CanonicalShape>;
    static constexpr const auto& plan_ = Plan::fields;
    static constexpr auto slot_map_ = detail::make_slot_map(Shape{});
    static constexpr std::size_t field_count_ = ShapeInfo::size;

public:
    using shape_type = Shape;
    using policy_type = Policy;
    using canonical_shape_type = CanonicalShape;
    using plan_type = Plan;
    static constexpr std::string_view msg_type = MsgType.view();
    static constexpr std::size_t slot_count = field_count_;
    static constexpr std::size_t slot_storage_bytes = field_count_ * sizeof(detail::SlotValue);
    static constexpr auto wire_order = []() consteval {
        std::array<std::uint32_t, field_count_> order{};
        for (std::size_t i = 0; i < field_count_; ++i) order[i] = plan_[i].tag;
        return order;
    }();
    static constexpr const auto& encoding_plan = Plan::fields;
    static constexpr const auto& physical_slot_map = slot_map_;

    explicit MessageBuilder(MessageLimits limits = {}) noexcept : limits_(limits) {}
    MessageBuilder(detail::StorageBlock* block, MessageLimits limits, Status status,
        detail::AdoptStorage) noexcept : block_(block), limits_(limits), status_(status) {}
    MessageBuilder(const MessageBuilder&) = delete;
    MessageBuilder& operator=(const MessageBuilder&) = delete;
    MessageBuilder(MessageBuilder&& other) noexcept
        : block_(std::exchange(other.block_, nullptr)), limits_(other.limits_),
          status_(std::exchange(other.status_, Status{ErrorCode::invalid_value})) {}
    MessageBuilder& operator=(MessageBuilder&& other) noexcept {
        if (this == &other) return *this;
        std::free(block_);
        block_ = std::exchange(other.block_, nullptr);
        limits_ = other.limits_;
        status_ = std::exchange(other.status_, Status{ErrorCode::invalid_value});
        return *this;
    }
    ~MessageBuilder() { std::free(block_); }

    [[nodiscard]] Status status() const noexcept { return view().status; }

    template <std::uint32_t Tag, class V>
    [[nodiscard]] auto set(V&& value) && noexcept {
        static_assert(Tag > 0 && Tag <= detail::maximum_fix_tag, "FIX tags must be in 1..999999999");
        static_assert(!detail::is_managed_tag(Tag), "session and framing tags are engine managed");
        static_assert(detail::policy_allows_tag<Policy, Tag>(), "tag is not allowed by this generated message");
        static_assert(!detail::contains_tag<Shape, Tag>::value, "a flat message shape cannot declare the same tag twice");
        static_assert(detail::supported_value_v<V>, "unsupported static FIX field value type");
        constexpr auto wire_type = FieldWireType<Policy, Tag>::value;
        static_assert(detail::wire_value_compatible_v<wire_type, V>,
            "value type does not match the generated FIX field type");
        using NewField = Field<Tag, detail::schema_field_kind_v<Policy, Tag, V>,
            WireRank<Policy, Tag>::value, wire_type>;
        using Next = typename ShapeInfo::template append_builder<Policy, NewField>;
        if (status_.ok()) append<Tag>(std::forward<V>(value));
        auto* block = std::exchange(block_, nullptr);
        const auto result_status = std::exchange(status_, Status{ErrorCode::invalid_value});
        return Next{block, limits_, result_status, detail::AdoptStorage{}};
    }

    [[nodiscard]] detail::MessageView view() const noexcept {
        const auto* values = block_ == nullptr ? nullptr : block_->slots();
        const auto* bytes = block_ == nullptr ? nullptr : block_->value_bytes();
        const auto byte_count = block_ == nullptr ? 0 : block_->value_byte_count;
        detail::MessageView result{values, bytes, byte_count, plan_.data(), slot_map_.data(),
            plan_.size(), nullptr, msg_type, status_};
        detail::attach_policy_validation<Policy>(result);
        if (result.status.ok()) result.status = detail::validate_schema_presence(result);
        return result;
    }

private:
    template <std::uint32_t Tag, class V>
    void append(V&& value) noexcept {
        std::size_t value_size = 0;
        std::string_view text;
        std::size_t aliased_value_offset = 0;
        bool value_aliases_storage = false;
        if constexpr (detail::is_string_v<V>) {
            text = detail::as_string_view(std::forward<V>(value));
            value_size = text.size();
            if (!detail::is_valid_text(text)) {
                status_ = Status{ErrorCode::invalid_value, static_cast<int>(Tag)};
                return;
            }
            if constexpr (FieldWireType<Policy, Tag>::value == WireType::utc_timestamp) {
                if (!detail::is_valid_utc_timestamp(text)) {
                    status_ = Status{ErrorCode::invalid_timestamp, static_cast<int>(Tag)};
                    return;
                }
            }
            if (block_ != nullptr && !text.empty()) {
                const auto source = reinterpret_cast<std::uintptr_t>(text.data());
                const auto arena = reinterpret_cast<std::uintptr_t>(block_->value_bytes());
                if (source >= arena) {
                    const auto offset = source - arena;
                    if (offset <= block_->value_byte_count &&
                        text.size() <= block_->value_byte_count - offset) {
                        value_aliases_storage = true;
                        aliased_value_offset = static_cast<std::size_t>(offset);
                    }
                }
            }
        } else if constexpr (detail::is_decimal_v<V>) {
            if (value.scale() > 18) {
                status_ = Status{ErrorCode::invalid_decimal, static_cast<int>(Tag)};
                return;
            }
        } else if constexpr (detail::is_character_v<V>) {
            const auto character = static_cast<unsigned char>(value);
            if (character < 0x20 || character == 0x7f) {
                status_ = Status{ErrorCode::invalid_value, static_cast<int>(Tag)};
                return;
            }
        }
        if constexpr (FieldWireType<Policy, Tag>::value == WireType::integer && detail::is_unsigned_integer_v<V>) {
            if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                status_ = Status{ErrorCode::invalid_value, static_cast<int>(Tag)};
                return;
            }
        }
        if constexpr (FieldWireType<Policy, Tag>::value == WireType::decimal && detail::is_unsigned_integer_v<V>) {
            if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                status_ = Status{ErrorCode::invalid_value, static_cast<int>(Tag)};
                return;
            }
        }

        const std::size_t old_byte_count = block_ == nullptr ? 0 : block_->value_byte_count;
        if (value_size > detail::clamp_value_limit(limits_.max_value_bytes) - old_byte_count) {
            status_ = Status{ErrorCode::capacity_exceeded, static_cast<int>(Tag)};
            return;
        }
        const auto next_count = (block_ == nullptr ? 0 : block_->field_count) + 1;
        if (const auto result = detail::ensure_storage(block_, limits_, next_count, old_byte_count + value_size);
            !result.ok()) {
            status_ = Status{result.code, static_cast<int>(Tag)};
            return;
        }

        auto& slot = block_->slots()[block_->field_count];
        std::size_t byte_count = block_->value_byte_count;
        Status stored{};
        if constexpr (detail::is_string_v<V>) {
            if (value_aliases_storage) text = {block_->value_bytes() + aliased_value_offset, text.size()};
            stored = detail::store_typed_slot_value<FieldWireType<Policy, Tag>::value>(slot,
                block_->value_bytes(), byte_count, block_->value_byte_capacity, text);
        } else {
            stored = detail::store_typed_slot_value<FieldWireType<Policy, Tag>::value>(slot,
                block_->value_bytes(), byte_count, block_->value_byte_capacity, std::forward<V>(value));
        }
        if (!stored.ok()) {
            status_ = Status{stored.code, static_cast<int>(Tag)};
            return;
        }
        block_->value_byte_count = byte_count;
        ++block_->field_count;
    }

    detail::StorageBlock* block_{};
    MessageLimits limits_{};
    Status status_{};
};

} // namespace nimblefix
