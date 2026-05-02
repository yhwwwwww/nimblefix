#pragma once

#include <cstdint>
#include <string_view>

#include "nimblefix/codec/fix_codec.h"

namespace nimble::session {

/// Optional per-session callback for fine-grained validation decisions.
/// Install on CounterpartyConfig to receive notifications about validation
/// events that would otherwise be silently accepted or rejected.
class ValidationCallback
{
public:
  virtual ~ValidationCallback() = default;

  /// Called when an unknown field is encountered and the policy action is kLogAndProcess.
  /// Return true to accept the field, false to reject.
  virtual auto OnUnknownField(std::uint64_t session_id,
                              std::uint32_t tag,
                              std::string_view value,
                              std::string_view msg_type) -> bool
  {
    (void)session_id;
    (void)tag;
    (void)value;
    (void)msg_type;
    return true;
  }

  /// Called when a malformed field value is encountered and the policy action is kLog.
  /// Return true to accept, false to reject.
  virtual auto OnMalformedField(std::uint64_t session_id,
                                std::uint32_t tag,
                                std::string_view value,
                                std::string_view msg_type,
                                std::string_view issue_text) -> bool
  {
    (void)session_id;
    (void)tag;
    (void)value;
    (void)msg_type;
    (void)issue_text;
    return true;
  }

  /// Called for any validation issue that the policy allows through.
  /// Informational only — the message will be processed regardless.
  virtual auto OnValidationWarning(std::uint64_t session_id,
                                   const codec::ValidationIssue& issue,
                                   std::string_view msg_type) -> void
  {
    (void)session_id;
    (void)issue;
    (void)msg_type;
  }
};

} // namespace nimble::session
