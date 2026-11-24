#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>
#include <type_traits>
#include <tuple>
#include <utility>

#include <nimblefix/message.hpp>

namespace nimblefix {

// Bounded owner for one schema-described repeating group. Each entry has one
// compile-time shape; optional members are represented by that entry's presence
// bits. Nested groups stay owned by their entry builders.
template <std::uint32_t CountTag, std::uint32_t DelimiterTag, class Entry,
    std::size_t MaxEntries = 16, bool Required = false>
class GroupBuilder {
    static_assert(CountTag > 0 && DelimiterTag > 0 && CountTag != DelimiterTag);
    static_assert(MaxEntries > 0);
    static_assert(std::is_default_constructible_v<Entry>);
    static_assert(std::is_move_assignable_v<Entry>);

public:
    static constexpr std::uint32_t count_tag = CountTag;
    static constexpr std::uint32_t delimiter_tag = DelimiterTag;
    static constexpr std::size_t max_entries = MaxEntries;
    static constexpr bool required = Required;

    GroupBuilder() = default;
    GroupBuilder(const GroupBuilder&) = delete;
    GroupBuilder& operator=(const GroupBuilder&) = delete;
    GroupBuilder(GroupBuilder&&) noexcept = default;
    GroupBuilder& operator=(GroupBuilder&&) noexcept = default;

    [[nodiscard]] std::size_t size() const noexcept { return entry_count_; }
    [[nodiscard]] bool empty() const noexcept { return entry_count_ == 0; }
    [[nodiscard]] bool touched() const noexcept { return touched_; }
    [[nodiscard]] Status status() const noexcept { return status_; }

    // Mark an explicitly present empty group. Required groups are present even
    // when this is not called; optional nested groups otherwise stay absent.
    void set_empty() noexcept { touched_ = true; }

    void reset() noexcept {
        entry_count_ = 0;
        status_ = Status::success();
        touched_ = false;
    }

    [[nodiscard]] Status add(Entry&& entry) noexcept {
        if (!status_.ok()) return status_;
        if constexpr (requires(Entry& item) { item.prepare(); }) {
            if (const auto prepared = entry.prepare(); !prepared.ok()) {
                status_ = prepared;
                return status_;
            }
        }
        const auto entry_status = entry.status();
        if (!entry_status.ok()) {
            status_ = entry_status;
            return status_;
        }
        if (entry_count_ == MaxEntries) {
            status_ = Status{ErrorCode::capacity_exceeded, static_cast<int>(CountTag), entry_count_};
            return status_;
        }
        entries_[entry_count_++] = std::move(entry);
        touched_ = true;
        return Status::success();
    }

    [[nodiscard]] detail::RepeatingGroupView view() const noexcept {
        for (std::size_t i = 0; i < entry_count_; ++i) entry_views_[i] = entries_[i].view();
        return {CountTag, DelimiterTag, entry_views_.data(), entry_count_};
    }

private:
    std::array<Entry, MaxEntries> entries_{};
    mutable std::array<detail::MessageView, MaxEntries> entry_views_{};
    std::size_t entry_count_{};
    Status status_{};
    bool touched_{};
};

namespace detail {

template <class T, class = void>
struct schema_type_alias {};

template <class T>
struct schema_type_alias<T, std::void_t<typename T::schema_type>> {
    using schema_type = typename T::schema_type;
};

template <class Derived>
struct empty_named_setters {};

template <class Builder, class Derived, class = void>
struct group_attachment_setters {
    using type = empty_named_setters<Derived>;
};

template <class Builder, class Derived>
struct group_attachment_setters<Builder, Derived,
    std::void_t<typename Builder::policy_type::template named_setters<Derived>>> {
    using type = typename Builder::policy_type::template named_setters<Derived>;
};

template <class Builder, class Derived>
using group_attachment_setters_t = typename group_attachment_setters<Builder, Derived>::type;

template <class T, class = void>
struct is_group_entry_helper : std::false_type {};

template <class T>
struct is_group_entry_helper<T, std::void_t<decltype(std::remove_cvref_t<T>::is_group_entry)>>
    : std::bool_constant<std::remove_cvref_t<T>::is_group_entry> {};

template <class T>
inline constexpr bool is_group_entry_v = is_group_entry_helper<std::remove_cvref_t<T>>::value;

} // namespace detail

struct OwnedGroupLimits {
    std::size_t max_fields_per_message{256};
    std::size_t max_value_bytes_per_message{16 * 1024};
    std::size_t max_groups_per_message{64};
    std::size_t max_depth{16};
    std::size_t max_total_fields{16 * 1024};
    std::size_t max_total_value_bytes{1024 * 1024};
    std::size_t max_total_entries{4096};
};

struct GroupPayloadUsage {
    std::size_t fields{};
    std::size_t value_bytes{};
    std::size_t entries{};
};

class OwnedMessage {
public:
    OwnedMessage() = default;
    OwnedMessage(const OwnedMessage&) = delete;
    OwnedMessage& operator=(const OwnedMessage&) = delete;
    OwnedMessage(OwnedMessage&&) noexcept = default;
    OwnedMessage& operator=(OwnedMessage&&) noexcept = default;
    ~OwnedMessage();

