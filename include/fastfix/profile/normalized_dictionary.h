#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fastfix/base/result.h"
#include "fastfix/profile/artifact.h"

namespace fastfix::profile {

enum class ValueType : std::uint32_t {
    kUnknown = 0,
    kString = 1,
    kInt = 2,
    kChar = 3,
    kFloat = 4,
    kBoolean = 5,
    kTimestamp = 6,
};

enum class FieldFlags : std::uint32_t {
    kNone = 0,
    kCustom = 1U << 0,
    kAllowTypeOverride = 1U << 1,
};

enum class MessageFlags : std::uint32_t {
    kNone = 0,
    kAdmin = 1U << 0,
};

enum class GroupFlags : std::uint32_t {
    kNone = 0,
    kAllowDelimiterOverride = 1U << 0,
    kRequiredBitmapOverflow = 1U << 1,
};

enum class FieldRuleFlags : std::uint32_t {
    kNone = 0,
    kRequired = 1U << 0,
};

#pragma pack(push, 1)
struct FieldDefRecord {
    std::uint32_t tag;
    std::uint32_t name_offset;
    std::uint32_t value_type;
    std::uint32_t flags;
};

struct FieldRuleRecord {
    std::uint32_t tag;
    std::uint32_t flags;
};

struct MessageDefRecord {
    std::uint32_t msg_type_offset;
    std::uint32_t name_offset;
    std::uint32_t first_field_rule;
    std::uint32_t field_rule_count;
    std::uint32_t flags;
    std::uint32_t reserved0;
};

struct GroupDefRecord {
    std::uint32_t count_tag;
    std::uint32_t delimiter_tag;
    std::uint32_t name_offset;
    std::uint32_t first_field_rule;
    std::uint32_t field_rule_count;
    std::uint32_t flags;
    std::uint64_t required_field_bitmap;
};

struct AdminRuleEntry {
    std::uint32_t msg_type_offset;
    std::uint32_t flags;
};

struct ValidationRuleEntry {
    std::uint32_t tag;
    std::uint32_t value_type;
    std::uint32_t flags;
};

struct LookupTableEntry {
    std::uint32_t field_index;
};

struct TemplateDescriptorEntry {
    std::uint32_t msg_type_offset;
    std::uint32_t field_count;
    std::uint32_t first_field_rule_index;
    std::uint32_t flags;
};
#pragma pack(pop)

static_assert(sizeof(FieldDefRecord) == 16);
static_assert(sizeof(FieldRuleRecord) == 8);
static_assert(sizeof(MessageDefRecord) == 24);
static_assert(sizeof(GroupDefRecord) == 32);
static_assert(sizeof(AdminRuleEntry) == 8);
static_assert(sizeof(ValidationRuleEntry) == 12);
static_assert(sizeof(LookupTableEntry) == 4);
static_assert(sizeof(TemplateDescriptorEntry) == 16);

struct FieldDef {
    std::uint32_t tag{0};
    std::string name;
    ValueType value_type{ValueType::kUnknown};
    std::uint32_t flags{0};
};

struct FieldRule {
    std::uint32_t tag{0};
    std::uint32_t flags{0};

    [[nodiscard]] bool required() const {
        return (flags & static_cast<std::uint32_t>(FieldRuleFlags::kRequired)) != 0;
    }
};

struct MessageDef {
    std::string msg_type;
    std::string name;
    std::vector<FieldRule> field_rules;
    std::uint32_t flags{0};
};

struct GroupDef {
    std::uint32_t count_tag{0};
    std::uint32_t delimiter_tag{0};
    std::string name;
    std::vector<FieldRule> field_rules;
    std::uint32_t flags{0};

    [[nodiscard]] bool delimiter_override_allowed() const {
        return (flags & static_cast<std::uint32_t>(GroupFlags::kAllowDelimiterOverride)) != 0U;
    }
};

struct NormalizedDictionary {
    std::uint64_t schema_hash{0};
    std::uint64_t profile_id{0};
    std::vector<FieldDef> fields;
    std::vector<MessageDef> messages;
    std::vector<GroupDef> groups;
    std::vector<FieldRule> header_fields;
    std::vector<FieldRule> trailer_fields;
};

class NormalizedDictionaryView {
  public:
    [[nodiscard]] static auto FromProfile(LoadedProfile profile) -> base::Result<NormalizedDictionaryView>;

