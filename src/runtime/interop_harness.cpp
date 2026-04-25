#include "nimblefix/runtime/interop_harness.h"

#include "nimblefix/runtime/internal_config_parser.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string_view>
#include <unordered_map>

#include "nimblefix/runtime/sharded_runtime.h"
#include "nimblefix/session/resend_recovery.h"
#include "nimblefix/store/durable_batch_store.h"
#include "nimblefix/store/memory_store.h"
#include "nimblefix/store/mmap_store.h"

namespace nimble::runtime {

namespace {

constexpr char kInteropCommentPrefix = '#';
constexpr char kInteropFieldSeparator = '|';
constexpr std::string_view kInteropConfigPrefix = "config=";

namespace action_columns {
constexpr std::size_t kKind = 0U;
constexpr std::size_t kAction = 1U;
constexpr std::size_t kSessionId = 2U;
constexpr std::size_t kSeqNum = 3U;
constexpr std::size_t kAdminFlag = 4U;
constexpr std::size_t kTimestampNs = 5U;
constexpr std::size_t kText = 6U;
constexpr std::size_t kMinimumCount = 3U;
constexpr std::size_t kMessageActionCount = 6U;
} // namespace action_columns

namespace session_expectation_columns {
constexpr std::size_t kKind = 0U;
constexpr std::size_t kSessionId = 1U;
constexpr std::size_t kState = 2U;
constexpr std::size_t kNextInSeq = 3U;
constexpr std::size_t kNextOutSeq = 4U;
constexpr std::size_t kHasPendingResend = 5U;
constexpr std::size_t kCount = 6U;
} // namespace session_expectation_columns

namespace metric_expectation_columns {
constexpr std::size_t kKind = 0U;
constexpr std::size_t kSessionId = 1U;
constexpr std::size_t kInboundMessages = 2U;
constexpr std::size_t kOutboundMessages = 3U;
constexpr std::size_t kAdminMessages = 4U;
constexpr std::size_t kResendRequests = 5U;
constexpr std::size_t kGapFills = 6U;
constexpr std::size_t kCount = 7U;
} // namespace metric_expectation_columns

namespace trace_expectation_columns {
constexpr std::size_t kKind = 0U;
constexpr std::size_t kMinimumEvents = 1U;
constexpr std::size_t kCount = 2U;
} // namespace trace_expectation_columns

struct ScenarioRuntimeContext
{
  MetricsRegistry metrics;
  TraceRecorder trace;
  std::unordered_map<std::uint64_t, session::SessionCore> sessions;
  std::unordered_map<std::uint64_t, CounterpartyConfig> counterparties;
  std::unordered_map<std::uint64_t, std::unique_ptr<store::SessionStore>> stores;
  std::unordered_map<std::uint64_t, std::uint32_t> workers;
};

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

template<typename Integer>
auto
ParseInteger(std::string_view token, const char* label) -> base::Result<Integer>
{
  try {
    return static_cast<Integer>(std::stoull(std::string(token), nullptr, 0));
  } catch (...) {
    return base::Status::InvalidArgument(std::string("invalid ") + label + " in interop scenario");
  }
}

auto
ParseBoolWord(std::string_view token) -> base::Result<bool>
{
  const auto value = Trim(token);
  if (value == "admin") {
    return true;
  }
  if (value == "app") {
    return false;
  }
  return base::Status::InvalidArgument("interop scenario expects 'admin' or 'app'");
}

auto
ParseSessionState(std::string_view token) -> base::Result<session::SessionState>
{
  const auto value = Trim(token);
  if (value == "disconnected") {
    return session::SessionState::kDisconnected;
  }
  if (value == "connected") {
    return session::SessionState::kConnected;
  }
  if (value == "pending-logon") {
    return session::SessionState::kPendingLogon;
  }
  if (value == "recovering") {
    return session::SessionState::kRecovering;
  }
  if (value == "active") {
    return session::SessionState::kActive;
  }
  if (value == "resend-processing") {
    return session::SessionState::kResendProcessing;
  }
  if (value == "awaiting-logout") {
    return session::SessionState::kAwaitingLogout;
  }
  return base::Status::InvalidArgument("unknown session state in interop scenario");
}

auto
MakeStore(const CounterpartyConfig& counterparty) -> base::Result<std::unique_ptr<store::SessionStore>>
{
  if (counterparty.store_mode == StoreMode::kMemory) {
    return std::unique_ptr<store::SessionStore>(std::make_unique<store::MemorySessionStore>());
  }

  if (counterparty.store_mode == StoreMode::kDurableBatch) {
    auto durable_store = std::make_unique<store::DurableBatchSessionStore>(
      counterparty.store_path,
      store::DurableBatchStoreOptions{
        .flush_threshold = counterparty.durable_flush_threshold,
        .rollover_mode = counterparty.durable_rollover_mode,
        .max_archived_segments = counterparty.durable_archive_limit,
        .local_utc_offset_seconds = counterparty.durable_local_utc_offset_seconds,
        .use_system_timezone = counterparty.durable_use_system_timezone,
      });
    auto status = durable_store->Open();
    if (!status.ok()) {
      return status;
    }
    return std::unique_ptr<store::SessionStore>(std::move(durable_store));
  }

  auto mmap_store = std::make_unique<store::MmapSessionStore>(counterparty.store_path);
  auto status = mmap_store->Open();
  if (!status.ok()) {
    return status;
  }
  return std::unique_ptr<store::SessionStore>(std::move(mmap_store));
}

auto
FindMutableSession(ScenarioRuntimeContext& context, std::uint64_t session_id) -> session::SessionCore*
{
  const auto it = context.sessions.find(session_id);
  if (it == context.sessions.end()) {
    return nullptr;
  }
  return &it->second;
}

auto
FindStore(ScenarioRuntimeContext& context, std::uint64_t session_id) -> store::SessionStore*
{
  const auto it = context.stores.find(session_id);
  if (it == context.stores.end()) {
    return nullptr;
  }
  return it->second.get();
}

auto
SaveRecoverySnapshot(ScenarioRuntimeContext& context, std::uint64_t session_id) -> base::Status
{
  auto* session = FindMutableSession(context, session_id);
  auto* store = FindStore(context, session_id);
  if (session == nullptr || store == nullptr) {
    return base::Status::NotFound("interop scenario session/store not found for recovery snapshot");
  }

  const auto snapshot = session->Snapshot();
  const store::SessionRecoveryState state{
    .session_id = session_id,
    .next_in_seq = snapshot.next_in_seq,
    .next_out_seq = snapshot.next_out_seq,
    .last_inbound_ns = snapshot.last_inbound_ns,
    .last_outbound_ns = snapshot.last_outbound_ns,
    .active = snapshot.state == session::SessionState::kActive,
  };
  return store->SaveRecoveryState(state);
}

auto
ReplaceAndRecover(ScenarioRuntimeContext& context, std::uint64_t session_id, session::RecoveryMode mode) -> base::Status
{
  const auto counterparty_it = context.counterparties.find(session_id);
  if (counterparty_it == context.counterparties.end()) {
    return base::Status::NotFound("interop scenario counterparty not found");
  }
  auto* store = FindStore(context, session_id);
  if (store == nullptr) {
    return base::Status::NotFound("interop scenario store not found");
  }

  session::SessionCore recovered(counterparty_it->second.session);
  auto status = session::RecoverSession(recovered, *store, mode);
  if (!status.ok()) {
    return status;
  }
  auto existing = context.sessions.find(session_id);
  if (existing == context.sessions.end()) {
    return base::Status::NotFound("interop scenario session not found during recovery replacement");
  }
  existing->second = std::move(recovered);
  return base::Status::Ok();
}

auto
MakeRecord(std::uint64_t session_id,
           std::uint32_t seq_num,
           bool is_admin,
           std::uint64_t timestamp_ns,
           std::string_view text) -> store::MessageRecord
{
  store::MessageRecord record;
  record.session_id = session_id;
  record.seq_num = seq_num;
  record.timestamp_ns = timestamp_ns;
  record.flags = is_admin ? static_cast<std::uint16_t>(store::MessageRecordFlags::kAdmin) : 0U;
  record.payload.reserve(text.size());
  for (const auto ch : text) {
    record.payload.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
  }
  return record;
}

auto
ValidateExpectations(const InteropScenario& scenario, const InteropReport& report) -> base::Status
{
  for (const auto& expectation : scenario.session_expectations) {
    const auto it = std::find_if(report.sessions.begin(), report.sessions.end(), [&](const auto& snapshot) {
      return snapshot.session_id == expectation.session_id;
    });
    if (it == report.sessions.end()) {
      return base::Status::NotFound("interop report missing expected session snapshot");
    }
    if (it->state != expectation.state || it->next_in_seq != expectation.next_in_seq ||
        it->next_out_seq != expectation.next_out_seq || it->has_pending_resend != expectation.has_pending_resend) {
      return base::Status::InvalidArgument("interop session expectation mismatch");
    }
  }

  for (const auto& expectation : scenario.metric_expectations) {
    const auto it = std::find_if(report.metrics.sessions.begin(),
                                 report.metrics.sessions.end(),
                                 [&](const auto& metrics) { return metrics.session_id == expectation.session_id; });
    if (it == report.metrics.sessions.end()) {
      return base::Status::NotFound("interop report missing expected session metrics");
    }
    if (it->inbound_messages != expectation.inbound_messages ||
        it->outbound_messages != expectation.outbound_messages || it->admin_messages != expectation.admin_messages ||
        it->resend_requests != expectation.resend_requests || it->gap_fills != expectation.gap_fills) {
      return base::Status::InvalidArgument("interop metric expectation mismatch");
    }
  }

  if (report.trace_events.size() < scenario.min_trace_events) {
    return base::Status::InvalidArgument("interop trace event count is below the scenario minimum");
  }

  return base::Status::Ok();
}

} // namespace

auto
LoadInteropScenarioText(std::string_view text, const std::filesystem::path& base_dir) -> base::Result<InteropScenario>
{
  std::filesystem::path config_path;
  std::vector<std::string> config_lines;
  InteropScenario scenario;

  for (const auto& raw_line : SplitLines(text)) {
    const auto trimmed = Trim(raw_line);
    if (trimmed.empty() || trimmed.starts_with(kInteropCommentPrefix)) {
      continue;
    }

    if (trimmed.starts_with(kInteropConfigPrefix)) {
      config_path = base_dir / std::string(Trim(trimmed.substr(kInteropConfigPrefix.size())));
      continue;
    }

    const auto parts = Split(trimmed, kInteropFieldSeparator);
    if (parts.empty()) {
      continue;
    }

    if (parts[action_columns::kKind] == "action") {
      if (parts.size() < action_columns::kMinimumCount) {
        return base::Status::InvalidArgument("interop action requires at least 3 fields");
      }
      auto session_id = ParseInteger<std::uint64_t>(parts[action_columns::kSessionId], "session_id");
      if (!session_id.ok()) {
        return session_id.status();
      }

      InteropAction action;
      action.session_id = session_id.value();
      if (parts[action_columns::kAction] == "connect") {
        action.kind = InteropActionKind::kConnect;
      } else if (parts[action_columns::kAction] == "logon") {
        action.kind = InteropActionKind::kLogon;
      } else if (parts[action_columns::kAction] == "complete-resend") {
        action.kind = InteropActionKind::kCompleteResend;
      } else if (parts[action_columns::kAction] == "save-recovery") {
        action.kind = InteropActionKind::kSaveRecovery;
      } else if (parts[action_columns::kAction] == "recover-warm") {
        action.kind = InteropActionKind::kRecoverWarm;
      } else if (parts[action_columns::kAction] == "recover-cold") {
        action.kind = InteropActionKind::kRecoverCold;
      } else if (parts[action_columns::kAction] == "inbound" || parts[action_columns::kAction] == "outbound" ||
                 parts[action_columns::kAction] == "gap") {
        if (parts.size() < action_columns::kMessageActionCount) {
          return base::Status::InvalidArgument("interop message actions require 6 fields");
        }
        auto seq_num = ParseInteger<std::uint32_t>(parts[action_columns::kSeqNum], "seq_num");
        if (!seq_num.ok()) {
          return seq_num.status();
        }
        auto is_admin = ParseBoolWord(parts[action_columns::kAdminFlag]);
        if (!is_admin.ok()) {
          return is_admin.status();
        }
        auto timestamp_ns = ParseInteger<std::uint64_t>(parts[action_columns::kTimestampNs], "timestamp_ns");
        if (!timestamp_ns.ok()) {
          return timestamp_ns.status();
        }
        action.seq_num = seq_num.value();
        action.is_admin = is_admin.value();
        action.timestamp_ns = timestamp_ns.value();
        if (parts.size() > action_columns::kText) {
          action.text = parts[action_columns::kText];
        }
        if (parts[action_columns::kAction] == "inbound") {
          action.kind = InteropActionKind::kInbound;
        } else if (parts[action_columns::kAction] == "outbound") {
          action.kind = InteropActionKind::kOutbound;
        } else {
          action.kind = InteropActionKind::kGap;
        }
      } else {
        return base::Status::InvalidArgument("unknown interop action kind");
      }
      scenario.actions.push_back(std::move(action));
      continue;
    }

    if (parts[session_expectation_columns::kKind] == "expect-session") {
      if (parts.size() != session_expectation_columns::kCount) {
        return base::Status::InvalidArgument("expect-session requires 6 fields");
      }
      auto session_id = ParseInteger<std::uint64_t>(parts[session_expectation_columns::kSessionId], "session_id");
      auto state = ParseSessionState(parts[session_expectation_columns::kState]);
      auto next_in_seq = ParseInteger<std::uint32_t>(parts[session_expectation_columns::kNextInSeq], "next_in_seq");
      auto next_out_seq = ParseInteger<std::uint32_t>(parts[session_expectation_columns::kNextOutSeq], "next_out_seq");
      auto has_pending =
        ParseInteger<std::uint32_t>(parts[session_expectation_columns::kHasPendingResend], "has_pending_resend");
      if (!session_id.ok()) {
        return session_id.status();
      }
      if (!state.ok()) {
        return state.status();
      }
      if (!next_in_seq.ok()) {
        return next_in_seq.status();
      }
      if (!next_out_seq.ok()) {
        return next_out_seq.status();
      }
      if (!has_pending.ok()) {
        return has_pending.status();
      }
      scenario.session_expectations.push_back(InteropSessionExpectation{
        .session_id = session_id.value(),
        .state = state.value(),
        .next_in_seq = next_in_seq.value(),
        .next_out_seq = next_out_seq.value(),
        .has_pending_resend = has_pending.value() != 0,
      });
      continue;
    }

    if (parts[metric_expectation_columns::kKind] == "expect-metric") {
      if (parts.size() != metric_expectation_columns::kCount) {
        return base::Status::InvalidArgument("expect-metric requires 7 fields");
      }
      auto session_id = ParseInteger<std::uint64_t>(parts[metric_expectation_columns::kSessionId], "session_id");
      auto inbound =
        ParseInteger<std::uint64_t>(parts[metric_expectation_columns::kInboundMessages], "inbound_messages");
      auto outbound =
        ParseInteger<std::uint64_t>(parts[metric_expectation_columns::kOutboundMessages], "outbound_messages");
      auto admin = ParseInteger<std::uint64_t>(parts[metric_expectation_columns::kAdminMessages], "admin_messages");
      auto resend = ParseInteger<std::uint64_t>(parts[metric_expectation_columns::kResendRequests], "resend_requests");
      auto gap_fill = ParseInteger<std::uint64_t>(parts[metric_expectation_columns::kGapFills], "gap_fills");
      if (!session_id.ok()) {
        return session_id.status();
      }
      if (!inbound.ok()) {
        return inbound.status();
      }
      if (!outbound.ok()) {
        return outbound.status();
      }
      if (!admin.ok()) {
        return admin.status();
      }
      if (!resend.ok()) {
        return resend.status();
      }
      if (!gap_fill.ok()) {
        return gap_fill.status();
      }
      scenario.metric_expectations.push_back(InteropMetricExpectation{
        .session_id = session_id.value(),
        .inbound_messages = inbound.value(),
        .outbound_messages = outbound.value(),
        .admin_messages = admin.value(),
        .resend_requests = resend.value(),
        .gap_fills = gap_fill.value(),
      });
      continue;
    }

    if (parts[trace_expectation_columns::kKind] == "expect-trace-min") {
      if (parts.size() != trace_expectation_columns::kCount) {
        return base::Status::InvalidArgument("expect-trace-min requires 2 fields");
      }
      auto min_events = ParseInteger<std::uint64_t>(parts[trace_expectation_columns::kMinimumEvents], "trace count");
      if (!min_events.ok()) {
        return min_events.status();
      }
      scenario.min_trace_events = static_cast<std::size_t>(min_events.value());
      continue;
    }

    return base::Status::InvalidArgument("unknown interop scenario record kind");
  }

  if (config_path.empty()) {
    return base::Status::InvalidArgument("interop scenario is missing config=<path>");
  }

  auto config = LoadEngineConfigFile(config_path);
  if (!config.ok()) {
    return config.status();
  }
  scenario.engine_config = std::move(config).value();
  return scenario;
}

auto
LoadInteropScenarioFile(const std::filesystem::path& path) -> base::Result<InteropScenario>
{
  std::ifstream in(path);
  if (!in.is_open()) {
    return base::Status::IoError("unable to open interop scenario: '" + path.string() + "'");
  }

  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return LoadInteropScenarioText(text, path.parent_path());
}

auto
RunInteropScenario(const InteropScenario& scenario) -> base::Result<InteropReport>
{
  auto validation = ValidateEngineConfig(scenario.engine_config);
  if (!validation.ok()) {
    return validation;
  }

  ScenarioRuntimeContext context;
  context.metrics.Reset(scenario.engine_config.worker_count);
  context.trace.Configure(
    scenario.engine_config.trace_mode, scenario.engine_config.trace_capacity, scenario.engine_config.worker_count);

  const ShardedRuntime routing(scenario.engine_config.worker_count);
  for (const auto& counterparty : scenario.engine_config.counterparties) {
    auto store = MakeStore(counterparty);
    if (!store.ok()) {
      return store.status();
    }
    session::SessionCore session(counterparty.session);
    const auto worker_id = routing.RouteSession(session.key());
    auto status = context.metrics.RegisterSession(session.session_id(), worker_id);
    if (!status.ok()) {
      return status;
    }
    context.trace.Record(TraceEventKind::kSessionRegistered,
                         session.session_id(),
                         worker_id,
                         0U,
                         session.profile_id(),
                         0U,
                         counterparty.name);
    context.counterparties.emplace(session.session_id(), counterparty);
    context.workers.emplace(session.session_id(), worker_id);
    context.stores.emplace(session.session_id(), std::move(store).value());
    context.sessions.emplace(session.session_id(), std::move(session));
  }

  for (const auto& action : scenario.actions) {
    auto* session = FindMutableSession(context, action.session_id);
    if (session == nullptr) {
      return base::Status::NotFound("interop action references unknown session");
    }
    auto* store = FindStore(context, action.session_id);
    if (store == nullptr) {
      return base::Status::NotFound("interop action references unknown session store");
    }
    const auto worker_id = context.workers[action.session_id];

    base::Status status;
    switch (action.kind) {
      case InteropActionKind::kConnect:
        status = session->OnTransportConnected();
        context.trace.Record(
          TraceEventKind::kSessionEvent, action.session_id, worker_id, action.timestamp_ns, 0U, 0U, "connect");
        break;
      case InteropActionKind::kLogon:
        status = session->OnLogonAccepted();
        context.trace.Record(
          TraceEventKind::kSessionEvent, action.session_id, worker_id, action.timestamp_ns, 0U, 0U, "logon");
        break;
      case InteropActionKind::kOutbound: {
        auto seq = session->AllocateOutboundSeq();
        if (!seq.ok()) {
          return seq.status();
        }
        if (action.seq_num != 0 && seq.value() != action.seq_num) {
          return base::Status::InvalidArgument("interop outbound seq does not match expectation");
        }
        status = session->RecordOutboundActivity(action.timestamp_ns);
        if (!status.ok()) {
          return status;
        }
        status = store->SaveOutbound(
          MakeRecord(action.session_id, seq.value(), action.is_admin, action.timestamp_ns, action.text));
        if (!status.ok()) {
          return status;
        }
        status = context.metrics.RecordOutbound(action.session_id, action.is_admin);
        if (!status.ok()) {
          return status;
        }
        context.trace.Record(TraceEventKind::kSessionEvent,
                             action.session_id,
                             worker_id,
                             action.timestamp_ns,
                             seq.value(),
                             action.is_admin ? 1U : 0U,
                             "outbound");
        continue;
      }
      case InteropActionKind::kInbound:
        status = session->ObserveInboundSeq(action.seq_num);
        if (!status.ok()) {
          return status;
        }
        status = session->RecordInboundActivity(action.timestamp_ns);
        if (!status.ok()) {
          return status;
        }
        status = store->SaveInbound(
          MakeRecord(action.session_id, action.seq_num, action.is_admin, action.timestamp_ns, action.text));
        if (!status.ok()) {
          return status;
        }
        status = context.metrics.RecordInbound(action.session_id, action.is_admin);
        if (!status.ok()) {
          return status;
        }
        context.trace.Record(TraceEventKind::kSessionEvent,
                             action.session_id,
                             worker_id,
                             action.timestamp_ns,
                             action.seq_num,
                             action.is_admin ? 1U : 0U,
                             "inbound");
        continue;
      case InteropActionKind::kGap:
        status = session->ObserveInboundSeq(action.seq_num);
        if (status.ok()) {
          return base::Status::InvalidArgument("interop gap action expected a sequence gap");
        }
        status = context.metrics.RecordResendRequest(action.session_id);
        if (!status.ok()) {
          return status;
        }
        context.trace.Record(
          TraceEventKind::kSessionEvent, action.session_id, worker_id, action.timestamp_ns, action.seq_num, 0U, "gap");
        continue;
      case InteropActionKind::kCompleteResend:
        status = session->CompleteResend();
        context.trace.Record(
          TraceEventKind::kSessionEvent, action.session_id, worker_id, action.timestamp_ns, 0U, 0U, "complete-resend");
        break;
      case InteropActionKind::kSaveRecovery:
        status = SaveRecoverySnapshot(context, action.session_id);
        context.trace.Record(
          TraceEventKind::kStoreEvent, action.session_id, worker_id, action.timestamp_ns, 0U, 0U, "save-recovery");
        break;
      case InteropActionKind::kRecoverWarm:
        status = ReplaceAndRecover(context, action.session_id, session::RecoveryMode::kWarmRestart);
        context.trace.Record(
          TraceEventKind::kStoreEvent, action.session_id, worker_id, action.timestamp_ns, 1U, 0U, "recover-warm");
        break;
      case InteropActionKind::kRecoverCold:
        status = ReplaceAndRecover(context, action.session_id, session::RecoveryMode::kColdStart);
        context.trace.Record(
          TraceEventKind::kStoreEvent, action.session_id, worker_id, action.timestamp_ns, 2U, 0U, "recover-cold");
        break;
    }

    if (!status.ok()) {
      return status;
    }
  }

  InteropReport report;
  report.sessions.reserve(context.sessions.size());
  for (const auto& [_, session] : context.sessions) {
    report.sessions.push_back(session.Snapshot());
  }
  std::sort(report.sessions.begin(), report.sessions.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.session_id < rhs.session_id;
  });
  report.metrics = context.metrics.Snapshot();
  report.trace_events = context.trace.Snapshot();

  auto status = ValidateExpectations(scenario, report);
  if (!status.ok()) {
    return status;
  }
  return report;
}

} // namespace nimble::runtime