    [[nodiscard]] Status copy_from(detail::MessageView source, OwnedGroupLimits limits,
        GroupPayloadUsage& usage, std::size_t depth = 0) noexcept;
    [[nodiscard]] detail::MessageView view() const noexcept;

private:
    struct GroupStorage {
        std::uint32_t count_tag{};
        std::uint32_t delimiter_tag{};
        std::vector<std::unique_ptr<OwnedMessage>> entries{};
        mutable std::vector<detail::MessageView> entry_views{};
    };

    std::vector<detail::SlotValue> slots_{};
    std::vector<char> value_bytes_{};
    std::vector<std::uint32_t> slot_map_{};
    std::vector<std::uint8_t> presence_{};
    std::vector<detail::PlanField> plan_{};
    std::vector<std::uint32_t> required_tags_{};
    std::vector<GroupStorage> groups_{};
    std::string msg_type_{};
    Status status_{};
    detail::EnumValueValidator enum_value_allowed_{};
    mutable std::vector<detail::RepeatingGroupView> group_views_{};
};

inline OwnedMessage::~OwnedMessage() = default;

inline Status OwnedMessage::copy_from(detail::MessageView source, OwnedGroupLimits limits,
    GroupPayloadUsage& usage, std::size_t depth) noexcept {
    if (depth > limits.max_depth || !source.status.ok() ||
        (source.field_count != 0 && (source.plan == nullptr || source.slots == nullptr)) ||
        (source.value_byte_count != 0 && source.value_bytes == nullptr) ||
        (source.group_count != 0 && source.groups == nullptr) || source.msg_type.empty() ||
        source.field_count > limits.max_fields_per_message ||
        source.value_byte_count > limits.max_value_bytes_per_message ||
        source.group_count > limits.max_groups_per_message ||
        (source.required_tag_count != 0 && source.required_tags == nullptr)) {
        return source.status.ok() ? Status{ErrorCode::capacity_exceeded} : source.status;
    }

    auto new_usage = usage;
    if (new_usage.fields > limits.max_total_fields ||
        source.field_count > limits.max_total_fields - new_usage.fields ||
        new_usage.value_bytes > limits.max_total_value_bytes ||
        source.value_byte_count > limits.max_total_value_bytes - new_usage.value_bytes ||
        new_usage.entries > limits.max_total_entries) {
        return {ErrorCode::capacity_exceeded};
    }
    new_usage.fields += source.field_count;
    new_usage.value_bytes += source.value_byte_count;

    try {
        OwnedMessage replacement;
        replacement.status_ = source.status;
        replacement.msg_type_.assign(source.msg_type.data(), source.msg_type.size());
        replacement.enum_value_allowed_ = source.enum_value_allowed;
        if (source.field_count != 0) {
            replacement.slots_.assign(source.slots, source.slots + source.field_count);
            replacement.plan_.assign(source.plan, source.plan + source.field_count);
        }
        if (source.slot_map != nullptr && source.field_count != 0)
            replacement.slot_map_.assign(source.slot_map, source.slot_map + source.field_count);
        if (source.value_byte_count != 0)
            replacement.value_bytes_.assign(source.value_bytes, source.value_bytes + source.value_byte_count);
        if (source.presence_bits != nullptr)
            replacement.presence_.assign(source.presence_bits, source.presence_bits + (source.field_count + 7) / 8);
        if (source.required_tag_count != 0) {
            if (source.required_tags == nullptr) return {ErrorCode::invalid_value};
            replacement.required_tags_.assign(source.required_tags,
                source.required_tags + source.required_tag_count);
        }
        replacement.groups_.reserve(source.group_count);
        replacement.group_views_.resize(source.group_count);
        for (std::size_t group_index = 0; group_index < source.group_count; ++group_index) {
            const auto& source_group = source.groups[group_index];
            if (source_group.count_tag == 0 || source_group.delimiter_tag == 0 ||
                (source_group.entry_count != 0 && source_group.entries == nullptr) ||
                source_group.entry_count > limits.max_total_entries -
                    (new_usage.entries > limits.max_total_entries ? limits.max_total_entries : new_usage.entries)) {
                return {ErrorCode::capacity_exceeded, static_cast<int>(source_group.count_tag)};
            }
            GroupStorage group{};
            group.count_tag = source_group.count_tag;
            group.delimiter_tag = source_group.delimiter_tag;
            group.entries.reserve(source_group.entry_count);
            group.entry_views.resize(source_group.entry_count);
            for (std::size_t entry_index = 0; entry_index < source_group.entry_count; ++entry_index) {
                ++new_usage.entries;
                auto entry = std::unique_ptr<OwnedMessage>(new (std::nothrow) OwnedMessage{});
                if (!entry) return {ErrorCode::capacity_exceeded};
                if (const auto status = entry->copy_from(source_group.entries[entry_index],
                    limits, new_usage, depth + 1); !status.ok()) return status;
                group.entries.push_back(std::move(entry));
            }
            replacement.groups_.push_back(std::move(group));
        }
        *this = std::move(replacement);
        usage = new_usage;
        return Status::success();
    } catch (...) {
        return {ErrorCode::capacity_exceeded};
    }
}

inline detail::MessageView OwnedMessage::view() const noexcept {
    for (std::size_t group_index = 0; group_index < groups_.size(); ++group_index) {
        const auto& group = groups_[group_index];
        for (std::size_t entry_index = 0; entry_index < group.entries.size(); ++entry_index)
            group.entry_views[entry_index] = group.entries[entry_index]->view();
        group_views_[group_index] = {group.count_tag, group.delimiter_tag,
            group.entry_views.data(), group.entry_views.size()};
    }
    detail::MessageView result{};
    result.slots = slots_.empty() ? nullptr : slots_.data();
    result.value_bytes = value_bytes_.empty() ? nullptr : value_bytes_.data();
    result.value_byte_count = value_bytes_.size();
    result.plan = plan_.empty() ? nullptr : plan_.data();
    result.slot_map = slot_map_.empty() ? nullptr : slot_map_.data();
    result.field_count = plan_.size();
    result.presence_bits = presence_.empty() ? nullptr : presence_.data();
    result.msg_type = msg_type_;
    result.status = status_;
    result.required_tags = required_tags_.empty() ? nullptr : required_tags_.data();
    result.required_tag_count = required_tags_.size();
    result.enum_value_allowed = enum_value_allowed_;
    result.groups = group_views_.empty() ? nullptr : group_views_.data();
    result.group_count = group_views_.size();
    return result;
}

// Compile-time fixed-count repeating group holder: stores entries directly in a
// std::tuple with zero copying, zero heap allocations, and zero arena overflow.
template <std::uint32_t CountTag, std::uint32_t DelimiterTag, class... Entries>
class StaticGroup {
    static_assert(CountTag > 0 && DelimiterTag > 0 && CountTag != DelimiterTag);
    static_assert(sizeof...(Entries) > 0);
    static_assert((requires(const Entries& entry) { entry.view(); } && ...));

public:
    static constexpr std::uint32_t count_tag = CountTag;
    static constexpr std::uint32_t delimiter_tag = DelimiterTag;
    static constexpr std::size_t entry_count = sizeof...(Entries);

