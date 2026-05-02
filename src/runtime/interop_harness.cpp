#include "nimblefix/runtime/interop_harness.h"

#include "nimblefix/codec/fix_codec.h"
#include "nimblefix/codec/fix_tags.h"
#include "nimblefix/profile/normalized_dictionary.h"
#include "nimblefix/profile/profile_loader.h"
#include "nimblefix/runtime/contract_binding.h"
#include "nimblefix/runtime/internal_config_parser.h"
#include "nimblefix/session/admin_protocol.h"

#include <algorithm>
#include <cctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

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
constexpr char kReadableFixDelimiter = '^';
constexpr std::string_view kReadableEmbeddedSohToken = "<SOH>";
constexpr std::string_view kDefaultSendingTime = "20260425-12:00:00.000";
constexpr std::uint32_t kBusinessRejectReasonTag = 380U;

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

struct ProtocolSessionContext
{
  const CounterpartyConfig* counterparty{ nullptr };
  const profile::NormalizedDictionaryView* dictionary{ nullptr };
  std::unique_ptr<session::AdminProtocol> protocol;
  std::deque<message::MessageRef> queued_application_messages;
};

struct ScenarioRuntimeContext
{
  MetricsRegistry metrics;
  TraceRecorder trace;
  std::vector<profile::NormalizedDictionaryView> dictionary_storage;
  std::unordered_map<std::uint64_t, const profile::NormalizedDictionaryView*> dictionaries_by_profile_id;
  std::unordered_map<std::uint64_t, session::SessionCore> sessions;
  std::unordered_map<std::uint64_t, CounterpartyConfig> counterparties;
  std::unordered_map<std::uint64_t, ProtocolSessionContext> protocols;
  std::unordered_map<std::uint64_t, std::unique_ptr<store::SessionStore>> stores;
  std::unordered_map<std::uint64_t, std::unordered_set<std::string>> seen_application_ids;
  std::unordered_map<std::uint64_t, std::uint32_t> workers;
};

