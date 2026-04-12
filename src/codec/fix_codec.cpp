#include "fastfix/codec/fix_codec.h"
#include "fastfix/codec/compiled_decoder.h"
#include "fastfix/codec/simd_scan.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstring>
#include <ctime>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <tuple>

namespace fastfix::codec {

namespace {

template <typename Integer>
inline constexpr std::size_t kIntegerTextBufferBytes =
    static_cast<std::size_t>(std::numeric_limits<Integer>::digits10) + 3U;

struct SeenTagBuffer {
    // Fixed-size bitset covering FIX tags 0..kBitsetTagLimit-1 (~1.2 KB).
    // Tags above the threshold fall back to a small overflow vector which
    // is almost never needed in practice (standard FIX tags < 10000).
    static constexpr std::size_t kBitsetTagLimit = 10048U;  // 157 * 64
    static constexpr std::size_t kBitsetWords = kBitsetTagLimit / 64U;

    auto contains(std::uint32_t tag) const -> bool {
        if (tag < kBitsetTagLimit) {
            const auto word = tag / 64U;
            const auto bit = tag % 64U;
            return (bitset_[word] & (std::uint64_t{1} << bit)) != 0U;
        }
        return std::find(overflow_tags_.begin(), overflow_tags_.end(), tag) != overflow_tags_.end();
    }

    auto push_back(std::uint32_t tag) -> void {
        if (tag < kBitsetTagLimit) {
            const auto word = tag / 64U;
            const auto bit = tag % 64U;
            bitset_[word] |= std::uint64_t{1} << bit;
        } else {
            overflow_tags_.push_back(tag);
        }
    }

    auto reserve(std::size_t /*count*/) -> void {
        // No-op: bitset is pre-allocated, overflow is rare.
    }

    auto clear() -> void {
        bitset_.fill(0U);
        overflow_tags_.clear();
    }

    std::array<std::uint64_t, kBitsetWords> bitset_{};
    std::vector<std::uint32_t> overflow_tags_;
};

struct ParsedContainerRef {
    bool root{false};
    std::uint32_t entry_index{message::kInvalidParsedIndex};
};

struct ScopeValidationState {
    SeenTagBuffer seen_tags;
};

struct CompiledScopeTemplate;

struct CompiledScopeStep {
    enum class Kind : std::uint8_t {
        kScalar,
        kGroup,
    };

    Kind kind{Kind::kScalar};
    std::uint32_t tag{0};
    std::string prefix;
    std::shared_ptr<const CompiledScopeTemplate> group_scope;
};

struct CompiledScopeTemplate {
    std::vector<CompiledScopeStep> steps;
    std::vector<std::uint32_t> scalar_tags;
    std::vector<std::uint32_t> group_tags;
};

auto IsStandardSessionField(std::uint32_t tag) -> bool {
    switch (tag) {
        case 34U:
        case 35U:
        case 43U:
        case 49U:
        case 52U:
        case 56U:
        case 97U:
        case 122U:
        case 1137U:
            return true;
        default:
            return false;
    }
}

auto IsImplicitStandardField(std::uint32_t tag) -> bool {
    switch (tag) {
        case 7U:
        case 16U:
        case 34U:
        case 35U:
        case 36U:
        case 43U:
        case 45U:
        case 49U:
        case 52U:
        case 56U:
        case 58U:
        case 97U:
        case 98U:
        case 108U:
        case 112U:
        case 123U:
        case 141U:
        case 371U:
        case 372U:
        case 373U:
        case 122U:
        case 1137U:
            return true;
        default:
            return false;
    }
}

auto SetValidationIssue(
    ValidationIssue* issue,
    ValidationIssueKind kind,
    std::uint32_t tag,
    std::string text) -> void {
    if (issue == nullptr || issue->present()) {
        return;
    }
    issue->kind = kind;
    issue->tag = tag;
    issue->text = std::move(text);
}

auto HasSeenTag(const ScopeValidationState& state, std::uint32_t tag) -> bool {
    return state.seen_tags.contains(tag);
}

template <typename RuleSpan>
auto ValidateConsumedField(
    const profile::NormalizedDictionaryView& dictionary,
    std::uint32_t tag,
    RuleSpan rules,
    int rule_index,
    ScopeValidationState* state,
    ValidationIssue* issue,
    bool allow_standard_session_fields,
    bool schema_known,
    const char* scope_name,
    const profile::FieldDefRecord* known_field_def = nullptr) -> void {
    if (state == nullptr) {
        return;
    }

    if (HasSeenTag(*state, tag)) {
        SetValidationIssue(
            issue,
            ValidationIssueKind::kDuplicateField,
            tag,
            "field " + std::to_string(tag) + " appears more than once");
        return;
    }
    state->seen_tags.push_back(tag);

    if (allow_standard_session_fields && IsStandardSessionField(tag)) {
        return;
    }
    if (!schema_known) {
        return;
    }
    const auto* field_ptr = known_field_def != nullptr ? known_field_def : dictionary.find_field(tag);
    if (field_ptr == nullptr && !(rules.empty() && IsImplicitStandardField(tag))) {
        SetValidationIssue(
            issue,
            ValidationIssueKind::kUnknownField,
            tag,
            "field " + std::to_string(tag) + " is not present in the bound dictionary");
        return;
    }

    if (!rules.empty()) {
        if (rule_index < 0) {
            SetValidationIssue(
                issue,
                ValidationIssueKind::kFieldNotAllowed,
                tag,
                "field " + std::to_string(tag) + " is not allowed by the bound " + std::string(scope_name) +
                    " definition");
            return;
        }
    }
}

auto ParseUnsigned(std::string_view text, const char* label) -> base::Result<std::uint32_t> {
    std::uint32_t value = 0;
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc() || ptr != end) {
        return base::Status::InvalidArgument(std::string("invalid ") + label);
    }
    return value;
}

struct InlineField {
    std::uint32_t tag;
    std::size_t start_offset;
    std::size_t value_offset;
    std::uint32_t value_length;
    std::size_t next_pos;
};

auto ScanNextField(
    std::span<const std::byte> bytes,
    std::size_t field_start,
    std::byte delimiter_byte,
    std::byte equals_byte) -> base::Result<InlineField> {
    if (field_start >= bytes.size()) {
        return base::Status::FormatError("FIX frame is missing its final delimiter");
    }
    const auto remaining = bytes.size() - field_start;
    const auto* soh_ptr = FindByte(bytes.data() + field_start, remaining, delimiter_byte);
    const auto soh_index = static_cast<std::size_t>(soh_ptr - bytes.data());
    if (soh_index >= bytes.size()) {
        return base::Status::FormatError("FIX frame is missing its final delimiter");
    }
    if (soh_index == field_start) {
        return base::Status::FormatError("empty FIX field is not allowed");
    }
    const auto field_len = soh_index - field_start;
    const auto* eq_ptr = FindByte(bytes.data() + field_start, field_len, equals_byte);
    const auto equals = static_cast<std::size_t>(eq_ptr - (bytes.data() + field_start));
    if (equals >= field_len || equals == 0U || equals == field_len - 1U) {
        return base::Status::FormatError("invalid FIX field syntax");
    }
    const auto* field_bytes = reinterpret_cast<const char*>(bytes.data() + field_start);
    auto tag = ParseUnsigned(std::string_view(field_bytes, equals), "field tag");
    if (!tag.ok()) {
        return tag.status();
    }
    return InlineField{
        .tag = tag.value(),
        .start_offset = field_start,
        .value_offset = field_start + equals + 1U,
        .value_length = static_cast<std::uint32_t>(field_len - equals - 1U),
        .next_pos = soh_index + 1U,
    };
}

auto ParseSigned(std::string_view text, const char* label) -> base::Result<std::int64_t> {
    std::int64_t value = 0;
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc() || ptr != end) {
        return base::Status::InvalidArgument(std::string("invalid ") + label);
    }
    return value;
}

auto ParseDouble(std::string_view text, const char* label) -> base::Result<double> {
    double value = 0.0;
    const auto* begin = text.data();
    const auto* end = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc() || ptr != end) {
        return base::Status::InvalidArgument(std::string("invalid ") + label);
    }
    return value;
}

auto ParseBoolean(std::string_view text, const char* label) -> base::Result<bool> {
    if (text == "Y") {
        return true;
    }
    if (text == "N") {
        return false;
    }
    return base::Status::InvalidArgument(std::string("invalid ") + label);
}

auto FieldTypeFromDef(const profile::FieldDefRecord& field) -> message::FieldValueType {
    switch (static_cast<profile::ValueType>(field.value_type)) {
        case profile::ValueType::kInt:
            return message::FieldValueType::kInt;
        case profile::ValueType::kChar:
            return message::FieldValueType::kChar;
        case profile::ValueType::kFloat:
            return message::FieldValueType::kFloat;
        case profile::ValueType::kBoolean:
            return message::FieldValueType::kBoolean;
        default:
            return message::FieldValueType::kString;
    }
}

auto FieldTypeFallback(std::uint32_t tag) -> message::FieldValueType {
    switch (tag) {
        case 9:
        case 34:
        case 45:
        case 371:
        case 373:
        case 98:
        case 108:
        case 7:
        case 16:
        case 36:
            return message::FieldValueType::kInt;
        case 141:
        case 43:
        case 97:
        case 123:
            return message::FieldValueType::kBoolean;
        default:
            return message::FieldValueType::kString;
    }
}

auto ResolveFieldType(
    std::uint32_t tag,
    const profile::NormalizedDictionaryView& dictionary) -> message::FieldValueType {
    if (const auto* field = dictionary.find_field(tag); field != nullptr) {
        return FieldTypeFromDef(*field);
    }
    return FieldTypeFallback(tag);
}

auto ResolveFieldTypeWithDef(
    std::uint32_t tag,
    const profile::FieldDefRecord* field_def) -> message::FieldValueType {
    if (field_def != nullptr) {
        return FieldTypeFromDef(*field_def);
    }
    return FieldTypeFallback(tag);
}