    explicit StaticGroup(Entries&&... entries) noexcept
        : entries_(std::move(entries)...) {}
    StaticGroup(const StaticGroup&) = delete;
    StaticGroup& operator=(const StaticGroup&) = delete;
    StaticGroup(StaticGroup&& other) noexcept : entries_(std::move(other.entries_)) {}
    StaticGroup& operator=(StaticGroup&& other) noexcept {
        if (this != &other) entries_ = std::move(other.entries_);
        return *this;
    }

    [[nodiscard]] constexpr std::size_t size() const noexcept { return entry_count; }
    [[nodiscard]] constexpr bool empty() const noexcept { return false; }

    [[nodiscard]] Status status() const noexcept {
        Status out = Status::success();
        validate_entries(out, std::make_index_sequence<entry_count>{});
        return out;
    }

    [[nodiscard]] detail::RepeatingGroupView view() const noexcept {
        populate_views(std::make_index_sequence<entry_count>{});
        return {CountTag, DelimiterTag, entry_views_.data(), entry_count};
    }

private:
    template <std::size_t... I>
    void validate_entries(Status& out, std::index_sequence<I...>) const noexcept {
        const auto check_one = [&](const auto& entry) noexcept -> bool {
            const auto v = entry.view();
            if (!v.status.ok()) {
                out = v.status;
                return false;
            }
            if (v.field_count == 0 || v.plan == nullptr || v.plan[0].tag != DelimiterTag ||
                !detail::is_present(v, v.plan[0].slot_index)) {
                out = Status{ErrorCode::required_field_missing, static_cast<int>(DelimiterTag)};
                return false;
            }
            return true;
        };
        (void)(check_one(std::get<I>(entries_)) && ...);
    }

