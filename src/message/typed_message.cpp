#include "fastfix/message/typed_message.h"

#include <bit>

namespace fastfix::message {

namespace {

auto MessageHasTag(MessageView message, std::uint32_t tag) -> bool {
    if (message.has_field(tag)) {
        return true;
    }
    return message.group(tag).has_value();
}

auto MissingRequiredField(std::uint32_t tag, std::uint32_t* missing_tag) -> base::Status {
    if (missing_tag != nullptr) {
        *missing_tag = tag;
    }
    return base::Status::InvalidArgument("typed message is missing required field " + std::to_string(tag));
}

template <typename RuleSpan>
auto ValidateRequiredRules(
    const profile::NormalizedDictionaryView& dictionary,
    MessageView message,
    RuleSpan rules,
    std::uint32_t* missing_tag) -> base::Status;

auto ValidateRequiredGroup(
    const profile::NormalizedDictionaryView& dictionary,
    GroupView group,
    const profile::GroupDefRecord& group_def,
    std::uint32_t* missing_tag) -> base::Status {
    const auto rules = dictionary.group_field_rules(group_def);
    const bool bitmap_complete =
        (group_def.flags & static_cast<std::uint32_t>(profile::GroupFlags::kRequiredBitmapOverflow)) == 0U;
    for (std::size_t index = 0; index < group.size(); ++index) {
        std::uint64_t present_bitmap = 0U;
        for (std::size_t rule_index = 0; rule_index < rules.size(); ++rule_index) {
            const auto& rule = rules[rule_index];
            const bool present = MessageHasTag(group[index], rule.tag);
            if (present && rule_index < 64U) {
                present_bitmap |= (std::uint64_t{1} << rule_index);
            }
            if (!bitmap_complete) {
                const bool required =
                    (rule.flags & static_cast<std::uint32_t>(profile::FieldRuleFlags::kRequired)) != 0U;
                if (required && !present) {
                    return MissingRequiredField(rule.tag, missing_tag);
                }
            }

            const auto* nested_group = dictionary.find_group(rule.tag);
            if (nested_group == nullptr) {
                continue;
            }

            const auto nested = group[index].group(rule.tag);
            if (!nested.has_value()) {
                continue;
            }

            auto status = ValidateRequiredGroup(dictionary, *nested, *nested_group, missing_tag);
            if (!status.ok()) {
                return status;
            }
        }

        if (bitmap_complete) {
            const auto missing = group_def.required_field_bitmap & ~present_bitmap;
            if (missing != 0U) {
                const auto missing_index = static_cast<std::size_t>(std::countr_zero(missing));
                return MissingRequiredField(rules[missing_index].tag, missing_tag);
            }
        }
    }
    return base::Status::Ok();
}

template <typename RuleSpan>
auto ValidateRequiredRules(
    const profile::NormalizedDictionaryView& dictionary,
    MessageView message,
    RuleSpan rules,
    std::uint32_t* missing_tag) -> base::Status {
    for (const auto& rule : rules) {
        const bool required = (rule.flags & static_cast<std::uint32_t>(profile::FieldRuleFlags::kRequired)) != 0U;
        if (required && !MessageHasTag(message, rule.tag)) {
            return MissingRequiredField(rule.tag, missing_tag);
        }

        const auto* group_def = dictionary.find_group(rule.tag);
        if (group_def == nullptr) {
            continue;
        }

        const auto group = message.group(rule.tag);
        if (!group.has_value()) {
            continue;
        }

        auto status = ValidateRequiredGroup(dictionary, *group, *group_def, missing_tag);
        if (!status.ok()) {
            return status;
        }
    }
    return base::Status::Ok();
}

auto GroupEntryGetString(MessageView view, std::uint32_t tag) -> std::optional<std::string_view> {
    return view.get_string(tag);
}

auto GroupEntryGetInt(MessageView view, std::uint32_t tag) -> std::optional<std::int64_t> {
    return view.get_int(tag);
}

auto GroupEntryGetChar(MessageView view, std::uint32_t tag) -> std::optional<char> {
    return view.get_char(tag);
}

auto GroupEntryGetFloat(MessageView view, std::uint32_t tag) -> std::optional<double> {
    return view.get_float(tag);
}

auto GroupEntryGetBoolean(MessageView view, std::uint32_t tag) -> std::optional<bool> {
    return view.get_boolean(tag);
}

}  // namespace

auto TypedMessageView::Bind(
    const profile::NormalizedDictionaryView& dictionary,
    MessageView message) -> base::Result<TypedMessageView> {
    const auto* message_def = dictionary.find_message(message.msg_type());
    if (message_def == nullptr) {
        return base::Status::NotFound("message type is not present in the dictionary");
    }

    return TypedMessageView(&dictionary, message, message_def);
}

auto TypedMessageView::validate_required_fields(std::uint32_t* missing_tag) const -> base::Status {
    if (dictionary_ == nullptr || message_def_ == nullptr) {
        return base::Status::InvalidArgument("typed message view is not bound to a dictionary message definition");
    }
    return ValidateRequiredRules(*dictionary_, message_, dictionary_->message_field_rules(*message_def_), missing_tag);
}

auto TypedMessageView::get_string(std::uint32_t tag) const -> std::optional<std::string_view> {
    return message_.get_string(tag);
}

auto TypedMessageView::get_timestamp(std::uint32_t tag) const -> std::optional<std::string_view> {
    const auto* field = dictionary_ == nullptr ? nullptr : dictionary_->find_field(tag);
    if (field == nullptr || static_cast<profile::ValueType>(field->value_type) != profile::ValueType::kTimestamp) {
        return std::nullopt;
    }
    return message_.get_string(tag);
}

auto TypedMessageView::get_int(std::uint32_t tag) const -> std::optional<std::int64_t> {
    return message_.get_int(tag);
}

auto TypedMessageView::get_char(std::uint32_t tag) const -> std::optional<char> {
    return message_.get_char(tag);
}

auto TypedMessageView::get_float(std::uint32_t tag) const -> std::optional<double> {
    return message_.get_float(tag);
}

auto TypedMessageView::get_boolean(std::uint32_t tag) const -> std::optional<bool> {
    return message_.get_boolean(tag);
}

auto TypedMessageView::group(std::uint32_t count_tag) const -> std::optional<TypedGroupView> {
    if (dictionary_ == nullptr) {
        return std::nullopt;
    }
    const auto* group_def = dictionary_->find_group(count_tag);
    if (group_def == nullptr) {
        return std::nullopt;
    }

    const auto group = message_.group(count_tag);
    if (!group.has_value()) {
        return std::nullopt;
    }

    return TypedGroupView(dictionary_, *group, group_def);
}

auto TypedGroupEntryView::get_string(std::uint32_t tag) const -> std::optional<std::string_view> {
    return GroupEntryGetString(entry_, tag);
}

auto TypedGroupEntryView::get_timestamp(std::uint32_t tag) const -> std::optional<std::string_view> {
    const auto* field = dictionary_ == nullptr ? nullptr : dictionary_->find_field(tag);
    if (field == nullptr || static_cast<profile::ValueType>(field->value_type) != profile::ValueType::kTimestamp) {
        return std::nullopt;
    }
    return GroupEntryGetString(entry_, tag);
}

auto TypedGroupEntryView::get_int(std::uint32_t tag) const -> std::optional<std::int64_t> {
    return GroupEntryGetInt(entry_, tag);
}

auto TypedGroupEntryView::get_char(std::uint32_t tag) const -> std::optional<char> {
    return GroupEntryGetChar(entry_, tag);
}

auto TypedGroupEntryView::get_float(std::uint32_t tag) const -> std::optional<double> {
    return GroupEntryGetFloat(entry_, tag);
}

auto TypedGroupEntryView::get_boolean(std::uint32_t tag) const -> std::optional<bool> {
    return GroupEntryGetBoolean(entry_, tag);
}

auto TypedGroupEntryView::group(std::uint32_t count_tag) const -> std::optional<TypedGroupView> {
    if (dictionary_ == nullptr) {
        return std::nullopt;
    }

    const auto* group_def = dictionary_->find_group(count_tag);
    if (group_def == nullptr) {
        return std::nullopt;
    }

    const auto group = entry_.group(count_tag);
    if (!group.has_value()) {
        return std::nullopt;
    }

    return TypedGroupView(dictionary_, *group, group_def);
}

auto TypedGroupView::Iterator::operator*() const -> TypedGroupEntryView {
    return TypedGroupEntryView(dictionary_, group_[index_], group_def_);
}

auto TypedGroupView::operator[](std::size_t index) const -> TypedGroupEntryView {
    return TypedGroupEntryView(dictionary_, group_[index], group_def_);
}

auto TypedGroupView::begin() const -> Iterator {
    return Iterator(dictionary_, group_, group_def_, 0U);
}

auto TypedGroupView::end() const -> Iterator {
    return Iterator(dictionary_, group_, group_def_, group_.size());
}

}  // namespace fastfix::message