#include <iostream>

#include "nimblefix/runtime/soak.h"

namespace {

auto
PrintUsage() -> void
{
  std::cout << "usage: nimblefix-soak --profile <profile.nfa> [--workers N] "
               "[--sessions N] [--iterations N]"
               " [--gap-every N] [--duplicate-every N] [--replay-every N] "
               "[--reorder-every N]"
               " [--drop-every N] [--jitter-every N] [--jitter-step-ms N]"
               " [--disconnect-every N]"
               " [--timer-pulse-every N] [--timer-step-sec N] [--trace]\n";
}

} // namespace

int
main(int argc, char** argv)
{
  nimble::runtime::SoakConfig config;

  for (int index = 1; index < argc; ++index) {
    const std::string_view arg(argv[index]);
    if (arg == "--profile" && index + 1 < argc) {
      config.profile_artifact = argv[++index];
      continue;
    }
    if (arg == "--workers" && index + 1 < argc) {
      config.worker_count = static_cast<std::uint32_t>(std::stoul(argv[++index]));
      continue;
    }
    if (arg == "--sessions" && index + 1 < argc) {
      config.session_count = static_cast<std::uint32_t>(std::stoul(argv[++index]));
      continue;
    }
    if (arg == "--iterations" && index + 1 < argc) {
      config.iterations = static_cast<std::uint32_t>(std::stoul(argv[++index]));
      continue;
    }
    if (arg == "--gap-every" && index + 1 < argc) {
      config.gap_every = static_cast<std::uint32_t>(std::stoul(argv[++index]));
      continue;
    }
    if (arg == "--duplicate-every" && index + 1 < argc) {
      config.duplicate_every = static_cast<std::uint32_t>(std::stoul(argv[++index]));
      continue;
    }
    if (arg == "--replay-every" && index + 1 < argc) {
      config.replay_every = static_cast<std::uint32_t>(std::stoul(argv[++index]));
      continue;
    }
    if (arg == "--reorder-every" && index + 1 < argc) {
      config.reorder_every = static_cast<std::uint32_t>(std::stoul(argv[++index]));
      continue;
    }
    if (arg == "--drop-every" && index + 1 < argc) {
      config.drop_every = static_cast<std::uint32_t>(std::stoul(argv[++index]));
      continue;
    }
    if (arg == "--jitter-every" && index + 1 < argc) {
      config.jitter_every = static_cast<std::uint32_t>(std::stoul(argv[++index]));
      continue;
    }
    if (arg == "--jitter-step-ms" && index + 1 < argc) {
      config.jitter_step_millis = static_cast<std::uint32_t>(std::stoul(argv[++index]));
      continue;
    }
    if (arg == "--disconnect-every" && index + 1 < argc) {
      config.disconnect_every = static_cast<std::uint32_t>(std::stoul(argv[++index]));
      continue;
    }
    if (arg == "--timer-pulse-every" && index + 1 < argc) {
      config.timer_pulse_every = static_cast<std::uint32_t>(std::stoul(argv[++index]));
      continue;
    }
    if (arg == "--timer-step-sec" && index + 1 < argc) {
      config.timer_step_seconds = static_cast<std::uint32_t>(std::stoul(argv[++index]));
      continue;
    }
    if (arg == "--trace") {
      config.enable_trace = true;
      continue;
    }
    PrintUsage();
    return 1;
  }

  if (config.profile_artifact.empty()) {
    PrintUsage();
    return 1;
  }

  auto report = nimble::runtime::RunSoak(config);
  if (!report.ok()) {
    std::cerr << report.status().message() << '\n';
    return 1;
  }

  std::cout << "soak completed: iterations=" << report.value().iterations_completed
            << " inbound=" << report.value().total_inbound_messages
            << " outbound=" << report.value().total_outbound_messages
            << " resend=" << report.value().total_resend_requests << " gapfill=" << report.value().total_gap_fills
            << " replay=" << report.value().total_replay_requests << " reorder=" << report.value().total_reorder_events
            << " drop=" << report.value().total_drop_events << " jitter=" << report.value().total_jitter_events
            << " reconnect=" << report.value().total_reconnects
            << " duplicate=" << report.value().total_duplicate_inbound_messages
            << " timer=" << report.value().total_timer_events << " app=" << report.value().total_application_messages
            << " trace=" << report.value().trace_event_count << '\n';
  return 0;
}