    template <std::size_t... I>
    void populate_views(std::index_sequence<I...>) const noexcept {
        ((entry_views_[I] = std::get<I>(entries_).view()), ...);
    }

    std::tuple<Entries...> entries_;
    mutable std::array<detail::MessageView, entry_count> entry_views_{};
};

template <std::uint32_t CountTag, std::uint32_t DelimiterTag, class... Entries>
[[nodiscard]] auto make_static_group(Entries&&... entries) noexcept {
    static_assert((!std::is_lvalue_reference_v<Entries> && ...),
        "pass repeating group entries by value or std::move(entry)");
    return StaticGroup<CountTag, DelimiterTag, std::remove_cvref_t<Entries>...>{
        std::move(entries)...};
}

// Runtime-shaped, bounded owner used by generated group aliases. Entries may
// use different compile-time shapes; typical flat entries pack into one shared
// inline stack arena (0 heap allocations) using byte offsets (move-safe), and
// seamlessly spill to OwnedMessage heap storage when InlineBytes is exceeded or
// when entries contain nested repeating groups.
template <std::uint32_t CountTag, std::uint32_t DelimiterTag, std::size_t MaxEntries = 16,
    bool Required = false, std::size_t InlineBytes = 1024>
class OwnedGroupBuilder {
    static_assert(CountTag > 0 && DelimiterTag > 0 && CountTag != DelimiterTag);
    static_assert(MaxEntries > 0);

