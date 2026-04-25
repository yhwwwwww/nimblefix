#include "nimblefix/runtime/fix_session_testcases.h"

#include "nimblefix/runtime/interop_harness.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace nimble::runtime {

namespace {

constexpr char kCommentPrefix = '#';
constexpr char kFieldSeparator = '|';

auto
Trim(std::string_view input) -> std::string_view
{
  std::size_t begin = 0;
  while (begin < input.size() && std::isspace(static_cast<unsigned char>(input[begin])) != 0) {
    ++begin;
  }

  std::size_t end = input.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
    --end;
  }
  return input.substr(begin, end - begin);
}

auto
Split(std::string_view input, char delimiter) -> std::vector<std::string>
{
  std::vector<std::string> parts;
  std::size_t begin = 0;
  while (begin <= input.size()) {
    const auto end = input.find(delimiter, begin);
    if (end == std::string_view::npos) {
      parts.emplace_back(Trim(input.substr(begin)));
      break;
    }
    parts.emplace_back(Trim(input.substr(begin, end - begin)));
    begin = end + 1;
  }
  return parts;
}

auto
SplitLines(std::string_view text) -> std::vector<std::string>
{
  std::vector<std::string> lines;
  std::string current;
  for (const auto ch : text) {
    if (ch == '\r') {
      continue;
    }
    if (ch == '\n') {
      lines.push_back(current);
      current.clear();
      continue;
    }
    current.push_back(ch);
  }
  if (!current.empty() || text.empty()) {
    lines.push_back(std::move(current));
  }
  return lines;
}

auto
ParseRole(std::string_view token) -> base::Result<OfficialCaseRole>
{
  const auto value = Trim(token);
  if (value == "initiator") {
    return OfficialCaseRole::kInitiator;
  }
  if (value == "acceptor") {
    return OfficialCaseRole::kAcceptor;
  }
  if (value == "all") {
    return OfficialCaseRole::kAll;
  }
  return base::Status::InvalidArgument("unknown official case role");
}

auto
ParseRequirement(std::string_view token) -> base::Result<OfficialCaseRequirement>
{
  const auto value = Trim(token);
  if (value == "mandatory") {
    return OfficialCaseRequirement::kMandatory;
  }
  if (value == "optional") {
    return OfficialCaseRequirement::kOptional;
  }
  return base::Status::InvalidArgument("unknown official case requirement");
}

auto
ParseSupport(std::string_view token) -> base::Result<OfficialCaseSupport>
{
  const auto value = Trim(token);
  if (value == "mapped") {
    return OfficialCaseSupport::kMapped;
  }
  if (value == "unsupported") {
    return OfficialCaseSupport::kUnsupported;
  }
  if (value == "xfail") {
    return OfficialCaseSupport::kExpectedFail;
  }
  return base::Status::InvalidArgument("unknown official case support status");
}

auto
RoleText(OfficialCaseRole role) -> std::string_view
{
  switch (role) {
    case OfficialCaseRole::kInitiator:
      return "initiator";
    case OfficialCaseRole::kAcceptor:
      return "acceptor";
    case OfficialCaseRole::kAll:
      return "all";
  }
  return "all";
}

auto
RequirementText(OfficialCaseRequirement requirement) -> std::string_view
{
  switch (requirement) {
    case OfficialCaseRequirement::kMandatory:
      return "mandatory";
    case OfficialCaseRequirement::kOptional:
      return "optional";
  }
  return "mandatory";
}

auto
SupportText(OfficialCaseSupport support) -> std::string_view
{
  switch (support) {
    case OfficialCaseSupport::kMapped:
      return "mapped";
    case OfficialCaseSupport::kUnsupported:
      return "unsupported";
    case OfficialCaseSupport::kExpectedFail:
      return "xfail";
  }
  return "unsupported";
}

