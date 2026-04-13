#include "fastfix/profile/artifact_builder.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>

#include "fastfix/profile/artifact.h"

namespace fastfix::profile {

namespace {

class StringTableBuilder {
  public:
    StringTableBuilder() {
        bytes_.push_back(std::byte{0});
        offsets_.emplace(std::string(), 0U);
    }

    auto Intern(std::string_view value) -> std::uint32_t {
        if (value.empty()) {
            return 0U;
        }

        const auto key = std::string(value);
        const auto it = offsets_.find(key);
        if (it != offsets_.end()) {
            return it->second;
        }

        const auto offset = static_cast<std::uint32_t>(bytes_.size());
        for (const auto ch : key) {
            bytes_.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
        }
        bytes_.push_back(std::byte{0});
        offsets_.emplace(key, offset);
        return offset;
    }

    [[nodiscard]] auto bytes() const -> const std::vector<std::byte>& {
        return bytes_;
    }

  private:
    std::vector<std::byte> bytes_;
    std::unordered_map<std::string, std::uint32_t> offsets_;
};

template <typename Entry>
auto SerializeEntries(std::span<const Entry> entries) -> std::vector<std::byte> {
    std::vector<std::byte> bytes(entries.size_bytes());
    if (!bytes.empty()) {
        std::memcpy(bytes.data(), entries.data(), bytes.size());
    }
    return bytes;
}

struct PendingSection {
    SectionKind kind;
    std::vector<std::byte> bytes;
    std::uint64_t entry_count{0};
    std::uint64_t entry_size{0};
};

auto ValidateDictionary(const NormalizedDictionary& dictionary) -> base::Status {
    std::unordered_map<std::uint32_t, bool> field_tags;
    for (const auto& field : dictionary.fields) {
        if (!field_tags.emplace(field.tag, true).second) {
            return base::Status::InvalidArgument("dictionary contains duplicate field tags");
        }
    }

    std::unordered_map<std::string, bool> message_types;
    for (const auto& message : dictionary.messages) {
        if (!message_types.emplace(message.msg_type, true).second) {
            return base::Status::InvalidArgument("dictionary contains duplicate message types");
        }
    }

    std::unordered_map<std::uint32_t, bool> group_tags;
    for (const auto& group : dictionary.groups) {
        if (!group_tags.emplace(group.count_tag, true).second) {
            return base::Status::InvalidArgument("dictionary contains duplicate group count tags");
        }
    }

    return base::Status::Ok();
}

auto ComputeRequiredFieldBitmap(std::span<const FieldRule> rules) -> std::pair<std::uint64_t, bool> {
    std::uint64_t bitmap = 0U;
    bool overflow = false;
    for (std::size_t index = 0; index < rules.size(); ++index) {
        const bool required =
            (rules[index].flags & static_cast<std::uint32_t>(FieldRuleFlags::kRequired)) != 0U;
        if (!required) {
            continue;
        }
        if (index >= 64U) {
            overflow = true;
            continue;
        }
        bitmap |= (std::uint64_t{1} << index);
    }
    return {bitmap, overflow};
}

}  // namespace

auto BuildProfileArtifact(const NormalizedDictionary& dictionary)
    -> base::Result<std::vector<std::byte>> {
    auto validation = ValidateDictionary(dictionary);
    if (!validation.ok()) {
        return validation;
    }

    StringTableBuilder strings;

    std::vector<FieldDefRecord> field_defs;
    field_defs.reserve(dictionary.fields.size());
    for (const auto& field : dictionary.fields) {
        field_defs.push_back(FieldDefRecord{
            .tag = field.tag,
            .name_offset = strings.Intern(field.name),
            .value_type = static_cast<std::uint32_t>(field.value_type),
            .flags = field.flags,
        });
    }

    std::vector<FieldRuleRecord> message_rules;
    std::vector<MessageDefRecord> message_defs;
    message_defs.reserve(dictionary.messages.size());
    for (const auto& message : dictionary.messages) {
        const auto first_rule = static_cast<std::uint32_t>(message_rules.size());
        for (const auto& rule : message.field_rules) {
            message_rules.push_back(FieldRuleRecord{.tag = rule.tag, .flags = rule.flags});
        }
        message_defs.push_back(MessageDefRecord{
            .msg_type_offset = strings.Intern(message.msg_type),
            .name_offset = strings.Intern(message.name),
            .first_field_rule = first_rule,
            .field_rule_count = static_cast<std::uint32_t>(message.field_rules.size()),
            .flags = message.flags,
            .reserved0 = 0,
        });
    }

    std::vector<FieldRuleRecord> group_rules;
    std::vector<GroupDefRecord> group_defs;
    group_defs.reserve(dictionary.groups.size());
    for (const auto& group : dictionary.groups) {
        const auto first_rule = static_cast<std::uint32_t>(group_rules.size());
        const auto [required_field_bitmap, required_bitmap_overflow] =
            ComputeRequiredFieldBitmap(group.field_rules);
        for (const auto& rule : group.field_rules) {
            group_rules.push_back(FieldRuleRecord{.tag = rule.tag, .flags = rule.flags});
        }
        group_defs.push_back(GroupDefRecord{
            .count_tag = group.count_tag,
            .delimiter_tag = group.delimiter_tag,
            .name_offset = strings.Intern(group.name),
            .first_field_rule = first_rule,
            .field_rule_count = static_cast<std::uint32_t>(group.field_rules.size()),
            .flags = group.flags |
                (required_bitmap_overflow ? static_cast<std::uint32_t>(GroupFlags::kRequiredBitmapOverflow) : 0U),
            .required_field_bitmap = required_field_bitmap,
        });
    }

    // --- kAdminRules: one entry per admin message ---
    std::vector<AdminRuleEntry> admin_rules;
    for (const auto& message : dictionary.messages) {
        if ((message.flags & static_cast<std::uint32_t>(MessageFlags::kAdmin)) != 0U) {
            admin_rules.push_back(AdminRuleEntry{
                .msg_type_offset = strings.Intern(message.msg_type),
                .flags = message.flags,
            });
        }
    }

    // --- kValidationRules: one entry per field ---
    std::vector<ValidationRuleEntry> validation_rules;
    validation_rules.reserve(dictionary.fields.size());
    for (const auto& field : dictionary.fields) {
        validation_rules.push_back(ValidationRuleEntry{
            .tag = field.tag,
            .value_type = static_cast<std::uint32_t>(field.value_type),
            .flags = field.flags,
        });
    }

    // --- kLookupTables: direct-address array for O(1) tag->field_index ---
    static constexpr std::size_t kLookupTableSize = 10000;
    static constexpr std::uint32_t kLookupAbsent = 0xFFFFFFFFU;
    std::vector<LookupTableEntry> lookup_table(kLookupTableSize, LookupTableEntry{.field_index = kLookupAbsent});
    for (std::size_t i = 0; i < dictionary.fields.size(); ++i) {
        const auto tag = dictionary.fields[i].tag;
        if (tag < kLookupTableSize) {
            lookup_table[tag].field_index = static_cast<std::uint32_t>(i);
        }
    }

    // --- kTemplateDescriptors: one entry per message ---
    std::vector<TemplateDescriptorEntry> template_descriptors;
    template_descriptors.reserve(dictionary.messages.size());
    {
        std::uint32_t rule_offset = 0;
        for (const auto& message : dictionary.messages) {
            template_descriptors.push_back(TemplateDescriptorEntry{
                .msg_type_offset = strings.Intern(message.msg_type),
                .field_count = static_cast<std::uint32_t>(message.field_rules.size()),
                .first_field_rule_index = rule_offset,
                .flags = message.flags,
            });
            rule_offset += static_cast<std::uint32_t>(message.field_rules.size());
        }
    }

    std::vector<PendingSection> pending_sections;
    pending_sections.push_back(PendingSection{
        .kind = SectionKind::kStringTable,
        .bytes = strings.bytes(),
        .entry_count = strings.bytes().size(),
        .entry_size = 1,
    });
    pending_sections.push_back(PendingSection{
        .kind = SectionKind::kFieldDefs,
        .bytes = SerializeEntries<FieldDefRecord>(field_defs),
        .entry_count = field_defs.size(),
        .entry_size = sizeof(FieldDefRecord),
    });
    pending_sections.push_back(PendingSection{
        .kind = SectionKind::kMessageDefs,
        .bytes = SerializeEntries<MessageDefRecord>(message_defs),
        .entry_count = message_defs.size(),
        .entry_size = sizeof(MessageDefRecord),
    });
    pending_sections.push_back(PendingSection{
        .kind = SectionKind::kGroupDefs,
        .bytes = SerializeEntries<GroupDefRecord>(group_defs),
        .entry_count = group_defs.size(),
        .entry_size = sizeof(GroupDefRecord),
    });
    pending_sections.push_back(PendingSection{
        .kind = SectionKind::kMessageFieldRules,
        .bytes = SerializeEntries<FieldRuleRecord>(message_rules),
        .entry_count = message_rules.size(),
        .entry_size = sizeof(FieldRuleRecord),
    });
    pending_sections.push_back(PendingSection{
        .kind = SectionKind::kGroupFieldRules,
        .bytes = SerializeEntries<FieldRuleRecord>(group_rules),
        .entry_count = group_rules.size(),
        .entry_size = sizeof(FieldRuleRecord),
    });
    pending_sections.push_back(PendingSection{
        .kind = SectionKind::kAdminRules,
        .bytes = SerializeEntries<AdminRuleEntry>(admin_rules),
        .entry_count = admin_rules.size(),
        .entry_size = sizeof(AdminRuleEntry),
    });
    pending_sections.push_back(PendingSection{
        .kind = SectionKind::kValidationRules,
        .bytes = SerializeEntries<ValidationRuleEntry>(validation_rules),
        .entry_count = validation_rules.size(),
        .entry_size = sizeof(ValidationRuleEntry),
    });
    pending_sections.push_back(PendingSection{
        .kind = SectionKind::kLookupTables,
        .bytes = SerializeEntries<LookupTableEntry>(lookup_table),
        .entry_count = lookup_table.size(),
        .entry_size = sizeof(LookupTableEntry),
    });
    pending_sections.push_back(PendingSection{
        .kind = SectionKind::kTemplateDescriptors,
        .bytes = SerializeEntries<TemplateDescriptorEntry>(template_descriptors),
        .entry_count = template_descriptors.size(),
        .entry_size = sizeof(TemplateDescriptorEntry),
    });

    std::vector<FieldRuleRecord> header_rules;
    header_rules.reserve(dictionary.header_fields.size());
    for (const auto& rule : dictionary.header_fields) {
        header_rules.push_back(FieldRuleRecord{.tag = rule.tag, .flags = rule.flags});
    }
    pending_sections.push_back(PendingSection{
        .kind = SectionKind::kHeaderFieldRules,
        .bytes = SerializeEntries<FieldRuleRecord>(header_rules),
        .entry_count = header_rules.size(),
        .entry_size = sizeof(FieldRuleRecord),
    });

    std::vector<FieldRuleRecord> trailer_rules;
    trailer_rules.reserve(dictionary.trailer_fields.size());
    for (const auto& rule : dictionary.trailer_fields) {
        trailer_rules.push_back(FieldRuleRecord{.tag = rule.tag, .flags = rule.flags});
    }
    pending_sections.push_back(PendingSection{
        .kind = SectionKind::kTrailerFieldRules,
        .bytes = SerializeEntries<FieldRuleRecord>(trailer_rules),
        .entry_count = trailer_rules.size(),
        .entry_size = sizeof(FieldRuleRecord),
    });

    std::vector<ArtifactSection> sections;
    sections.reserve(pending_sections.size());

    std::uint64_t offset = sizeof(ArtifactHeader) +
        static_cast<std::uint64_t>(pending_sections.size()) * sizeof(ArtifactSection);
    for (const auto& pending : pending_sections) {
        sections.push_back(ArtifactSection{
            .kind = static_cast<std::uint32_t>(pending.kind),
            .flags = static_cast<std::uint32_t>(SectionFlags::kNone),
            .offset = offset,
            .size = static_cast<std::uint64_t>(pending.bytes.size()),
            .entry_count = pending.entry_count,
            .entry_size = pending.entry_size,
        });
        offset += pending.bytes.size();
    }

    ArtifactHeader header{};
    std::memcpy(header.magic, kArtifactMagic.data(), kArtifactMagic.size());
    header.format_version = kArtifactFormatVersion;
    header.header_size = sizeof(ArtifactHeader);
    header.section_entry_size = sizeof(ArtifactSection);
    header.endian_tag = kArtifactEndianLittle;
    header.file_size = offset;
    header.section_table_offset = sizeof(ArtifactHeader);
    header.section_count = static_cast<std::uint32_t>(sections.size());
    header.flags = static_cast<std::uint32_t>(ArtifactFlags::kNone);
    header.schema_hash = dictionary.schema_hash;
    header.profile_id = dictionary.profile_id;
    header.reserved1 = 0;

    std::vector<std::byte> bytes(static_cast<std::size_t>(header.file_size));
    auto* cursor = bytes.data();
    std::memcpy(cursor, &header, sizeof(header));
    cursor += sizeof(header);
    std::memcpy(cursor, sections.data(), sections.size() * sizeof(ArtifactSection));
    cursor += sections.size() * sizeof(ArtifactSection);

    for (const auto& pending : pending_sections) {
        if (!pending.bytes.empty()) {
            std::memcpy(cursor, pending.bytes.data(), pending.bytes.size());
            cursor += pending.bytes.size();
        }
    }

    return bytes;
}

auto WriteProfileArtifact(const std::filesystem::path& path, std::span<const std::byte> bytes)
    -> base::Status {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return base::Status::IoError("unable to open artifact for writing: '" + path.string() + "'");
    }

    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out.good()) {
        return base::Status::IoError("unable to write artifact: '" + path.string() + "'");
    }

    return base::Status::Ok();
}

}  // namespace fastfix::profile