    struct EntryRecord {
        std::uint32_t slots_offset{};
        std::uint32_t value_bytes_offset{};
        std::uint32_t presence_offset{};
        std::uint32_t plan_offset{};
        std::uint32_t slot_map_offset{};
        std::uint32_t required_tags_offset{};
        std::uint32_t msg_type_offset{};
        std::uint32_t field_count{};
        std::uint32_t value_byte_count{};
        std::uint32_t required_tag_count{};
        std::uint32_t msg_type_size{};
        std::uint32_t heap_index{};
        const detail::PlanField* static_plan_ptr{};
        const std::uint32_t* static_slot_map_ptr{};
        const std::uint32_t* static_required_tags_ptr{};
        std::string_view static_msg_type{};
        detail::EnumValueValidator enum_value_allowed{};
        Status status{};
        bool in_heap{};
        bool has_presence{};
        bool has_slot_map{};
        bool has_static_plan{};
        bool prevalidated_scalars{};
        bool presence_validated{};
    };

    [[nodiscard]] static constexpr std::size_t align_up(std::size_t offset, std::size_t align) noexcept {
        return (offset + align - 1U) & ~(align - 1U);
    }

public:
    static constexpr std::uint32_t count_tag = CountTag;
    static constexpr std::uint32_t delimiter_tag = DelimiterTag;
    static constexpr std::size_t max_entries = MaxEntries;
    static constexpr bool required = Required;
    static constexpr std::size_t inline_bytes = InlineBytes;

    explicit OwnedGroupBuilder(OwnedGroupLimits limits = {}) noexcept : limits_(limits) {}
    OwnedGroupBuilder(const OwnedGroupBuilder&) = delete;
    OwnedGroupBuilder& operator=(const OwnedGroupBuilder&) = delete;
    OwnedGroupBuilder(OwnedGroupBuilder&& other) noexcept
        : limits_(other.limits_),
          arena_used_(other.arena_used_),
          heap_entries_(std::move(other.heap_entries_)),
          usage_(other.usage_),
          entry_count_(other.entry_count_),
          status_(other.status_),
          touched_(other.touched_) {
        if (arena_used_ != 0) std::memcpy(arena_, other.arena_, arena_used_);
        for (std::size_t i = 0; i < entry_count_; ++i) records_[i] = other.records_[i];
        other.arena_used_ = 0;
        other.entry_count_ = 0;
        other.usage_ = {};
        other.touched_ = false;
    }
    OwnedGroupBuilder& operator=(OwnedGroupBuilder&& other) noexcept {
        if (this == &other) return *this;
        limits_ = other.limits_;
        arena_used_ = other.arena_used_;
        if (arena_used_ != 0) std::memcpy(arena_, other.arena_, arena_used_);
        for (std::size_t i = 0; i < other.entry_count_; ++i) records_[i] = other.records_[i];
        heap_entries_ = std::move(other.heap_entries_);
        usage_ = other.usage_;
        entry_count_ = other.entry_count_;
        status_ = other.status_;
        touched_ = other.touched_;
        other.arena_used_ = 0;
        other.entry_count_ = 0;
        other.usage_ = {};
        other.touched_ = false;
        return *this;
    }

    [[nodiscard]] std::size_t size() const noexcept { return entry_count_; }
    [[nodiscard]] bool empty() const noexcept { return entry_count_ == 0; }
    [[nodiscard]] bool touched() const noexcept { return touched_; }
    [[nodiscard]] Status status() const noexcept { return status_; }
    void set_empty() noexcept { touched_ = true; }

    void reset() noexcept {
        arena_used_ = 0;
        entry_count_ = 0;
        usage_ = {};
        status_ = Status::success();
        touched_ = false;
        heap_entries_.clear();
    }

