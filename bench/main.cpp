#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "fix44_builders.h"
#include "nimblefix/codec/compiled_decoder.h"
#include "nimblefix/codec/fix_codec.h"
#include "nimblefix/codec/raw_passthrough.h"
#include "nimblefix/message/fixed_layout_writer.h"
#include "nimblefix/message/message_builder.h"
#include "nimblefix/message/message_view.h"
#include "nimblefix/profile/normalized_dictionary.h"
#include "nimblefix/profile/profile_loader.h"
#include "nimblefix/session/admin_protocol.h"
#include "nimblefix/store/memory_store.h"
#include "nimblefix/transport/tcp_transport.h"

#include "bench_support.h"

namespace {

using bench_support::BenchmarkMeasurement;
using bench_support::BenchmarkResult;
using bench_support::BuildFix44BusinessOrder;
using bench_support::DurationNs;
using bench_support::Fix44BusinessOrder;
using bench_support::NowNs;

auto
PrintUsage() -> void
{
  std::cout << "usage: nimblefix-bench [--artifact <profile.nfa> | --dictionary "
               "<profile.nfd> [--dictionary <overlay.nfd> ...]] [--iterations "
               "<count>] [--loopback <count>] [--replay <count>] [--replay-span "
               "<count>] [--begin-string <value>] [--default-appl-ver-id <value>]\n";
}

auto
LoadDictionary(const std::filesystem::path& artifact_path, std::span<const std::filesystem::path> dictionary_paths)
  -> nimble::base::Result<nimble::profile::NormalizedDictionaryView>
{
  nimble::base::Result<nimble::profile::LoadedProfile> profile = [&]() {
    if (!dictionary_paths.empty()) {
      return nimble::profile::LoadProfileFromDictionaryFiles(dictionary_paths);
    }
    return nimble::profile::LoadProfileArtifact(artifact_path);
  }();
  if (!profile.ok()) {
    return profile.status();
  }
  return nimble::profile::NormalizedDictionaryView::FromProfile(std::move(profile).value());
}

auto
PopulateFix44MessageBuilder(nimble::message::MessageBuilder& builder, const Fix44BusinessOrder& order) -> void
{
  builder.set(35U, "D")
    .set(11U, order.cl_ord_id)
    .set(55U, order.symbol)
    .set(54U, order.side)
    .set(60U, order.transact_time.text)
    .set(38U, order.order_qty)
    .set(40U, order.ord_type);
  if (order.price.has_value()) {
    builder.set(44U, order.price.value());
  }
  auto party = builder.add_group_entry(453U);
  party.set(448U, order.party_id).set(447U, order.party_id_source).set(452U, order.party_role);
}

auto
ExtractOrderFromMessageView(nimble::message::MessageView view) -> bench_support::ParsedOrder
{
  bench_support::ParsedOrder order;
  if (auto v = view.get_string(11U)) {
    order.cl_ord_id = *v;
  }
  if (auto v = view.get_string(55U)) {
    order.symbol = *v;
  }
  if (auto v = view.get_char(54U)) {
    order.side = *v;
  }
  if (auto v = view.get_string(60U)) {
    order.transact_time = *v;
  }
  if (auto v = view.get_float(38U)) {
    order.order_qty = *v;
  }
  if (auto v = view.get_char(40U)) {
    order.ord_type = *v;
  }
  if (auto v = view.get_float(44U)) {
    order.price = *v;
    order.has_price = true;
  }
  if (auto grp = view.raw_group(453U); grp.has_value() && grp->size() > 0U) {
    auto entry = (*grp)[0U];
    if (auto v = entry.field(448U)) {
      order.party_id = *v;
    }
    if (auto v = entry.field(447U); v.has_value() && !v->empty()) {
      order.party_id_source = (*v)[0];
    }
    if (auto v = entry.field(452U); v.has_value() && !v->empty()) {
      order.party_role = static_cast<int>(*v->data() - '0');
    }
  }
  return order;
}

auto
BuildFix44MessageFromBusinessOrder(const Fix44BusinessOrder& order) -> nimble::message::Message
{
  nimble::message::MessageBuilder builder{ "D" };
  builder.reserve_fields(order.price.has_value() ? 7U : 6U).reserve_groups(1U).reserve_group_entries(453U, 1U);
  PopulateFix44MessageBuilder(builder, order);
  return std::move(builder).build();
}

auto
BuildSampleMessage(bool include_price = true) -> nimble::message::Message
{
  return BuildFix44MessageFromBusinessOrder(BuildFix44BusinessOrder(include_price));
}

auto
BuildEncodedMixedExtras() -> nimble::codec::EncodedOutboundExtras
{
  return nimble::codec::EncodedOutboundExtras{
    .header_fragment = "50=DESK-9\x01"
                       "57=ROUTE-7\x01"
                       "115=CLIENT-A\x01"
                       "128=VENUE-B\x01"
                       "97=Y\x01",
    .body_fragment = "1=ACC-77\x01"
                     "9999=TAIL\x01",
  };
}

auto
PopulateGeneratedWriter(nimble::generated::profile_4400::NewOrderSingleWriter* writer,
                        const Fix44BusinessOrder& business_order) -> void
{
  if (writer == nullptr) {
    return;
  }

  writer->clear();
  writer->set_cl_ord_id(business_order.cl_ord_id);
  writer->set_symbol(business_order.symbol);
  writer->set_side(business_order.side);
  writer->set_transact_time(business_order.transact_time.text);
  writer->set_order_qty(business_order.order_qty);
  writer->set_ord_type(business_order.ord_type);
  if (business_order.price.has_value()) {
    writer->set_price(business_order.price.value());
  }
  writer->reserve_no_party_i_ds(1U);
  writer->add_no_party_i_ds()
    .set_party_id(business_order.party_id)
    .set_party_id_source(business_order.party_id_source)
    .set_party_role(business_order.party_role);
}

auto
ExtractOrderQty(nimble::message::MessageView order) -> std::optional<double>
{
  if (auto qty = order.get_float(38U); qty.has_value()) {
    return qty.value();
  }
  if (auto qty = order.get_int(38U); qty.has_value()) {
    return static_cast<double>(qty.value());
  }
  return std::nullopt;
}

auto
BuildFix44OrderAckFromNewOrder(nimble::message::MessageView order, std::uint32_t execution_id)
  -> nimble::base::Result<nimble::message::Message>
{
  if (order.msg_type() != "D") {
    return nimble::base::Status::InvalidArgument("loopback benchmark expected NewOrderSingle (35=D)");
  }

  const auto cl_ord_id = order.get_string(11U);
  const auto side = order.get_char(54U);
  const auto order_qty = ExtractOrderQty(order);
  if (!cl_ord_id.has_value() || !side.has_value() || !order_qty.has_value()) {
    return nimble::base::Status::InvalidArgument(
      "loopback benchmark NewOrderSingle missing required ack source fields");
  }

  const auto symbol = order.get_string(55U);
  const auto order_id = std::string("ORDER-") + std::to_string(execution_id);
  const auto exec_id = std::string("EXEC-") + std::to_string(execution_id);

  nimble::message::MessageBuilder ack{ "8" };
  ack.reserve_fields(10U)
    .set(35U, "8")
    .set(37U, order_id)
    .set(11U, cl_ord_id.value())
    .set(17U, exec_id)
    .set(150U, '0')
    .set(39U, '0')
    .set(54U, side.value())
    .set(151U, order_qty.value())
    .set(14U, 0.0)
    .set(6U, 0.0);
  if (symbol.has_value()) {
    ack.set(55U, symbol.value());
  }
  ack.set(38U, order_qty.value());
  return std::move(ack).build();
}

auto
RunEncodeBenchmark(const Fix44BusinessOrder& business_order,
                   const nimble::profile::NormalizedDictionaryView& dictionary,
                   const nimble::codec::EncodeOptions& base_options,
                   std::uint32_t iterations) -> nimble::base::Result<BenchmarkResult>
{
  auto layout = nimble::message::FixedLayout::Build(dictionary, "D");
  if (!layout.ok()) {
    return layout.status();
  }
  const auto extras = BuildEncodedMixedExtras();
  BenchmarkResult result;
  result.samples_ns.reserve(iterations);
  BenchmarkMeasurement measurement;
  nimble::codec::EncodeBuffer encode_buffer;
  nimble::generated::profile_4400::NewOrderSingleWriter writer(layout.value());
  writer.bind_session(base_options.begin_string, base_options.sender_comp_id, base_options.target_comp_id);
  auto options = base_options;
  for (std::uint32_t index = 0; index < iterations; ++index) {
    const auto sample_started = std::chrono::steady_clock::now();
    options.msg_seq_num = index + 1U;
    PopulateGeneratedWriter(&writer, business_order);
    auto status = writer.encode_to_buffer(dictionary, options, extras.view(), &encode_buffer);
    if (!status.ok()) {
      return status;
    }
    result.samples_ns.push_back(DurationNs(sample_started, std::chrono::steady_clock::now()));
  }
  measurement.Finish(result);
  return result;
}

auto
BuildFrame(const nimble::message::Message& message,
           const nimble::profile::NormalizedDictionaryView& dictionary,
           std::string begin_string,
           std::string sender_comp_id,
           std::string target_comp_id,
           std::string default_appl_ver_id,
           std::uint32_t msg_seq_num) -> nimble::base::Result<std::vector<std::byte>>
{
  nimble::codec::EncodeOptions options;
  options.begin_string = std::move(begin_string);
  options.sender_comp_id = std::move(sender_comp_id);
  options.target_comp_id = std::move(target_comp_id);
  options.default_appl_ver_id = std::move(default_appl_ver_id);
  options.msg_seq_num = msg_seq_num;
  return nimble::codec::EncodeFixMessage(message, dictionary, options);
}

auto
BuildPreEncodedApplicationMessage(const nimble::message::Message& message,
                                  const nimble::profile::NormalizedDictionaryView& dictionary,
                                  std::string begin_string,
                                  std::string sender_comp_id,
                                  std::string sender_sub_id,
                                  std::string target_comp_id,
                                  std::string target_sub_id,
                                  std::string default_appl_ver_id)
  -> nimble::base::Result<nimble::session::EncodedApplicationMessage>
{
  nimble::codec::EncodeOptions options;
  options.begin_string = std::move(begin_string);
  options.sender_comp_id = std::move(sender_comp_id);
  options.sender_sub_id = std::move(sender_sub_id);
  options.target_comp_id = std::move(target_comp_id);
  options.target_sub_id = std::move(target_sub_id);
  options.default_appl_ver_id = std::move(default_appl_ver_id);
  options.msg_seq_num = 2U;
  options.sending_time = "20260417-12:34:56.789";

  auto frame = nimble::codec::EncodeFixMessage(message, dictionary, options);
  if (!frame.ok()) {
    return frame.status();
  }

  auto decoded =
    nimble::codec::DecodeRawPassThrough(std::span<const std::byte>(frame.value().data(), frame.value().size()));
  if (!decoded.ok()) {
    return decoded.status();
  }

  return nimble::session::EncodedApplicationMessage(decoded.value().msg_type, decoded.value().raw_body);
}

auto
TotalFrameBytes(const std::vector<std::vector<std::byte>>& frames) -> std::size_t
{
  std::size_t total = 0U;
  for (const auto& frame : frames) {
    total += frame.size();
  }
  return total;
}

auto
ActivateProtocolPair(nimble::session::AdminProtocol& initiator, nimble::session::AdminProtocol& acceptor)
  -> nimble::base::Status
{
  auto acceptor_connected = acceptor.OnTransportConnected(NowNs());
  if (!acceptor_connected.ok()) {
    return acceptor_connected.status();
  }

  auto logon = initiator.OnTransportConnected(NowNs());
  if (!logon.ok()) {
    return logon.status();
  }

  for (const auto& outbound : logon.value().outbound_frames) {
    auto acceptor_event = acceptor.OnInbound(outbound.bytes, NowNs());
    if (!acceptor_event.ok()) {
      return acceptor_event.status();
    }
    for (const auto& response : acceptor_event.value().outbound_frames) {
      auto initiator_event = initiator.OnInbound(response.bytes, NowNs());
      if (!initiator_event.ok()) {
        return initiator_event.status();
      }
    }
  }

  if (initiator.session().state() != nimble::session::SessionState::kActive ||
      acceptor.session().state() != nimble::session::SessionState::kActive) {
    return nimble::base::Status::InvalidArgument("failed to activate benchmark FIX session pair");
  }

  return nimble::base::Status::Ok();
}

using bench_support::LabeledResult;

auto
RunLoopbackBenchmark(const nimble::profile::NormalizedDictionaryView& dictionary,
                     std::uint32_t iterations,
                     std::string begin_string,
                     std::string default_appl_ver_id) -> nimble::base::Result<BenchmarkResult>
{
  auto acceptor = nimble::transport::TcpAcceptor::Listen("127.0.0.1", 0U);
  if (!acceptor.ok()) {
    return acceptor.status();
  }
  const auto port = acceptor.value().port();
  const auto sample = BuildSampleMessage();
  auto sample_frame = BuildFrame(sample, dictionary, begin_string, "BUY", "SELL", default_appl_ver_id, 2U);
  if (!sample_frame.ok()) {
    return sample_frame.status();
  }

  auto sample_order_ack = BuildFix44OrderAckFromNewOrder(sample.view(), 1U);
  if (!sample_order_ack.ok()) {
    return sample_order_ack.status();
  }
  auto sample_ack_frame =
    BuildFrame(sample_order_ack.value(), dictionary, begin_string, "SELL", "BUY", default_appl_ver_id, 2U);
  if (!sample_ack_frame.ok()) {
    return sample_ack_frame.status();
  }

  const auto sample_frame_bytes = sample_frame.value().size();
  const auto sample_ack_frame_bytes = sample_ack_frame.value().size();
  const auto message_count = static_cast<std::size_t>(iterations);
  constexpr std::size_t kLoopbackAdminReserveRecords = 8U;
  constexpr std::size_t kLoopbackAdminReserveBytes = 4096U;
  const auto per_session_payload_bytes =
    (message_count * (sample_frame_bytes + sample_ack_frame_bytes)) + kLoopbackAdminReserveBytes;

  std::promise<nimble::base::Status> acceptor_result;
  auto future = acceptor_result.get_future();

  std::jthread acceptor_thread([socket = std::move(acceptor).value(),
                                &acceptor_result,
                                &dictionary,
                                begin_string,
                                default_appl_ver_id,
                                message_count,
                                per_session_payload_bytes]() mutable {
    nimble::store::MemorySessionStore store;
    store.ReserveAdditionalSessionStorage(42U,
                                          message_count + kLoopbackAdminReserveRecords,
                                          message_count + kLoopbackAdminReserveRecords,
                                          per_session_payload_bytes);
    nimble::session::AdminProtocol protocol(
      nimble::session::AdminProtocolConfig{
        .session =
          nimble::session::SessionConfig{
            .session_id = 42U,
            .key = nimble::session::SessionKey{ begin_string, "SELL", "BUY" },
            .profile_id = dictionary.profile().header().profile_id,
            .default_appl_ver_id = default_appl_ver_id,
            .heartbeat_interval_seconds = 30U,
            .is_initiator = false,
          },
        .begin_string = begin_string,
        .sender_comp_id = "SELL",
        .target_comp_id = "BUY",
        .default_appl_ver_id = default_appl_ver_id,
        .heartbeat_interval_seconds = 30U,
      },
      dictionary,
      &store);

    auto connection = socket.Accept(std::chrono::seconds(10));
    if (!connection.ok()) {
      acceptor_result.set_value(connection.status());
      return;
    }
    auto start = protocol.OnTransportConnected(NowNs());
    if (!start.ok()) {
      acceptor_result.set_value(start.status());
      return;
    }

    std::uint32_t execution_id = 0U;

    while (true) {
      auto frame = connection.value().ReceiveFrameView(std::chrono::seconds(10));
      if (!frame.ok()) {
        acceptor_result.set_value(frame.status());
        return;
      }
      auto decoded = nimble::codec::DecodeFixMessageView(frame.value(), dictionary);
      if (!decoded.ok()) {
        acceptor_result.set_value(decoded.status());
        return;
      }
      auto event = protocol.OnInbound(decoded.value(), NowNs());
      if (!event.ok()) {
        acceptor_result.set_value(event.status());
        return;
      }
      for (const auto& outbound : event.value().outbound_frames) {
        auto status = connection.value().Send(outbound.bytes, std::chrono::seconds(10));
        if (!status.ok()) {
          acceptor_result.set_value(status);
          return;
        }
      }
      for (const auto& app : event.value().application_messages) {
        auto order_ack = BuildFix44OrderAckFromNewOrder(app.view(), ++execution_id);
        if (!order_ack.ok()) {
          acceptor_result.set_value(order_ack.status());
          return;
        }
        auto ack = protocol.SendApplication(order_ack.value(), NowNs());
        if (!ack.ok()) {
          acceptor_result.set_value(ack.status());
          return;
        }
        auto status = connection.value().Send(ack.value().bytes, std::chrono::seconds(10));
        if (!status.ok()) {
          acceptor_result.set_value(status);
          return;
        }
      }
      if (event.value().disconnect) {
        protocol.OnTransportClosed();
        connection.value().Close();
        acceptor_result.set_value(nimble::base::Status::Ok());
        return;
      }
    }
  });

  auto connection = nimble::transport::TcpConnection::Connect("127.0.0.1", port, std::chrono::seconds(10));
  if (!connection.ok()) {
    return connection.status();
  }

  nimble::store::MemorySessionStore initiator_store;
  nimble::session::AdminProtocol initiator(
    nimble::session::AdminProtocolConfig{
      .session =
        nimble::session::SessionConfig{
          .session_id = 24U,
          .key = nimble::session::SessionKey{ begin_string, "BUY", "SELL" },
          .profile_id = dictionary.profile().header().profile_id,
          .default_appl_ver_id = default_appl_ver_id,
          .heartbeat_interval_seconds = 30U,
          .is_initiator = true,
        },
      .begin_string = begin_string,
      .sender_comp_id = "BUY",
      .target_comp_id = "SELL",
      .default_appl_ver_id = default_appl_ver_id,
      .heartbeat_interval_seconds = 30U,
    },
    dictionary,
    &initiator_store);

  auto start = initiator.OnTransportConnected(NowNs());
  if (!start.ok()) {
    return start.status();
  }
  for (const auto& outbound : start.value().outbound_frames) {
    auto status = connection.value().Send(outbound.bytes, std::chrono::seconds(10));
    if (!status.ok()) {
      return status;
    }
  }

  bool active = false;
  while (!active) {
    auto frame = connection.value().BusyReceiveFrameView(std::chrono::seconds(10));
    if (!frame.ok()) {
      return frame.status();
    }
    auto decoded = nimble::codec::DecodeFixMessageView(frame.value(), dictionary);
    if (!decoded.ok()) {
      return decoded.status();
    }
    auto event = initiator.OnInbound(decoded.value(), NowNs());
    if (!event.ok()) {
      return event.status();
    }
    for (const auto& outbound : event.value().outbound_frames) {
      auto status = connection.value().Send(outbound.bytes, std::chrono::seconds(10));
      if (!status.ok()) {
        return status;
      }
    }
    active = event.value().session_active;
  }

  initiator_store.ReserveAdditionalSessionStorage(24U,
                                                  message_count + kLoopbackAdminReserveRecords,
                                                  message_count + kLoopbackAdminReserveRecords,
                                                  per_session_payload_bytes);

  std::uint32_t acknowledged = 0;
  BenchmarkResult result;
  result.samples_ns.reserve(iterations);

  std::int64_t total_session_outbound_ns = 0;
  std::int64_t total_transport_send_ns = 0;
  std::int64_t total_transport_recv_ns = 0;
  std::int64_t total_session_inbound_ns = 0;

  BenchmarkMeasurement measurement;
  for (std::uint32_t index = 0; index < iterations; ++index) {
    const auto started = std::chrono::steady_clock::now();

    auto t0 = std::chrono::steady_clock::now();
    auto outbound = initiator.SendApplication(sample, NowNs());
    if (!outbound.ok()) {
      return outbound.status();
    }
    auto t1 = std::chrono::steady_clock::now();
    total_session_outbound_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

    t0 = std::chrono::steady_clock::now();
    auto status = connection.value().BusySend(outbound.value().bytes, std::chrono::seconds(10));
    if (!status.ok()) {
      return status;
    }
    t1 = std::chrono::steady_clock::now();
    total_transport_send_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

    while (acknowledged <= index) {
      t0 = std::chrono::steady_clock::now();
      auto frame = connection.value().BusyReceiveFrameView(std::chrono::seconds(10));
      if (!frame.ok()) {
        return frame.status();
      }
      t1 = std::chrono::steady_clock::now();
      total_transport_recv_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

      t0 = std::chrono::steady_clock::now();
      auto decoded = nimble::codec::DecodeFixMessageView(frame.value(), dictionary);
      if (!decoded.ok()) {
        return decoded.status();
      }
      auto event = initiator.OnInbound(decoded.value(), NowNs());
      if (!event.ok()) {
        return event.status();
      }
      t1 = std::chrono::steady_clock::now();
      total_session_inbound_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();

      for (const auto& outbound_frame : event.value().outbound_frames) {
        t0 = std::chrono::steady_clock::now();
        status = connection.value().BusySend(outbound_frame.bytes, std::chrono::seconds(10));
        if (!status.ok()) {
          return status;
        }
        t1 = std::chrono::steady_clock::now();
        total_transport_send_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
      }
      for (const auto& app : event.value().application_messages) {
        if (app.view().msg_type() != "8") {
          return nimble::base::Status::InvalidArgument("loopback benchmark expected ExecutionReport order ack");
        }
        ++acknowledged;
      }
    }
    result.samples_ns.push_back(DurationNs(started, std::chrono::steady_clock::now()));
  }
  measurement.Finish(result);

  {
    const auto n = static_cast<std::int64_t>(iterations);
    std::cout << "  loopback-breakdown:\n"
              << "    session-outbound  avg=" << (total_session_outbound_ns / n)
              << "ns  total=" << (total_session_outbound_ns / 1000) << "us\n"
              << "    transport-send    avg=" << (total_transport_send_ns / n)
              << "ns  total=" << (total_transport_send_ns / 1000) << "us\n"
              << "    transport-recv    avg=" << (total_transport_recv_ns / n)
              << "ns  total=" << (total_transport_recv_ns / 1000) << "us\n"
              << "    session-inbound   avg=" << (total_session_inbound_ns / n)
              << "ns  total=" << (total_session_inbound_ns / 1000) << "us\n";
  }

  auto logout = initiator.BeginLogout({}, NowNs());
  if (!logout.ok()) {
    return logout.status();
  }
  auto status = connection.value().Send(logout.value().bytes, std::chrono::seconds(10));
  if (!status.ok()) {
    return status;
  }
  auto logout_ack = connection.value().BusyReceiveFrameView(std::chrono::seconds(10));
  if (!logout_ack.ok()) {
    return logout_ack.status();
  }
  auto decoded = nimble::codec::DecodeFixMessageView(logout_ack.value(), dictionary);
  if (!decoded.ok()) {
    return decoded.status();
  }
  auto event = initiator.OnInbound(decoded.value(), NowNs());
  if (!event.ok()) {
    return event.status();
  }

  initiator.OnTransportClosed();
  connection.value().Close();
  const auto peer_status = future.get();
  if (!peer_status.ok()) {
    return peer_status;
  }

  return result;
}

auto
RunReplayBenchmark(const nimble::profile::NormalizedDictionaryView& dictionary,
                   std::uint32_t iterations,
                   std::uint32_t replay_span,
                   std::string begin_string,
                   std::string default_appl_ver_id) -> nimble::base::Result<BenchmarkResult>
{
  if (replay_span == 0U) {
    return nimble::base::Status::InvalidArgument("replay benchmark requires replay_span > 0");
  }

  nimble::store::MemorySessionStore acceptor_store;
  nimble::store::MemorySessionStore initiator_store;
  nimble::session::AdminProtocol acceptor(
    nimble::session::AdminProtocolConfig{
      .session =
        nimble::session::SessionConfig{
          .session_id = 420U,
          .key = nimble::session::SessionKey{ begin_string, "SELL", "BUY" },
          .profile_id = dictionary.profile().header().profile_id,
          .default_appl_ver_id = default_appl_ver_id,
          .heartbeat_interval_seconds = 30U,
          .is_initiator = false,
        },
      .begin_string = begin_string,
      .sender_comp_id = "SELL",
      .target_comp_id = "BUY",
      .default_appl_ver_id = default_appl_ver_id,
      .heartbeat_interval_seconds = 30U,
    },
    dictionary,
    &acceptor_store);
  nimble::session::AdminProtocol initiator(
    nimble::session::AdminProtocolConfig{
      .session =
        nimble::session::SessionConfig{
          .session_id = 240U,
          .key = nimble::session::SessionKey{ begin_string, "BUY", "SELL" },
          .profile_id = dictionary.profile().header().profile_id,
          .default_appl_ver_id = default_appl_ver_id,
          .heartbeat_interval_seconds = 30U,
          .is_initiator = true,
        },
      .begin_string = begin_string,
      .sender_comp_id = "BUY",
      .target_comp_id = "SELL",
      .default_appl_ver_id = default_appl_ver_id,
      .heartbeat_interval_seconds = 30U,
    },
    dictionary,
    &initiator_store);

  auto status = ActivateProtocolPair(initiator, acceptor);
  if (!status.ok()) {
    return status;
  }

  const auto sample = BuildSampleMessage();
  for (std::uint32_t index = 0; index < replay_span; ++index) {
    auto outbound = acceptor.SendApplication(sample, NowNs());
    if (!outbound.ok()) {
      return outbound.status();
    }
  }

  nimble::message::MessageBuilder resend_request_builder{ "2" };
  resend_request_builder.set(35U, "2")
    .set(7U, static_cast<std::int64_t>(2))
    .set(16U, static_cast<std::int64_t>(replay_span + 1U));
  const auto resend_request = std::move(resend_request_builder).build();

  std::vector<std::vector<std::byte>> requests;
  requests.reserve(iterations);
  for (std::uint32_t index = 0; index < iterations; ++index) {
    auto encoded = BuildFrame(resend_request, dictionary, begin_string, "BUY", "SELL", default_appl_ver_id, index + 2U);
    if (!encoded.ok()) {
      return encoded.status();
    }
    requests.push_back(std::move(encoded).value());
  }
  acceptor_store.ReserveAdditionalSessionStorage(420U, requests.size(), 0U, TotalFrameBytes(requests));
  acceptor.ReserveReplayStorage(replay_span);

  BenchmarkResult result;
  result.samples_ns.reserve(iterations);
  result.work_label = "frames";

  BenchmarkMeasurement measurement;
  for (auto& request : requests) {
    const auto started = std::chrono::steady_clock::now();
    auto event = acceptor.OnInbound(std::move(request), NowNs());
    const auto finished = std::chrono::steady_clock::now();
    if (!event.ok()) {
      return event.status();
    }
    result.samples_ns.push_back(DurationNs(started, finished));
    result.work_count += static_cast<std::uint64_t>(event.value().outbound_frames.size());
  }
  measurement.Finish(result);
  return result;
}

auto
RunSessionBenchmark(const nimble::profile::NormalizedDictionaryView& dictionary,
                    std::uint32_t iterations,
                    std::string begin_string,
                    std::string default_appl_ver_id) -> nimble::base::Result<BenchmarkResult>
{
  nimble::store::MemorySessionStore acceptor_store;
  nimble::store::MemorySessionStore initiator_store;
  nimble::session::AdminProtocol acceptor(
    nimble::session::AdminProtocolConfig{
      .session =
        nimble::session::SessionConfig{
          .session_id = 421U,
          .key = nimble::session::SessionKey{ begin_string, "SELL", "BUY" },
          .profile_id = dictionary.profile().header().profile_id,
          .default_appl_ver_id = default_appl_ver_id,
          .heartbeat_interval_seconds = 30U,
          .is_initiator = false,
        },
      .begin_string = begin_string,
      .sender_comp_id = "SELL",
      .target_comp_id = "BUY",
      .default_appl_ver_id = default_appl_ver_id,
      .heartbeat_interval_seconds = 30U,
    },
    dictionary,
    &acceptor_store);
  nimble::session::AdminProtocol initiator(
    nimble::session::AdminProtocolConfig{
      .session =
        nimble::session::SessionConfig{
          .session_id = 241U,
          .key = nimble::session::SessionKey{ begin_string, "BUY", "SELL" },
          .profile_id = dictionary.profile().header().profile_id,
          .default_appl_ver_id = default_appl_ver_id,
          .heartbeat_interval_seconds = 30U,
          .is_initiator = true,
        },
      .begin_string = begin_string,
      .sender_comp_id = "BUY",
      .target_comp_id = "SELL",
      .default_appl_ver_id = default_appl_ver_id,
      .heartbeat_interval_seconds = 30U,
    },
    dictionary,
    &initiator_store);

  auto status = ActivateProtocolPair(initiator, acceptor);
  if (!status.ok()) {
    return status;
  }

  const auto sample = BuildSampleMessage();
  nimble::message::Message augmented_sample;
  const auto& bench_message = [&]() -> const nimble::message::Message& {
    if (dictionary.find_field(5001U) != nullptr) {
      nimble::message::MessageBuilder builder{ "D" };
      PopulateFix44MessageBuilder(builder, BuildFix44BusinessOrder());
      builder.set(5001U, "L");
      augmented_sample = std::move(builder).build();
      return augmented_sample;
    }
    return sample;
  }();
  std::vector<std::vector<std::byte>> inbound_frames;
  inbound_frames.reserve(iterations);
  for (std::uint32_t index = 0; index < iterations; ++index) {
    auto frame = BuildFrame(bench_message, dictionary, begin_string, "BUY", "SELL", default_appl_ver_id, index + 2U);
    if (!frame.ok()) {
      return frame.status();
    }
    inbound_frames.push_back(std::move(frame).value());
  }
  acceptor_store.ReserveAdditionalSessionStorage(421U, inbound_frames.size(), 0U, TotalFrameBytes(inbound_frames));

  BenchmarkResult result;
  result.samples_ns.reserve(iterations);
  result.work_label = "messages";

  BenchmarkMeasurement measurement;
  for (auto& frame : inbound_frames) {
    const auto started = std::chrono::steady_clock::now();
    auto event = acceptor.OnInbound(std::move(frame), NowNs());
    const auto finished = std::chrono::steady_clock::now();
    if (!event.ok()) {
      return event.status();
    }
    result.samples_ns.push_back(DurationNs(started, finished));
    result.work_count += static_cast<std::uint64_t>(event.value().application_messages.size());
  }
  measurement.Finish(result);
  return result;
}

auto
RunSessionOutboundBenchmark(const nimble::profile::NormalizedDictionaryView& dictionary,
                            std::uint32_t iterations,
                            std::string begin_string,
                            std::string default_appl_ver_id,
                            bool pre_encoded) -> nimble::base::Result<BenchmarkResult>
{
  nimble::store::MemorySessionStore acceptor_store;
  nimble::store::MemorySessionStore initiator_store;
  nimble::session::AdminProtocol acceptor(
    nimble::session::AdminProtocolConfig{
      .session =
        nimble::session::SessionConfig{
          .session_id = 422U,
          .key = nimble::session::SessionKey{ begin_string, "SELL", "BUY" },
          .profile_id = dictionary.profile().header().profile_id,
          .default_appl_ver_id = default_appl_ver_id,
          .heartbeat_interval_seconds = 30U,
          .is_initiator = false,
        },
      .begin_string = begin_string,
      .sender_comp_id = "SELL",
      .target_comp_id = "BUY",
      .default_appl_ver_id = default_appl_ver_id,
      .heartbeat_interval_seconds = 30U,
    },
    dictionary,
    &acceptor_store);
  nimble::session::AdminProtocol initiator(
    nimble::session::AdminProtocolConfig{
      .session =
        nimble::session::SessionConfig{
          .session_id = 242U,
          .key = nimble::session::SessionKey{ begin_string, "BUY", "SELL" },
          .profile_id = dictionary.profile().header().profile_id,
          .default_appl_ver_id = default_appl_ver_id,
          .heartbeat_interval_seconds = 30U,
          .is_initiator = true,
        },
      .begin_string = begin_string,
      .sender_comp_id = "BUY",
      .target_comp_id = "SELL",
      .default_appl_ver_id = default_appl_ver_id,
      .heartbeat_interval_seconds = 30U,
    },
    dictionary,
    &initiator_store);

  auto status = ActivateProtocolPair(initiator, acceptor);
  if (!status.ok()) {
    return status;
  }

  const auto sample = BuildSampleMessage();
  nimble::message::Message augmented_sample;
  const auto& bench_message = [&]() -> const nimble::message::Message& {
    if (dictionary.find_field(5001U) != nullptr) {
      nimble::message::MessageBuilder builder{ "D" };
      PopulateFix44MessageBuilder(builder, BuildFix44BusinessOrder());
      builder.set(5001U, "L");
      augmented_sample = std::move(builder).build();
      return augmented_sample;
    }
    return sample;
  }();

  const nimble::session::SessionSendEnvelope envelope{ "DESK-9", "ROUTE-7" };
  nimble::codec::EncodeOptions reserve_options;
  reserve_options.begin_string = begin_string;
  reserve_options.sender_comp_id = "BUY";
  reserve_options.sender_sub_id = envelope.sender_sub_id;
  reserve_options.target_comp_id = "SELL";
  reserve_options.target_sub_id = envelope.target_sub_id;
  reserve_options.default_appl_ver_id = default_appl_ver_id;
  reserve_options.msg_seq_num = 2U;
  reserve_options.sending_time = "20260417-12:34:56.789";
  auto reserved_frame = nimble::codec::EncodeFixMessage(bench_message, dictionary, reserve_options);
  if (!reserved_frame.ok()) {
    return reserved_frame.status();
  }
  initiator_store.ReserveAdditionalSessionStorage(
    242U, iterations, 0U, reserved_frame.value().size() * static_cast<std::size_t>(iterations));

  nimble::session::EncodedApplicationMessage encoded_message;
  if (pre_encoded) {
    auto prepared = BuildPreEncodedApplicationMessage(bench_message,
                                                      dictionary,
                                                      begin_string,
                                                      "BUY",
                                                      envelope.sender_sub_id,
                                                      "SELL",
                                                      envelope.target_sub_id,
                                                      default_appl_ver_id);
    if (!prepared.ok()) {
      return prepared.status();
    }
    encoded_message = std::move(prepared).value();
  }

  BenchmarkResult result;
  result.samples_ns.reserve(iterations);
  result.work_label = "messages";

  BenchmarkMeasurement measurement;
  for (std::uint32_t index = 0; index < iterations; ++index) {
    const auto started = std::chrono::steady_clock::now();
    auto outbound = pre_encoded ? initiator.SendEncodedApplication(encoded_message, 10'000U + index, envelope.view())
                                : initiator.SendApplication(bench_message, 10'000U + index, envelope.view());
    const auto finished = std::chrono::steady_clock::now();
    if (!outbound.ok()) {
      return outbound.status();
    }
    result.samples_ns.push_back(DurationNs(started, finished));
    ++result.work_count;
  }
  measurement.Finish(result);
  return result;
}

} // namespace

#if defined(_MSC_VER)
__declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static void
FreeBenchmarkMemory(void* memory) noexcept
{
  std::free(memory);
}

void*
operator new(std::size_t size)
{
  auto* memory = bench_support::AllocateRaw(size, alignof(std::max_align_t));
  if (memory == nullptr) {
    throw std::bad_alloc();
  }
  bench_support::RecordAllocation(size);
  return memory;
}

void*
operator new[](std::size_t size)
{
  auto* memory = bench_support::AllocateRaw(size, alignof(std::max_align_t));
  if (memory == nullptr) {
    throw std::bad_alloc();
  }
  bench_support::RecordAllocation(size);
  return memory;
}

void*
operator new(std::size_t size, const std::nothrow_t&) noexcept
{
  auto* memory = bench_support::AllocateRaw(size, alignof(std::max_align_t));
  if (memory != nullptr) {
    bench_support::RecordAllocation(size);
  }
  return memory;
}

void*
operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
  auto* memory = bench_support::AllocateRaw(size, alignof(std::max_align_t));
  if (memory != nullptr) {
    bench_support::RecordAllocation(size);
  }
  return memory;
}

void*
operator new(std::size_t size, std::align_val_t alignment)
{
  auto* memory = bench_support::AllocateRaw(size, static_cast<std::size_t>(alignment));
  if (memory == nullptr) {
    throw std::bad_alloc();
  }
  bench_support::RecordAllocation(size);
  return memory;
}

void*
operator new[](std::size_t size, std::align_val_t alignment)
{
  auto* memory = bench_support::AllocateRaw(size, static_cast<std::size_t>(alignment));
  if (memory == nullptr) {
    throw std::bad_alloc();
  }
  bench_support::RecordAllocation(size);
  return memory;
}

void*
operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
  auto* memory = bench_support::AllocateRaw(size, static_cast<std::size_t>(alignment));
  if (memory != nullptr) {
    bench_support::RecordAllocation(size);
  }
  return memory;
}