auto
OutcomeText(OfficialCaseOutcome outcome) -> std::string_view
{
  switch (outcome) {
    case OfficialCaseOutcome::kPassed:
      return "pass";
    case OfficialCaseOutcome::kFailed:
      return "fail";
    case OfficialCaseOutcome::kUnsupported:
      return "unsupported";
    case OfficialCaseOutcome::kExpectedFail:
      return "xfail";
    case OfficialCaseOutcome::kUnexpectedPass:
      return "unexpected-pass";
  }
  return "unsupported";
}

auto
SplitVersions(std::string_view text) -> std::vector<std::string>
{
  if (Trim(text).empty()) {
    return {};
  }
  return Split(text, ',');
}

auto
JoinVersions(const std::vector<std::string>& versions) -> std::string
{
  std::string joined;
  for (std::size_t index = 0; index < versions.size(); ++index) {
    if (index != 0U) {
      joined.push_back(',');
    }
    joined.append(versions[index]);
  }
  return joined;
}

auto
ReadFileText(const std::filesystem::path& path) -> base::Result<std::string>
{
  std::ifstream in(path);
  if (!in.is_open()) {
    return base::Status::IoError("unable to open file: '" + path.string() + "'");
  }
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

auto
DecodeHtmlEntity(std::string_view entity) -> std::string_view
{
  if (entity == "nbsp") {
    return " ";
  }
  if (entity == "amp") {
    return "&";
  }
  if (entity == "lt") {
    return "<";
  }
  if (entity == "gt") {
    return ">";
  }
  if (entity == "quot") {
    return "\"";
  }
  if (entity == "apos") {
    return "'";
  }
  return {};
}

auto
StripHtml(std::string_view text) -> std::string
{
  std::string output;
  output.reserve(text.size());
  bool in_tag = false;
  bool in_entity = false;
  std::string entity;

  for (const auto ch : text) {
    if (in_tag) {
      if (ch == '>') {
        in_tag = false;
        output.push_back('\n');
      }
      continue;
    }

    if (in_entity) {
      if (ch == ';') {
        const auto decoded = DecodeHtmlEntity(entity);
        if (!decoded.empty()) {
          output.append(decoded);
        }
        entity.clear();
        in_entity = false;
        continue;
      }
      entity.push_back(ch);
      continue;
    }

    if (ch == '<') {
      in_tag = true;
      continue;
    }
    if (ch == '&') {
      in_entity = true;
      entity.clear();
      continue;
    }
    output.push_back(ch);
  }

  return output;
}

auto
NormalizeScenarioId(std::string_view scenario_id) -> std::string
{
  auto value = std::string(Trim(scenario_id));
  while (!value.empty() && value.back() == '.') {
    value.pop_back();
  }
  return value;
}

auto
ExtractScenarioHeader(std::string_view line, std::string* scenario_id) -> bool
{
  if (!line.starts_with("Scenario ")) {
    return false;
  }
  const auto remainder = Trim(line.substr(std::string_view("Scenario ").size()));
  const auto first_space = remainder.find(' ');
  if (first_space == std::string_view::npos) {
    *scenario_id = NormalizeScenarioId(remainder);
    return true;
  }
  *scenario_id = NormalizeScenarioId(remainder.substr(0, first_space));
  return !scenario_id->empty();
}

auto
ExtractSubcaseLetter(std::string_view line) -> char
{
  if (line.size() >= 2U && std::islower(static_cast<unsigned char>(line[0])) != 0 && line[1] == '.') {
    return line[0];
  }
  return '\0';
}

auto
LooksLikeContentLine(std::string_view line) -> bool
{
  if (line.empty()) {
    return false;
  }
  if (line == "Mandatory" || line == "Optional") {
    return false;
  }
  if (line.starts_with("Scenario ")) {
    return false;
  }
  if (line.starts_with("#") || line.starts_with("FIX session layer test cases") || line == "Test Cases") {
    return false;
  }
  return true;
}

} // namespace