    template <class Entry>
        requires requires(const Entry& entry) { entry.view(); }
    [[nodiscard]] Status add(Entry&& entry) noexcept {
        if (!status_.ok()) return status_;
        if (entry_count_ == MaxEntries) {
            status_ = {ErrorCode::capacity_exceeded, static_cast<int>(CountTag), entry_count_};
            return status_;
        }
        const auto view = entry.view();
        if (!view.status.ok()) {
            status_ = view.status;
            return status_;
        }
        if (view.field_count == 0 || view.plan == nullptr || view.plan[0].tag != DelimiterTag ||
            !detail::is_present(view, view.plan[0].slot_index)) {
            status_ = {ErrorCode::required_field_missing, static_cast<int>(DelimiterTag)};
            return status_;
        }
        auto usage = usage_;
        ++usage.entries; // include this entry and any nested entries it owns
        if (usage.entries > limits_.max_total_entries) {
            status_ = {ErrorCode::capacity_exceeded, static_cast<int>(CountTag), entry_count_};
            return status_;
        }

        // Fast path: flat entry fits into shared inline arena (0 heap allocations).
        if (view.group_count == 0 && try_store_inline(view, usage)) {
            usage_ = usage;
            ++entry_count_;
            touched_ = true;
            return Status::success();
        }

        // Fallback path: arena overflow or nested repeating groups -> OwnedMessage on heap.
        OwnedMessage owned;
        if (const auto copied = owned.copy_from(view, limits_, usage); !copied.ok()) {
            status_ = copied;
            return status_;
        }
        try {
            EntryRecord rec{};
            rec.in_heap = true;
            rec.heap_index = static_cast<std::uint32_t>(heap_entries_.size());
            heap_entries_.push_back(std::move(owned));
            records_[entry_count_] = rec;
        } catch (...) {
            status_ = {ErrorCode::capacity_exceeded};
            return status_;
        }
        usage_ = usage;
        ++entry_count_;
        touched_ = true;
        return Status::success();
    }

