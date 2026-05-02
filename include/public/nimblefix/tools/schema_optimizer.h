#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "nimblefix/base/result.h"
#include "nimblefix/profile/normalized_dictionary.h"

namespace nimble::tools {

/// Per-message-type tag usage statistics.
struct MessageTagUsage
{
  std::string msg_type;
  std::string msg_name;             // from dictionary (if available)
  std::uint64_t message_count{ 0 }; // total messages seen
  /// tag -> count of messages where this tag appeared
  std::map<std::uint32_t, std::uint64_t> tag_counts;
  /// Maximum numeric tag seen in this message type
  std::uint32_t max_tag{ 0 };
};

/// Overall schema usage analysis result.
struct SchemaUsageReport
{
  std::uint64_t total_messages{ 0 };
  std::map<std::string, MessageTagUsage> by_msg_type; // msg_type -> usage
  std::set<std::uint32_t> all_observed_tags;          // union of all tags seen
  std::uint32_t global_max_tag{ 0 };
};

/// Configuration for schema optimization.
struct SchemaOptimizerConfig
{
  /// Minimum frequency (0.0 - 1.0) for a tag to be included.
  /// Tags appearing in fewer than this fraction of messages are trimmed.
  double min_frequency{ 0.0 };
  /// Always include these tags regardless of frequency (e.g., required session tags).
  std::set<std::uint32_t> always_include_tags;
  /// Profile ID for the generated .nfd.
  std::uint64_t profile_id{ 0 };
};

/// FixedLayout size estimate for a message type.
struct LayoutSizeEstimate
{
  std::string msg_type;
  std::uint32_t original_max_tag{ 0 };
  std::uint32_t trimmed_max_tag{ 0 };
  std::size_t original_field_slots{ 0 };
  std::size_t trimmed_field_slots{ 0 };
  /// Estimated tag_to_slot_ vector size reduction in bytes (sizeof(int) * (original - trimmed))
  std::size_t tag_to_slot_bytes_saved{ 0 };
};

/// Schema optimization result.
struct SchemaOptimizationResult
{
  std::string trimmed_nfd_text;
  std::vector<LayoutSizeEstimate> layout_estimates;
  std::uint64_t total_bytes_saved{ 0 };
};

/// Analyze raw FIX messages to build tag usage statistics.
/// Each message is a string_view of the raw FIX tag=value body (with SOH delimiters).
auto
AnalyzeMessages(const std::vector<std::string_view>& messages, char delimiter = '\x01') -> SchemaUsageReport;

/// Analyze raw FIX messages provided as byte spans.
auto
AnalyzeMessageBytes(const std::vector<std::span<const std::byte>>& messages, char delimiter = '\x01')
  -> SchemaUsageReport;

/// Generate a trimmed .nfd dictionary text from a usage report and a base dictionary.
/// Tags not observed (or below min_frequency) are removed from message field rules.
/// Fields still referenced by at least one message are kept in the field definitions.
auto
GenerateTrimmedNfd(const SchemaUsageReport& report,
                   const profile::NormalizedDictionary& base_dictionary,
                   const SchemaOptimizerConfig& config) -> SchemaOptimizationResult;

/// Estimate FixedLayout size for each message type given original and trimmed dictionaries.
auto
EstimateLayoutSizes(const profile::NormalizedDictionary& original, const profile::NormalizedDictionary& trimmed)
  -> std::vector<LayoutSizeEstimate>;

/// Format a usage report as human-readable text.
auto
FormatUsageReport(const SchemaUsageReport& report) -> std::string;

/// Format optimization results as human-readable text.
auto
FormatOptimizationResult(const SchemaOptimizationResult& result) -> std::string;

} // namespace nimble::tools