auto
LoadOfficialCaseManifestText(std::string_view text, const std::filesystem::path& base_dir)
  -> base::Result<OfficialCaseManifest>
{
  OfficialCaseManifest manifest;

  for (const auto& raw_line : SplitLines(text)) {
    const auto trimmed = Trim(raw_line);
    if (trimmed.empty() || trimmed.starts_with(kCommentPrefix)) {
      continue;
    }

    const auto parts = Split(trimmed, kFieldSeparator);
    if (parts.empty()) {
      continue;
    }

    if (parts[0] == "source") {
      if (parts.size() != 4U) {
        return base::Status::InvalidArgument("source record requires 4 fields");
      }
      manifest.source_name = parts[1];
      manifest.source_version = parts[2];
      manifest.source_url = parts[3];
      continue;
    }

    if (parts[0] == "case") {
      if (parts.size() != 8U) {
        return base::Status::InvalidArgument("case record requires 8 fields");
      }
      auto role = ParseRole(parts[2]);
      auto requirement = ParseRequirement(parts[3]);
      auto support = ParseSupport(parts[5]);
      if (!role.ok()) {
        return role.status();
      }
      if (!requirement.ok()) {
        return requirement.status();
      }
      if (!support.ok()) {
        return support.status();
      }

      OfficialCaseManifestEntry entry;
      entry.official_case_id = parts[1];
      entry.role = role.value();
      entry.requirement = requirement.value();
      entry.protocol_versions = SplitVersions(parts[4]);
      entry.support = support.value();
      if (!parts[6].empty()) {
        entry.scenario_path = base_dir / parts[6];
      }
      entry.note = parts[7];
      manifest.entries.push_back(std::move(entry));
      continue;
    }

    return base::Status::InvalidArgument("unknown official case manifest record kind");
  }

  if (manifest.source_name.empty()) {
    return base::Status::InvalidArgument("official case manifest is missing source metadata");
  }
  return manifest;
}

auto
LoadOfficialCaseManifestFile(const std::filesystem::path& path) -> base::Result<OfficialCaseManifest>
{
  auto text = ReadFileText(path);
  if (!text.ok()) {
    return text.status();
  }
  return LoadOfficialCaseManifestText(text.value(), path.parent_path());
}

auto
SerializeOfficialCaseManifest(const OfficialCaseManifest& manifest, const std::filesystem::path& base_dir) -> std::string
{
  std::string output;
  output.append("source|");
  output.append(manifest.source_name);
  output.push_back('|');
  output.append(manifest.source_version);
  output.push_back('|');
  output.append(manifest.source_url);
  output.push_back('\n');

  for (const auto& entry : manifest.entries) {
    output.append("case|");
    output.append(entry.official_case_id);
    output.push_back('|');
    output.append(RoleText(entry.role));
    output.push_back('|');
    output.append(RequirementText(entry.requirement));
    output.push_back('|');
    output.append(JoinVersions(entry.protocol_versions));
    output.push_back('|');
    output.append(SupportText(entry.support));
    output.push_back('|');
    if (!entry.scenario_path.empty()) {
      if (!base_dir.empty()) {
        output.append(entry.scenario_path.lexically_relative(base_dir).string());
      } else {
        output.append(entry.scenario_path.string());
      }
    }
    output.push_back('|');
    output.append(entry.note);
    output.push_back('\n');
  }

  return output;
}

