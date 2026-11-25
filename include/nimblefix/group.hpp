#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

#include <nimblefix/message.hpp>

namespace nimblefix {

// Bounded owner for one schema-described repeating group used with FixedShapeMessage.
// Each entry has one compile-time shape; optional members are represented by that
// entry's presence bits. Nested groups stay owned by their entry builders.
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

} // namespace nimblefix
