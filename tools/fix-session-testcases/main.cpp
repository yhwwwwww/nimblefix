#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string_view>

#include "nimblefix/runtime/fix_session_testcases.h"

namespace {

auto
PrintUsage() -> void
{
  std::cout
    << "usage:\n"
    << "  nimblefix-fix-session-testcases --manifest <cases.nfcases> [--report <coverage.md>] [--filter <prefix>]\n"
    << "  nimblefix-fix-session-testcases --import-html <session-cases.html> --output <cases.nfcases>\n";
}

} // namespace

int
main(int argc, char** argv)
{
  std::filesystem::path manifest_path;
  std::filesystem::path report_path;
  std::filesystem::path import_html_path;
  std::filesystem::path output_path;
  std::optional<std::string> filter_prefix;

  for (int index = 1; index < argc; ++index) {
    const std::string_view arg(argv[index]);
    if (arg == "--manifest" && index + 1 < argc) {
      manifest_path = argv[++index];
      continue;
    }
    if (arg == "--report" && index + 1 < argc) {
      report_path = argv[++index];
      continue;
    }
    if (arg == "--filter" && index + 1 < argc) {
      filter_prefix = argv[++index];
      continue;
    }
    if (arg == "--import-html" && index + 1 < argc) {
      import_html_path = argv[++index];
      continue;
    }
    if (arg == "--output" && index + 1 < argc) {
      output_path = argv[++index];
      continue;
    }
    PrintUsage();
    return 1;
  }

  if (!import_html_path.empty()) {
    if (output_path.empty()) {
      PrintUsage();
      return 1;
    }

    auto imported = nimble::runtime::ImportOfficialCaseHtmlFile(import_html_path);
    if (!imported.ok()) {
      std::cerr << imported.status().message() << '\n';
      return 1;
    }

    std::ofstream out(output_path, std::ios::trunc);
    if (!out.is_open()) {
      std::cerr << "unable to write output file: " << output_path.string() << '\n';
      return 1;
    }
    out << nimble::runtime::SerializeOfficialCaseManifest(imported.value(), output_path.parent_path());
    return 0;
  }

  if (manifest_path.empty()) {
    PrintUsage();
    return 1;
  }

  auto manifest = nimble::runtime::LoadOfficialCaseManifestFile(manifest_path);
  if (!manifest.ok()) {
    std::cerr << manifest.status().message() << '\n';
    return 1;
  }

  nimble::runtime::OfficialCaseManifest filtered = manifest.value();
  if (filter_prefix.has_value()) {
    filtered.entries.clear();
    for (const auto& entry : manifest.value().entries) {
      if (std::string_view(entry.official_case_id).starts_with(*filter_prefix)) {
        filtered.entries.push_back(entry);
      }
    }
  }

  auto summary = nimble::runtime::RunOfficialCaseManifest(filtered);
  if (!summary.ok()) {
    std::cerr << summary.status().message() << '\n';
    return 1;
  }

  for (const auto& result : summary.value().results) {
    std::cout << result.official_case_id << ' ';
    switch (result.outcome) {
      case nimble::runtime::OfficialCaseOutcome::kPassed:
        std::cout << "pass";
        break;
      case nimble::runtime::OfficialCaseOutcome::kFailed:
        std::cout << "fail";
        break;
      case nimble::runtime::OfficialCaseOutcome::kUnsupported:
        std::cout << "unsupported";
        break;
      case nimble::runtime::OfficialCaseOutcome::kExpectedFail:
        std::cout << "xfail";
        break;
      case nimble::runtime::OfficialCaseOutcome::kUnexpectedPass:
        std::cout << "unexpected-pass";
        break;
    }
    if (!result.message.empty()) {
      std::cout << " - " << result.message;
    }
    std::cout << '\n';
  }

  std::cout << "summary: total=" << summary.value().total_cases << " mapped=" << summary.value().mapped_cases
            << " pass=" << summary.value().passed_cases << " fail=" << summary.value().failed_cases
            << " unsupported=" << summary.value().unsupported_cases << " xfail=" << summary.value().expected_fail_cases
            << " unexpected-pass=" << summary.value().unexpected_pass_cases << '\n';

  if (!report_path.empty()) {
    std::ofstream out(report_path, std::ios::trunc);
    if (!out.is_open()) {
      std::cerr << "unable to write report file: " << report_path.string() << '\n';
      return 1;
    }
    out << nimble::runtime::RenderOfficialCaseCoverageReport(summary.value());
  }

  return summary.value().failed_cases == 0U && summary.value().unexpected_pass_cases == 0U ? 0 : 1;
}