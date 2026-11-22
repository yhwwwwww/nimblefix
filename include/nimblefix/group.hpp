#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
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

// Runtime-shaped, bounded owner used by generated group aliases. Entries may
// use different compile-time shapes; each payload keeps only its actual slots,
// used bytes, and nested groups.
template <std::uint32_t CountTag, std::uint32_t DelimiterTag, std::size_t MaxEntries = 16,
    bool Required = false>
class OwnedGroupBuilder {
    static_assert(CountTag > 0 && DelimiterTag > 0 && CountTag != DelimiterTag);
    static_assert(MaxEntries > 0);

public:
    static constexpr std::uint32_t count_tag = CountTag;
    static constexpr std::uint32_t delimiter_tag = DelimiterTag;
    static constexpr std::size_t max_entries = MaxEntries;
    static constexpr bool required = Required;

    explicit OwnedGroupBuilder(OwnedGroupLimits limits = {}) noexcept : limits_(limits) {}
    OwnedGroupBuilder(const OwnedGroupBuilder&) = delete;
    OwnedGroupBuilder& operator=(const OwnedGroupBuilder&) = delete;
    OwnedGroupBuilder(OwnedGroupBuilder&&) noexcept = default;
    OwnedGroupBuilder& operator=(OwnedGroupBuilder&&) noexcept = default;

    [[nodiscard]] std::size_t size() const noexcept { return entry_count_; }
    [[nodiscard]] bool empty() const noexcept { return entry_count_ == 0; }
    [[nodiscard]] bool touched() const noexcept { return touched_; }
    [[nodiscard]] Status status() const noexcept { return status_; }
    void set_empty() noexcept { touched_ = true; }

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
        if (const auto copied = entries_[entry_count_].copy_from(view, limits_, usage); !copied.ok()) {
            status_ = copied;
            return status_;
        }
        usage_ = usage;
        ++entry_count_;
        touched_ = true;
        return Status::success();
    }

    [[nodiscard]] detail::RepeatingGroupView view() const noexcept {
        for (std::size_t i = 0; i < entry_count_; ++i) entry_views_[i] = entries_[i].view();
        return {CountTag, DelimiterTag, entry_views_.data(), entry_count_};
    }

private:
    OwnedGroupLimits limits_{};
    std::array<OwnedMessage, MaxEntries> entries_{};
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