template <typename Builder>
auto ApplyScalarField(
    Builder& builder,
    std::uint32_t tag,
    std::string_view value,
    const profile::NormalizedDictionaryView& dictionary) -> base::Status {
    switch (ResolveFieldType(tag, dictionary)) {
        case message::FieldValueType::kInt: {
            auto parsed = ParseSigned(value, "integer field");
            if (!parsed.ok()) {
                return parsed.status();
            }
            builder.set_int(tag, parsed.value());
            return base::Status::Ok();
        }
        case message::FieldValueType::kChar:
            if (value.size() != 1U) {
                return base::Status::InvalidArgument("char field must have exactly one byte");
            }
            builder.set_char(tag, value.front());
            return base::Status::Ok();
        case message::FieldValueType::kFloat: {
            auto parsed = ParseDouble(value, "floating-point field");
            if (!parsed.ok()) {
                return parsed.status();
            }
            builder.set_float(tag, parsed.value());
            return base::Status::Ok();
        }
        case message::FieldValueType::kBoolean: {
            auto parsed = ParseBoolean(value, "boolean field");
            if (!parsed.ok()) {
                return parsed.status();
            }
            builder.set_boolean(tag, parsed.value());
            return base::Status::Ok();
        }
        default:
            builder.set_string(tag, value);
            return base::Status::Ok();
    }
}

auto RootContainer() -> ParsedContainerRef {
    return ParsedContainerRef{.root = true};
}

auto EntryContainer(std::uint32_t entry_index) -> ParsedContainerRef {
    return ParsedContainerRef{.root = false, .entry_index = entry_index};
}

auto ResolveContainer(const message::ParsedMessageData& parsed, ParsedContainerRef ref)
    -> const message::ParsedEntryData& {
    return ref.root ? parsed.root : parsed.entries[ref.entry_index];
}

auto ResolveContainer(message::ParsedMessageData* parsed, ParsedContainerRef ref)
    -> message::ParsedEntryData& {
    return ref.root ? parsed->root : parsed->entries[ref.entry_index];
}

auto FindParsedFieldIndex(
    const message::ParsedMessageData& parsed,
    ParsedContainerRef container,
    std::uint32_t tag) -> std::uint32_t {
    const auto& entry = ResolveContainer(parsed, container);
    if (container.root) {
        // Use hash table for root
        auto probe = static_cast<std::uint16_t>(tag % message::kFieldHashTableSize);
        for (std::size_t i = 0; i < message::kFieldHashTableSize; ++i) {
            const auto slot_index = parsed.field_hash_table[probe];
            if (slot_index == message::kInvalidHashSlot) {
                return message::kInvalidParsedIndex;
            }
            if (parsed.field_slots[slot_index].tag == tag) {
                return static_cast<std::uint32_t>(slot_index);
            }
            probe = static_cast<std::uint16_t>((probe + 1U) % message::kFieldHashTableSize);
        }
        return message::kInvalidParsedIndex;
    }
    // For group entries, linear scan over contiguous range
    if (entry.first_field_index == message::kInvalidParsedIndex || entry.field_count == 0U) {
        return message::kInvalidParsedIndex;
    }
    const auto end_index = entry.first_field_index + entry.field_count;
    for (auto i = entry.first_field_index; i < end_index; ++i) {
        if (parsed.field_slots[i].tag == tag) {
            return i;
        }
    }
    return message::kInvalidParsedIndex;
}

auto InsertIntoRootHashTable(
    message::ParsedMessageData* parsed,
    std::uint32_t slot_index) -> void {
    const auto tag = parsed->field_slots[slot_index].tag;
    auto probe = static_cast<std::uint16_t>(tag % message::kFieldHashTableSize);
    for (std::size_t i = 0; i < message::kFieldHashTableSize; ++i) {
        if (parsed->field_hash_table[probe] == message::kInvalidHashSlot) {
            parsed->field_hash_table[probe] = static_cast<std::uint16_t>(slot_index);
            return;
        }
        probe = static_cast<std::uint16_t>((probe + 1U) % message::kFieldHashTableSize);
    }
}

auto AppendParsedFieldSlot(
    message::ParsedMessageData* parsed,
    ParsedContainerRef container,
    message::ParsedFieldSlot slot) -> bool {
    auto existing = FindParsedFieldIndex(*parsed, container, slot.tag);
    if (existing != message::kInvalidParsedIndex) {
        parsed->field_slots[existing] = slot;
        return true;
    }

    const auto new_index = static_cast<std::uint32_t>(parsed->field_slots.size());
    parsed->field_slots.push_back(slot);

    auto& entry = ResolveContainer(parsed, container);
    if (entry.first_field_index == message::kInvalidParsedIndex) {
        entry.first_field_index = new_index;
    }
    ++entry.field_count;

    if (container.root) {
        InsertIntoRootHashTable(parsed, new_index);
    }
    return false;
}

auto MakeParsedFieldSlot(
    std::uint32_t tag,
    std::uint32_t value_offset,
    std::uint32_t value_length,
    const profile::NormalizedDictionaryView& dictionary) -> message::ParsedFieldSlot {
    message::ParsedFieldSlot slot;
    slot.tag = tag;
    slot.set_type(ResolveFieldType(tag, dictionary));
    slot.value_offset = value_offset;
    slot.value_length = value_length;
    return slot;
}

auto MakeParsedFieldSlotWithType(
    std::uint32_t tag,
    std::uint32_t value_offset,
    std::uint32_t value_length,
    message::FieldValueType field_type) -> message::ParsedFieldSlot {
    message::ParsedFieldSlot slot;
    slot.tag = tag;
    slot.set_type(field_type);
    slot.value_offset = value_offset;
    slot.value_length = value_length;
    return slot;
}

auto FindParsedGroupIndex(
    const message::ParsedMessageData& parsed,
    ParsedContainerRef container,
    std::uint32_t count_tag) -> std::uint32_t {
    auto group_index = ResolveContainer(parsed, container).first_group;
    while (group_index != message::kInvalidParsedIndex) {
        if (parsed.groups[group_index].count_tag == count_tag) {
            return group_index;
        }
        group_index = parsed.groups[group_index].next_group;
    }
    return message::kInvalidParsedIndex;
}

auto EnsureParsedGroup(
    message::ParsedMessageData* parsed,
    ParsedContainerRef container,
    std::uint32_t count_tag,
    std::uint16_t depth) -> std::uint32_t {
    auto existing = FindParsedGroupIndex(*parsed, container, count_tag);
    if (existing != message::kInvalidParsedIndex) {
        return existing;
    }

    const auto new_index = static_cast<std::uint32_t>(parsed->groups.size());
    parsed->groups.push_back(message::ParsedGroupFrame{
        .count_tag = count_tag,
        .first_entry = message::kInvalidParsedIndex,
        .entry_count = 0U,
        .depth = depth,
        .next_group = message::kInvalidParsedIndex,
    });

    auto& entry = ResolveContainer(parsed, container);
    if (entry.first_group == message::kInvalidParsedIndex) {
        entry.first_group = new_index;
    } else {
        auto tail = entry.first_group;
        while (parsed->groups[tail].next_group != message::kInvalidParsedIndex) {
            tail = parsed->groups[tail].next_group;
        }
        parsed->groups[tail].next_group = new_index;
    }
    ++entry.group_count;
    return new_index;
}

auto AppendParsedEntry(message::ParsedMessageData* parsed, std::uint32_t group_index) -> std::uint32_t {
    const auto new_index = static_cast<std::uint32_t>(parsed->entries.size());
    parsed->entries.push_back(message::ParsedEntryData{});

    auto& group = parsed->groups[group_index];
    if (group.first_entry == message::kInvalidParsedIndex) {
        group.first_entry = new_index;
    } else {
        // Walk the linked list to find the last entry.
        auto tail = group.first_entry;
        while (parsed->entries[tail].next_entry != message::kInvalidParsedIndex) {
            tail = parsed->entries[tail].next_entry;
        }
        parsed->entries[tail].next_entry = new_index;
    }
    ++group.entry_count;
    return new_index;
}

