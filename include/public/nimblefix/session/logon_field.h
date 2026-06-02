#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace nimble::session {

/// Extra body field appended to outbound Logon(35=A) frames.
///
/// Values are stored as FIX wire text. This keeps common credential fields such
/// as Username(553) and Password(554) simple while still allowing venue-specific
/// numeric or enum tags by passing their encoded value.
struct LogonField
{
  std::uint32_t tag{ 0 };
  std::string value;

  LogonField() = default;

  LogonField(std::uint32_t field_tag, std::string field_value)
    : tag(field_tag)
    , value(std::move(field_value))
  {
  }

  friend auto operator==(const LogonField&, const LogonField&) -> bool = default;
};

} // namespace nimble::session