struct InteropInboundIdentity
{
  std::string_view begin_string;
  std::string_view sender_comp_id;
  std::string_view target_comp_id;
  std::string_view default_appl_ver_id;
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
ParseBoolean(std::string_view token, const char* label) -> base::Result<bool>
{
  const auto value = Trim(token);
  if (value == "1" || value == "true" || value == "yes") {
    return true;
  }
  if (value == "0" || value == "false" || value == "no") {
    return false;
  }
  return base::Status::InvalidArgument(std::string("invalid ") + label + " in interop scenario");
}

auto
ParseOptions(const std::vector<std::string>& parts, std::size_t begin_index)
  -> base::Result<std::unordered_map<std::string, std::string>>
{
  std::unordered_map<std::string, std::string> options;
  for (std::size_t index = begin_index; index < parts.size(); ++index) {
    const auto& raw = parts[index];
    const auto equals = raw.find('=');
    if (equals == std::string::npos || equals == 0U) {
      return base::Status::InvalidArgument("interop scenario option must be key=value");
    }
    auto key = std::string(Trim(std::string_view(raw).substr(0, equals)));
    auto value = std::string(Trim(std::string_view(raw).substr(equals + 1)));
    if (!options.emplace(std::move(key), std::move(value)).second) {
      return base::Status::InvalidArgument("interop scenario option specified more than once");
    }
  }
  return options;
}

template<typename Integer>
auto
ParseOptionInteger(const std::unordered_map<std::string, std::string>& options,
                   std::string_view key,
                   const char* label,
                   std::optional<Integer> fallback = std::nullopt) -> base::Result<Integer>
{
  const auto it = options.find(std::string(key));
  if (it == options.end()) {
    if (fallback.has_value()) {
      return fallback.value();
    }
    return base::Status::InvalidArgument(std::string("interop scenario is missing option '") + std::string(key) + "'");
  }
  return ParseInteger<Integer>(it->second, label);
}

auto
ParseOptionBoolean(const std::unordered_map<std::string, std::string>& options,
                   std::string_view key,
                   const char* label,
                   std::optional<bool> fallback = std::nullopt) -> base::Result<bool>
{
  const auto it = options.find(std::string(key));
  if (it == options.end()) {
    if (fallback.has_value()) {
      return fallback.value();
    }
    return base::Status::InvalidArgument(std::string("interop scenario is missing option '") + std::string(key) + "'");
  }
  return ParseBoolean(it->second, label);
}

auto
GetOptionString(const std::unordered_map<std::string, std::string>& options,
                std::string_view key,
                std::string fallback = {}) -> std::string
{
  const auto it = options.find(std::string(key));
  if (it == options.end()) {
    return fallback;
  }
  return it->second;
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
FindProtocol(ScenarioRuntimeContext& context, std::uint64_t session_id) -> ProtocolSessionContext*
{
  const auto it = context.protocols.find(session_id);
  if (it == context.protocols.end()) {
    return nullptr;
  }
  return &it->second;
}

auto
MakeProtocolConfig(const CounterpartyConfig& counterparty) -> session::AdminProtocolConfig
{
  session::AdminProtocolConfig config;
  config.session = counterparty.session;
  config.transport_profile = counterparty.transport_profile;
  config.begin_string = counterparty.session.key.begin_string;
  config.sender_comp_id = counterparty.session.key.sender_comp_id;
  config.target_comp_id = counterparty.session.key.target_comp_id;
  config.default_appl_ver_id = counterparty.default_appl_ver_id;
  config.supported_app_msg_types = counterparty.supported_app_msg_types;
  config.heartbeat_interval_seconds = counterparty.session.heartbeat_interval_seconds;
  config.sending_time_threshold_seconds = counterparty.sending_time_threshold_seconds;
  config.timestamp_resolution = counterparty.timestamp_resolution;
  config.application_messages_available = counterparty.application_messages_available;
  config.reset_seq_num_on_logon = counterparty.reset_seq_num_on_logon;
  config.reset_seq_num_on_logout = counterparty.reset_seq_num_on_logout;
  config.reset_seq_num_on_disconnect = counterparty.reset_seq_num_on_disconnect;
  config.refresh_on_logon = counterparty.refresh_on_logon;
  config.send_next_expected_msg_seq_num = counterparty.send_next_expected_msg_seq_num;
  config.validation_policy = counterparty.validation_policy;
  config.validation_callback = counterparty.validation_callback;
  return config;
}

auto
ReplaceReadableDelimiter(std::string value) -> std::string
{
  std::string replaced;
  replaced.reserve(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (value.compare(index, kReadableEmbeddedSohToken.size(), kReadableEmbeddedSohToken) == 0) {
      replaced.push_back('\x01');
      index += kReadableEmbeddedSohToken.size() - 1U;
      continue;
    }
    if (value[index] == kReadableFixDelimiter) {
      replaced.push_back('\x01');
      continue;
    }
    replaced.push_back(value[index]);
  }
  if (!replaced.empty() && replaced.back() != '\x01') {
    replaced.push_back('\x01');
  }
  return replaced;
}

auto
BytesFromText(std::string_view text) -> std::vector<std::byte>
{
  std::vector<std::byte> bytes;
  bytes.reserve(text.size());
  for (const auto ch : text) {
    bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
  }
  return bytes;
}

auto
MakeProtocolInboundFrame(const CounterpartyConfig& counterparty, const InteropAction& action) -> std::vector<std::byte>
{
  if (action.kind == InteropActionKind::kProtocolInboundRaw) {
    const auto raw = ReplaceReadableDelimiter(action.text);
    return BytesFromText(raw);
  }

  const auto begin_string = action.begin_string.empty() ? counterparty.session.key.begin_string : action.begin_string;
  const auto sender = action.sender_comp_id.empty() ? counterparty.session.key.target_comp_id : action.sender_comp_id;
  const auto target = action.target_comp_id.empty() ? counterparty.session.key.sender_comp_id : action.target_comp_id;
  const auto default_appl_ver_id = action.default_appl_ver_id;

  auto body = ReplaceReadableDelimiter(action.text);
  const auto first_delimiter = body.find('\x01');
  if (first_delimiter == std::string::npos || !body.starts_with("35=")) {
    return {};
  }

  std::string payload;
  payload.reserve(body.size() + 96U + default_appl_ver_id.size() + action.orig_sending_time.size());
  payload.append(body.substr(0, first_delimiter + 1U));
  payload.append("49=");
  payload.append(sender);
  payload.push_back('\x01');
  payload.append("56=");
  payload.append(target);
  payload.push_back('\x01');
  payload.append("34=");
  payload.append(std::to_string(action.seq_num));
  payload.push_back('\x01');
  payload.append("52=");
  payload.append(kDefaultSendingTime);
  payload.push_back('\x01');
  if (action.poss_dup) {
    payload.append("43=Y\x01");
  }
  if (!action.orig_sending_time.empty()) {
    payload.append("122=");
    payload.append(action.orig_sending_time);
    payload.push_back('\x01');
  }
  if (!default_appl_ver_id.empty()) {
    payload.append("1137=");
    payload.append(default_appl_ver_id);
    payload.push_back('\x01');
  }
  payload.append(body.substr(first_delimiter + 1U));

  std::string full;
  full.reserve(payload.size() + begin_string.size() + 32U);
  full.append("8=");
  full.append(begin_string);
  full.push_back('\x01');
  full.append("9=");
  full.append(std::to_string(payload.size()));
  full.push_back('\x01');
  full.append(payload);

  std::uint32_t checksum = 0;
  for (const auto ch : full) {
    checksum += static_cast<unsigned char>(ch);
  }
  checksum %= 256U;

  full.append("10=");
  if (checksum < 100U) {
    full.push_back('0');
  }
  if (checksum < 10U) {
    full.push_back('0');
  }
  full.append(std::to_string(checksum));
  full.push_back('\x01');

  return BytesFromText(full);
}

auto
ResolveAcceptorLogonIdentity(const InteropScenario& scenario, const InteropAction& action)
  -> base::Result<InteropInboundIdentity>
{
  std::string_view begin_string = action.begin_string;
  std::string_view sender = action.sender_comp_id;
  std::string_view target = action.target_comp_id;
  std::string_view default_appl_ver_id = action.default_appl_ver_id;

  if (scenario.engine_config.counterparties.size() == 1U) {
    const auto& counterparty = scenario.engine_config.counterparties.front();
    if (begin_string.empty()) {
      begin_string = counterparty.session.key.begin_string;
    }
    if (sender.empty()) {
      sender = counterparty.session.key.target_comp_id;
    }
    if (target.empty()) {
      target = counterparty.session.key.sender_comp_id;
    }
    if (default_appl_ver_id.empty()) {
      default_appl_ver_id = counterparty.default_appl_ver_id;
    }
  }

  if (begin_string.empty() || sender.empty() || target.empty()) {
    return base::Status::InvalidArgument(
      "acceptor-logon-attempt requires begin/sender/target identity unless the scenario has exactly one counterparty");
  }

  return InteropInboundIdentity{
    .begin_string = begin_string,
    .sender_comp_id = sender,
    .target_comp_id = target,
    .default_appl_ver_id = default_appl_ver_id,
  };
}

auto
ResolveAcceptorLogonCounterparty(const InteropScenario& scenario, const InteropInboundIdentity& identity)
  -> const CounterpartyConfig*
{
  for (const auto& counterparty : scenario.engine_config.counterparties) {
    const auto& key = counterparty.session.key;
    if (key.begin_string != identity.begin_string) {
      continue;
    }
    if (key.sender_comp_id != identity.target_comp_id) {
      continue;
    }
    if (key.target_comp_id != identity.sender_comp_id) {
      continue;
    }
    if (!counterparty.default_appl_ver_id.empty() && counterparty.default_appl_ver_id != identity.default_appl_ver_id) {
      continue;
    }
    return &counterparty;
  }
  return nullptr;
}

auto
SummarizeProtocolEvent(const session::ProtocolEvent& event,
                       const profile::NormalizedDictionaryView& dictionary,
                       std::uint64_t session_id,
                       InteropActionKind kind) -> base::Result<InteropActionReport>
{
  using namespace codec::tags;

  InteropActionReport report;
  report.session_id = session_id;
  report.kind = kind;
  report.outbound_frames = event.outbound_frames.size();
  report.application_messages = event.application_messages.size();
  report.session_active = event.session_active;
  report.disconnect = event.disconnect;
  report.session_reject = event.session_reject;
  report.warnings = event.warnings;
  report.errors = event.errors;
  report.outbound_frame_summaries.reserve(event.outbound_frames.size());

  for (const auto& frame : event.outbound_frames) {
    auto decoded = codec::DecodeFixMessage(frame.bytes, dictionary);
    if (!decoded.ok()) {
      return decoded.status();
    }

    InteropOutboundFrameSummary summary;
    summary.msg_type = decoded.value().header.msg_type;
    summary.msg_seq_num = decoded.value().header.msg_seq_num;
    const auto view = decoded.value().message.view();
    summary.ref_seq_num = view.get_int(kRefSeqNum);
    summary.ref_tag_id = view.get_int(kRefTagID);
    summary.reject_reason = view.get_int(kRejectReason);
    summary.business_reject_reason = view.get_int(kBusinessRejectReasonTag);
    summary.begin_seq_no = view.get_int(kBeginSeqNo);
    summary.end_seq_no = view.get_int(kEndSeqNo);
    summary.new_seq_no = view.get_int(kNewSeqNo);
    summary.gap_fill_flag = view.get_boolean(kGapFillFlag);
    summary.ref_msg_type = std::string(view.get_string(kRefMsgType).value_or(std::string_view{}));
    summary.test_req_id = std::string(view.get_string(kTestReqID).value_or(std::string_view{}));
    summary.sending_time = std::string(decoded.value().header.sending_time);
    summary.text = std::string(view.get_string(kText).value_or(std::string_view{}));
    report.outbound_frame_summaries.push_back(std::move(summary));
  }

  return report;
}

auto
AttachActionSessionSnapshot(const session::SessionCore& session, InteropActionReport* report) -> void
{
  if (report == nullptr) {
    return;
  }
  const auto snapshot = session.Snapshot();
  report->next_in_seq_after_action = snapshot.next_in_seq;
  report->next_out_seq_after_action = snapshot.next_out_seq;
}

auto
ApplyProtocolDisconnect(ProtocolSessionContext& context, const session::ProtocolEvent& event) -> base::Status
{
  if (!event.disconnect) {
    return base::Status::Ok();
  }
  return context.protocol->OnTransportClosed();
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
DecodeProtocolApplicationAction(const CounterpartyConfig& counterparty,
                                const profile::NormalizedDictionaryView& dictionary,
                                const InteropAction& action) -> base::Result<message::MessageRef>
{
  InteropAction decoded_action = action;
  decoded_action.kind = InteropActionKind::kProtocolInbound;
  decoded_action.seq_num = 1U;
  decoded_action.begin_string = counterparty.session.key.begin_string;
  decoded_action.sender_comp_id = counterparty.session.key.sender_comp_id;
  decoded_action.target_comp_id = counterparty.session.key.target_comp_id;
  decoded_action.default_appl_ver_id = counterparty.default_appl_ver_id;

  auto frame = MakeProtocolInboundFrame(counterparty, decoded_action);
  if (frame.empty()) {
    return base::Status::InvalidArgument("interop protocol application action could not build a FIX frame");
  }

  auto decoded = codec::DecodeFixMessage(std::span<const std::byte>(frame.data(), frame.size()), dictionary);
  if (!decoded.ok()) {
    return decoded.status();
  }
  return message::MessageRef::Copy(decoded.value().message.view());
}

auto
ResolveInteropApplicationId(message::MessageView view) -> std::string
{
  using namespace codec::tags;

  return std::string(view.get_string(kClOrdID).value_or(std::string_view{}));
}

auto
SummarizeApplicationOutcomes(ScenarioRuntimeContext& context,
                             std::uint64_t session_id,
                             std::uint32_t worker_id,
                             std::uint64_t timestamp_ns,
                             const session::ProtocolEvent& event,
                             InteropActionReport* report) -> void
{
  report->processed_application_messages = event.application_messages.size();
  if (event.application_messages.empty()) {
    return;
  }

  auto& seen_application_ids = context.seen_application_ids[session_id];
  for (const auto& application_message : event.application_messages) {
    const auto application_id = ResolveInteropApplicationId(application_message.view());
    if (!event.poss_resend) {
      if (!application_id.empty()) {
        seen_application_ids.insert(application_id);
      }
      continue;
    }

    ++report->poss_resend_application_messages;
    if (application_id.empty()) {
      continue;
    }

    const auto [_, inserted] = seen_application_ids.insert(application_id);
    if (inserted) {
      context.trace.Record(
        TraceEventKind::kSessionEvent, session_id, worker_id, timestamp_ns, 0U, 0U, "app-poss-resend-first-seen");
      continue;
    }

    if (report->processed_application_messages != 0U) {
      --report->processed_application_messages;
    }
    ++report->ignored_application_messages;
    report->warnings.push_back("PossResend duplicate application identifier ignored");
    context.trace.Record(
      TraceEventKind::kSessionEvent, session_id, worker_id, timestamp_ns, 0U, 0U, "app-poss-resend-duplicate");
  }
}

auto
FlushQueuedProtocolApplications(ProtocolSessionContext& protocol_context,
                                std::uint64_t timestamp_ns,
                                session::ProtocolEvent* event) -> base::Status
{
  if (event == nullptr) {
    return base::Status::InvalidArgument("queued protocol application flush requires an event");
  }

  while (!protocol_context.queued_application_messages.empty()) {
    auto queued_message = std::move(protocol_context.queued_application_messages.front());
    protocol_context.queued_application_messages.pop_front();
    auto outbound = protocol_context.protocol->SendApplication(queued_message, timestamp_ns);
    if (!outbound.ok()) {
      return outbound.status();
    }
    event->outbound_frames.push_back(std::move(outbound).value());
  }
  return base::Status::Ok();
}

auto
ValidateExpectations(const InteropScenario& scenario, const InteropReport& report) -> base::Status
{
  for (const auto& expectation : scenario.action_expectations) {
    if (expectation.action_index == 0U || expectation.action_index > report.action_reports.size()) {
      return base::Status::InvalidArgument("interop action expectation references an invalid action index");
    }

    const auto& action = report.action_reports[expectation.action_index - 1U];
    if (expectation.outbound_frames.has_value() && action.outbound_frames != expectation.outbound_frames.value()) {
      return base::Status::InvalidArgument("interop action expectation mismatch for outbound frame count");
    }
    if (expectation.application_messages.has_value() &&
        action.application_messages != expectation.application_messages.value()) {
      return base::Status::InvalidArgument("interop action expectation mismatch for application message count");
    }
    if (expectation.queued_application_messages.has_value() &&
        action.queued_application_messages != expectation.queued_application_messages.value()) {
      return base::Status::InvalidArgument("interop action expectation mismatch for queued application message count");
    }
    if (expectation.processed_application_messages.has_value() &&
        action.processed_application_messages != expectation.processed_application_messages.value()) {
      return base::Status::InvalidArgument(
        "interop action expectation mismatch for processed application message count");
    }
    if (expectation.ignored_application_messages.has_value() &&
        action.ignored_application_messages != expectation.ignored_application_messages.value()) {
      return base::Status::InvalidArgument("interop action expectation mismatch for ignored application message count");
    }
    if (expectation.poss_resend_application_messages.has_value() &&
        action.poss_resend_application_messages != expectation.poss_resend_application_messages.value()) {
      return base::Status::InvalidArgument(
        "interop action expectation mismatch for PossResend application message count");
    }
    if (expectation.session_active.has_value() && action.session_active != expectation.session_active.value()) {
      return base::Status::InvalidArgument("interop action expectation mismatch for session_active");
    }
    if (expectation.disconnect.has_value() && action.disconnect != expectation.disconnect.value()) {
      return base::Status::InvalidArgument("interop action expectation mismatch for disconnect");
    }
    if (expectation.session_reject.has_value() && action.session_reject != expectation.session_reject.value()) {
      return base::Status::InvalidArgument("interop action expectation mismatch for session_reject");
    }
    if (expectation.warning_generated.has_value() &&
        (!action.warnings.empty()) != expectation.warning_generated.value()) {
      return base::Status::InvalidArgument("interop action expectation mismatch for warning");
    }
    if (expectation.error_generated.has_value() && (!action.errors.empty()) != expectation.error_generated.value()) {
      return base::Status::InvalidArgument("interop action expectation mismatch for error");
    }
    if (expectation.next_in_seq_after_action.has_value() &&
        action.next_in_seq_after_action != expectation.next_in_seq_after_action.value()) {
      return base::Status::InvalidArgument("interop action expectation mismatch for next_in_after");
    }
  }

  for (const auto& expectation : scenario.outbound_expectations) {
    if (expectation.action_index == 0U || expectation.action_index > report.action_reports.size()) {
      return base::Status::InvalidArgument("interop outbound expectation references an invalid action index");
    }

    const auto& action = report.action_reports[expectation.action_index - 1U];
    if (expectation.frame_index == 0U || expectation.frame_index > action.outbound_frame_summaries.size()) {
      return base::Status::InvalidArgument("interop outbound expectation references an invalid frame index");
    }

    const auto& frame = action.outbound_frame_summaries[expectation.frame_index - 1U];
    if (!expectation.msg_type.empty() && frame.msg_type != expectation.msg_type) {
      return base::Status::InvalidArgument("interop outbound expectation mismatch for msg_type");
    }
    if (expectation.msg_seq_num.has_value() && frame.msg_seq_num != expectation.msg_seq_num.value()) {
      return base::Status::InvalidArgument("interop outbound expectation mismatch for msg_seq_num");
    }
    if (expectation.ref_seq_num.has_value() && frame.ref_seq_num != expectation.ref_seq_num) {
      return base::Status::InvalidArgument("interop outbound expectation mismatch for RefSeqNum");
    }
    if (expectation.ref_tag_id.has_value() && frame.ref_tag_id != expectation.ref_tag_id) {
      return base::Status::InvalidArgument("interop outbound expectation mismatch for RefTagID");
    }
    if (expectation.reject_reason.has_value() && frame.reject_reason != expectation.reject_reason) {
      return base::Status::InvalidArgument("interop outbound expectation mismatch for RejectReason");
    }
    if (expectation.business_reject_reason.has_value() &&
        frame.business_reject_reason != expectation.business_reject_reason) {
      return base::Status::InvalidArgument("interop outbound expectation mismatch for BusinessRejectReason");
    }
    if (expectation.begin_seq_no.has_value() && frame.begin_seq_no != expectation.begin_seq_no) {
      return base::Status::InvalidArgument("interop outbound expectation mismatch for BeginSeqNo");
    }
    if (expectation.end_seq_no.has_value() && frame.end_seq_no != expectation.end_seq_no) {
      return base::Status::InvalidArgument("interop outbound expectation mismatch for EndSeqNo");
    }
    if (expectation.new_seq_no.has_value() && frame.new_seq_no != expectation.new_seq_no) {
      return base::Status::InvalidArgument("interop outbound expectation mismatch for NewSeqNo");
    }
    if (expectation.gap_fill_flag.has_value() && frame.gap_fill_flag != expectation.gap_fill_flag) {
      return base::Status::InvalidArgument("interop outbound expectation mismatch for GapFillFlag");
    }
    if (!expectation.ref_msg_type.empty() && frame.ref_msg_type != expectation.ref_msg_type) {
      return base::Status::InvalidArgument("interop outbound expectation mismatch for RefMsgType");
    }
    if (!expectation.test_req_id.empty() && frame.test_req_id != expectation.test_req_id) {
      return base::Status::InvalidArgument("interop outbound expectation mismatch for TestReqID");
    }
    if (!expectation.text_contains.empty() && frame.text.find(expectation.text_contains) == std::string::npos) {
      return base::Status::InvalidArgument("interop outbound expectation mismatch for Text(58)");
    }
    if (!expectation.text_exact.empty() && frame.text != expectation.text_exact) {
      return base::Status::InvalidArgument("interop outbound expectation mismatch for exact Text(58)");
    }
    if (expectation.sending_time_present.has_value() &&
        frame.sending_time.empty() == expectation.sending_time_present.value()) {
      return base::Status::InvalidArgument("interop outbound expectation mismatch for SendingTime presence");
    }
    if (!expectation.sending_time_not.empty() && frame.sending_time == expectation.sending_time_not) {
      return base::Status::InvalidArgument("interop outbound expectation mismatch for excluded SendingTime");
    }
  }

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
      if (parts[action_columns::kAction].starts_with("protocol-") ||
          parts[action_columns::kAction] == "acceptor-logon-attempt") {
        auto options = ParseOptions(parts, action_columns::kSeqNum);
        if (!options.ok()) {
          return options.status();
        }

        if (parts[action_columns::kAction] == "protocol-connect") {
          action.kind = InteropActionKind::kProtocolConnect;
          auto timestamp_ns = ParseOptionInteger<std::uint64_t>(options.value(), "ts", "timestamp_ns", 0U);
          if (!timestamp_ns.ok()) {
            return timestamp_ns.status();
          }
          action.timestamp_ns = timestamp_ns.value();
        } else if (parts[action_columns::kAction] == "protocol-inbound") {
          action.kind = InteropActionKind::kProtocolInbound;
          auto seq_num = ParseOptionInteger<std::uint32_t>(options.value(), "seq", "seq_num");
          auto timestamp_ns = ParseOptionInteger<std::uint64_t>(options.value(), "ts", "timestamp_ns", 0U);
          if (!seq_num.ok()) {
            return seq_num.status();
          }
          if (!timestamp_ns.ok()) {
            return timestamp_ns.status();
          }
          action.seq_num = seq_num.value();
          action.timestamp_ns = timestamp_ns.value();
          action.poss_dup = ParseOptionBoolean(options.value(), "possdup", "possdup", false).value();
          action.begin_string = GetOptionString(options.value(), "begin");
          action.sender_comp_id = GetOptionString(options.value(), "sender");
          action.target_comp_id = GetOptionString(options.value(), "target");
          action.default_appl_ver_id = GetOptionString(options.value(), "default-appl-ver-id");
          action.orig_sending_time = GetOptionString(options.value(), "orig-sending-time");
          action.text = GetOptionString(options.value(), "body");
          if (action.text.empty()) {
            return base::Status::InvalidArgument("protocol-inbound requires body=<fields>");
          }
        } else if (parts[action_columns::kAction] == "protocol-inbound-raw") {
          action.kind = InteropActionKind::kProtocolInboundRaw;
          auto timestamp_ns = ParseOptionInteger<std::uint64_t>(options.value(), "ts", "timestamp_ns", 0U);
          if (!timestamp_ns.ok()) {
            return timestamp_ns.status();
          }
          action.timestamp_ns = timestamp_ns.value();
          action.text = GetOptionString(options.value(), "raw");
          if (action.text.empty()) {
            return base::Status::InvalidArgument("protocol-inbound-raw requires raw=<frame>");
          }
        } else if (parts[action_columns::kAction] == "protocol-send-application") {
          action.kind = InteropActionKind::kProtocolSendApplication;
          auto timestamp_ns = ParseOptionInteger<std::uint64_t>(options.value(), "ts", "timestamp_ns", 0U);
          if (!timestamp_ns.ok()) {
            return timestamp_ns.status();
          }
          action.timestamp_ns = timestamp_ns.value();
          action.text = GetOptionString(options.value(), "body");
          if (action.text.empty()) {
            return base::Status::InvalidArgument("protocol-send-application requires body=<fields>");
          }
        } else if (parts[action_columns::kAction] == "protocol-queue-application") {
          action.kind = InteropActionKind::kProtocolQueueApplication;
          auto timestamp_ns = ParseOptionInteger<std::uint64_t>(options.value(), "ts", "timestamp_ns", 0U);
          if (!timestamp_ns.ok()) {
            return timestamp_ns.status();
          }
          action.timestamp_ns = timestamp_ns.value();
          action.text = GetOptionString(options.value(), "body");
          if (action.text.empty()) {
            return base::Status::InvalidArgument("protocol-queue-application requires body=<fields>");
          }
        } else if (parts[action_columns::kAction] == "protocol-timer") {
          action.kind = InteropActionKind::kProtocolTimer;
          auto timestamp_ns = ParseOptionInteger<std::uint64_t>(options.value(), "ts", "timestamp_ns");
          if (!timestamp_ns.ok()) {
            return timestamp_ns.status();
          }
          action.timestamp_ns = timestamp_ns.value();
        } else if (parts[action_columns::kAction] == "protocol-begin-logout") {
          action.kind = InteropActionKind::kProtocolBeginLogout;
          auto timestamp_ns = ParseOptionInteger<std::uint64_t>(options.value(), "ts", "timestamp_ns", 0U);
          if (!timestamp_ns.ok()) {
            return timestamp_ns.status();
          }
          action.timestamp_ns = timestamp_ns.value();
          action.text = GetOptionString(options.value(), "text");
        } else if (parts[action_columns::kAction] == "acceptor-logon-attempt") {
          action.kind = InteropActionKind::kAcceptorLogonAttempt;
          auto timestamp_ns = ParseOptionInteger<std::uint64_t>(options.value(), "ts", "timestamp_ns", 0U);
          if (!timestamp_ns.ok()) {
            return timestamp_ns.status();
          }
          action.timestamp_ns = timestamp_ns.value();
          action.begin_string = GetOptionString(options.value(), "begin");
          action.sender_comp_id = GetOptionString(options.value(), "sender");
          action.target_comp_id = GetOptionString(options.value(), "target");
          action.default_appl_ver_id = GetOptionString(options.value(), "default-appl-ver-id");
          action.text = GetOptionString(options.value(), "body");
          if (action.text.empty()) {
            return base::Status::InvalidArgument("acceptor-logon-attempt requires body=<fields>");
          }
        } else {
          return base::Status::InvalidArgument("unknown protocol interop action kind");
        }

        scenario.actions.push_back(std::move(action));
        continue;
      }

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

    if (parts[0] == "expect-action") {
      if (parts.size() < 2U) {
        return base::Status::InvalidArgument("expect-action requires an action index");
      }
      auto action_index = ParseInteger<std::uint64_t>(parts[1], "action_index");
      if (!action_index.ok()) {
        return action_index.status();
      }
      auto options = ParseOptions(parts, 2U);
      if (!options.ok()) {
        return options.status();
      }

      InteropActionExpectation expectation;
      expectation.action_index = static_cast<std::size_t>(action_index.value());
      if (options.value().contains("outbound")) {
        auto value = ParseOptionInteger<std::uint64_t>(options.value(), "outbound", "outbound_frames");
        if (!value.ok()) {
          return value.status();
        }
        expectation.outbound_frames = static_cast<std::size_t>(value.value());
      }
      if (options.value().contains("app")) {
        auto value = ParseOptionInteger<std::uint64_t>(options.value(), "app", "application_messages");
        if (!value.ok()) {
          return value.status();
        }
        expectation.application_messages = static_cast<std::size_t>(value.value());
      }
      if (options.value().contains("queued-app")) {
        auto value = ParseOptionInteger<std::uint64_t>(options.value(), "queued-app", "queued_application_messages");
        if (!value.ok()) {
          return value.status();
        }
        expectation.queued_application_messages = static_cast<std::size_t>(value.value());
      }
      if (options.value().contains("processed-app")) {
        auto value =
          ParseOptionInteger<std::uint64_t>(options.value(), "processed-app", "processed_application_messages");
        if (!value.ok()) {
          return value.status();
        }
        expectation.processed_application_messages = static_cast<std::size_t>(value.value());
      }
      if (options.value().contains("ignored-app")) {
        auto value = ParseOptionInteger<std::uint64_t>(options.value(), "ignored-app", "ignored_application_messages");
        if (!value.ok()) {
          return value.status();
        }
        expectation.ignored_application_messages = static_cast<std::size_t>(value.value());
      }
      if (options.value().contains("poss-resend-app")) {
        auto value =
          ParseOptionInteger<std::uint64_t>(options.value(), "poss-resend-app", "poss_resend_application_messages");
        if (!value.ok()) {
          return value.status();
        }
        expectation.poss_resend_application_messages = static_cast<std::size_t>(value.value());
      }
      if (options.value().contains("active")) {
        auto value = ParseOptionBoolean(options.value(), "active", "session_active");
        if (!value.ok()) {
          return value.status();
        }
        expectation.session_active = value.value();
      }
      if (options.value().contains("disconnect")) {
        auto value = ParseOptionBoolean(options.value(), "disconnect", "disconnect");
        if (!value.ok()) {
          return value.status();
        }
        expectation.disconnect = value.value();
      }
      if (options.value().contains("session-reject")) {
        auto value = ParseOptionBoolean(options.value(), "session-reject", "session_reject");
        if (!value.ok()) {
          return value.status();
        }
        expectation.session_reject = value.value();
      }
      if (options.value().contains("warning")) {
        auto value = ParseOptionBoolean(options.value(), "warning", "warning");
        if (!value.ok()) {
          return value.status();
        }
        expectation.warning_generated = value.value();
      }
      if (options.value().contains("error")) {
        auto value = ParseOptionBoolean(options.value(), "error", "error");
        if (!value.ok()) {
          return value.status();
        }
        expectation.error_generated = value.value();
      }
      if (options.value().contains("next-in-after")) {
        auto value = ParseOptionInteger<std::uint32_t>(options.value(), "next-in-after", "next_in_after");
        if (!value.ok()) {
          return value.status();
        }
        expectation.next_in_seq_after_action = value.value();
      }

      scenario.action_expectations.push_back(std::move(expectation));
      continue;
    }

    if (parts[0] == "expect-outbound") {
      if (parts.size() < 3U) {
        return base::Status::InvalidArgument("expect-outbound requires action and frame indexes");
      }
      auto action_index = ParseInteger<std::uint64_t>(parts[1], "action_index");
      auto frame_index = ParseInteger<std::uint64_t>(parts[2], "frame_index");
      if (!action_index.ok()) {
        return action_index.status();
      }
      if (!frame_index.ok()) {
        return frame_index.status();
      }

      auto options = ParseOptions(parts, 3U);
      if (!options.ok()) {
        return options.status();
      }

      InteropOutboundExpectation expectation;
      expectation.action_index = static_cast<std::size_t>(action_index.value());
      expectation.frame_index = static_cast<std::size_t>(frame_index.value());
      expectation.msg_type = GetOptionString(options.value(), "msg-type");
      expectation.ref_msg_type = GetOptionString(options.value(), "ref-msg-type");
      expectation.test_req_id = GetOptionString(options.value(), "test-req-id");
      expectation.text_contains = GetOptionString(options.value(), "text-contains");
      expectation.text_exact = GetOptionString(options.value(), "text-exact");
      expectation.sending_time_not = GetOptionString(options.value(), "sending-time-not");
      if (options.value().contains("msg-seq-num")) {
        auto value = ParseOptionInteger<std::uint32_t>(options.value(), "msg-seq-num", "msg_seq_num");
        if (!value.ok()) {
          return value.status();
        }
        expectation.msg_seq_num = value.value();
      }
      if (options.value().contains("ref-seq")) {
        auto value = ParseOptionInteger<std::int64_t>(options.value(), "ref-seq", "ref_seq_num");
        if (!value.ok()) {
          return value.status();
        }
        expectation.ref_seq_num = value.value();
      }
      if (options.value().contains("ref-tag")) {
        auto value = ParseOptionInteger<std::int64_t>(options.value(), "ref-tag", "ref_tag_id");
        if (!value.ok()) {
          return value.status();
        }
        expectation.ref_tag_id = value.value();
      }
      if (options.value().contains("reject-reason")) {
        auto value = ParseOptionInteger<std::int64_t>(options.value(), "reject-reason", "reject_reason");
        if (!value.ok()) {
          return value.status();
        }
        expectation.reject_reason = value.value();
      }
      if (options.value().contains("business-reject-reason")) {
        auto value =
          ParseOptionInteger<std::int64_t>(options.value(), "business-reject-reason", "business_reject_reason");
        if (!value.ok()) {
          return value.status();
        }
        expectation.business_reject_reason = value.value();
      }
      if (options.value().contains("begin-seq")) {
        auto value = ParseOptionInteger<std::int64_t>(options.value(), "begin-seq", "begin_seq_no");
        if (!value.ok()) {
          return value.status();
        }
        expectation.begin_seq_no = value.value();
      }
      if (options.value().contains("end-seq")) {
        auto value = ParseOptionInteger<std::int64_t>(options.value(), "end-seq", "end_seq_no");
        if (!value.ok()) {
          return value.status();
        }
        expectation.end_seq_no = value.value();
      }
      if (options.value().contains("new-seq")) {
        auto value = ParseOptionInteger<std::int64_t>(options.value(), "new-seq", "new_seq_no");
        if (!value.ok()) {
          return value.status();
        }
        expectation.new_seq_no = value.value();
      }
      if (options.value().contains("gap-fill")) {
        auto value = ParseOptionBoolean(options.value(), "gap-fill", "gap_fill_flag");
        if (!value.ok()) {
          return value.status();
        }
        expectation.gap_fill_flag = value.value();
      }
      if (options.value().contains("sending-time-present")) {
        auto value = ParseOptionBoolean(options.value(), "sending-time-present", "sending_time_present");
        if (!value.ok()) {
          return value.status();
        }
        expectation.sending_time_present = value.value();
      }

      scenario.outbound_expectations.push_back(std::move(expectation));
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

  auto loaded_contracts = LoadContractMap(scenario.engine_config.profile_contracts);
  if (!loaded_contracts.ok()) {
    return loaded_contracts.status();
  }

  ScenarioRuntimeContext context;
  context.metrics.Reset(scenario.engine_config.worker_count);
  context.trace.Configure(
    scenario.engine_config.trace_mode, scenario.engine_config.trace_capacity, scenario.engine_config.worker_count);
  context.dictionary_storage.reserve(scenario.engine_config.profile_artifacts.size());

  for (const auto& artifact_path : scenario.engine_config.profile_artifacts) {
    auto loaded = profile::LoadProfileArtifact(artifact_path);
    if (!loaded.ok()) {
      return loaded.status();
    }
    auto dictionary = profile::NormalizedDictionaryView::FromProfile(std::move(loaded).value());
    if (!dictionary.ok()) {
      return dictionary.status();
    }
    const auto profile_id = dictionary.value().profile().header().profile_id;
    context.dictionary_storage.push_back(std::move(dictionary).value());
    context.dictionaries_by_profile_id.emplace(profile_id, &context.dictionary_storage.back());
  }

  const ShardedRuntime routing(scenario.engine_config.worker_count);
  for (const auto& counterparty : scenario.engine_config.counterparties) {
    auto effective_counterparty = ResolveEffectiveCounterpartyConfig(counterparty, loaded_contracts.value());
    if (!effective_counterparty.ok()) {
      return effective_counterparty.status();
    }

    auto store = MakeStore(effective_counterparty.value());
    if (!store.ok()) {
      return store.status();
    }
    session::SessionCore session(effective_counterparty.value().session);
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
                         effective_counterparty.value().name);
    context.counterparties.emplace(session.session_id(), effective_counterparty.value());
    context.workers.emplace(session.session_id(), worker_id);
    context.stores.emplace(session.session_id(), std::move(store).value());
    context.sessions.emplace(session.session_id(), std::move(session));

    const auto dictionary_it =
      context.dictionaries_by_profile_id.find(effective_counterparty.value().session.profile_id);
    if (dictionary_it == context.dictionaries_by_profile_id.end()) {
      return base::Status::NotFound("interop scenario missing profile artifact for protocol session");
    }
    auto protocol = std::make_unique<session::AdminProtocol>(MakeProtocolConfig(effective_counterparty.value()),
                                                             *dictionary_it->second,
                                                             context.stores.at(counterparty.session.session_id).get());
    context.protocols.emplace(counterparty.session.session_id,
                              ProtocolSessionContext{
                                .counterparty = &context.counterparties.at(counterparty.session.session_id),
                                .dictionary = dictionary_it->second,
                                .protocol = std::move(protocol),
                              });
  }

  std::vector<InteropActionReport> action_reports;
  action_reports.reserve(scenario.actions.size());
  for (const auto& action : scenario.actions) {
    session::SessionCore* session = nullptr;
    store::SessionStore* store = nullptr;
    std::uint32_t worker_id = 0U;
    if (action.kind != InteropActionKind::kAcceptorLogonAttempt) {
      session = FindMutableSession(context, action.session_id);
      if (session == nullptr) {
        return base::Status::NotFound("interop action references unknown session");
      }
      store = FindStore(context, action.session_id);
      if (store == nullptr) {
        return base::Status::NotFound("interop action references unknown session store");
      }
      worker_id = context.workers[action.session_id];
    }

    base::Status status;
    InteropActionReport action_report;
    action_report.session_id = action.session_id;
    action_report.kind = action.kind;
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
        action_reports.push_back(std::move(action_report));
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
        action_reports.push_back(std::move(action_report));
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
        action_reports.push_back(std::move(action_report));
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
      case InteropActionKind::kProtocolConnect: {
        auto* protocol_context = FindProtocol(context, action.session_id);
        if (protocol_context == nullptr) {
          return base::Status::NotFound("interop action references unknown admin protocol session");
        }
        auto event = protocol_context->protocol->OnTransportConnected(action.timestamp_ns);
        if (!event.ok()) {
          return event.status();
        }
        auto summary =
          SummarizeProtocolEvent(event.value(), *protocol_context->dictionary, action.session_id, action.kind);
        if (!summary.ok()) {
          return summary.status();
        }
        action_report = std::move(summary).value();
        SummarizeApplicationOutcomes(
          context, action.session_id, worker_id, action.timestamp_ns, event.value(), &action_report);
        status = ApplyProtocolDisconnect(*protocol_context, event.value());
        if (!status.ok()) {
          return status;
        }
        context.sessions.at(action.session_id) = protocol_context->protocol->session();
        AttachActionSessionSnapshot(protocol_context->protocol->session(), &action_report);
        context.trace.Record(
          TraceEventKind::kSessionEvent, action.session_id, worker_id, action.timestamp_ns, 0U, 0U, "protocol-connect");
        action_reports.push_back(std::move(action_report));
        continue;
      }
      case InteropActionKind::kProtocolInbound:
      case InteropActionKind::kProtocolInboundRaw: {
        auto* protocol_context = FindProtocol(context, action.session_id);
        if (protocol_context == nullptr) {
          return base::Status::NotFound("interop action references unknown admin protocol session");
        }
        auto frame = MakeProtocolInboundFrame(*protocol_context->counterparty, action);
        if (frame.empty()) {
          return base::Status::InvalidArgument("interop protocol inbound action could not build a FIX frame");
        }
        auto event = protocol_context->protocol->OnInbound(std::span<const std::byte>(frame.data(), frame.size()),
                                                           action.timestamp_ns);
        if (!event.ok()) {
          return event.status();
        }
        if (event.value().session_active && !protocol_context->queued_application_messages.empty()) {
          status = FlushQueuedProtocolApplications(*protocol_context, action.timestamp_ns, &event.value());
          if (!status.ok()) {
            return status;
          }
        }
        auto summary =
          SummarizeProtocolEvent(event.value(), *protocol_context->dictionary, action.session_id, action.kind);
        if (!summary.ok()) {
          return summary.status();
        }
        action_report = std::move(summary).value();
        SummarizeApplicationOutcomes(
          context, action.session_id, worker_id, action.timestamp_ns, event.value(), &action_report);
        status = ApplyProtocolDisconnect(*protocol_context, event.value());
        if (!status.ok()) {
          return status;
        }
        context.sessions.at(action.session_id) = protocol_context->protocol->session();
        AttachActionSessionSnapshot(protocol_context->protocol->session(), &action_report);
        context.trace.Record(TraceEventKind::kSessionEvent,
                             action.session_id,
                             worker_id,
                             action.timestamp_ns,
                             action.seq_num,
                             action.poss_dup ? 1U : 0U,
                             action.kind == InteropActionKind::kProtocolInbound ? "protocol-inbound"
                                                                                : "protocol-inbound-raw");
        action_reports.push_back(std::move(action_report));
        continue;
      }
      case InteropActionKind::kProtocolSendApplication: {
        auto* protocol_context = FindProtocol(context, action.session_id);
        if (protocol_context == nullptr) {
          return base::Status::NotFound("interop action references unknown admin protocol session");
        }
        auto decoded =
          DecodeProtocolApplicationAction(*protocol_context->counterparty, *protocol_context->dictionary, action);
        if (!decoded.ok()) {
          return decoded.status();
        }

        session::ProtocolEvent event;
        auto outbound = protocol_context->protocol->SendApplication(decoded.value(), action.timestamp_ns);
        if (!outbound.ok()) {
          return outbound.status();
        }
        event.outbound_frames.push_back(std::move(outbound).value());

        auto summary = SummarizeProtocolEvent(event, *protocol_context->dictionary, action.session_id, action.kind);
        if (!summary.ok()) {
          return summary.status();
        }
        action_report = std::move(summary).value();
        SummarizeApplicationOutcomes(context, action.session_id, worker_id, action.timestamp_ns, event, &action_report);
        status = ApplyProtocolDisconnect(*protocol_context, event);
        if (!status.ok()) {
          return status;
        }
        context.sessions.at(action.session_id) = protocol_context->protocol->session();
        AttachActionSessionSnapshot(protocol_context->protocol->session(), &action_report);
        context.trace.Record(TraceEventKind::kSessionEvent,
                             action.session_id,
                             worker_id,
                             action.timestamp_ns,
                             0U,
                             0U,
                             "protocol-send-application");
        action_reports.push_back(std::move(action_report));
        continue;
      }
      case InteropActionKind::kProtocolQueueApplication: {
        auto* protocol_context = FindProtocol(context, action.session_id);
        if (protocol_context == nullptr) {
          return base::Status::NotFound("interop action references unknown admin protocol session");
        }
        const auto snapshot = protocol_context->protocol->session().Snapshot();
        if (snapshot.state == session::SessionState::kActive ||
            snapshot.state == session::SessionState::kAwaitingLogout ||
            snapshot.state == session::SessionState::kResendProcessing) {
          return base::Status::InvalidArgument("protocol-queue-application only supports inactive protocol sessions");
        }

        auto decoded =
          DecodeProtocolApplicationAction(*protocol_context->counterparty, *protocol_context->dictionary, action);
        if (!decoded.ok()) {
          return decoded.status();
        }

        protocol_context->queued_application_messages.push_back(std::move(decoded).value());
        action_report.queued_application_messages = 1U;
        context.trace.Record(TraceEventKind::kSessionEvent,
                             action.session_id,
                             worker_id,
                             action.timestamp_ns,
                             1U,
                             0U,
                             "protocol-queue-application");
        action_reports.push_back(std::move(action_report));
        continue;
      }
      case InteropActionKind::kProtocolTimer: {
        auto* protocol_context = FindProtocol(context, action.session_id);
        if (protocol_context == nullptr) {
          return base::Status::NotFound("interop action references unknown admin protocol session");
        }
        auto event = protocol_context->protocol->OnTimer(action.timestamp_ns);
        if (!event.ok()) {
          return event.status();
        }
        auto summary =
          SummarizeProtocolEvent(event.value(), *protocol_context->dictionary, action.session_id, action.kind);
        if (!summary.ok()) {
          return summary.status();
        }
        action_report = std::move(summary).value();
        SummarizeApplicationOutcomes(
          context, action.session_id, worker_id, action.timestamp_ns, event.value(), &action_report);
        status = ApplyProtocolDisconnect(*protocol_context, event.value());
        if (!status.ok()) {
          return status;
        }
        context.sessions.at(action.session_id) = protocol_context->protocol->session();
        AttachActionSessionSnapshot(protocol_context->protocol->session(), &action_report);
        context.trace.Record(
          TraceEventKind::kSessionEvent, action.session_id, worker_id, action.timestamp_ns, 0U, 0U, "protocol-timer");
        action_reports.push_back(std::move(action_report));
        continue;
      }
      case InteropActionKind::kProtocolBeginLogout: {
        auto* protocol_context = FindProtocol(context, action.session_id);
        if (protocol_context == nullptr) {
          return base::Status::NotFound("interop action references unknown admin protocol session");
        }
        auto frame = protocol_context->protocol->BeginLogout(action.text, action.timestamp_ns);
        if (!frame.ok()) {
          return frame.status();
        }
        session::ProtocolEvent event;
        event.outbound_frames.push_back(std::move(frame).value());
        auto summary = SummarizeProtocolEvent(event, *protocol_context->dictionary, action.session_id, action.kind);
        if (!summary.ok()) {
          return summary.status();
        }
        action_report = std::move(summary).value();
        SummarizeApplicationOutcomes(context, action.session_id, worker_id, action.timestamp_ns, event, &action_report);
        status = ApplyProtocolDisconnect(*protocol_context, event);
        if (!status.ok()) {
          return status;
        }
        context.sessions.at(action.session_id) = protocol_context->protocol->session();
        AttachActionSessionSnapshot(protocol_context->protocol->session(), &action_report);
        context.trace.Record(TraceEventKind::kSessionEvent,
                             action.session_id,
                             worker_id,
                             action.timestamp_ns,
                             0U,
                             0U,
                             "protocol-begin-logout");
        action_reports.push_back(std::move(action_report));
        continue;
      }
      case InteropActionKind::kAcceptorLogonAttempt: {
        if (!std::string_view(action.text).starts_with("35=A")) {
          return base::Status::InvalidArgument("acceptor-logon-attempt only supports Logon(35=A) bodies");
        }

        auto identity = ResolveAcceptorLogonIdentity(scenario, action);
        if (!identity.ok()) {
          return identity.status();
        }

        const auto* matched = ResolveAcceptorLogonCounterparty(scenario, identity.value());
        if (matched == nullptr) {
          if (scenario.engine_config.accept_unknown_sessions) {
            return base::Status::InvalidArgument(
              "acceptor-logon-attempt does not model dynamic accept_unknown_sessions flows");
          }
          action_report.disconnect = true;
          action_report.errors.push_back("acceptor Logon identity did not match a configured session");
          context.trace.Record(TraceEventKind::kSessionEvent,
                               action.session_id,
                               worker_id,
                               action.timestamp_ns,
                               0U,
                               0U,
                               "acceptor-logon-attempt-unmatched");
          action_reports.push_back(std::move(action_report));
          continue;
        }

        const auto session_it = context.sessions.find(matched->session.session_id);
        if (session_it == context.sessions.end()) {
          return base::Status::NotFound("acceptor-logon-attempt matched an unknown session");
        }

        if (session_it->second.state() == session::SessionState::kActive) {
          action_report.disconnect = true;
          action_report.errors.push_back("acceptor Logon identity is already bound to an active session");
          context.trace.Record(TraceEventKind::kSessionEvent,
                               matched->session.session_id,
                               context.workers.at(matched->session.session_id),
                               action.timestamp_ns,
                               0U,
                               0U,
                               "acceptor-logon-attempt-duplicate");
          action_reports.push_back(std::move(action_report));
          continue;
        }

        return base::Status::InvalidArgument(
          "acceptor-logon-attempt only models duplicate or unknown identity rejection paths");
      }
    }

    if (!status.ok()) {
      return status;
    }
    action_reports.push_back(std::move(action_report));
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
  report.action_reports = std::move(action_reports);

  auto status = ValidateExpectations(scenario, report);
  if (!status.ok()) {
    return status;
  }
  return report;
}

} // namespace nimble::runtime