auto
ImportOfficialCaseHtmlText(std::string_view text) -> base::Result<OfficialCaseManifest>
{
  OfficialCaseManifest manifest;
  manifest.source_name = "FIX Session Layer Test Cases";
  manifest.source_version = "1.0";
  manifest.source_url = "https://dev.fixtrading.org/standards/fix-session-testcases-online/";

  const auto stripped = StripHtml(text);
  auto lines = SplitLines(stripped);

  OfficialCaseRole current_role = OfficialCaseRole::kAll;
  OfficialCaseRequirement current_requirement = OfficialCaseRequirement::kMandatory;
  std::string current_scenario_id;
  bool current_requirement_known = false;
  bool current_scenario_has_subcases = false;
  bool current_scenario_emitted_base = false;

  for (const auto& raw_line : lines) {
    const auto line = Trim(raw_line);
    if (line.empty()) {
      continue;
    }

    if (line.find("Buyside-oriented") != std::string_view::npos) {
      current_role = OfficialCaseRole::kInitiator;
      continue;
    }
    if (line.find("Sellside-oriented") != std::string_view::npos) {
      current_role = OfficialCaseRole::kAcceptor;
      continue;
    }
    if (line.find("Test cases applicable to all FIX systems") != std::string_view::npos) {
      current_role = OfficialCaseRole::kAll;
      continue;
    }

    std::string scenario_id;
    if (ExtractScenarioHeader(line, &scenario_id)) {
      current_scenario_id = std::move(scenario_id);
      current_requirement_known = false;
      current_scenario_has_subcases = false;
      current_scenario_emitted_base = false;
      continue;
    }

    if (line == "Mandatory") {
      current_requirement = OfficialCaseRequirement::kMandatory;
      current_requirement_known = true;
      continue;
    }
    if (line == "Optional") {
      current_requirement = OfficialCaseRequirement::kOptional;
      current_requirement_known = true;
      continue;
    }

    if (current_scenario_id.empty() || !current_requirement_known) {
      continue;
    }

    const auto subcase_letter = ExtractSubcaseLetter(line);
    if (subcase_letter != '\0') {
      current_scenario_has_subcases = true;
      current_scenario_emitted_base = true;
      manifest.entries.push_back(OfficialCaseManifestEntry{
        .official_case_id = current_scenario_id + "." + std::string(1U, subcase_letter),
        .role = current_role,
        .requirement = current_requirement,
        .protocol_versions = { "FIX.4.2", "FIX.4.4", "FIXT.1.1" },
        .support = OfficialCaseSupport::kUnsupported,
        .note = "imported skeleton",
      });
      continue;
    }

    if (!current_scenario_has_subcases && !current_scenario_emitted_base && LooksLikeContentLine(line)) {
      current_scenario_emitted_base = true;
      manifest.entries.push_back(OfficialCaseManifestEntry{
        .official_case_id = current_scenario_id,
        .role = current_role,
        .requirement = current_requirement,
        .protocol_versions = { "FIX.4.2", "FIX.4.4", "FIXT.1.1" },
        .support = OfficialCaseSupport::kUnsupported,
        .note = "imported skeleton",
      });
    }
  }

  if (manifest.entries.empty()) {
    return base::Status::InvalidArgument("no official case ids were discovered in the supplied HTML");
  }

  std::sort(manifest.entries.begin(), manifest.entries.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.official_case_id < rhs.official_case_id;
  });
  manifest.entries.erase(std::unique(manifest.entries.begin(), manifest.entries.end(), [](const auto& lhs, const auto& rhs) {
                           return lhs.official_case_id == rhs.official_case_id;
                         }),
                         manifest.entries.end());
  return manifest;
}

auto
ImportOfficialCaseHtmlFile(const std::filesystem::path& path) -> base::Result<OfficialCaseManifest>
{
  auto text = ReadFileText(path);
  if (!text.ok()) {
    return text.status();
  }
  return ImportOfficialCaseHtmlText(text.value());
}