auto ParseParsedGroupEntries(
    const profile::NormalizedDictionaryView& dictionary,
    std::span<const std::byte> bytes,
    std::size_t byte_pos,
    std::byte delimiter_byte,
    std::byte equals_byte,
    std::string_view count_value,
    const profile::GroupDefRecord& group_def,
    message::ParsedMessageData* parsed,
    ParsedContainerRef parent,
    std::uint16_t depth,
    ValidationIssue* validation_issue) -> base::Result<std::size_t> {
    auto count = ParseUnsigned(count_value, "group count");
    if (!count.ok()) {
        return count.status();
    }
    if (count.value() > kMaxGroupEntryCount) {
        return base::Status::FormatError(
            "group " + std::to_string(group_def.count_tag) +
            " entry count " + std::to_string(count.value()) +
            " exceeds maximum " + std::to_string(kMaxGroupEntryCount));
    }
    if (depth >= kMaxGroupNestingDepth) {
        return base::Status::FormatError(
            "group " + std::to_string(group_def.count_tag) +
            " nesting depth " + std::to_string(depth) +
            " exceeds maximum " + std::to_string(kMaxGroupNestingDepth));
    }
    const auto frame_index = EnsureParsedGroup(parsed, parent, group_def.count_tag, depth);
    const auto group_rules = dictionary.group_field_rules(group_def);
    parsed->entries.reserve(parsed->entries.size() + count.value());

    for (std::uint32_t entry_index = 0; entry_index < count.value(); ++entry_index) {
        ScopeValidationState validation_state;
        validation_state.seen_tags.reserve(group_rules.size());

        if (byte_pos >= bytes.size()) {
            SetValidationIssue(
                validation_issue,
                ValidationIssueKind::kIncorrectNumInGroupCount,
                group_def.count_tag,
                "group " + std::to_string(group_def.count_tag) + " expected delimiter tag " +
                    std::to_string(group_def.delimiter_tag) + " for entry " +
                    std::to_string(entry_index + 1U));
            return byte_pos;
        }
        {
            auto peek = ScanNextField(bytes, byte_pos, delimiter_byte, equals_byte);
            if (!peek.ok()) {
                return peek.status();
            }
            if (peek.value().tag != group_def.delimiter_tag) {
                SetValidationIssue(
                    validation_issue,
                    ValidationIssueKind::kIncorrectNumInGroupCount,
                    group_def.count_tag,
                    "group " + std::to_string(group_def.count_tag) + " expected delimiter tag " +
                        std::to_string(group_def.delimiter_tag) + " for entry " +
                        std::to_string(entry_index + 1U));
                return byte_pos;
            }
        }

        const auto parsed_entry_index = AppendParsedEntry(parsed, frame_index);
        const auto entry_ref = EntryContainer(parsed_entry_index);
        bool seen_any_field = false;

        // Defer field slot appends so that nested group processing does not
        // interleave child-entry slots between this entry's slots.  After
        // the loop, all deferred slots are batch-appended, guaranteeing the
        // contiguous layout that FindParsedFieldIndex relies on.
        static constexpr std::size_t kMaxDeferredSlots = 128U;
        std::array<message::ParsedFieldSlot, kMaxDeferredSlots> deferred_slots{};
        std::size_t deferred_count = 0U;

        while (byte_pos < bytes.size()) {
            auto field = ScanNextField(bytes, byte_pos, delimiter_byte, equals_byte);
            if (!field.ok()) {
                return field.status();
            }
            const auto tag = field.value().tag;
            if (seen_any_field && tag == group_def.delimiter_tag) {
                break;
            }

            if (const auto ri = dictionary.group_rule_index(group_def, tag); ri >= 0) {
                ValidateConsumedField(
                    dictionary,
                    tag,
                    group_rules,
                    ri,
                    &validation_state,
                    validation_issue,
                    false,
                    true,
                    "group");

                auto slot = MakeParsedFieldSlot(
                    tag,
                    static_cast<std::uint32_t>(field.value().value_offset),
                    field.value().value_length,
                    dictionary);
                if (deferred_count < kMaxDeferredSlots) {
                    deferred_slots[deferred_count++] = slot;
                }

                if (const auto* nested_group = dictionary.find_group(tag); nested_group != nullptr) {
                    auto value_sv = std::string_view(
                        reinterpret_cast<const char*>(bytes.data() + field.value().value_offset),
                        field.value().value_length);
                    auto next_pos = ParseParsedGroupEntries(
                        dictionary,
                        bytes,
                        field.value().next_pos,
                        delimiter_byte,
                        equals_byte,
                        value_sv,
                        *nested_group,
                        parsed,
                        entry_ref,
                        static_cast<std::uint16_t>(depth + 1U),
                        validation_issue);
                    if (!next_pos.ok()) {
                        return next_pos.status();
                    }
                    byte_pos = next_pos.value();
                    seen_any_field = true;
                    continue;
                }

                byte_pos = field.value().next_pos;
                seen_any_field = true;
                continue;
            }

            break;
        }

        // Batch-append all deferred field slots so they are contiguous.
        for (std::size_t i = 0; i < deferred_count; ++i) {
            AppendParsedFieldSlot(parsed, entry_ref, deferred_slots[i]);
        }
    }

    return byte_pos;
}

auto ShouldSkipField(std::uint32_t tag) -> bool {
    return tag == 8U || tag == 9U || tag == 10U;
}

auto IsTemplateManagedHeaderField(std::uint32_t tag) -> bool {
    switch (tag) {
        case 34U:
        case 35U:
        case 43U:
        case 49U:
        case 52U:
        case 56U:
            return true;
        default:
            return false;
    }
}

auto ContainsTag(const std::vector<std::uint32_t>& tags, std::uint32_t tag) -> bool {
    return std::find(tags.begin(), tags.end(), tag) != tags.end();
}

auto MakeTagPrefix(std::uint32_t tag) -> std::string {
    std::string prefix = std::to_string(tag);
    prefix.push_back('=');
    return prefix;
}

auto MakeFixedFieldFragment(std::uint32_t tag, std::string_view value, char delimiter) -> std::string {
    std::string fragment = MakeTagPrefix(tag);
    fragment.append(value);
    fragment.push_back(delimiter);
    return fragment;
}

auto AccumulateChecksum(std::string_view text, std::uint32_t* checksum) -> void {
    if (checksum == nullptr) {
        return;
    }
    for (const auto ch : text) {
        *checksum += static_cast<unsigned char>(ch);
    }
}

auto AccumulateAppendedRange(const std::string& out, std::size_t start_offset, std::uint32_t* checksum) -> void {
    if (checksum == nullptr || start_offset >= out.size()) {
        return;
    }
    AccumulateChecksum(std::string_view(out.data() + start_offset, out.size() - start_offset), checksum);
}

auto AppendTracked(std::string& out, std::uint32_t* checksum, std::string_view text) -> void {
    out.append(text);
    AccumulateChecksum(text, checksum);
}

auto AppendTracked(std::string& out, std::uint32_t* checksum, char value) -> void {
    out.push_back(value);
    if (checksum != nullptr) {
        *checksum += static_cast<unsigned char>(value);
    }
}

template <typename Integer>
auto AppendIntegerDigits(std::string& out, std::uint32_t* checksum, Integer value) -> void {
    std::array<char, kIntegerTextBufferBytes<Integer>> buffer{};
    const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (ec != std::errc()) {
        return;
    }
    AppendTracked(out, checksum, std::string_view(buffer.data(), static_cast<std::size_t>(ptr - buffer.data())));
}

auto AppendPrefixedStringField(
    std::string& out,
    std::uint32_t* checksum,
    std::string_view prefix,
    std::string_view value,
    char delimiter) -> void {
    AppendTracked(out, checksum, prefix);
    AppendTracked(out, checksum, value);
    AppendTracked(out, checksum, delimiter);
}

auto AppendPrefixedCountField(
    std::string& out,
    std::uint32_t* checksum,
    std::string_view prefix,
    std::uint32_t value,
    char delimiter) -> void {
    AppendTracked(out, checksum, prefix);
    AppendIntegerDigits(out, checksum, value);
    AppendTracked(out, checksum, delimiter);
}

auto AppendField(std::string& out, std::uint32_t tag, std::string_view value, char delimiter) -> void {
    std::array<char, kIntegerTextBufferBytes<std::uint32_t>> tag_buf{};
    const auto [tag_ptr, tag_ec] = std::to_chars(tag_buf.data(), tag_buf.data() + tag_buf.size(), tag);
    if (tag_ec != std::errc()) {
        return;
    }
    out.append(tag_buf.data(), static_cast<std::size_t>(tag_ptr - tag_buf.data()));
    out.push_back('=');
    out.append(value);
    out.push_back(delimiter);
}

auto AppendField(std::string& out, std::uint32_t tag, std::int64_t value, char delimiter) -> void {
    std::array<char, kIntegerTextBufferBytes<std::int64_t>> val_buf{};
    const auto [val_ptr, val_ec] = std::to_chars(val_buf.data(), val_buf.data() + val_buf.size(), value);
    if (val_ec != std::errc()) {
        return;
    }
    AppendField(out, tag, std::string_view(val_buf.data(), static_cast<std::size_t>(val_ptr - val_buf.data())), delimiter);
}

auto AppendField(std::string& out, std::uint32_t tag, double value, char delimiter) -> void {
    std::array<char, 32> buffer{};
    const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::general, 12);
    if (ec != std::errc()) {
        return;
    }
    AppendField(out, tag, std::string_view(buffer.data(), static_cast<std::size_t>(ptr - buffer.data())), delimiter);
}

auto AppendField(std::string& out, std::uint32_t tag, bool value, char delimiter) -> void {
    AppendField(out, tag, std::string_view(value ? "Y" : "N"), delimiter);
}

auto AppendPrefixedFieldValue(
    std::string& out,
    std::uint32_t* checksum,
    std::string_view prefix,
    const message::FieldView& field,
    char delimiter) -> void {
    AppendTracked(out, checksum, prefix);
    switch (field.type) {
        case message::FieldValueType::kInt:
            AppendIntegerDigits(out, checksum, field.int_value);
            break;
        case message::FieldValueType::kChar:
            AppendTracked(out, checksum, field.char_value);
            break;
        case message::FieldValueType::kFloat: {
            std::array<char, 32> float_buf{};
            const auto [fptr, fec] = std::to_chars(float_buf.data(), float_buf.data() + float_buf.size(), field.float_value, std::chars_format::general, 12);
            if (fec == std::errc()) {
                AppendTracked(out, checksum, std::string_view(float_buf.data(), static_cast<std::size_t>(fptr - float_buf.data())));
            }
            break;
        }
        case message::FieldValueType::kBoolean:
            AppendTracked(out, checksum, std::string_view(field.bool_value ? "Y" : "N"));
            break;
        default:
            AppendTracked(out, checksum, field.string_value);
            break;
    }
    AppendTracked(out, checksum, delimiter);
}

auto EncodeFieldValue(std::string& out, const message::FieldValue& field, char delimiter) -> void {
    switch (field.type) {
        case message::FieldValueType::kInt:
            return AppendField(out, field.tag, field.int_value, delimiter);
        case message::FieldValueType::kChar:
            return AppendField(out, field.tag, std::string_view(&field.char_value, 1U), delimiter);
        case message::FieldValueType::kFloat:
            return AppendField(out, field.tag, field.float_value, delimiter);
        case message::FieldValueType::kBoolean:
            return AppendField(out, field.tag, field.bool_value, delimiter);
        default:
            return AppendField(out, field.tag, field.string_value, delimiter);
    }
}

auto EncodeFieldValue(std::string& out, const message::FieldView& field, char delimiter) -> void {
    switch (field.type) {
        case message::FieldValueType::kInt:
            return AppendField(out, field.tag, field.int_value, delimiter);
        case message::FieldValueType::kChar:
            return AppendField(out, field.tag, std::string_view(&field.char_value, 1U), delimiter);
        case message::FieldValueType::kFloat:
            return AppendField(out, field.tag, field.float_value, delimiter);
        case message::FieldValueType::kBoolean:
            return AppendField(out, field.tag, field.bool_value, delimiter);
        default:
            return AppendField(out, field.tag, field.string_value, delimiter);
    }
}