    [[nodiscard]] detail::RepeatingGroupView view() const noexcept {
        for (std::size_t i = 0; i < entry_count_; ++i) {
            entry_views_[i] = make_entry_view(records_[i]);
        }
        return {CountTag, DelimiterTag, entry_views_.data(), entry_count_};
    }

private:
    [[nodiscard]] bool try_store_inline(const detail::MessageView& source,
                                        GroupPayloadUsage& usage) noexcept {
        if ((source.field_count != 0 && (source.plan == nullptr || source.slots == nullptr)) ||
            (source.value_byte_count != 0 && source.value_bytes == nullptr) ||
            source.msg_type.empty() ||
            source.field_count > limits_.max_fields_per_message ||
            source.value_byte_count > limits_.max_value_bytes_per_message ||
            (source.required_tag_count != 0 && source.required_tags == nullptr)) {
            return false;
        }
        if (usage.fields > limits_.max_total_fields ||
            source.field_count > limits_.max_total_fields - usage.fields ||
            usage.value_bytes > limits_.max_total_value_bytes ||
            source.value_byte_count > limits_.max_total_value_bytes - usage.value_bytes) {
            return false;
        }

        const bool copy_plan = !source.static_plan;
        const std::size_t slots_bytes = source.field_count * sizeof(detail::SlotValue);
        const std::size_t presence_bytes =
            source.presence_bits != nullptr ? (source.field_count + 7U) / 8U : 0U;
        const std::size_t plan_bytes =
            copy_plan ? source.field_count * sizeof(detail::PlanField) : 0U;
        const std::size_t slot_map_bytes =
            (copy_plan && source.slot_map != nullptr) ? source.field_count * sizeof(std::uint32_t) : 0U;
        const std::size_t req_bytes =
            copy_plan ? source.required_tag_count * sizeof(std::uint32_t) : 0U;
        const std::size_t msg_type_bytes = copy_plan ? source.msg_type.size() : 0U;

        std::size_t pos = arena_used_;
        std::size_t slots_off = 0, plan_off = 0, slot_map_off = 0, req_off = 0;
        std::size_t val_off = 0, pres_off = 0, mt_off = 0;

        if (slots_bytes != 0) {
            pos = align_up(pos, alignof(detail::SlotValue));
            slots_off = pos;
            pos += slots_bytes;
        }
        if (plan_bytes != 0) {
            pos = align_up(pos, alignof(detail::PlanField));
            plan_off = pos;
            pos += plan_bytes;
        }
        if (slot_map_bytes != 0) {
            pos = align_up(pos, alignof(std::uint32_t));
            slot_map_off = pos;
            pos += slot_map_bytes;
        }
        if (req_bytes != 0) {
            pos = align_up(pos, alignof(std::uint32_t));
            req_off = pos;
            pos += req_bytes;
        }
        if (source.value_byte_count != 0) {
            val_off = pos;
            pos += source.value_byte_count;
        }
        if (presence_bytes != 0) {
            pres_off = pos;
            pos += presence_bytes;
        }
        if (msg_type_bytes != 0) {
            mt_off = pos;
            pos += msg_type_bytes;
        }
        if (pos > InlineBytes) return false;

        if (slots_bytes != 0) std::memcpy(arena_ + slots_off, source.slots, slots_bytes);
        if (plan_bytes != 0) std::memcpy(arena_ + plan_off, source.plan, plan_bytes);
        if (slot_map_bytes != 0) std::memcpy(arena_ + slot_map_off, source.slot_map, slot_map_bytes);
        if (req_bytes != 0) std::memcpy(arena_ + req_off, source.required_tags, req_bytes);
        if (source.value_byte_count != 0) {
            std::memcpy(arena_ + val_off, source.value_bytes, source.value_byte_count);
        }
        if (presence_bytes != 0) std::memcpy(arena_ + pres_off, source.presence_bits, presence_bytes);
        if (msg_type_bytes != 0) std::memcpy(arena_ + mt_off, source.msg_type.data(), msg_type_bytes);

        EntryRecord rec{};
        rec.slots_offset = static_cast<std::uint32_t>(slots_off);
        rec.value_bytes_offset = static_cast<std::uint32_t>(val_off);
        rec.presence_offset = static_cast<std::uint32_t>(pres_off);
        rec.plan_offset = static_cast<std::uint32_t>(plan_off);
        rec.slot_map_offset = static_cast<std::uint32_t>(slot_map_off);
        rec.required_tags_offset = static_cast<std::uint32_t>(req_off);
        rec.msg_type_offset = static_cast<std::uint32_t>(mt_off);
        rec.field_count = static_cast<std::uint32_t>(source.field_count);
        rec.value_byte_count = static_cast<std::uint32_t>(source.value_byte_count);
        rec.required_tag_count = static_cast<std::uint32_t>(source.required_tag_count);
        rec.msg_type_size = static_cast<std::uint32_t>(source.msg_type.size());
        rec.has_presence = presence_bytes != 0;
        rec.has_slot_map = source.slot_map != nullptr && source.field_count != 0;
        rec.has_static_plan = source.static_plan;
        rec.prevalidated_scalars = source.prevalidated_scalars;
        rec.presence_validated = source.presence_validated;
        if (source.static_plan) {
            rec.static_plan_ptr = source.plan;
            rec.static_slot_map_ptr = source.slot_map;
            rec.static_required_tags_ptr = source.required_tags;
            rec.static_msg_type = source.msg_type;
        }
        rec.enum_value_allowed = source.enum_value_allowed;
        rec.status = source.status;

        records_[entry_count_] = rec;
        arena_used_ = pos;
        usage.fields += source.field_count;
        usage.value_bytes += source.value_byte_count;
        return true;
    }

    [[nodiscard]] detail::MessageView make_entry_view(const EntryRecord& rec) const noexcept {
        if (rec.in_heap) return heap_entries_[rec.heap_index].view();
        detail::MessageView result{};
        result.slots = rec.field_count != 0
            ? reinterpret_cast<const detail::SlotValue*>(arena_ + rec.slots_offset) : nullptr;
        result.value_bytes = rec.value_byte_count != 0
            ? reinterpret_cast<const char*>(arena_ + rec.value_bytes_offset) : nullptr;
        result.value_byte_count = rec.value_byte_count;
        if (rec.has_static_plan) {
            result.plan = rec.static_plan_ptr;
            result.slot_map = rec.static_slot_map_ptr;
            result.required_tags = rec.static_required_tags_ptr;
            result.msg_type = rec.static_msg_type;
            result.static_plan = true;
        } else {
            result.plan = rec.field_count != 0
                ? reinterpret_cast<const detail::PlanField*>(arena_ + rec.plan_offset) : nullptr;
            result.slot_map = rec.has_slot_map
                ? reinterpret_cast<const std::uint32_t*>(arena_ + rec.slot_map_offset) : nullptr;
            result.required_tags = rec.required_tag_count != 0
                ? reinterpret_cast<const std::uint32_t*>(arena_ + rec.required_tags_offset) : nullptr;
            result.msg_type = {reinterpret_cast<const char*>(arena_ + rec.msg_type_offset), rec.msg_type_size};
        }
        result.field_count = rec.field_count;
        result.presence_bits = rec.has_presence
            ? reinterpret_cast<const std::uint8_t*>(arena_ + rec.presence_offset) : nullptr;
        result.status = rec.status;
        result.required_tag_count = rec.required_tag_count;
        result.enum_value_allowed = rec.enum_value_allowed;
        result.prevalidated_scalars = rec.prevalidated_scalars;
        result.presence_validated = rec.presence_validated;
        return result;
    }

