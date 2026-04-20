#include <iostream>

#include "nimblefix/runtime/interop_harness.h"

namespace {

auto
PrintUsage() -> void
{
  std::cout << "usage: nimblefix-interop-runner --scenario <scenario.ffscenario>\n";
}

} // namespace

int
main(int argc, char** argv)
{
  std::filesystem::path scenario_path;
  for (int index = 1; index < argc; ++index) {
    const std::string_view arg(argv[index]);
    if (arg == "--scenario" && index + 1 < argc) {
      scenario_path = argv[++index];
      continue;
    }
    PrintUsage();
    return 1;
  }

  if (scenario_path.empty()) {
    PrintUsage();
    return 1;
  }

  auto scenario = nimble::runtime::LoadInteropScenarioFile(scenario_path);
  if (!scenario.ok()) {
    std::cerr << scenario.status().message() << '\n';
    return 1;
  }

  auto report = nimble::runtime::RunInteropScenario(scenario.value());
  if (!report.ok()) {
    std::cerr << report.status().message() << '\n';
    return 1;
  }

  std::cout << "interop scenario passed with " << report.value().sessions.size() << " sessions, "
            << report.value().metrics.sessions.size() << " metric entries, and " << report.value().trace_events.size()
            << " trace events\n";
  return 0;
}