auto EncodeGroups(std::string& out, const std::vector<message::GroupData>& groups, char delimiter) -> void;
auto EncodeMessageBody(
    std::string& out,
    const message::MessageData& data,
    char delimiter,
    bool skip_standard_header) -> void;

auto EncodeGroupData(std::string& out, const message::GroupData& group, char delimiter) -> void {
    AppendField(out, group.count_tag, static_cast<std::int64_t>(group.entries.size()), delimiter);
    for (const auto& entry : group.entries) {
        EncodeMessageBody(out, entry, delimiter, false);
    }
}

auto AppendTrackedGenericField(
    std::string& out,
    std::uint32_t* checksum,
    const message::FieldView& field,
    char delimiter) -> void {
    const auto start_offset = out.size();
    EncodeFieldValue(out, field, delimiter);
    AccumulateAppendedRange(out, start_offset, checksum);
}

auto EncodeGroupData(std::string& out, message::GroupView group, char delimiter) -> void;
auto EncodeMessageBody(
    std::string& out,
    message::MessageView view,
    char delimiter,
    bool skip_standard_header) -> void;

auto AppendTrackedGenericGroup(
    std::string& out,
    std::uint32_t* checksum,
    message::GroupView group,
    char delimiter) -> void {
    const auto start_offset = out.size();
    EncodeGroupData(out, group, delimiter);
    AccumulateAppendedRange(out, start_offset, checksum);
}

auto HasGroupCountTag(const std::vector<message::GroupData>& groups, std::uint32_t tag) -> bool {
    return std::any_of(groups.begin(), groups.end(), [&](const auto& group) { return group.count_tag == tag; });
}

auto HasGroupCountTag(message::MessageView view, std::uint32_t tag) -> bool {
    for (std::size_t index = 0; index < view.group_count(); ++index) {
        const auto group = view.group_at(index);
        if (group.has_value() && group->count_tag() == tag) {
            return true;
        }
    }
    return false;
}

auto EncodeMessageBody(
    std::string& out,
    const message::MessageData& data,
    char delimiter,
    bool skip_standard_header) -> void {
    for (const auto& field : data.fields) {
        if (field.tag == 8U || field.tag == 9U || field.tag == 10U) {
            continue;
        }
        if (HasGroupCountTag(data.groups, field.tag)) {
            continue;
        }
        if (skip_standard_header && (field.tag == 35U || field.tag == 34U || field.tag == 49U || field.tag == 56U ||
                                     field.tag == 52U || field.tag == 43U)) {
            continue;
        }
        EncodeFieldValue(out, field, delimiter);
    }
    EncodeGroups(out, data.groups, delimiter);
}

auto EncodeGroups(std::string& out, const std::vector<message::GroupData>& groups, char delimiter) -> void {
    for (const auto& group : groups) {
        EncodeGroupData(out, group, delimiter);
    }
}

auto EncodeMessageBody(
    std::string& out,
    message::MessageView view,
    char delimiter,
    bool skip_standard_header) -> void {
    for (std::size_t index = 0; index < view.field_count(); ++index) {
        const auto field = view.field_at(index);
        if (!field.has_value()) {
            continue;
        }
        if (field->tag == 8U || field->tag == 9U || field->tag == 10U) {
            continue;
        }
        if (HasGroupCountTag(view, field->tag)) {
            continue;
        }
        if (skip_standard_header && (field->tag == 35U || field->tag == 34U || field->tag == 49U || field->tag == 56U ||
                                     field->tag == 52U || field->tag == 43U)) {
            continue;
        }
        EncodeFieldValue(out, *field, delimiter);
    }
    for (std::size_t index = 0; index < view.group_count(); ++index) {
        const auto group = view.group_at(index);
        if (group.has_value()) {
            EncodeGroupData(out, *group, delimiter);
        }
    }
}

auto EncodeGroupData(std::string& out, message::GroupView group, char delimiter) -> void {
    AppendField(out, group.count_tag(), static_cast<std::int64_t>(group.size()), delimiter);
    for (const auto entry : group) {
        EncodeMessageBody(out, entry, delimiter, false);
    }
}

auto NormalizeDelimiter(char delimiter) -> char {
    return delimiter == '\0' ? kFixSoh : delimiter;
}

auto CompileScopeTemplate(
    const profile::NormalizedDictionaryView& dictionary,
    std::span<const profile::FieldRuleRecord> rules,
    bool skip_managed_header_fields) -> std::shared_ptr<const CompiledScopeTemplate> {
    auto scope = std::make_shared<CompiledScopeTemplate>();
    for (const auto& rule : rules) {
        if (skip_managed_header_fields && IsTemplateManagedHeaderField(rule.tag)) {
            continue;
        }

        CompiledScopeStep step;
        step.tag = rule.tag;
        step.prefix = MakeTagPrefix(rule.tag);
        if (const auto* group_def = dictionary.find_group(rule.tag); group_def != nullptr) {
            step.kind = CompiledScopeStep::Kind::kGroup;
            step.group_scope = CompileScopeTemplate(dictionary, dictionary.group_field_rules(*group_def), false);
            scope->group_tags.push_back(rule.tag);
        } else {
            step.kind = CompiledScopeStep::Kind::kScalar;
            scope->scalar_tags.push_back(rule.tag);
        }
        scope->steps.push_back(std::move(step));
    }
    return scope;
}

auto EncodeScopeTemplate(
    std::string& out,
    std::uint32_t* checksum,
    message::MessageView view,
    const CompiledScopeTemplate& scope,
    char delimiter,
    bool skip_standard_header) -> void {
    for (const auto& step : scope.steps) {
        if (step.kind == CompiledScopeStep::Kind::kScalar) {
            const auto field = view.find_field_view(step.tag);
            if (!field.has_value()) {
                continue;
            }
            if (HasGroupCountTag(view, field->tag)) {
                continue;
            }
            AppendPrefixedFieldValue(out, checksum, step.prefix, *field, delimiter);
            continue;
        }

        const auto group = view.group(step.tag);
        if (!group.has_value()) {
            continue;
        }

        AppendPrefixedCountField(
            out,
            checksum,
            step.prefix,
            static_cast<std::uint32_t>(group->size()),
            delimiter);
        for (const auto entry : *group) {
            EncodeScopeTemplate(out, checksum, entry, *step.group_scope, delimiter, false);
        }
    }

    for (std::size_t index = 0; index < view.field_count(); ++index) {
        const auto field = view.field_at(index);
        if (!field.has_value()) {
            continue;
        }
        if (ShouldSkipField(field->tag)) {
            continue;
        }
        if (HasGroupCountTag(view, field->tag)) {
            continue;
        }
        if (ContainsTag(scope.scalar_tags, field->tag) || ContainsTag(scope.group_tags, field->tag)) {
            continue;
        }
        if (skip_standard_header && IsTemplateManagedHeaderField(field->tag)) {
            continue;
        }
        AppendTrackedGenericField(out, checksum, *field, delimiter);
    }

    for (std::size_t index = 0; index < view.group_count(); ++index) {
        const auto group = view.group_at(index);
        if (!group.has_value()) {
            continue;
        }
        if (ContainsTag(scope.group_tags, group->count_tag())) {
            continue;
        }
        AppendTrackedGenericGroup(out, checksum, *group, delimiter);
    }
}

