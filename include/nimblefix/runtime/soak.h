#pragma once

#include <cstdint>
#include <filesystem>

#include "nimblefix/base/result.h"
#include "nimblefix/runtime/metrics.h"

namespace nimble::runtime {

struct SoakConfig
{
  std::filesystem::path profile_artifact;
  std::uint32_t worker_count{ 1 };
  std::uint32_t session_count{ 1 };
  std::uint32_t iterations{ 1 };
  std::uint32_t gap_every{ 16 };
  std::uint32_t duplicate_every{ 8 };
  std::uint32_t replay_every{ 16 };
  std::uint32_t reorder_every{ 0 };
  std::uint32_t drop_every{ 0 };
  std::uint32_t jitter_every{ 0 };
  std::uint32_t jitter_step_millis{ 250 };
  std::uint32_t disconnect_every{ 0 };
  std::uint32_t timer_pulse_every{ 12 };
  std::uint32_t timer_step_seconds{ 31 };
  bool enable_trace{ false };
};

struct SoakReport
{
  std::uint64_t iterations_completed{ 0 };
  std::uint64_t total_inbound_messages{ 0 };
  std::uint64_t total_outbound_messages{ 0 };
  std::uint64_t total_resend_requests{ 0 };
  std::uint64_t total_gap_fills{ 0 };
  std::uint64_t total_replay_requests{ 0 };
  std::uint64_t total_reorder_events{ 0 };
  std::uint64_t total_drop_events{ 0 };
  std::uint64_t total_jitter_events{ 0 };
  std::uint64_t total_reconnects{ 0 };
  std::uint64_t total_duplicate_inbound_messages{ 0 };
  std::uint64_t total_timer_events{ 0 };
  std::uint64_t total_application_messages{ 0 };
  RuntimeMetricsSnapshot metrics;
  std::size_t trace_event_count{ 0 };
};

auto
RunSoak(const SoakConfig& config) -> base::Result<SoakReport>;

} // namespace nimble::runtime