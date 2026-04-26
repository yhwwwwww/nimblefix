#include "nimblefix/runtime/soak.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nimblefix/codec/fix_codec.h"
#include "nimblefix/codec/fix_tags.h"
#include "nimblefix/message/message_builder.h"
#include "nimblefix/profile/normalized_dictionary.h"
#include "nimblefix/profile/profile_loader.h"
#include "nimblefix/runtime/engine.h"
#include "nimblefix/session/admin_protocol.h"
#include "nimblefix/store/memory_store.h"

namespace nimble::runtime {

namespace {

using namespace nimble::codec::tags;

constexpr std::uint64_t kNanosPerSecond = 1'000'000'000ULL;

struct SoakSessionHarness
{
  CounterpartyConfig counterparty;
  store::MemorySessionStore store;
  std::unique_ptr<session::AdminProtocol> protocol;
  std::uint64_t time_ns{ 0 };
};

auto
AdvanceTime(SoakSessionHarness* harness, std::uint64_t delta_ns) -> std::uint64_t
{
  harness->time_ns += std::max<std::uint64_t>(1U, delta_ns);
  return harness->time_ns;
}

auto
BuildAdminMessage(std::string_view msg_type) -> message::MessageBuilder
{
  message::MessageBuilder builder{ std::string(msg_type) };
  builder.set_string(kMsgType, std::string(msg_type));
  return builder;
}

auto
BuildLogonMessage(std::uint32_t heartbeat_interval_seconds) -> message::Message
{
  auto builder = BuildAdminMessage("A");
  builder.set_int(kEncryptMethod, 0).set_int(kHeartBtInt, static_cast<std::int64_t>(heartbeat_interval_seconds));
  return std::move(builder).build();
}

auto
BuildHeartbeatMessage(std::string test_request_id) -> message::Message
{
  auto builder = BuildAdminMessage("0");
  if (!test_request_id.empty()) {
    builder.set_string(kTestReqID, std::move(test_request_id));
  }
  return std::move(builder).build();
}

auto
BuildResendRequestMessage(std::uint32_t begin_seq, std::uint32_t end_seq) -> message::Message
{
  auto builder = BuildAdminMessage("2");
  builder.set_int(kBeginSeqNo, static_cast<std::int64_t>(begin_seq))
    .set_int(kEndSeqNo, static_cast<std::int64_t>(end_seq));
  return std::move(builder).build();
}

auto
BuildGapFillMessage(std::uint32_t new_seq_num) -> message::Message
{
  auto builder = BuildAdminMessage("4");
  builder.set_boolean(kGapFillFlag, true).set_int(kNewSeqNo, static_cast<std::int64_t>(new_seq_num));
  return std::move(builder).build();
}

auto
BuildApplicationMessage(const profile::NormalizedDictionaryView& dictionary, std::uint32_t round, bool with_group)
  -> message::Message
{
  message::MessageBuilder builder("D");
  builder.set_string(kMsgType, "D");
  builder.set_string(kClOrdID, "ORD-" + std::to_string(round + 1U));
  builder.set_string(kSymbol, "AAPL");
  builder.set_char(kSide, '1');
  builder.set_string(kTransactTime, "20260406-12:00:00.000");
  builder.set_int(kOrderQty, static_cast<std::int64_t>(100U + round));
  builder.set_char(kOrdType, '2');
  builder.set_string(kPrice, "100.00");
  if (dictionary.find_field(kAccount) != nullptr && (round % 2U) == 0U) {
    builder.set_string(kAccount, "ACCT-" + std::to_string(round + 1U));
  }
  if (with_group) {
    auto party = builder.add_group_entry(kNoPartyIDs);
    party.set_string(kPartyID, "PTY-" + std::to_string(round + 1U))
      .set_char(kPartyIDSource, 'D')
      .set_int(kPartyRole, static_cast<std::int64_t>(7U + (round % 3U)));
  }
  return std::move(builder).build();
}

auto
EncodeInboundFrame(const message::Message& message,
                   const profile::NormalizedDictionaryView& dictionary,
                   const CounterpartyConfig& counterparty,
                   std::uint32_t seq_num,
                   bool poss_dup = false,
                   std::string orig_sending_time = {}) -> base::Result<std::vector<std::byte>>
{
  codec::EncodeOptions options;
  options.begin_string = counterparty.session.key.begin_string;
  options.sender_comp_id = counterparty.session.key.target_comp_id;
  options.target_comp_id = counterparty.session.key.sender_comp_id;
  options.default_appl_ver_id = counterparty.session.default_appl_ver_id;
  options.msg_seq_num = seq_num;
  options.poss_dup = poss_dup;
  options.orig_sending_time = std::move(orig_sending_time);
  return codec::EncodeFixMessage(message, dictionary, options);
}

auto
ProcessEvent(SoakSessionHarness* harness,
             const session::ProtocolEvent& event,
             MetricsRegistry* metrics,
             const profile::NormalizedDictionaryView& dictionary,
             SoakReport* report,
             bool from_timer,
             std::vector<std::string>* test_request_ids) -> base::Status
{
  if (event.disconnect) {
    return base::Status::InvalidArgument("soak session disconnected unexpectedly");
  }

  report->total_application_messages += event.application_messages.size();
  for (const auto& outbound : event.outbound_frames) {
    auto status = metrics->RecordOutbound(harness->counterparty.session.session_id, outbound.admin);
    if (!status.ok()) {
      return status;
    }

    if (outbound.msg_type == "2") {
      ++report->total_resend_requests;
      status = metrics->RecordResendRequest(harness->counterparty.session.session_id);
      if (!status.ok()) {
        return status;
      }
    }

    if (outbound.msg_type == "4") {
      ++report->total_gap_fills;
      status = metrics->RecordGapFill(harness->counterparty.session.session_id, 1U);
      if (!status.ok()) {
        return status;
      }
    }

    if (from_timer && (outbound.msg_type == "0" || outbound.msg_type == "1")) {
      ++report->total_timer_events;
    }

    if (test_request_ids != nullptr && outbound.msg_type == "1") {
      auto decoded = codec::DecodeFixMessage(outbound.bytes, dictionary);
      if (!decoded.ok()) {
        return decoded.status();
      }
      const auto test_request_id = decoded.value().message.view().get_string(kTestReqID);
      if (test_request_id.has_value()) {
        test_request_ids->push_back(std::string(test_request_id.value()));
      }
    }
  }

  return base::Status::Ok();
}

auto
DeliverInbound(SoakSessionHarness* harness,
               std::vector<std::byte> frame,
               bool is_admin,
               bool is_duplicate,
               MetricsRegistry* metrics,
               const profile::NormalizedDictionaryView& dictionary,
               SoakReport* report) -> base::Status
{
  auto event = harness->protocol->OnInbound(std::move(frame), AdvanceTime(harness, 1U));
  if (!event.ok()) {
    return event.status();
  }

  auto status = metrics->RecordInbound(harness->counterparty.session.session_id, is_admin);
  if (!status.ok()) {
    return status;
  }
  if (is_duplicate) {
    ++report->total_duplicate_inbound_messages;
  }

  return ProcessEvent(harness, event.value(), metrics, dictionary, report, false, nullptr);
}

auto
DriveTimerPulse(SoakSessionHarness* harness,
                const SoakConfig& config,
                MetricsRegistry* metrics,
                const profile::NormalizedDictionaryView& dictionary,
                SoakReport* report) -> base::Status
{
  std::vector<std::string> test_request_ids;
  const auto timer_ns = AdvanceTime(harness, std::max<std::uint64_t>(1U, config.timer_step_seconds) * kNanosPerSecond);
  auto event = harness->protocol->OnTimer(timer_ns);
  if (!event.ok()) {
    return event.status();
  }

  auto status = ProcessEvent(harness, event.value(), metrics, dictionary, report, true, &test_request_ids);
  if (!status.ok()) {
    return status;
  }

  for (const auto& test_request_id : test_request_ids) {
    auto inbound = EncodeInboundFrame(BuildHeartbeatMessage(test_request_id),
                                      dictionary,
                                      harness->counterparty,
                                      harness->protocol->session().Snapshot().next_in_seq);
    if (!inbound.ok()) {
      return inbound.status();
    }
    status = DeliverInbound(harness, std::move(inbound).value(), true, false, metrics, dictionary, report);
    if (!status.ok()) {
      return status;
    }
  }

  return base::Status::Ok();
}

auto
ReconnectSession(SoakSessionHarness* harness,
                 MetricsRegistry* metrics,
                 const profile::NormalizedDictionaryView& dictionary,
                 SoakReport* report) -> base::Status
{
  auto disconnected = harness->protocol->OnTransportClosed();
  if (!disconnected.ok()) {
    return disconnected;
  }

  auto connected = harness->protocol->OnTransportConnected(AdvanceTime(harness, 1U));
  if (!connected.ok()) {
    return connected.status();
  }

  auto status = ProcessEvent(harness, connected.value(), metrics, dictionary, report, false, nullptr);
  if (!status.ok()) {
    return status;
  }

  auto inbound_logon = EncodeInboundFrame(BuildLogonMessage(harness->counterparty.session.heartbeat_interval_seconds),
                                          dictionary,
                                          harness->counterparty,
                                          harness->protocol->session().Snapshot().next_in_seq);
  if (!inbound_logon.ok()) {
    return inbound_logon.status();
  }

  status = DeliverInbound(harness, std::move(inbound_logon).value(), true, false, metrics, dictionary, report);
  if (status.ok()) {
    ++report->total_reconnects;
  }
  return status;
}

auto
ActivateSession(SoakSessionHarness* harness,
                MetricsRegistry* metrics,
                const profile::NormalizedDictionaryView& dictionary,
                SoakReport* report) -> base::Status
{
  auto connected = harness->protocol->OnTransportConnected(AdvanceTime(harness, 1U));
  if (!connected.ok()) {
    return connected.status();
  }
  auto status = ProcessEvent(harness, connected.value(), metrics, dictionary, report, false, nullptr);
  if (!status.ok()) {
    return status;
  }

  auto inbound_logon = EncodeInboundFrame(
    BuildLogonMessage(harness->counterparty.session.heartbeat_interval_seconds), dictionary, harness->counterparty, 1U);
  if (!inbound_logon.ok()) {
    return inbound_logon.status();
  }

  return DeliverInbound(harness, std::move(inbound_logon).value(), true, false, metrics, dictionary, report);
}

auto
SendOutboundApplication(SoakSessionHarness* harness,
                        std::uint32_t round,
                        const profile::NormalizedDictionaryView& dictionary,
                        MetricsRegistry* metrics) -> base::Status
{
  const auto outbound = harness->protocol->SendApplication(
    BuildApplicationMessage(dictionary, round, (round % 4U) == 0U).view(), AdvanceTime(harness, 1U));
  if (!outbound.ok()) {
    return outbound.status();
  }
  return metrics->RecordOutbound(harness->counterparty.session.session_id, false);
}

auto
ReplayOutboundRange(SoakSessionHarness* harness,
                    MetricsRegistry* metrics,
                    const profile::NormalizedDictionaryView& dictionary,
                    SoakReport* report) -> base::Status
{
  const auto snapshot = harness->protocol->session().Snapshot();
  if (snapshot.next_out_seq <= 2U) {
    return base::Status::Ok();
  }

  auto replay_request =
    EncodeInboundFrame(BuildResendRequestMessage(1U, 2U), dictionary, harness->counterparty, snapshot.next_in_seq);
  if (!replay_request.ok()) {
    return replay_request.status();
  }

  auto status = DeliverInbound(harness, std::move(replay_request).value(), true, false, metrics, dictionary, report);
  if (status.ok()) {
    ++report->total_replay_requests;
  }
  return status;
}

auto
InjectNetworkJitter(SoakSessionHarness* harness, const SoakConfig& config, SoakReport* report) -> void
{
  if (config.jitter_every == 0U || config.jitter_step_millis == 0U) {
    return;
  }

  AdvanceTime(harness, static_cast<std::uint64_t>(config.jitter_step_millis) * 1'000'000ULL);
  ++report->total_jitter_events;
}

auto
InjectInboundGap(SoakSessionHarness* harness,
                 std::uint32_t round,
                 MetricsRegistry* metrics,
                 const profile::NormalizedDictionaryView& dictionary,
                 SoakReport* report) -> base::Status
{
  const auto expected_seq = harness->protocol->session().Snapshot().next_in_seq;
  const auto skipped_seq = expected_seq + 1U;
  const auto app_message = BuildApplicationMessage(dictionary, round, true);

  auto skipped = EncodeInboundFrame(app_message, dictionary, harness->counterparty, skipped_seq);
  if (!skipped.ok()) {
    return skipped.status();
  }
  auto status = DeliverInbound(harness, std::move(skipped).value(), false, false, metrics, dictionary, report);
  if (!status.ok()) {
    return status;
  }

  auto gap_fill =
    EncodeInboundFrame(BuildGapFillMessage(expected_seq + 1U), dictionary, harness->counterparty, expected_seq);
  if (!gap_fill.ok()) {
    return gap_fill.status();
  }
  status = DeliverInbound(harness, std::move(gap_fill).value(), true, false, metrics, dictionary, report);
  if (!status.ok()) {
    return status;
  }

  auto replayed =
    EncodeInboundFrame(app_message, dictionary, harness->counterparty, skipped_seq, true, "20260403-00:00:00.000");
  if (!replayed.ok()) {
    return replayed.status();
  }
  return DeliverInbound(harness, std::move(replayed).value(), false, false, metrics, dictionary, report);
}

auto
InjectInboundReorder(SoakSessionHarness* harness,
                     std::uint32_t round,
                     MetricsRegistry* metrics,
                     const profile::NormalizedDictionaryView& dictionary,
                     SoakReport* report) -> base::Status
{
  const auto expected_seq = harness->protocol->session().Snapshot().next_in_seq;
  const auto ahead_seq = expected_seq + 1U;
  const auto ahead_message = BuildApplicationMessage(dictionary, round + 1U, ((round + 1U) % 3U) == 0U);

  auto ahead = EncodeInboundFrame(ahead_message, dictionary, harness->counterparty, ahead_seq);
  if (!ahead.ok()) {
    return ahead.status();
  }
  auto status = DeliverInbound(harness, std::move(ahead).value(), false, false, metrics, dictionary, report);
  if (!status.ok()) {
    return status;
  }

  auto gap_fill =
    EncodeInboundFrame(BuildGapFillMessage(expected_seq + 1U), dictionary, harness->counterparty, expected_seq);
  if (!gap_fill.ok()) {
    return gap_fill.status();
  }
  status = DeliverInbound(harness, std::move(gap_fill).value(), true, false, metrics, dictionary, report);
  if (!status.ok()) {
    return status;
  }

  auto replayed =
    EncodeInboundFrame(ahead_message, dictionary, harness->counterparty, ahead_seq, true, "20260403-00:00:00.000");
  if (!replayed.ok()) {
    return replayed.status();
  }

  status = DeliverInbound(harness, std::move(replayed).value(), false, false, metrics, dictionary, report);
  if (status.ok()) {
    ++report->total_reorder_events;
  }
  return status;
}

auto
InjectInboundDrop(SoakSessionHarness* harness,
                  std::uint32_t round,
                  MetricsRegistry* metrics,
                  const profile::NormalizedDictionaryView& dictionary,
                  SoakReport* report) -> base::Status
{
  const auto expected_seq = harness->protocol->session().Snapshot().next_in_seq;
  const auto ahead_seq = expected_seq + 1U;
  const auto ahead_message = BuildApplicationMessage(dictionary, round + 1U, ((round + 1U) % 2U) == 0U);

  auto ahead = EncodeInboundFrame(ahead_message, dictionary, harness->counterparty, ahead_seq);
  if (!ahead.ok()) {
    return ahead.status();
  }
  auto status = DeliverInbound(harness, std::move(ahead).value(), false, false, metrics, dictionary, report);
  if (!status.ok()) {
    return status;
  }

  auto gap_fill =
    EncodeInboundFrame(BuildGapFillMessage(expected_seq + 1U), dictionary, harness->counterparty, expected_seq);
  if (!gap_fill.ok()) {
    return gap_fill.status();
  }
  status = DeliverInbound(harness, std::move(gap_fill).value(), true, false, metrics, dictionary, report);
  if (!status.ok()) {
    return status;
  }

  auto replayed_ahead =
    EncodeInboundFrame(ahead_message, dictionary, harness->counterparty, ahead_seq, true, "20260403-00:00:00.000");
  if (!replayed_ahead.ok()) {
    return replayed_ahead.status();
  }
  status = DeliverInbound(harness, std::move(replayed_ahead).value(), false, false, metrics, dictionary, report);
  if (status.ok()) {
    ++report->total_drop_events;
  }
  return status;
}

auto
DeliverInboundApplication(SoakSessionHarness* harness,
                          std::uint32_t round,
                          MetricsRegistry* metrics,
                          const profile::NormalizedDictionaryView& dictionary,
                          SoakReport* report) -> base::Status
{
  auto inbound = EncodeInboundFrame(BuildApplicationMessage(dictionary, round, (round % 3U) == 0U),
                                    dictionary,
                                    harness->counterparty,
                                    harness->protocol->session().Snapshot().next_in_seq);
  if (!inbound.ok()) {
    return inbound.status();
  }
  return DeliverInbound(harness, std::move(inbound).value(), false, false, metrics, dictionary, report);
}

auto
DeliverDuplicateHeartbeat(SoakSessionHarness* harness,
                          MetricsRegistry* metrics,
                          const profile::NormalizedDictionaryView& dictionary,
                          SoakReport* report) -> base::Status
{
  auto duplicate =
    EncodeInboundFrame(BuildHeartbeatMessage({}), dictionary, harness->counterparty, 1U, true, "20260403-00:00:00.000");
  if (!duplicate.ok()) {
    return duplicate.status();
  }
  return DeliverInbound(harness, std::move(duplicate).value(), true, true, metrics, dictionary, report);
}

} // namespace

auto
RunSoak(const SoakConfig& config) -> base::Result<SoakReport>
{
  if (config.profile_artifact.empty()) {
    return base::Status::InvalidArgument("soak runner requires a profile artifact path");
  }
  if (config.session_count == 0 || config.iterations == 0) {
    return base::Status::InvalidArgument("soak runner requires positive session_count and iterations");
  }

  auto loaded_profile = profile::LoadProfileArtifact(config.profile_artifact);
  if (!loaded_profile.ok()) {
    return loaded_profile.status();
  }

  auto dictionary = profile::NormalizedDictionaryView::FromProfile(std::move(loaded_profile).value());
  if (!dictionary.ok()) {
    return dictionary.status();
  }

  EngineConfig engine_config;
  engine_config.worker_count = config.worker_count == 0 ? 1U : config.worker_count;
  engine_config.enable_metrics = true;
  engine_config.trace_mode = config.enable_trace ? TraceMode::kRing : TraceMode::kDisabled;
  engine_config.trace_capacity = config.enable_trace ? (config.session_count * config.iterations * 8U) : 0U;
  engine_config.profile_artifacts.push_back(config.profile_artifact);

  std::vector<SoakSessionHarness> sessions;
  sessions.reserve(config.session_count);
  for (std::uint32_t index = 0; index < config.session_count; ++index) {
    CounterpartyConfig counterparty;
    counterparty.name = "soak-" + std::to_string(index + 1U);
    counterparty.session.session_id = 10'000U + index;
    counterparty.session.profile_id = dictionary.value().profile().header().profile_id;
    counterparty.session.key.begin_string = "FIX.4.4";
    counterparty.transport_profile = session::TransportSessionProfile::Fix44();
    counterparty.session.key.sender_comp_id = "SELL" + std::to_string(index + 1U);
    counterparty.session.key.target_comp_id = "BUY" + std::to_string(index + 1U);
    counterparty.session.heartbeat_interval_seconds = 30U;
    counterparty.session.is_initiator = false;
    counterparty.store_mode = StoreMode::kMemory;
    counterparty.recovery_mode = session::RecoveryMode::kMemoryOnly;
    counterparty.dispatch_mode = AppDispatchMode::kInline;
    engine_config.counterparties.push_back(counterparty);

    SoakSessionHarness harness;
    harness.counterparty = std::move(counterparty);
    sessions.push_back(std::move(harness));
  }

  Engine engine;
  auto status = engine.Boot(engine_config);
  if (!status.ok()) {
    return status;
  }

  MetricsRegistry metrics;
  metrics.Reset(engine_config.worker_count);
  const auto* runtime = engine.runtime();

  SoakReport report;
  for (auto& harness : sessions) {
    harness.protocol = std::make_unique<session::AdminProtocol>(
      session::AdminProtocolConfig{
        .session = harness.counterparty.session,
        .transport_profile = harness.counterparty.transport_profile,
        .begin_string = harness.counterparty.session.key.begin_string,
        .sender_comp_id = harness.counterparty.session.key.sender_comp_id,
        .target_comp_id = harness.counterparty.session.key.target_comp_id,
        .default_appl_ver_id = harness.counterparty.session.default_appl_ver_id,
        .heartbeat_interval_seconds = harness.counterparty.session.heartbeat_interval_seconds,
      },
      dictionary.value(),
      &harness.store);

    const auto worker_id = runtime->RouteSession(harness.counterparty.session.key);
    status = metrics.RegisterSession(harness.counterparty.session.session_id, worker_id);
    if (!status.ok()) {
      return status;
    }

    status = ActivateSession(&harness, &metrics, dictionary.value(), &report);
    if (!status.ok()) {
      return status;
    }
  }

  for (std::uint32_t round = 0; round < config.iterations; ++round) {
    for (auto& harness : sessions) {
      if (config.jitter_every != 0U && ((round + 1U) % config.jitter_every) == 0U) {
        InjectNetworkJitter(&harness, config, &report);
      }

      if (config.timer_pulse_every != 0U && ((round + 1U) % config.timer_pulse_every) == 0U) {
        status = DriveTimerPulse(&harness, config, &metrics, dictionary.value(), &report);
        if (!status.ok()) {
          return status;
        }
      }

      status = SendOutboundApplication(&harness, round, dictionary.value(), &metrics);
      if (!status.ok()) {
        return status;
      }

      if (config.replay_every != 0U && ((round + 1U) % config.replay_every) == 0U) {
        status = ReplayOutboundRange(&harness, &metrics, dictionary.value(), &report);
        if (!status.ok()) {
          return status;
        }
      }

      const bool inject_gap = config.gap_every != 0U && ((round + 1U) % config.gap_every) == 0U;
      const bool inject_reorder = config.reorder_every != 0U && ((round + 1U) % config.reorder_every) == 0U;
      const bool inject_drop = config.drop_every != 0U && ((round + 1U) % config.drop_every) == 0U;

      if (inject_drop) {
        status = InjectInboundDrop(&harness, round, &metrics, dictionary.value(), &report);
        if (!status.ok()) {
          return status;
        }
      } else if (inject_gap) {
        status = InjectInboundGap(&harness, round, &metrics, dictionary.value(), &report);
        if (!status.ok()) {
          return status;
        }
      } else if (inject_reorder) {
        status = InjectInboundReorder(&harness, round, &metrics, dictionary.value(), &report);
        if (!status.ok()) {
          return status;
        }
      } else {
        status = DeliverInboundApplication(&harness, round, &metrics, dictionary.value(), &report);
        if (!status.ok()) {
          return status;
        }
      }

      if (config.duplicate_every != 0U && ((round + 1U) % config.duplicate_every) == 0U) {
        status = DeliverDuplicateHeartbeat(&harness, &metrics, dictionary.value(), &report);
        if (!status.ok()) {
          return status;
        }
      }

      if (config.disconnect_every != 0U && ((round + 1U) % config.disconnect_every) == 0U) {
        status = ReconnectSession(&harness, &metrics, dictionary.value(), &report);
        if (!status.ok()) {
          return status;
        }
      }

      status = metrics.UpdateOutboundQueueDepth(
        harness.counterparty.session.session_id,
        static_cast<std::uint32_t>((round + harness.counterparty.session.session_id) % 4U));
      if (!status.ok()) {
        return status;
      }
      status = metrics.ObserveStoreFlushLatency(harness.counterparty.session.session_id,
                                                static_cast<std::uint64_t>(round + 1U) * 10U);
      if (!status.ok()) {
        return status;
      }
    }
  }

  report.iterations_completed = config.iterations;
  report.metrics = metrics.Snapshot();
  for (const auto& session_metrics : report.metrics.sessions) {
    report.total_inbound_messages += session_metrics.inbound_messages;
    report.total_outbound_messages += session_metrics.outbound_messages;
  }
  report.trace_event_count = engine.trace().Snapshot().size();
  return report;
}

} // namespace nimble::runtime