auto ReplaceUnsignedPlaceholder(
    std::string& out,
    std::size_t offset,
    std::size_t placeholder_width,
    std::uint32_t value,
    std::uint32_t* checksum) -> void {
    std::array<char, kIntegerTextBufferBytes<std::uint32_t>> buffer{};
    const auto [ptr, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    if (ec != std::errc()) {
        return;
    }

    const auto original = std::string_view(out.data() + offset, placeholder_width);
    if (checksum != nullptr) {
        std::uint32_t original_sum = 0;
        for (const auto ch : original) {
            original_sum += static_cast<unsigned char>(ch);
        }
        *checksum -= original_sum;
        AccumulateChecksum(std::string_view(buffer.data(), static_cast<std::size_t>(ptr - buffer.data())), checksum);
    }

    out.replace(offset, placeholder_width, buffer.data(), static_cast<std::size_t>(ptr - buffer.data()));
}

auto CopyTextToBytes(std::string_view text) -> std::vector<std::byte> {
    std::vector<std::byte> bytes(text.size());
    if (!text.empty()) {
        std::memcpy(bytes.data(), text.data(), text.size());
    }
    return bytes;
}

auto WriteTwoDigits(char* out, int value) -> void {
    out[0] = static_cast<char>('0' + ((value / 10) % 10));
    out[1] = static_cast<char>('0' + (value % 10));
}

auto WriteThreeDigits(char* out, int value) -> void {
    out[0] = static_cast<char>('0' + ((value / 100) % 10));
    out[1] = static_cast<char>('0' + ((value / 10) % 10));
    out[2] = static_cast<char>('0' + (value % 10));
}

auto WriteFourDigits(char* out, int value) -> void {
    out[0] = static_cast<char>('0' + ((value / 1000) % 10));
    out[1] = static_cast<char>('0' + ((value / 100) % 10));
    out[2] = static_cast<char>('0' + ((value / 10) % 10));
    out[3] = static_cast<char>('0' + (value % 10));
}

auto EncodeFixMessageGenericToBuffer(
    message::MessageView message,
    const EncodeOptions& options,
    EncodeBuffer* buffer) -> base::Status {
    if (buffer == nullptr) {
        return base::Status::InvalidArgument("encode buffer is null");
    }

    const char delimiter = NormalizeDelimiter(options.delimiter);
    buffer->storage.clear();
    auto& full = buffer->storage;

    // BeginString + BodyLength placeholder
    const auto begin_string = options.begin_string.empty() ? std::string_view("FIX.4.4") : std::string_view(options.begin_string);
    full.append("8=");
    full.append(begin_string);
    full.push_back(delimiter);
    full.append("9=");

    constexpr std::size_t kBodyLengthPlaceholderWidth = 7U;
    const auto body_length_offset = full.size();
    full.append(kBodyLengthPlaceholderWidth, '0');
    full.push_back(delimiter);
    const auto body_start = full.size();

    // Body content directly into buffer (untracked — checksum computed at end)
    const auto msg_type = message.msg_type();
    AppendField(full, 35U, msg_type.empty() ? std::string_view("UNKNOWN") : msg_type, delimiter);

    const auto seq_num = options.msg_seq_num == 0U ? 1U : options.msg_seq_num;
    AppendField(full, 34U, static_cast<std::int64_t>(seq_num), delimiter);
    if (!options.sender_comp_id.empty()) {
        AppendField(full, 49U, options.sender_comp_id, delimiter);
    }
    if (!options.target_comp_id.empty()) {
        AppendField(full, 56U, options.target_comp_id, delimiter);
    }
    UtcTimestampBuffer timestamp_buffer;
    const auto sending_time = options.sending_time.empty() ? CurrentUtcTimestamp(&timestamp_buffer) : options.sending_time;
    AppendField(full, 52U, sending_time, delimiter);
    if (msg_type == "A" && !options.default_appl_ver_id.empty()) {
        AppendField(full, 1137U, options.default_appl_ver_id, delimiter);
    }
    if (options.poss_dup) {
        AppendField(full, 43U, std::string_view("Y"), delimiter);
    }
    if (!options.orig_sending_time.empty()) {
        AppendField(full, 122U, options.orig_sending_time, delimiter);
    }

    EncodeMessageBody(full, message, delimiter, true);

    // Backfill body length
    const auto body_length = static_cast<std::uint32_t>(full.size() - body_start);
    {
        std::array<char, kIntegerTextBufferBytes<std::uint32_t>> bl_buf{};
        const auto [ptr, ec] = std::to_chars(bl_buf.data(), bl_buf.data() + bl_buf.size(), body_length);
        if (ec != std::errc()) {
            return base::Status::FormatError("BodyLength conversion failed");
        }
        const auto digits = static_cast<std::size_t>(ptr - bl_buf.data());
        if (digits > kBodyLengthPlaceholderWidth) {
            return base::Status::FormatError(
                "encoded body length exceeds BodyLength placeholder width");
        }
        full.replace(body_length_offset, kBodyLengthPlaceholderWidth,
                     bl_buf.data(), digits);
    }

    // Compute checksum over entire buffer so far (same approach as post-hoc scan)
    std::uint32_t checksum = 0;
    for (const auto ch : full) {
        checksum += static_cast<unsigned char>(ch);
    }
    checksum %= 256U;

    // Append 10=XXX<SOH> without ostringstream
    std::array<char, 3> cksum_digits{};
    cksum_digits[0] = static_cast<char>('0' + ((checksum / 100U) % 10U));
    cksum_digits[1] = static_cast<char>('0' + ((checksum / 10U) % 10U));
    cksum_digits[2] = static_cast<char>('0' + (checksum % 10U));
    AppendField(full, 10U, std::string_view(cksum_digits.data(), 3U), delimiter);
    return base::Status::Ok();
}

}  // namespace

struct FrameEncodeTemplate::State {
    std::string msg_type;
    std::string begin_string;
    std::string sender_comp_id;
    std::string target_comp_id;
    std::string default_appl_ver_id;
    char delimiter{kFixSoh};
    std::string begin_prefix;
    std::string msg_type_fragment;
    std::string msg_seq_prefix;
    std::string sender_fragment;
    std::string target_fragment;
    std::string sending_time_prefix;
    std::string default_appl_ver_fragment;
    std::string poss_dup_fragment;
    std::string orig_sending_time_prefix;
    std::shared_ptr<const CompiledScopeTemplate> body_scope;
};

auto CurrentUtcTimestamp(UtcTimestampBuffer* buffer) -> std::string_view {
    if (buffer == nullptr) {
        return {};
    }

    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
    const auto millis = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now - seconds).count());

    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&time, &tm);

    auto* out = buffer->storage.data();
    WriteFourDigits(out, tm.tm_year + 1900);
    WriteTwoDigits(out + 4, tm.tm_mon + 1);
    WriteTwoDigits(out + 6, tm.tm_mday);
    out[8] = '-';
    WriteTwoDigits(out + 9, tm.tm_hour);
    out[11] = ':';
    WriteTwoDigits(out + 12, tm.tm_min);
    out[14] = ':';
    WriteTwoDigits(out + 15, tm.tm_sec);
    out[17] = '.';
    WriteThreeDigits(out + 18, millis);
    return buffer->view();
}

auto CurrentUtcTimestamp() -> std::string {
    UtcTimestampBuffer buffer;
    return std::string(CurrentUtcTimestamp(&buffer));
}

auto FrameEncodeTemplate::msg_type() const -> std::string_view {
    return state_ == nullptr ? std::string_view{} : std::string_view(state_->msg_type);
}

auto CompileFrameEncodeTemplate(
    const profile::NormalizedDictionaryView& dictionary,
    std::string_view msg_type,
    const EncodeTemplateConfig& config) -> base::Result<FrameEncodeTemplate> {
    if (msg_type.empty()) {
        return base::Status::InvalidArgument("cannot compile a frame template without MsgType");
    }

    const auto* message_def = dictionary.find_message(msg_type);
    if (message_def == nullptr) {
        return base::Status::NotFound("message type is not present in the bound dictionary");
    }

    const char delimiter = NormalizeDelimiter(config.delimiter);
    auto state = std::make_shared<FrameEncodeTemplate::State>();
    state->msg_type = std::string(msg_type);
    state->begin_string = config.begin_string.empty() ? std::string("FIX.4.4") : config.begin_string;
    state->sender_comp_id = config.sender_comp_id;
    state->target_comp_id = config.target_comp_id;
    state->default_appl_ver_id = config.default_appl_ver_id;
    state->delimiter = delimiter;
    state->begin_prefix = MakeFixedFieldFragment(8U, state->begin_string, delimiter);
    state->begin_prefix.append("9=");
    state->msg_type_fragment = MakeFixedFieldFragment(35U, state->msg_type, delimiter);
    state->msg_seq_prefix = MakeTagPrefix(34U);
    state->sending_time_prefix = MakeTagPrefix(52U);
    state->orig_sending_time_prefix = MakeTagPrefix(122U);
    state->poss_dup_fragment = MakeFixedFieldFragment(43U, "Y", delimiter);
    if (!state->sender_comp_id.empty()) {
        state->sender_fragment = MakeFixedFieldFragment(49U, state->sender_comp_id, delimiter);
    }
    if (!state->target_comp_id.empty()) {
        state->target_fragment = MakeFixedFieldFragment(56U, state->target_comp_id, delimiter);
    }
    if (state->msg_type == "A" && !state->default_appl_ver_id.empty()) {
        state->default_appl_ver_fragment = MakeFixedFieldFragment(1137U, state->default_appl_ver_id, delimiter);
    }
    state->body_scope = CompileScopeTemplate(dictionary, dictionary.message_field_rules(*message_def), true);
    return FrameEncodeTemplate(std::move(state));
}

auto FrameEncodeTemplate::Encode(
    const message::Message& message,
    const EncodeOptions& options) const -> base::Result<std::vector<std::byte>> {
    return Encode(message.view(), options);
}

auto FrameEncodeTemplate::Encode(
    message::MessageView message,
    const EncodeOptions& options) const -> base::Result<std::vector<std::byte>> {
    EncodeBuffer buffer;
    auto status = EncodeToBuffer(message, options, &buffer);
    if (!status.ok()) {
        return status;
    }
    return CopyTextToBytes(buffer.text());
}

auto FrameEncodeTemplate::EncodeToBuffer(
    const message::Message& message,
    const EncodeOptions& options,
    EncodeBuffer* buffer) const -> base::Status {
    return EncodeToBuffer(message.view(), options, buffer);
}