    [[nodiscard]] const LoadedProfile& profile() const {
        return profile_;
    }

    [[nodiscard]] std::size_t field_count() const {
        return field_defs_.size();
    }

    [[nodiscard]] std::size_t message_count() const {
        return message_defs_.size();
    }

    [[nodiscard]] std::size_t group_count() const {
        return group_defs_.size();
    }

    [[nodiscard]] std::span<const FieldDefRecord> fields() const {
        return field_defs_.entries();
    }

    [[nodiscard]] std::span<const MessageDefRecord> messages() const {
        return message_defs_.entries();
    }

    [[nodiscard]] std::span<const GroupDefRecord> groups() const {
        return group_defs_.entries();
    }

    [[nodiscard]] std::optional<std::string_view> string_at(std::uint32_t offset) const {
        return string_table_.string_at(offset);
    }

    [[nodiscard]] std::optional<std::string_view> field_name(const FieldDefRecord& record) const {
        return string_table_.string_at(record.name_offset);
    }

    [[nodiscard]] std::optional<std::string_view> message_name(const MessageDefRecord& record) const {
        return string_table_.string_at(record.name_offset);
    }

    [[nodiscard]] std::optional<std::string_view> message_type(const MessageDefRecord& record) const {
        return string_table_.string_at(record.msg_type_offset);
    }

    [[nodiscard]] std::optional<std::string_view> group_name(const GroupDefRecord& record) const {
        return string_table_.string_at(record.name_offset);
    }

    [[nodiscard]] auto find_field(std::uint32_t tag) const -> const FieldDefRecord*;
    [[nodiscard]] auto find_message(std::string_view msg_type) const -> const MessageDefRecord*;
    [[nodiscard]] auto find_group(std::uint32_t count_tag) const -> const GroupDefRecord*;

    [[nodiscard]] auto message_rule_allows_tag(const MessageDefRecord& record, std::uint32_t tag) const -> bool;
    [[nodiscard]] auto group_rule_allows_tag(const GroupDefRecord& record, std::uint32_t tag) const -> bool;
    [[nodiscard]] auto message_rule_index(const MessageDefRecord& record, std::uint32_t tag) const -> int;
    [[nodiscard]] auto group_rule_index(const GroupDefRecord& record, std::uint32_t tag) const -> int;

    [[nodiscard]] auto message_field_rules(const MessageDefRecord& record) const -> std::span<const FieldRuleRecord>;
    [[nodiscard]] auto group_field_rules(const GroupDefRecord& record) const -> std::span<const FieldRuleRecord>;

  private:
    struct TagIndexEntry {
        std::uint32_t key;
        std::uint32_t index;
    };
    struct MsgTypeIndexEntry {
        std::string_view msg_type;
        std::uint32_t index;
    };
    struct TagRuleEntry {
        std::uint32_t tag;
        std::uint32_t original_index;
    };

    static constexpr std::size_t kDirectLookupSize = 10000;
    static constexpr std::uint16_t kNoEntry = 0xFFFFU;

    LoadedProfile profile_;
    StringTableView string_table_;
    FixedSectionView<FieldDefRecord> field_defs_;
    FixedSectionView<MessageDefRecord> message_defs_;
    FixedSectionView<GroupDefRecord> group_defs_;
    FixedSectionView<FieldRuleRecord> message_field_rules_;
    FixedSectionView<FieldRuleRecord> group_field_rules_;

    std::vector<TagIndexEntry> field_index_;
    std::vector<TagIndexEntry> group_index_;
    std::vector<MsgTypeIndexEntry> message_index_;
    std::vector<TagRuleEntry> sorted_message_rules_;
    std::vector<TagRuleEntry> sorted_group_rules_;

    /// Direct-address table: tag -> index into field_defs_ for tags < kDirectLookupSize.
    /// Falls back to binary search on field_index_ for tags >= kDirectLookupSize.
    std::array<std::uint16_t, kDirectLookupSize> field_direct_lookup_;
};

}  // namespace fastfix::profile