void*
operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
  auto* memory = bench_support::AllocateRaw(size, static_cast<std::size_t>(alignment));
  if (memory != nullptr) {
    bench_support::RecordAllocation(size);
  }
  return memory;
}

void
operator delete(void* memory) noexcept
{
  FreeBenchmarkMemory(memory);
}

void
operator delete[](void* memory) noexcept
{
  FreeBenchmarkMemory(memory);
}

void
operator delete(void* memory, std::size_t) noexcept
{
  FreeBenchmarkMemory(memory);
}

void
operator delete[](void* memory, std::size_t) noexcept
{
  FreeBenchmarkMemory(memory);
}

void
operator delete(void* memory, std::align_val_t) noexcept
{
  FreeBenchmarkMemory(memory);
}

void
operator delete[](void* memory, std::align_val_t) noexcept
{
  FreeBenchmarkMemory(memory);
}

void
operator delete(void* memory, std::size_t, std::align_val_t) noexcept
{
  FreeBenchmarkMemory(memory);
}

void
operator delete[](void* memory, std::size_t, std::align_val_t) noexcept
{
  FreeBenchmarkMemory(memory);
}

int
main(int argc, char** argv)
{
  std::filesystem::path artifact_path;
  std::vector<std::filesystem::path> dictionary_paths;
  std::uint32_t iterations = 100000U;
  std::uint32_t loopback_iterations = 1000U;
  std::uint32_t replay_iterations = 1000U;
  std::uint32_t replay_span = 128U;
  std::string begin_string{ "FIX.4.4" };
  std::string default_appl_ver_id;

  for (int index = 1; index < argc; ++index) {
    const std::string_view arg(argv[index]);
    if (arg == "--artifact" && index + 1 < argc) {
      artifact_path = argv[++index];
      continue;
    }
    if (arg == "--dictionary" && index + 1 < argc) {
      dictionary_paths.emplace_back(argv[++index]);
      continue;
    }
    if (arg == "--iterations" && index + 1 < argc) {
      iterations = static_cast<std::uint32_t>(std::stoul(argv[++index]));
      continue;
    }
    if (arg == "--loopback" && index + 1 < argc) {
      loopback_iterations = static_cast<std::uint32_t>(std::stoul(argv[++index]));
      continue;
    }
    if (arg == "--replay" && index + 1 < argc) {
      replay_iterations = static_cast<std::uint32_t>(std::stoul(argv[++index]));
      continue;
    }
    if (arg == "--replay-span" && index + 1 < argc) {
      replay_span = static_cast<std::uint32_t>(std::stoul(argv[++index]));
      continue;
    }
    if (arg == "--begin-string" && index + 1 < argc) {
      begin_string = argv[++index];
      continue;
    }
    if (arg == "--default-appl-ver-id" && index + 1 < argc) {
      default_appl_ver_id = argv[++index];
      continue;
    }
    PrintUsage();
    return 1;
  }

  if (!artifact_path.empty() && !dictionary_paths.empty()) {
    PrintUsage();
    return 1;
  }

  if (artifact_path.empty() && dictionary_paths.empty()) {
    artifact_path = std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / "build/bench/quickfix_FIX44.nfa";
  } else if (!artifact_path.is_absolute()) {
    artifact_path = std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / artifact_path;
  }

  for (auto& dictionary_path : dictionary_paths) {
    if (!dictionary_path.is_absolute()) {
      dictionary_path = std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / dictionary_path;
    }
  }

  auto dictionary = LoadDictionary(artifact_path, dictionary_paths);
  if (!dictionary.ok()) {
    std::cerr << dictionary.status().message() << '\n';
    return 1;
  }

  const auto fix44_business_order = BuildFix44BusinessOrder();
  const auto sample = BuildFix44MessageFromBusinessOrder(fix44_business_order);
  nimble::codec::EncodeOptions options;
  options.begin_string = begin_string;
  options.sender_comp_id = "BUY";
  options.target_comp_id = "SELL";
  options.default_appl_ver_id = default_appl_ver_id;
  options.sending_time = "20260406-12:34:56.789";

  auto warmup = nimble::codec::EncodeFixMessage(sample, dictionary.value(), options);
  if (!warmup.ok()) {
    std::cerr << warmup.status().message() << '\n';
    return 1;
  }

  auto encode_result = RunEncodeBenchmark(fix44_business_order, dictionary.value(), options, iterations);
  if (!encode_result.ok()) {
    std::cerr << encode_result.status().message() << '\n';
    return 1;
  }

  auto compiled_decoders = nimble::codec::CompiledDecoderTable::Build(dictionary.value());

  BenchmarkResult parse_result;
  parse_result.samples_ns.reserve(iterations);
  BenchmarkMeasurement parse_measurement;
  double parse_sink = 0.0;
  nimble::codec::DecodedMessageView parse_decoded;
  for (std::uint32_t index = 0; index < iterations; ++index) {
    const auto sample_started = std::chrono::steady_clock::now();
    auto decode_status =
      nimble::codec::DecodeFixMessageView(warmup.value(), dictionary.value(), compiled_decoders, &parse_decoded);
    if (!decode_status.ok()) {
      std::cerr << decode_status.message() << '\n';
      return 1;
    }
    const auto extracted = ExtractOrderFromMessageView(parse_decoded.message.view());
    parse_result.samples_ns.push_back(DurationNs(sample_started, std::chrono::steady_clock::now()));
    parse_sink += extracted.order_qty;
  }
  parse_measurement.Finish(parse_result);
  static_cast<void>(parse_sink);

  auto session_benchmark = RunSessionBenchmark(dictionary.value(), iterations, begin_string, default_appl_ver_id);
  if (!session_benchmark.ok()) {
    std::cerr << session_benchmark.status().message() << '\n';
    return 1;
  }

  auto session_outbound =
    RunSessionOutboundBenchmark(dictionary.value(), iterations, begin_string, default_appl_ver_id, false);
  if (!session_outbound.ok()) {
    std::cerr << session_outbound.status().message() << '\n';
    return 1;
  }

  auto session_outbound_pre_encoded =
    RunSessionOutboundBenchmark(dictionary.value(), iterations, begin_string, default_appl_ver_id, true);
  if (!session_outbound_pre_encoded.ok()) {
    std::cerr << session_outbound_pre_encoded.status().message() << '\n';
    return 1;
  }

  // --- Shared benchmarks (aligned with QuickFIX compare order) ---
  std::vector<LabeledResult> results;
  results.push_back({ "encode", std::move(encode_result).value() });
  results.push_back({ "parse", std::move(parse_result) });
  results.push_back({ "session-inbound", std::move(session_benchmark).value() });

  if (replay_iterations > 0U) {
    auto replay =
      RunReplayBenchmark(dictionary.value(), replay_iterations, replay_span, begin_string, default_appl_ver_id);
    if (!replay.ok()) {
      std::cerr << replay.status().message() << '\n';
      return 1;
    }
    results.push_back({ "replay", std::move(replay).value() });
  } else {
    std::cout << "replay skipped: --replay 0\n";
  }

  if (loopback_iterations > 0U) {
    auto loopback = RunLoopbackBenchmark(dictionary.value(), loopback_iterations, begin_string, default_appl_ver_id);
    if (!loopback.ok()) {
      std::cerr << loopback.status().message() << '\n';
      return 1;
    }
    results.push_back({ "loopback-roundtrip", std::move(loopback).value() });
  } else {
    std::cout << "loopback skipped: --loopback 0\n";
  }

  // --- NimbleFIX-only benchmarks ---
  results.push_back({ "session-outbound", std::move(session_outbound).value() });
  results.push_back({ "session-outbound-pre-encoded", std::move(session_outbound_pre_encoded).value() });

  BenchmarkResult peek_result;
  peek_result.samples_ns.reserve(iterations);
  BenchmarkMeasurement peek_measurement;
  for (std::uint32_t index = 0; index < iterations; ++index) {
    const auto sample_started = std::chrono::steady_clock::now();
    auto header = nimble::codec::PeekSessionHeaderView(warmup.value());
    if (!header.ok()) {
      std::cerr << header.status().message() << '\n';
      return 1;
    }
    peek_result.samples_ns.push_back(DurationNs(sample_started, std::chrono::steady_clock::now()));
  }
  peek_measurement.Finish(peek_result);
  results.push_back({ "peek", std::move(peek_result) });

  bench_support::PrintResultTable(results);
  return 0;
}