auto FrameEncodeTemplate::EncodeToBuffer(
    message::MessageView message,
    const EncodeOptions& options,
    EncodeBuffer* buffer) const -> base::Status {
    if (state_ == nullptr) {
        return base::Status::InvalidArgument("frame template is not initialized");
    }
    if (buffer == nullptr) {
        return base::Status::InvalidArgument("encode buffer is null");
    }
    if (message.valid() && !message.msg_type().empty() && message.msg_type() != state_->msg_type) {
        return base::Status::InvalidArgument("message type does not match compiled frame template");
    }

    const char delimiter = NormalizeDelimiter(options.delimiter);
    if (delimiter != state_->delimiter) {
        return base::Status::InvalidArgument("delimiter does not match compiled frame template");
    }
    if (!options.begin_string.empty() && options.begin_string != state_->begin_string) {
        return base::Status::InvalidArgument("BeginString does not match compiled frame template");
    }
    if (!options.sender_comp_id.empty() && options.sender_comp_id != state_->sender_comp_id) {
        return base::Status::InvalidArgument("SenderCompID does not match compiled frame template");
    }
    if (!options.target_comp_id.empty() && options.target_comp_id != state_->target_comp_id) {
        return base::Status::InvalidArgument("TargetCompID does not match compiled frame template");
    }
    if (!options.default_appl_ver_id.empty() && options.default_appl_ver_id != state_->default_appl_ver_id) {
        return base::Status::InvalidArgument("DefaultApplVerID does not match compiled frame template");
    }

    buffer->storage.clear();
    auto& full = buffer->storage;
    std::uint32_t checksum = 0;
    AppendTracked(full, &checksum, state_->begin_prefix);

    constexpr std::size_t kBodyLengthPlaceholderWidth = 7U;
    const auto body_length_offset = full.size();
    AppendTracked(full, &checksum, std::string_view("0000000000", kBodyLengthPlaceholderWidth));
    AppendTracked(full, &checksum, delimiter);
    const auto body_start = full.size();

    AppendTracked(full, &checksum, state_->msg_type_fragment);

    const auto seq_num = options.msg_seq_num == 0U ? 1U : options.msg_seq_num;
    AppendPrefixedCountField(full, &checksum, state_->msg_seq_prefix, seq_num, delimiter);
    if (!state_->sender_fragment.empty()) {
        AppendTracked(full, &checksum, state_->sender_fragment);
    }
    if (!state_->target_fragment.empty()) {
        AppendTracked(full, &checksum, state_->target_fragment);
    }

    UtcTimestampBuffer timestamp_buffer;
    const auto sending_time = options.sending_time.empty() ? CurrentUtcTimestamp(&timestamp_buffer) : options.sending_time;
    AppendPrefixedStringField(full, &checksum, state_->sending_time_prefix, sending_time, delimiter);
    if (!state_->default_appl_ver_fragment.empty()) {
        AppendTracked(full, &checksum, state_->default_appl_ver_fragment);
    }
    if (options.poss_dup) {
        AppendTracked(full, &checksum, state_->poss_dup_fragment);
    }
    if (!options.orig_sending_time.empty()) {
        AppendPrefixedStringField(
            full,
            &checksum,
            state_->orig_sending_time_prefix,
            options.orig_sending_time,
            delimiter);
    }

    EncodeScopeTemplate(full, &checksum, message, *state_->body_scope, delimiter, true);

    {
        const auto body_length = static_cast<std::uint32_t>(full.size() - body_start);
        std::array<char, kIntegerTextBufferBytes<std::uint32_t>> bl_check{};
        const auto [bl_ptr, bl_ec] = std::to_chars(bl_check.data(), bl_check.data() + bl_check.size(), body_length);
        if (bl_ec != std::errc() ||
            static_cast<std::size_t>(bl_ptr - bl_check.data()) > kBodyLengthPlaceholderWidth) {
            return base::Status::FormatError(
                "encoded body length exceeds BodyLength placeholder width");
        }
        ReplaceUnsignedPlaceholder(
            full,
            body_length_offset,
            kBodyLengthPlaceholderWidth,
            body_length,
            &checksum);
    }

    checksum %= 256U;
    AppendTracked(full, nullptr, std::string_view("10="));
    std::array<char, 4> checksum_digits{};
    checksum_digits[0] = static_cast<char>('0' + ((checksum / 100U) % 10U));
    checksum_digits[1] = static_cast<char>('0' + ((checksum / 10U) % 10U));
    checksum_digits[2] = static_cast<char>('0' + (checksum % 10U));
    AppendTracked(full, nullptr, std::string_view(checksum_digits.data(), 3U));
    AppendTracked(full, nullptr, delimiter);
    return base::Status::Ok();
}

auto EncodeFixMessageToBuffer(
    const message::Message& message,
    const profile::NormalizedDictionaryView& dictionary,
    const EncodeOptions& options,
    EncodeBuffer* buffer) -> base::Status {
    return EncodeFixMessageToBuffer(message.view(), dictionary, options, buffer);
}

auto EncodeFixMessageToBuffer(
    message::MessageView message,
    const profile::NormalizedDictionaryView& dictionary,
    const EncodeOptions& options,
    EncodeBuffer* buffer) -> base::Status {
    return EncodeFixMessageGenericToBuffer(message, options, buffer);
}

auto EncodeFixMessageToBuffer(
    const message::Message& message,
    const profile::NormalizedDictionaryView& dictionary,
    const EncodeOptions& options,
    EncodeBuffer* buffer,
    const PrecompiledTemplateTable* precompiled) -> base::Status {
    return EncodeFixMessageToBuffer(message.view(), dictionary, options, buffer, precompiled);
}

auto EncodeFixMessageToBuffer(
    message::MessageView message,
    const profile::NormalizedDictionaryView& dictionary,
    const EncodeOptions& options,
    EncodeBuffer* buffer,
    const PrecompiledTemplateTable* precompiled) -> base::Status {
    const auto msg_type = message.msg_type();
    if (!msg_type.empty() && precompiled != nullptr) {
        if (const auto* tmpl = precompiled->find(msg_type); tmpl != nullptr) {
            auto status = tmpl->EncodeToBuffer(message, options, buffer);
            if (status.ok()) {
                return status;
            }
        }
    }
    return EncodeFixMessageGenericToBuffer(message, options, buffer);
}

auto PrecompiledTemplateTable::Build(
    const profile::NormalizedDictionaryView& dictionary,
    const EncodeTemplateConfig& config) -> base::Result<PrecompiledTemplateTable> {
    PrecompiledTemplateTable table;
    for (const auto& message_def : dictionary.messages()) {
        auto msg_type = dictionary.message_type(message_def);
        if (!msg_type.has_value() || msg_type->empty()) {
            continue;
        }

        auto compiled = CompileFrameEncodeTemplate(dictionary, *msg_type, config);
        if (!compiled.ok()) {
            continue;
        }

        table.entries_.push_back(Entry{
            .msg_type = std::string(*msg_type),
            .tmpl = std::move(compiled).value(),
        });
    }

    std::sort(table.entries_.begin(), table.entries_.end(),
        [](const auto& a, const auto& b) { return a.msg_type < b.msg_type; });
    return table;
}

auto PrecompiledTemplateTable::find(std::string_view msg_type) const -> const FrameEncodeTemplate* {
    auto it = std::lower_bound(entries_.begin(), entries_.end(), msg_type,
        [](const Entry& entry, std::string_view type) { return entry.msg_type < type; });
    if (it != entries_.end() && it->msg_type == msg_type) {
        return &it->tmpl;
    }
    return nullptr;
}

auto EncodeFixMessage(
    const message::Message& message,
    const profile::NormalizedDictionaryView& dictionary,
    const EncodeOptions& options) -> base::Result<std::vector<std::byte>> {
    return EncodeFixMessage(message.view(), dictionary, options);
}

auto EncodeFixMessage(
    message::MessageView message,
    const profile::NormalizedDictionaryView& dictionary,
    const EncodeOptions& options) -> base::Result<std::vector<std::byte>> {
    EncodeBuffer buffer;
    auto status = EncodeFixMessageToBuffer(message, dictionary, options, &buffer);
    if (!status.ok()) {
        return status;
    }
    return CopyTextToBytes(buffer.text());
}

auto DecodeFixMessageView(
    std::span<const std::byte> bytes,
    const profile::NormalizedDictionaryView& dictionary,
    char delimiter) -> base::Result<DecodedMessageView> {
    const char normalized_delimiter = NormalizeDelimiter(delimiter);
    const auto delimiter_byte = static_cast<std::byte>(static_cast<unsigned char>(normalized_delimiter));
    const auto equals_byte = static_cast<std::byte>('=');

    // Scan field 0: must be tag 8 (BeginString)
    auto field0 = ScanNextField(bytes, 0U, delimiter_byte, equals_byte);
    if (!field0.ok()) {
        return field0.status();
    }
    if (field0.value().tag != 8U) {
        return base::Status::FormatError("FIX frame must begin with 8 and 9 and end with 10");
    }

    // Scan field 1: must be tag 9 (BodyLength)
    auto field1 = ScanNextField(bytes, field0.value().next_pos, delimiter_byte, equals_byte);
    if (!field1.ok()) {
        return field1.status();
    }
    if (field1.value().tag != 9U) {
        return base::Status::FormatError("FIX frame must begin with 8 and 9 and end with 10");
    }

    auto declared_body_length = ParseUnsigned(
        std::string_view(
            reinterpret_cast<const char*>(bytes.data() + field1.value().value_offset),
            field1.value().value_length),
        "BodyLength");
    if (!declared_body_length.ok()) {
        return declared_body_length.status();
    }

    const auto body_start_offset = field1.value().next_pos;

    message::ParsedMessageData parsed_message;
    parsed_message.raw = bytes;
    SessionHeaderView header;
    ValidationIssue validation_issue;
    header.begin_string = std::string_view(
        reinterpret_cast<const char*>(bytes.data() + field0.value().value_offset),
        field0.value().value_length);
    header.body_length = declared_body_length.value();
    ScopeValidationState message_validation;
    message_validation.seen_tags.reserve(bytes.size() / 8U);

    std::size_t byte_pos = field1.value().next_pos;
    std::size_t field_count = 2U;
    std::size_t checksum_field_start = 0U;
    bool saw_checksum = false;

    // Cached across loop iterations — updated once when tag 35 is seen.
    const profile::MessageDefRecord* cached_message_def = nullptr;
    std::span<const profile::FieldRuleRecord> cached_rules{};

    while (byte_pos < bytes.size()) {
        auto scanned = ScanNextField(bytes, byte_pos, delimiter_byte, equals_byte);
        if (!scanned.ok()) {
            return scanned.status();
        }
        ++field_count;
        const auto tag = scanned.value().tag;
        const auto value_sv = std::string_view(
            reinterpret_cast<const char*>(bytes.data() + scanned.value().value_offset),
            scanned.value().value_length);

        if (tag == 10U) {
            auto expected_checksum = ParseUnsigned(value_sv, "CheckSum");
            if (!expected_checksum.ok()) {
                return expected_checksum.status();
            }
            header.checksum = expected_checksum.value();
            checksum_field_start = scanned.value().start_offset;
            saw_checksum = true;
            byte_pos = scanned.value().next_pos;
            break;
        }

        if (ShouldSkipField(tag)) {
            byte_pos = scanned.value().next_pos;
            continue;
        }

        if (tag == 35U) {
            parsed_message.msg_type = value_sv;
            header.msg_type = value_sv;
            cached_message_def = dictionary.find_message(header.msg_type);
            cached_rules = cached_message_def != nullptr
                               ? dictionary.message_field_rules(*cached_message_def)
                               : std::span<const profile::FieldRuleRecord>{};
            ValidateConsumedField(
                dictionary,
                tag,
                cached_rules,
                cached_message_def != nullptr ? dictionary.message_rule_index(*cached_message_def, tag) : -1,
                &message_validation,
                &validation_issue,
                true,
                cached_message_def != nullptr,
                "message");
            auto slot = MakeParsedFieldSlot(
                tag,
                static_cast<std::uint32_t>(scanned.value().value_offset),
                scanned.value().value_length,
                dictionary);
            AppendParsedFieldSlot(&parsed_message, RootContainer(), slot);
            byte_pos = scanned.value().next_pos;
            continue;
        }
        if (tag == 34U) {
            auto parsed = ParseUnsigned(value_sv, "MsgSeqNum");
            if (!parsed.ok()) {
                return parsed.status();
            }
            header.msg_seq_num = parsed.value();
        } else if (tag == 49U) {
            header.sender_comp_id = value_sv;
        } else if (tag == 56U) {
            header.target_comp_id = value_sv;
        } else if (tag == 52U) {
            header.sending_time = value_sv;
        } else if (tag == 122U) {
            header.orig_sending_time = value_sv;
        } else if (tag == 1137U) {
            header.default_appl_ver_id = value_sv;
        } else if (tag == 43U) {
            auto parsed = ParseBoolean(value_sv, "PossDupFlag");
            if (!parsed.ok()) {
                return parsed.status();
            }
            header.poss_dup = parsed.value();
        }

        // Single find_field per field — used for both validation and type resolution.
        const auto* field_def = dictionary.find_field(tag);

        ValidateConsumedField(
            dictionary,
            tag,
            cached_rules,
            cached_message_def != nullptr ? dictionary.message_rule_index(*cached_message_def, tag) : -1,
            &message_validation,
            &validation_issue,
            true,
            cached_message_def != nullptr,
            "message",
            field_def);

        const auto* group_def = (cached_message_def != nullptr &&
            dictionary.message_rule_allows_tag(*cached_message_def, tag))
            ? dictionary.find_group(tag) : nullptr;
        if (group_def != nullptr) {
            auto slot = MakeParsedFieldSlotWithType(
                tag,
                static_cast<std::uint32_t>(scanned.value().value_offset),
                scanned.value().value_length,
                ResolveFieldTypeWithDef(tag, field_def));
            AppendParsedFieldSlot(&parsed_message, RootContainer(), slot);
            auto next_pos = ParseParsedGroupEntries(
                dictionary,
                bytes,
                scanned.value().next_pos,
                delimiter_byte,
                equals_byte,
                value_sv,
                *group_def,
                &parsed_message,
                RootContainer(),
                1U,
                &validation_issue);
            if (!next_pos.ok()) {
                return next_pos.status();
            }
            byte_pos = next_pos.value();
            continue;
        }

        auto slot = MakeParsedFieldSlotWithType(
            tag,
            static_cast<std::uint32_t>(scanned.value().value_offset),
            scanned.value().value_length,
            ResolveFieldTypeWithDef(tag, field_def));
        AppendParsedFieldSlot(&parsed_message, RootContainer(), slot);
        byte_pos = scanned.value().next_pos;
    }

    if (byte_pos != bytes.size()) {
        return base::Status::FormatError("FIX frame has trailing data after CheckSum");
    }
    if (field_count < 4U) {
        return base::Status::FormatError("FIX frame is too short");
    }
    if (!saw_checksum) {
        return base::Status::FormatError("FIX frame must begin with 8 and 9 and end with 10");
    }

    const auto actual_body_length = checksum_field_start - body_start_offset;
    if (declared_body_length.value() != actual_body_length) {
        return base::Status::FormatError("BodyLength mismatch");
    }

    const auto actual_checksum = [&]() -> std::uint32_t {
        const auto* data = bytes.data();
        std::uint32_t sum = 0;
#if FASTFIX_HAS_SSE2
        const auto zero = _mm_setzero_si128();
        std::size_t i = 0;
        for (; i + 16 <= checksum_field_start; i += 16) {
            auto chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i));
            auto sad = _mm_sad_epu8(chunk, zero);
            sum += static_cast<std::uint32_t>(_mm_extract_epi16(sad, 0)) +
                   static_cast<std::uint32_t>(_mm_extract_epi16(sad, 4));
        }
        for (; i < checksum_field_start; ++i) {
            sum += std::to_integer<unsigned char>(data[i]);
        }
