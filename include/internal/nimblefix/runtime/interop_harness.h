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
  kProtocolConnect,
  kProtocolInbound,
  kProtocolInboundRaw,
  kProtocolSendApplication,
  kProtocolQueueApplication,
  kProtocolTimer,
  kProtocolBeginLogout,
  kAcceptorLogonAttempt,
};

struct InteropAction
{
  InteropActionKind kind{ InteropActionKind::kConnect };
  std::uint64_t session_id{ 0 };
  std::uint32_t seq_num{ 0 };
  bool is_admin{ false };
  bool poss_dup{ false };
  std::uint64_t timestamp_ns{ 0 };
  std::string begin_string;
  std::string sender_comp_id;
  std::string target_comp_id;
  std::string default_appl_ver_id;
  std::string orig_sending_time;
  std::string text;
};

struct InteropActionExpectation
{
  std::size_t action_index{ 0 };
  std::optional<std::size_t> outbound_frames;
  std::optional<std::size_t> application_messages;
  std::optional<std::size_t> queued_application_messages;
  std::optional<std::size_t> processed_application_messages;
  std::optional<std::size_t> ignored_application_messages;
  std::optional<std::size_t> poss_resend_application_messages;
  std::optional<bool> session_active;
  std::optional<bool> disconnect;
  std::optional<bool> session_reject;
};

struct InteropOutboundExpectation
{
  std::size_t action_index{ 0 };
  std::size_t frame_index{ 0 };
  std::string msg_type;
  std::optional<std::uint32_t> msg_seq_num;
  std::optional<std::int64_t> ref_seq_num;
  std::optional<std::int64_t> ref_tag_id;
  std::optional<std::int64_t> reject_reason;
  std::optional<std::int64_t> business_reject_reason;
  std::optional<std::int64_t> begin_seq_no;
  std::optional<std::int64_t> end_seq_no;
  std::optional<std::int64_t> new_seq_no;
  std::optional<bool> gap_fill_flag;
  std::string ref_msg_type;
  std::string test_req_id;
  std::string text_contains;
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
  std::vector<InteropActionExpectation> action_expectations;
  std::vector<InteropOutboundExpectation> outbound_expectations;
  std::vector<InteropSessionExpectation> session_expectations;
  std::vector<InteropMetricExpectation> metric_expectations;
  std::size_t min_trace_events{ 0 };
};

struct InteropOutboundFrameSummary
{
  std::string msg_type;
  std::uint32_t msg_seq_num{ 0 };
  std::optional<std::int64_t> ref_seq_num;
  std::optional<std::int64_t> ref_tag_id;
  std::optional<std::int64_t> reject_reason;
  std::optional<std::int64_t> business_reject_reason;
  std::optional<std::int64_t> begin_seq_no;
  std::optional<std::int64_t> end_seq_no;
  std::optional<std::int64_t> new_seq_no;
  std::optional<bool> gap_fill_flag;
  std::string ref_msg_type;
  std::string test_req_id;
  std::string text;
};

struct InteropActionReport
{
  std::uint64_t session_id{ 0 };
  InteropActionKind kind{ InteropActionKind::kConnect };
  std::size_t outbound_frames{ 0 };
  std::size_t application_messages{ 0 };
  std::size_t queued_application_messages{ 0 };
  std::size_t processed_application_messages{ 0 };
  std::size_t ignored_application_messages{ 0 };
  std::size_t poss_resend_application_messages{ 0 };
  bool session_active{ false };
  bool disconnect{ false };
  bool session_reject{ false };
  std::vector<InteropOutboundFrameSummary> outbound_frame_summaries;
};

struct InteropReport
{
  std::vector<session::SessionSnapshot> sessions;
  RuntimeMetricsSnapshot metrics;
  std::vector<TraceEvent> trace_events;
  std::vector<InteropActionReport> action_reports;
};

auto
LoadInteropScenarioText(std::string_view text, const std::filesystem::path& base_dir = {})
  -> base::Result<InteropScenario>;
auto
LoadInteropScenarioFile(const std::filesystem::path& path) -> base::Result<InteropScenario>;
auto
RunInteropScenario(const InteropScenario& scenario) -> base::Result<InteropReport>;

} // namespace nimble::runtime