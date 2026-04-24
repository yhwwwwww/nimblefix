#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace nimble::session {

/// Transport-layer FIX session versions understood by the runtime.
enum class TransportVersion : std::uint32_t
{
  kFix40 = 0,
  kFix41,
  kFix42,
  kFix43,
  kFix44,
  kFixT11, // FIXT.1.1
};

/// Normalized transport behavior flags for one FIX version family.
///
/// Design intent: capture transport-level protocol differences that matter to
/// the runtime state machine without forcing callers to infer behavior from the
/// raw `BeginString` alone.
struct TransportSessionProfile
{
  TransportVersion version{ TransportVersion::kFix44 };

  // BeginString value to place on the wire. This may be preserved from an
  // unknown begin string even when the fallback behavior matches FIX.4.4.
  std::string begin_string{ "FIX.4.4" };

  // Logon semantics.
  bool requires_default_appl_ver_id{ false }; // true for FIXT.1.1
  bool supports_reset_on_logon{ true };
  std::uint32_t default_heartbeat_interval_seconds{ 30 };

  // Admin message behavior.
  bool supports_next_expected_msg_seq_num{ true }; // tag 789 in Logon
  bool supports_msg_seq_num_reset{ true };         // SequenceReset-Reset

  // FIXT-specific transport/app-version split.
  bool transport_and_app_version_decoupled{ false }; // true for FIXT.1.1

  // Factory methods for standard profiles.
  static auto Fix40() -> TransportSessionProfile;
  static auto Fix41() -> TransportSessionProfile;
  static auto Fix42() -> TransportSessionProfile;
  static auto Fix43() -> TransportSessionProfile;
  static auto Fix44() -> TransportSessionProfile;
  static auto FixT11() -> TransportSessionProfile;

  /// Resolve a transport profile from the wire `BeginString(8)`.
  ///
  /// Boundary condition: unknown strings fall back to FIX.4.4 behavior while
  /// preserving the caller-supplied `begin_string` for outbound wire use.
  ///
  /// \param begin_string Wire begin string.
  /// \return Normalized transport profile.
  static auto FromBeginString(std::string_view begin_string) -> TransportSessionProfile;
};

} // namespace nimble::session
