#pragma once

#include <cstdint>
#include <string_view>

namespace nimble::session {

/// High-level inbound validation presets.
///
/// These presets are starting points. `ValidationPolicy` lets advanced callers
/// flip individual checks when they need protocol compatibility with a specific
/// counterparty.
enum class ValidationMode : std::uint32_t
{
  kStrict = 0,
  kCompatible,
  kPermissive,
  kRawPassThrough,
};

/// Inbound FIX validation contract for one session.
///
/// Design intent: keep the coarse mode (`Strict`/`Compatible`/...) visible for
/// config and metrics while still allowing each low-level rule to be tuned.
struct ValidationPolicy
{
  // Coarse preset name retained for configuration, logging, and diagnostics.
  ValidationMode mode{ ValidationMode::kStrict };
  // Reject SenderCompID/TargetCompID mismatches against the configured session key.
  bool enforce_comp_ids{ true };
  // Require DefaultApplVerID(1137) on FIXT Logon when the transport profile expects it.
  bool require_default_appl_ver_id_on_logon{ true };
  // Require OrigSendingTime(122) on PossDup resend traffic.
  bool require_orig_sending_time_on_poss_dup{ true };
  // Reject inbound messages whose MsgSeqNum is lower than expected.
  bool reject_on_stale_msg_seq_num{ true };
  // Reject application messages whose MsgType is absent from the loaded dictionary.
  bool require_known_app_message_type{ true };
  // Enforce dictionary-declared required fields for known application messages.
  bool require_required_fields_on_app_messages{ true };
  // Reject tags not present in the dictionary for the active message definition.
  bool reject_unknown_fields{ true };
  // Reject duplicate scalar tags within the same message or group entry.
  bool reject_duplicate_fields{ true };
  // Reject scalar tags whose value is empty.
  bool reject_tag_without_value{ true };
  // Reject scalar values that do not match the dictionary-declared type.
  bool reject_incorrect_data_format{ true };
  // Reject fields that appear before an earlier dictionary rule in the same scope.
  bool reject_fields_out_of_order{ true };
  // Reject malformed repeating-group structure or count mismatches.
  bool reject_invalid_group_structure{ true };

  [[nodiscard]] static auto Strict() -> ValidationPolicy { return ValidationPolicy{}; }

  [[nodiscard]] static auto Compatible() -> ValidationPolicy
  {
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
      .reject_tag_without_value = true,
      .reject_incorrect_data_format = true,
      .reject_fields_out_of_order = false,
      .reject_invalid_group_structure = true,
    };
  }

  [[nodiscard]] static auto Permissive() -> ValidationPolicy
  {
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
      .reject_tag_without_value = false,
      .reject_incorrect_data_format = false,
      .reject_fields_out_of_order = false,
      .reject_invalid_group_structure = false,
    };
  }

  [[nodiscard]] static auto RawPassThrough() -> ValidationPolicy
  {
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
      .reject_tag_without_value = false,
      .reject_incorrect_data_format = false,
      .reject_fields_out_of_order = false,
      .reject_invalid_group_structure = false,
    };
  }
};

/// Build one preset validation policy.
///
/// \param mode Preset selector.
/// \return Policy initialized to the corresponding preset values.
[[nodiscard]] inline auto
MakeValidationPolicy(ValidationMode mode) -> ValidationPolicy
{
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

/// Return a stable lowercase name for one validation preset.
///
/// \param mode Preset selector.
/// \return Human-readable preset name.
[[nodiscard]] inline auto
ValidationModeName(ValidationMode mode) -> std::string_view
{
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

} // namespace nimble::session