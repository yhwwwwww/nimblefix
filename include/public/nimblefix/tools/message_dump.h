#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace nimble::tools {

enum class DumpFormat : std::uint32_t
{
  kFixReadable = 0, // Tag=Value|Tag=Value|...
  kJson = 1,        // JSON object per message
};

struct DumpFilter
{
  std::optional<std::string> msg_type;     // Filter by MsgType
  std::optional<std::uint64_t> session_id; // Store runtime SessionID; CLI applies this before message filtering.
  std::optional<std::uint32_t> tag;        // Filter by presence of specific tag
  std::optional<std::string> tag_value;    // Filter by tag=value (requires tag)
  std::optional<std::uint32_t> seq_from;   // Start sequence
  std::optional<std::uint32_t> seq_to;     // End sequence
};

struct DumpOptions
{
  DumpFormat format{ DumpFormat::kFixReadable };
  DumpFilter filter;
  bool include_admin{ false };
  std::uint32_t limit{ 0 }; // 0 = unlimited
};

struct DumpEntry
{
  std::uint32_t sequence_number{ 0 };
  std::string direction; // "inbound" or "outbound"
  std::string msg_type;
  std::string raw_fix;
  std::string formatted;
};

/// Format a single FIX message as human-readable text.
[[nodiscard]] auto
FormatFixReadable(std::string_view raw_fix) -> std::string;

/// Format a single FIX message as JSON.
[[nodiscard]] auto
FormatFixJson(std::string_view raw_fix) -> std::string;

/// Format a FIX message in the specified format.
[[nodiscard]] auto
FormatFixMessage(std::string_view raw_fix, DumpFormat format) -> std::string;

/// Check if a raw FIX message matches the given filter.
[[nodiscard]] auto
MatchesFilter(std::string_view raw_fix, const DumpFilter& filter) -> bool;

} // namespace nimble::tools