auto
RunOfficialCaseManifest(const OfficialCaseManifest& manifest) -> base::Result<OfficialCaseRunSummary>
{
  OfficialCaseRunSummary summary;
  summary.source_name = manifest.source_name;
  summary.source_version = manifest.source_version;
  summary.source_url = manifest.source_url;
  summary.total_cases = manifest.entries.size();
  summary.results.reserve(manifest.entries.size());

  for (const auto& entry : manifest.entries) {
    OfficialCaseResult result;
    result.official_case_id = entry.official_case_id;
    result.role = entry.role;
    result.requirement = entry.requirement;
    result.protocol_versions = entry.protocol_versions;
    result.support = entry.support;
    result.scenario_path = entry.scenario_path;
    result.note = entry.note;

    switch (entry.support) {
      case OfficialCaseSupport::kUnsupported:
        result.outcome = OfficialCaseOutcome::kUnsupported;
        result.message = entry.note.empty() ? "unsupported" : entry.note;
        ++summary.unsupported_cases;
        break;
      case OfficialCaseSupport::kMapped:
      case OfficialCaseSupport::kExpectedFail: {
        if (entry.scenario_path.empty()) {
          return base::Status::InvalidArgument("mapped official case is missing a scenario path");
        }
        ++summary.mapped_cases;

        auto scenario = LoadInteropScenarioFile(entry.scenario_path);
        if (!scenario.ok()) {
          result.outcome = entry.support == OfficialCaseSupport::kExpectedFail ? OfficialCaseOutcome::kExpectedFail
                                                                              : OfficialCaseOutcome::kFailed;
          result.message = scenario.status().message();
        } else {
          auto run = RunInteropScenario(scenario.value());
          if (run.ok()) {
            if (entry.support == OfficialCaseSupport::kExpectedFail) {
              result.outcome = OfficialCaseOutcome::kUnexpectedPass;
              result.message = "scenario passed unexpectedly";
              ++summary.unexpected_pass_cases;
            } else {
              result.outcome = OfficialCaseOutcome::kPassed;
              result.message = "scenario passed";
              ++summary.passed_cases;
            }
          } else {
            if (entry.support == OfficialCaseSupport::kExpectedFail) {
              result.outcome = OfficialCaseOutcome::kExpectedFail;
              result.message = run.status().message();
              ++summary.expected_fail_cases;
            } else {
              result.outcome = OfficialCaseOutcome::kFailed;
              result.message = run.status().message();
            }
          }
        }

        if (result.outcome == OfficialCaseOutcome::kFailed) {
          ++summary.failed_cases;
        }
        break;
      }
    }

    summary.results.push_back(std::move(result));
  }

  return summary;
}

auto
RenderOfficialCaseCoverageReport(const OfficialCaseRunSummary& summary) -> std::string
{
  std::string output;
  output.append("# FIX Session Layer Coverage\n\n");
  output.append("- Source: ");
  output.append(summary.source_name);
  output.append(" v");
  output.append(summary.source_version);
  output.push_back('\n');
  output.append("- URL: ");
  output.append(summary.source_url);
  output.push_back('\n');
  output.append("- Total cases: ");
  output.append(std::to_string(summary.total_cases));
  output.push_back('\n');
  output.append("- Mapped: ");
  output.append(std::to_string(summary.mapped_cases));
  output.push_back('\n');
  output.append("- Passed: ");
  output.append(std::to_string(summary.passed_cases));
  output.push_back('\n');
  output.append("- Failed: ");
  output.append(std::to_string(summary.failed_cases));
  output.push_back('\n');
  output.append("- Unsupported: ");
  output.append(std::to_string(summary.unsupported_cases));
  output.push_back('\n');
  output.append("- Expected fail: ");
  output.append(std::to_string(summary.expected_fail_cases));
  output.push_back('\n');
  output.append("- Unexpected pass: ");
  output.append(std::to_string(summary.unexpected_pass_cases));
  output.append("\n\n");

  output.append("## Results\n\n");
  for (const auto& result : summary.results) {
    output.append("- ");
    output.append(result.official_case_id);
    output.append(": ");
    output.append(OutcomeText(result.outcome));
    if (!result.message.empty()) {
      output.append(" - ");
      output.append(result.message);
    }
    output.push_back('\n');
  }

  return output;
}

} // namespace nimble::runtime