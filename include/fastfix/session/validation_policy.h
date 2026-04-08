#pragma once

#include <cstdint>
#include <string_view>

namespace fastfix::session {

enum class ValidationMode : std::uint32_t {
    kStrict = 0,
    kCompatible,
    kPermissive,
    kRawPassThrough,
};

struct ValidationPolicy {
    ValidationMode mode{ValidationMode::kStrict};
    bool enforce_comp_ids{true};
    bool require_default_appl_ver_id_on_logon{true};
    bool require_orig_sending_time_on_poss_dup{true};
    bool reject_on_stale_msg_seq_num{true};
    bool require_known_app_message_type{true};
    bool require_required_fields_on_app_messages{true};
    bool reject_unknown_fields{true};
    bool reject_duplicate_fields{true};
    bool reject_invalid_group_structure{true};

    [[nodiscard]] static auto Strict() -> ValidationPolicy {
        return ValidationPolicy{};
    }

    [[nodiscard]] static auto Compatible() -> ValidationPolicy {
        return ValidationPolicy{
            .mode = ValidationMode::kCompatible,
            .enforce_comp_ids = true,
            .require_default_appl_ver_id_on_logon = false,
            .require_orig_sending_time_on_poss_dup = false,
            .reject_on_stale_msg_seq_num = true,
            .require_known_app_message_type = false,
            .require_required_fields_on_app_messages = true,
            .reject_unknown_fields = false,
            .reject_duplicate_fields = false,
            .reject_invalid_group_structure = false,
        };
    }

    [[nodiscard]] static auto Permissive() -> ValidationPolicy {
        return ValidationPolicy{
            .mode = ValidationMode::kPermissive,
            .enforce_comp_ids = false,
            .require_default_appl_ver_id_on_logon = false,
            .require_orig_sending_time_on_poss_dup = false,
            .reject_on_stale_msg_seq_num = false,
            .require_known_app_message_type = false,
            .require_required_fields_on_app_messages = false,
            .reject_unknown_fields = false,
            .reject_duplicate_fields = false,
            .reject_invalid_group_structure = false,
        };
    }

    [[nodiscard]] static auto RawPassThrough() -> ValidationPolicy {
        return ValidationPolicy{
            .mode = ValidationMode::kRawPassThrough,
            .enforce_comp_ids = false,
            .require_default_appl_ver_id_on_logon = false,
            .require_orig_sending_time_on_poss_dup = false,
            .reject_on_stale_msg_seq_num = false,
            .require_known_app_message_type = false,
            .require_required_fields_on_app_messages = false,
            .reject_unknown_fields = false,
            .reject_duplicate_fields = false,
            .reject_invalid_group_structure = false,
        };
    }
};

[[nodiscard]] inline auto MakeValidationPolicy(ValidationMode mode) -> ValidationPolicy {
    switch (mode) {
        case ValidationMode::kCompatible:
            return ValidationPolicy::Compatible();
        case ValidationMode::kPermissive:
            return ValidationPolicy::Permissive();
        case ValidationMode::kRawPassThrough:
            return ValidationPolicy::RawPassThrough();
        default:
            return ValidationPolicy::Strict();
    }
}

[[nodiscard]] inline auto ValidationModeName(ValidationMode mode) -> std::string_view {
    switch (mode) {
        case ValidationMode::kCompatible:
            return "compatible";
        case ValidationMode::kPermissive:
            return "permissive";
        case ValidationMode::kRawPassThrough:
            return "raw-pass-through";
        default:
            return "strict";
    }
}

}  // namespace fastfix::session