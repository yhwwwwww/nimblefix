#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "nimblefix/base/result.h"
#include "nimblefix/base/status.h"
#include "nimblefix/runtime/config.h"
#include "nimblefix/runtime/metrics.h"
#include "nimblefix/runtime/trace.h"
#include "nimblefix/session/session_core.h"

namespace nimble::runtime {

enum class InteropActionKind : std::uint32_t
{
  kConnect = 0,
  kLogon,
  kInbound,
  kOutbound,
  kGap,
  kCompleteResend,
  kSaveRecovery,
  kRecoverWarm,
  kRecoverCold,
};

struct InteropAction
{
  InteropActionKind kind{ InteropActionKind::kConnect };
  std::uint64_t session_id{ 0 };
  std::uint32_t seq_num{ 0 };
  bool is_admin{ false };
  std::uint64_t timestamp_ns{ 0 };
  std::string text;
};

struct InteropSessionExpectation
{
  std::uint64_t session_id{ 0 };
  session::SessionState state{ session::SessionState::kDisconnected };
  std::uint32_t next_in_seq{ 1 };
  std::uint32_t next_out_seq{ 1 };
  bool has_pending_resend{ false };
};

struct InteropMetricExpectation
{
  std::uint64_t session_id{ 0 };
  std::uint64_t inbound_messages{ 0 };
  std::uint64_t outbound_messages{ 0 };
  std::uint64_t admin_messages{ 0 };
  std::uint64_t resend_requests{ 0 };
  std::uint64_t gap_fills{ 0 };
};

struct InteropScenario
{
  EngineConfig engine_config;
  std::vector<InteropAction> actions;
  std::vector<InteropSessionExpectation> session_expectations;
  std::vector<InteropMetricExpectation> metric_expectations;
  std::size_t min_trace_events{ 0 };
};

struct InteropReport
{
  std::vector<session::SessionSnapshot> sessions;
  RuntimeMetricsSnapshot metrics;
  std::vector<TraceEvent> trace_events;
};

auto
LoadInteropScenarioText(std::string_view text, const std::filesystem::path& base_dir = {})
  -> base::Result<InteropScenario>;
auto
LoadInteropScenarioFile(const std::filesystem::path& path) -> base::Result<InteropScenario>;
auto
RunInteropScenario(const InteropScenario& scenario) -> base::Result<InteropReport>;

} // namespace nimble::runtime