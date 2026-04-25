#include <iostream>

#include "nimblefix/runtime/interop_harness.h"

namespace {

auto
PrintUsage() -> void
{
  std::cout << "usage: nimblefix-interop-runner --scenario <scenario.ffscenario> [--dump-report]\n";
}

} // namespace

int
main(int argc, char** argv)
{
  std::filesystem::path scenario_path;
  bool dump_report = false;
  for (int index = 1; index < argc; ++index) {
    const std::string_view arg(argv[index]);
    if (arg == "--scenario" && index + 1 < argc) {
      scenario_path = argv[++index];
      continue;
    }
    if (arg == "--dump-report") {
      dump_report = true;
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
  if (dump_report) {
    for (const auto& session : report.value().sessions) {
      std::cout << "session " << session.session_id << " state=" << static_cast<unsigned>(session.state)
                << " next_in=" << session.next_in_seq << " next_out=" << session.next_out_seq
                << " pending_resend=" << (session.has_pending_resend ? 1 : 0) << '\n';
    }
    for (std::size_t index = 0; index < report.value().action_reports.size(); ++index) {
      const auto& action = report.value().action_reports[index];
      std::cout << "action " << (index + 1U) << " session=" << action.session_id
                << " outbound=" << action.outbound_frames << " app=" << action.application_messages
                << " active=" << (action.session_active ? 1 : 0) << " disconnect=" << (action.disconnect ? 1 : 0)
                << " session_reject=" << (action.session_reject ? 1 : 0) << '\n';
    }
  }
  return 0;
}