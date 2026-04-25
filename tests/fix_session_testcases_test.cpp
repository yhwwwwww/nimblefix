#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>

#include "nimblefix/runtime/fix_session_testcases.h"

TEST_CASE("official FIX session manifest runner", "[fix-session-testcases]")
{
  const auto manifest_path =
    std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / "tests" / "data" / "fix-session" / "official-session-cases.ffcases";

  auto manifest = nimble::runtime::LoadOfficialCaseManifestFile(manifest_path);
  REQUIRE(manifest.ok());
  REQUIRE(manifest.value().entries.size() == 85U);

  auto summary = nimble::runtime::RunOfficialCaseManifest(manifest.value());
  REQUIRE(summary.ok());
  REQUIRE(summary.value().total_cases == 85U);
  REQUIRE(summary.value().mapped_cases == 73U);
  REQUIRE(summary.value().passed_cases == 73U);
  REQUIRE(summary.value().failed_cases == 0U);
  REQUIRE(summary.value().unsupported_cases == 12U);
  REQUIRE(summary.value().expected_fail_cases == 0U);
  REQUIRE(summary.value().unexpected_pass_cases == 0U);

  const auto report = nimble::runtime::RenderOfficialCaseCoverageReport(summary.value());
  REQUIRE(report.find("FIX Session Layer Coverage") != std::string::npos);
  REQUIRE(report.find("1S.a: pass") != std::string::npos);
  REQUIRE(report.find("1S.b: pass") != std::string::npos);
  REQUIRE(report.find("1S.c: pass") != std::string::npos);
  REQUIRE(report.find("1B.e: pass") != std::string::npos);
  REQUIRE(report.find("2.o: pass") != std::string::npos);
  REQUIRE(report.find("2.r: pass") != std::string::npos);
  REQUIRE(report.find("9: pass") != std::string::npos);
  REQUIRE(report.find("2.t: pass") != std::string::npos);
  REQUIRE(report.find("2.f: pass") != std::string::npos);
  REQUIRE(report.find("14.g: pass") != std::string::npos);
  REQUIRE(report.find("14.j: pass") != std::string::npos);
  REQUIRE(report.find("14.k: pass") != std::string::npos);
  REQUIRE(report.find("14.l: pass") != std::string::npos);
  REQUIRE(report.find("15: pass") != std::string::npos);
  REQUIRE(report.find("14.m: pass") != std::string::npos);
  REQUIRE(report.find("14.n: pass") != std::string::npos);
  REQUIRE(report.find("16.a: pass") != std::string::npos);
  REQUIRE(report.find("16.b: pass") != std::string::npos);
  REQUIRE(report.find("19.a: pass") != std::string::npos);
  REQUIRE(report.find("19.b: pass") != std::string::npos);
  REQUIRE(report.find("20: pass") != std::string::npos);
}

TEST_CASE("official FIX session HTML importer", "[fix-session-testcases]")
{
  const auto html_path =
    std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / "tests" / "data" / "fix-session" / "sample-session-testcases.html";

  auto manifest = nimble::runtime::ImportOfficialCaseHtmlFile(html_path);
  REQUIRE(manifest.ok());
  REQUIRE(manifest.value().entries.size() == 5U);

  std::vector<std::string> ids;
  ids.reserve(manifest.value().entries.size());
  for (const auto& entry : manifest.value().entries) {
    ids.push_back(entry.official_case_id);
  }
  std::sort(ids.begin(), ids.end());

  REQUIRE(ids[0] == "1B.a");
  REQUIRE(ids[1] == "1B.b");
  REQUIRE(ids[2] == "2S");
  REQUIRE(ids[3] == "4.a");
  REQUIRE(ids[4] == "4.b");
}