#else
        for (std::size_t i = 0; i < checksum_field_start; ++i) {
            sum += std::to_integer<unsigned char>(data[i]);
        }
#endif
        return sum % 256U;
    }();
    if (actual_checksum != header.checksum) {
        return base::Status::FormatError("CheckSum mismatch");
    }

    DecodedMessageView decoded;
    decoded.message = message::ParsedMessage(std::move(parsed_message));
    decoded.header = std::move(header);
    decoded.raw = bytes;
    decoded.validation_issue = std::move(validation_issue);
    return decoded;
}

auto DecodeFixMessageView(
    std::span<const std::byte> bytes,
    const profile::NormalizedDictionaryView& dictionary,
    const CompiledDecoderTable& compiled_decoders,
    char delimiter) -> base::Result<DecodedMessageView> {
    const char normalized_delimiter = NormalizeDelimiter(delimiter);
    const auto delimiter_byte = static_cast<std::byte>(static_cast<unsigned char>(normalized_delimiter));
    const auto equals_byte = static_cast<std::byte>('=');

    // Scan field 0: must be tag 8 (BeginString)
    auto field0 = ScanNextField(bytes, 0U, delimiter_byte, equals_byte);
    if (!field0.ok()) {
        return field0.status();
    }
    if (field0.value().tag != 8U) {
        return base::Status::FormatError("FIX frame must begin with 8 and 9 and end with 10");
    }

    // Scan field 1: must be tag 9 (BodyLength)
    auto field1 = ScanNextField(bytes, field0.value().next_pos, delimiter_byte, equals_byte);
    if (!field1.ok()) {
        return field1.status();
    }
    if (field1.value().tag != 9U) {
        return base::Status::FormatError("FIX frame must begin with 8 and 9 and end with 10");
    }

    auto declared_body_length = ParseUnsigned(
        std::string_view(
            reinterpret_cast<const char*>(bytes.data() + field1.value().value_offset),
            field1.value().value_length),
        "BodyLength");
    if (!declared_body_length.ok()) {
        return declared_body_length.status();
    }

    const auto body_start_offset = field1.value().next_pos;

    message::ParsedMessageData parsed_message;
    parsed_message.raw = bytes;
    SessionHeaderView header;
    ValidationIssue validation_issue;
    header.begin_string = std::string_view(
        reinterpret_cast<const char*>(bytes.data() + field0.value().value_offset),
        field0.value().value_length);
    header.body_length = declared_body_length.value();

    std::size_t byte_pos = field1.value().next_pos;
    std::size_t field_count = 2U;
    std::size_t checksum_field_start = 0U;
    bool saw_checksum = false;

    // The compiled decoder for the current message type, set when tag 35 is seen.
    const CompiledMessageDecoder* compiled = nullptr;

    while (byte_pos < bytes.size()) {
        auto scanned = ScanNextField(bytes, byte_pos, delimiter_byte, equals_byte);
        if (!scanned.ok()) {
            return scanned.status();
        }
        ++field_count;
        const auto tag = scanned.value().tag;
        const auto value_sv = std::string_view(
            reinterpret_cast<const char*>(bytes.data() + scanned.value().value_offset),
            scanned.value().value_length);

        if (tag == 10U) {
            auto expected_checksum = ParseUnsigned(value_sv, "CheckSum");
            if (!expected_checksum.ok()) {
                return expected_checksum.status();
            }
            header.checksum = expected_checksum.value();
            checksum_field_start = scanned.value().start_offset;
            saw_checksum = true;
            byte_pos = scanned.value().next_pos;
            break;
        }

        if (ShouldSkipField(tag)) {
            byte_pos = scanned.value().next_pos;
            continue;
        }

        if (tag == 35U) {
            parsed_message.msg_type = value_sv;
            header.msg_type = value_sv;
            compiled = compiled_decoders.find(header.msg_type);
            // Fall back to generic path if the compiled decoder could not
            // index all fields (overflow chain exceeded during build).
            if (compiled != nullptr && compiled->has_overflow()) {
                compiled = nullptr;
            }
            auto slot = MakeParsedFieldSlotWithType(
                tag,
                static_cast<std::uint32_t>(scanned.value().value_offset),
                scanned.value().value_length,
                message::FieldValueType::kString);
            AppendParsedFieldSlot(&parsed_message, RootContainer(), slot);
            byte_pos = scanned.value().next_pos;
            continue;
        }

        // Header field extraction — same for both paths.
        if (tag == 34U) {
            auto parsed = ParseUnsigned(value_sv, "MsgSeqNum");
            if (!parsed.ok()) {
                return parsed.status();
            }
            header.msg_seq_num = parsed.value();
        } else if (tag == 49U) {
            header.sender_comp_id = value_sv;
        } else if (tag == 56U) {
            header.target_comp_id = value_sv;
        } else if (tag == 52U) {
            header.sending_time = value_sv;
        } else if (tag == 122U) {
            header.orig_sending_time = value_sv;
        } else if (tag == 1137U) {
            header.default_appl_ver_id = value_sv;
        } else if (tag == 43U) {
            auto parsed = ParseBoolean(value_sv, "PossDupFlag");
            if (!parsed.ok()) {
                return parsed.status();
            }
            header.poss_dup = parsed.value();
        }

        // ---- FAST PATH: compiled decoder available ----
        if (compiled != nullptr) {
            if (CompiledMessageDecoder::is_header_tag(tag)) {
                auto slot = MakeParsedFieldSlotWithType(
                    tag,
                    static_cast<std::uint32_t>(scanned.value().value_offset),
                    scanned.value().value_length,
                    ResolveFieldTypeWithDef(tag, nullptr));
                if (AppendParsedFieldSlot(&parsed_message, RootContainer(), slot)) {
                    SetValidationIssue(
                        &validation_issue,
                        ValidationIssueKind::kDuplicateField,
                        tag,
                        "field " + std::to_string(tag) + " appears more than once");
                }
                byte_pos = scanned.value().next_pos;
                continue;
            }

            const auto slot_idx = compiled->lookup(tag);
            if (slot_idx == CompiledMessageDecoder::kInvalidSlot) {
                // Differentiate: unknown to dictionary vs not allowed for this message.
                const auto issue_kind = dictionary.find_field(tag) == nullptr
                    ? ValidationIssueKind::kUnknownField
                    : ValidationIssueKind::kFieldNotAllowed;
                SetValidationIssue(
                    &validation_issue,
                    issue_kind,
                    tag,
                    issue_kind == ValidationIssueKind::kUnknownField
                        ? "field " + std::to_string(tag) + " is not present in the bound dictionary"
                        : "field " + std::to_string(tag) + " is not allowed by the bound message definition");
                auto slot = MakeParsedFieldSlotWithType(
                    tag,
                    static_cast<std::uint32_t>(scanned.value().value_offset),
                    scanned.value().value_length,
                    message::FieldValueType::kString);
                AppendParsedFieldSlot(&parsed_message, RootContainer(), slot);
                byte_pos = scanned.value().next_pos;
                continue;
            }

            const auto& compiled_slot = compiled->slot(slot_idx);

            if (compiled_slot.is_group_count) {
                // Group field — use the pre-resolved GroupDefRecord.
                auto slot = MakeParsedFieldSlotWithType(
                    tag,
                    static_cast<std::uint32_t>(scanned.value().value_offset),
                    scanned.value().value_length,
                    compiled_slot.field_type);
                if (AppendParsedFieldSlot(&parsed_message, RootContainer(), slot)) {
                    SetValidationIssue(
                        &validation_issue,
                        ValidationIssueKind::kDuplicateField,
                        tag,
                        "field " + std::to_string(tag) + " appears more than once");
                }
                auto next_pos = ParseParsedGroupEntries(
                    dictionary,
                    bytes,
                    scanned.value().next_pos,
                    delimiter_byte,
                    equals_byte,
                    value_sv,
                    *compiled_slot.group_def,
                    &parsed_message,
                    RootContainer(),
                    1U,
                    &validation_issue);
                if (!next_pos.ok()) {
                    return next_pos.status();
                }
                byte_pos = next_pos.value();
                continue;
            }

            // Scalar field — type is pre-resolved, no dictionary lookup needed.
            auto slot = MakeParsedFieldSlotWithType(
                tag,
                static_cast<std::uint32_t>(scanned.value().value_offset),
                scanned.value().value_length,
                compiled_slot.field_type);
            if (AppendParsedFieldSlot(&parsed_message, RootContainer(), slot)) {
                SetValidationIssue(
                    &validation_issue,
                    ValidationIssueKind::kDuplicateField,
                    tag,
                    "field " + std::to_string(tag) + " appears more than once");
            }
            byte_pos = scanned.value().next_pos;
            continue;
        }

        // Unknown message type — store field as-is without validation.
        auto slot = MakeParsedFieldSlotWithType(
            tag,
            static_cast<std::uint32_t>(scanned.value().value_offset),
            scanned.value().value_length,
            message::FieldValueType::kString);
        AppendParsedFieldSlot(&parsed_message, RootContainer(), slot);
        byte_pos = scanned.value().next_pos;
    }

    if (byte_pos != bytes.size()) {
        return base::Status::FormatError("FIX frame has trailing data after CheckSum");
    }
    if (field_count < 4U) {
        return base::Status::FormatError("FIX frame is too short");
    }
    if (!saw_checksum) {
        return base::Status::FormatError("FIX frame must begin with 8 and 9 and end with 10");
    }

    const auto actual_body_length = checksum_field_start - body_start_offset;
    if (declared_body_length.value() != actual_body_length) {
        return base::Status::FormatError("BodyLength mismatch");
    }

    const auto actual_checksum = [&]() -> std::uint32_t {
        const auto* data = bytes.data();
        std::uint32_t sum = 0;
#if FASTFIX_HAS_SSE2
        const auto zero = _mm_setzero_si128();
        std::size_t i = 0;
        for (; i + 16 <= checksum_field_start; i += 16) {
            auto chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i));
            auto sad = _mm_sad_epu8(chunk, zero);
            sum += static_cast<std::uint32_t>(_mm_extract_epi16(sad, 0)) +
                   static_cast<std::uint32_t>(_mm_extract_epi16(sad, 4));
        }
        for (; i < checksum_field_start; ++i) {
            sum += std::to_integer<unsigned char>(data[i]);
        }
