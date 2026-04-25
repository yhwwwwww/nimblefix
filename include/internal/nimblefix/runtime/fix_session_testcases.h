#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "nimblefix/base/result.h"

namespace nimble::runtime {

enum class OfficialCaseRole : std::uint32_t
{
  kInitiator = 0,
  kAcceptor,
  kAll,
};

enum class OfficialCaseRequirement : std::uint32_t
{
  kMandatory = 0,
  kOptional,
};

enum class OfficialCaseSupport : std::uint32_t
{
  kMapped = 0,
  kUnsupported,
  kExpectedFail,
};

enum class OfficialCaseOutcome : std::uint32_t
{
  kPassed = 0,
  kFailed,
  kUnsupported,
  kExpectedFail,
  kUnexpectedPass,
};

struct OfficialCaseManifestEntry
{
  std::string official_case_id;
  OfficialCaseRole role{ OfficialCaseRole::kAll };
  OfficialCaseRequirement requirement{ OfficialCaseRequirement::kMandatory };
  std::vector<std::string> protocol_versions;
  OfficialCaseSupport support{ OfficialCaseSupport::kUnsupported };
  std::filesystem::path scenario_path;
  std::string note;
};

struct OfficialCaseManifest
{
  std::string source_name;
  std::string source_version;
  std::string source_url;
  std::vector<OfficialCaseManifestEntry> entries;
};

struct OfficialCaseResult
{
  std::string official_case_id;
  OfficialCaseRole role{ OfficialCaseRole::kAll };
  OfficialCaseRequirement requirement{ OfficialCaseRequirement::kMandatory };
  std::vector<std::string> protocol_versions;
  OfficialCaseSupport support{ OfficialCaseSupport::kUnsupported };
  OfficialCaseOutcome outcome{ OfficialCaseOutcome::kUnsupported };
  std::filesystem::path scenario_path;
  std::string note;
  std::string message;
};

struct OfficialCaseRunSummary
{
  std::string source_name;
  std::string source_version;
  std::string source_url;
  std::size_t total_cases{ 0 };
  std::size_t mapped_cases{ 0 };
  std::size_t passed_cases{ 0 };
  std::size_t failed_cases{ 0 };
  std::size_t unsupported_cases{ 0 };
  std::size_t expected_fail_cases{ 0 };
  std::size_t unexpected_pass_cases{ 0 };
  std::vector<OfficialCaseResult> results;
};

auto LoadOfficialCaseManifestText(std::string_view text,
                                  const std::filesystem::path& base_dir = {}) -> base::Result<OfficialCaseManifest>;
auto LoadOfficialCaseManifestFile(const std::filesystem::path& path) -> base::Result<OfficialCaseManifest>;
auto SerializeOfficialCaseManifest(const OfficialCaseManifest& manifest,
                                   const std::filesystem::path& base_dir = {}) -> std::string;
auto ImportOfficialCaseHtmlText(std::string_view text) -> base::Result<OfficialCaseManifest>;
auto ImportOfficialCaseHtmlFile(const std::filesystem::path& path) -> base::Result<OfficialCaseManifest>;
auto RunOfficialCaseManifest(const OfficialCaseManifest& manifest) -> base::Result<OfficialCaseRunSummary>;
auto RenderOfficialCaseCoverageReport(const OfficialCaseRunSummary& summary) -> std::string;

} // namespace nimble::runtime