    OwnedGroupLimits limits_{};
    alignas(detail::SlotValue) std::byte arena_[InlineBytes > 0 ? InlineBytes : 1];
    std::size_t arena_used_{};
    std::array<EntryRecord, MaxEntries> records_{};
    std::vector<OwnedMessage> heap_entries_{};
    mutable std::array<detail::MessageView, MaxEntries> entry_views_{};
    GroupPayloadUsage usage_{};
    std::size_t entry_count_{};
    Status status_{};
    bool touched_{};
};

template <class Builder, class Group>
class GroupAttachment
    : public detail::schema_type_alias<Builder>,
      public detail::group_attachment_setters_t<Builder, GroupAttachment<Builder, Group>> {
public:
    using policy_type = typename Builder::policy_type;
    static constexpr std::size_t attached_group_count = Builder::attached_group_count + 1;

    GroupAttachment(Builder&& builder, Group&& group) noexcept
        : builder_(std::move(builder)), group_(std::move(group)) {}
    GroupAttachment(const GroupAttachment&) = delete;
    GroupAttachment& operator=(const GroupAttachment&) = delete;
    GroupAttachment(GroupAttachment&&) noexcept = default;
    GroupAttachment& operator=(GroupAttachment&&) noexcept = default;

    [[nodiscard]] Status status() const noexcept { return view().status; }

    [[nodiscard]] detail::MessageView view() const noexcept {
        auto message = builder_.view();
        const auto group_status = group_.status();
        if (message.status.ok() && !group_status.ok()) message.status = group_status;
        if (!message.status.ok()) return message;
        for (std::size_t i = 0; i < Builder::attached_group_count; ++i)
            groups_[i] = message.groups[i];
        groups_[Builder::attached_group_count] = group_.view();
        message.groups = groups_.data();
        message.group_count = attached_group_count;
        return message;
    }

    [[nodiscard]] Builder& builder() noexcept { return builder_; }
    [[nodiscard]] const Builder& builder() const noexcept { return builder_; }

    template <std::uint32_t Tag, class V>
    [[nodiscard]] auto set(V&& value) && {
        auto next = std::move(builder_).template set<Tag>(std::forward<V>(value));
        using Next = decltype(next);
        return GroupAttachment<Next, Group>{std::move(next), std::move(group_)};
    }

    template <class NextGroup>
    [[nodiscard]] auto with_group(NextGroup&& group) && {
        return attach_group(std::move(*this), std::forward<NextGroup>(group));
    }

private:
    Builder builder_;
    Group group_;
    mutable std::array<detail::RepeatingGroupView, attached_group_count> groups_{};
};

template <class Builder, class Group>
[[nodiscard]] auto attach_group(Builder&& builder, Group&& group) noexcept {
    using GroupType = std::remove_cvref_t<Group>;
    static_assert(requires { GroupType::count_tag; }, "group must be a schema GroupBuilder");
    auto counted = std::forward<Builder>(builder).template set<GroupType::count_tag>(group.size());
    using CountedBuilder = decltype(counted);
    return GroupAttachment<CountedBuilder, GroupType>{
        std::move(counted), std::forward<Group>(group)};
}

} // namespace nimblefix