#else
        for (std::size_t i = 0; i < checksum_field_start; ++i) {
            sum += std::to_integer<unsigned char>(data[i]);
        }
#endif
        return sum % 256U;
    }();
    if (actual_checksum != header.checksum) {
        return base::Status::FormatError("CheckSum mismatch");
    }

    DecodedMessageView decoded;
    decoded.message = message::ParsedMessage(std::move(parsed_message));
    decoded.header = std::move(header);
    decoded.raw = bytes;
    decoded.validation_issue = std::move(validation_issue);
    return decoded;
}

auto DecodeFixMessage(
    std::span<const std::byte> bytes,
    const profile::NormalizedDictionaryView& dictionary,
    char delimiter) -> base::Result<DecodedMessage> {
    auto decoded = DecodeFixMessageView(bytes, dictionary, delimiter);
    if (!decoded.ok()) {
        return decoded.status();
    }
    return std::move(decoded).value().ToOwned();
}

auto PeekSessionHeaderView(
    std::span<const std::byte> bytes,
    char delimiter) -> base::Result<SessionHeaderView> {
    const char normalized_delimiter = NormalizeDelimiter(delimiter);
    const auto delimiter_byte = static_cast<std::byte>(static_cast<unsigned char>(normalized_delimiter));
    const auto equals_byte = static_cast<std::byte>('=');
    SessionHeaderView header;
    std::size_t field_start = 0U;
    std::size_t field_count = 0U;
    std::size_t body_start_offset = 0U;
    std::size_t checksum_field_start = 0U;
    bool saw_checksum = false;

    while (field_start < bytes.size()) {
        const auto remaining = bytes.size() - field_start;
        const auto* soh_ptr = FindByte(bytes.data() + field_start, remaining, delimiter_byte);
        const auto index = static_cast<std::size_t>(soh_ptr - bytes.data());

        if (index >= bytes.size()) {
            break;
        }

        if (index == field_start) {
            return base::Status::FormatError("empty FIX field is not allowed");
        }

        const auto field_len = index - field_start;
        const auto* eq_ptr = FindByte(bytes.data() + field_start, field_len, equals_byte);
        const auto equals = static_cast<std::size_t>(eq_ptr - (bytes.data() + field_start));
        if (equals >= field_len || equals == 0U || equals == field_len - 1U) {
            return base::Status::FormatError("invalid FIX field syntax");
        }

        const auto* field_bytes = reinterpret_cast<const char*>(bytes.data() + field_start);
        auto tag = ParseUnsigned(std::string_view(field_bytes, equals), "field tag");
        if (!tag.ok()) {
            return tag.status();
        }
        const auto value = std::string_view(
            reinterpret_cast<const char*>(bytes.data() + field_start + equals + 1U),
            field_len - equals - 1U);
        ++field_count;

        if (field_count == 1U) {
            if (tag.value() != 8U) {
                return base::Status::FormatError("FIX frame must begin with 8 and 9 and end with 10");
            }
            header.begin_string = value;
            field_start = index + 1U;
            continue;
        }

        if (field_count == 2U) {
            if (tag.value() != 9U) {
                return base::Status::FormatError("FIX frame must begin with 8 and 9 and end with 10");
            }
            auto declared_body_length = ParseUnsigned(value, "BodyLength");
            if (!declared_body_length.ok()) {
                return declared_body_length.status();
            }
            header.body_length = declared_body_length.value();
            body_start_offset = index + 1U;
            field_start = index + 1U;
            continue;
        }

        if (tag.value() == 10U) {
            if (index + 1U != bytes.size()) {
                return base::Status::FormatError("FIX frame must begin with 8 and 9 and end with 10");
            }

            auto expected_checksum = ParseUnsigned(value, "CheckSum");
            if (!expected_checksum.ok()) {
                return expected_checksum.status();
            }
            header.checksum = expected_checksum.value();
            checksum_field_start = field_start;
            saw_checksum = true;
            field_start = index + 1U;
            break;
        }

        switch (tag.value()) {
            case 35U:
                header.msg_type = value;
                break;
            case 34U: {
                auto parsed = ParseUnsigned(value, "MsgSeqNum");
                if (!parsed.ok()) {
                    return parsed.status();
                }
                header.msg_seq_num = parsed.value();
                break;
            }
            case 43U: {
                auto parsed = ParseBoolean(value, "PossDupFlag");
                if (!parsed.ok()) {
                    return parsed.status();
                }
                header.poss_dup = parsed.value();
                break;
            }
            case 49U:
                header.sender_comp_id = value;
                break;
            case 52U:
                header.sending_time = value;
                break;
            case 122U:
                header.orig_sending_time = value;
                break;
            case 56U:
                header.target_comp_id = value;
                break;
            case 1137U:
                header.default_appl_ver_id = value;
                break;
            default:
                break;
        }

        field_start = index + 1U;
    }

    if (field_start != bytes.size()) {
        return base::Status::FormatError("FIX frame is missing its final delimiter");
    }
    if (field_count == 0U) {
        return base::Status::FormatError("FIX frame has no fields");
    }
    if (field_count < 4U) {
        return base::Status::FormatError("FIX frame is too short");
    }
    if (!saw_checksum) {
        return base::Status::FormatError("FIX frame must begin with 8 and 9 and end with 10");
    }

    const auto actual_body_length = checksum_field_start - body_start_offset;
    if (header.body_length != actual_body_length) {
        return base::Status::FormatError("BodyLength mismatch");
    }

    std::uint32_t actual_checksum = 0;
    for (std::size_t index = 0; index < checksum_field_start; ++index) {
        actual_checksum += std::to_integer<unsigned char>(bytes[index]);
    }
    actual_checksum %= 256U;
    if (actual_checksum != header.checksum) {
        return base::Status::FormatError("CheckSum mismatch");
    }

    return header;
}

auto PeekSessionHeader(
    std::span<const std::byte> bytes,
    char delimiter) -> base::Result<SessionHeader> {
    auto header = PeekSessionHeaderView(bytes, delimiter);
    if (!header.ok()) {
        return header.status();
    }
    return header.value().ToOwned();
}

}  // namespace fastfix::codec