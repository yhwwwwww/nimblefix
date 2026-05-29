#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <new>

#include "inbound_profile.h"
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

#include "fix44_api.h"
#include "nimblefix/codec/compiled_decoder.h"
#include "nimblefix/codec/fast_int_format.h"
#include "nimblefix/codec/fix_codec.h"
#include "nimblefix/codec/simd_scan.h"
#include "nimblefix/message/message_view.h"
#include "nimblefix/profile/normalized_dictionary.h"
#include "nimblefix/profile/profile_loader.h"
#include "nimblefix/runtime/session.h"
#include "nimblefix/session/admin_protocol.h"
#include "nimblefix/store/memory_store.h"
#include "nimblefix/transport/tcp_transport.h"

#include "bench_support.h"

namespace {

namespace fix44 = nimble::generated::profile_4400;

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

class BenchmarkCommandSink final : public nimble::session::SessionCommandSink
{
public:
  auto EnqueueOwnedMessage(std::uint64_t session_id, nimble::message::MessageRef message)
    -> nimble::base::Status override
  {
    if (!message.valid()) {
      return nimble::base::Status::InvalidArgument("benchmark sink received invalid message");
    }
    observed_ += session_id;
    observed_ += message.view().msg_type().size();
    ++enqueued_;
    return nimble::base::Status::Ok();
  }

  auto EnqueueOwnedEncodedMessage(std::uint64_t session_id, nimble::session::EncodedApplicationMessageRef message)
    -> nimble::base::Status override
  {
    if (!message.valid()) {
      return nimble::base::Status::InvalidArgument("benchmark sink received invalid encoded message");
    }
    auto view = message.view();
    observed_ += session_id;
    observed_ += view.msg_type.size();
    ++enqueued_;
    return nimble::base::Status::Ok();
  }

  auto LoadSnapshot(std::uint64_t session_id) const -> nimble::base::Result<nimble::session::SessionSnapshot> override
  {
    (void)session_id;
    return nimble::base::Status::InvalidArgument("benchmark sink does not provide snapshots");
  }

  auto Subscribe(std::uint64_t session_id, std::size_t queue_capacity)
    -> nimble::base::Result<nimble::session::SessionSubscription> override
  {
    (void)session_id;
    (void)queue_capacity;
    return nimble::base::Status::InvalidArgument("benchmark sink does not provide subscriptions");
  }

  [[nodiscard]] auto enqueued() const -> std::uint64_t { return enqueued_; }
  [[nodiscard]] auto observed() const -> std::uint64_t { return observed_; }

private:
  std::uint64_t enqueued_{ 0 };
  std::uint64_t observed_{ 0 };
};

class InlineProcessingSink final : public nimble::session::SessionCommandSink
{
public:
  explicit InlineProcessingSink(nimble::session::AdminProtocol* protocol)
    : protocol_(protocol)
  {
  }

  auto EnqueueOwnedMessage(std::uint64_t session_id, nimble::message::MessageRef message)
    -> nimble::base::Status override
  {
    (void)session_id;
    (void)message;
    return nimble::base::Status::InvalidArgument("decoded message send not supported by inline processing sink");
  }

  auto EnqueueOwnedEncodedMessage(std::uint64_t session_id, nimble::session::EncodedApplicationMessageRef message)
    -> nimble::base::Status override
  {
    (void)session_id;
    auto result = protocol_->SendEncodedApplication(message.view(), bench_support::NowNs(), {});
    if (!result.ok()) {
      return result.status();
    }
    ++processed_;
    return nimble::base::Status::Ok();
  }

  auto LoadSnapshot(std::uint64_t session_id) const -> nimble::base::Result<nimble::session::SessionSnapshot> override
  {
    (void)session_id;
    return nimble::base::Status::InvalidArgument("inline processing sink does not provide snapshots");
  }

  auto Subscribe(std::uint64_t session_id, std::size_t queue_capacity)
    -> nimble::base::Result<nimble::session::SessionSubscription> override
  {
    (void)session_id;
    (void)queue_capacity;
    return nimble::base::Status::InvalidArgument("inline processing sink does not provide subscriptions");
  }

  [[nodiscard]] auto processed() const -> std::uint64_t { return processed_; }

private:
  nimble::session::AdminProtocol* protocol_;
  std::uint64_t processed_{ 0 };
};

auto
ExtractOrderFromMessageView(nimble::message::MessageView view) -> bench_support::ParsedOrder
{
  const auto typed_order = fix44::NewOrderSingleView::Bind(view).value();
  bench_support::ParsedOrder parsed_order;
  if (auto v = typed_order.cl_ord_id()) {
    parsed_order.cl_ord_id = *v;
  }
  if (auto v = typed_order.symbol()) {
    parsed_order.symbol = *v;
  }
  if (auto v = typed_order.side(); v.ok()) {
    parsed_order.side = fix44::ToWire(v.value());
  }
  if (auto v = typed_order.transact_time()) {
    parsed_order.transact_time = *v;
  }
  if (auto v = typed_order.order_qty()) {
    parsed_order.order_qty = *v;
  }
  if (auto v = typed_order.ord_type(); v.ok()) {
    parsed_order.ord_type = fix44::ToWire(v.value());
  }
  if (auto v = typed_order.price()) {
    parsed_order.price = *v;
    parsed_order.has_price = true;
  }
  if (auto parties = typed_order.parties(); parties.has_value() && parties->size() > 0U) {
    auto entry = (*parties)[0U];
    if (auto v = entry.party_id()) {
      parsed_order.party_id = *v;
    }
    if (auto v = entry.party_id_source(); v.ok()) {
      parsed_order.party_id_source = fix44::ToWire(v.value());
    }
    if (auto v = entry.party_role(); v.ok()) {
      parsed_order.party_role = static_cast<int>(fix44::ToWire(v.value()));
    }
  }
  return parsed_order;
}

auto
PopulateGeneratedOrder(fix44::NewOrderSingleBuilder* order, const Fix44BusinessOrder& business_order) -> void;

auto
BuildFix44MessageFromBusinessOrder(const Fix44BusinessOrder& order) -> nimble::message::Message
{
  fix44::NewOrderSingleBuilder generated_order;
  PopulateGeneratedOrder(&generated_order, order);
  return generated_order.ToMessage().value();
}

auto
BuildSampleMessage(bool include_price = true) -> nimble::message::Message
{
  return BuildFix44MessageFromBusinessOrder(BuildFix44BusinessOrder(include_price));
}

auto
GeneratedSide(char value) -> nimble::generated::profile_4400::Side
{
  switch (value) {
    case '1':
      return nimble::generated::profile_4400::Side::Buy;
    case '2':
      return nimble::generated::profile_4400::Side::Sell;
    default:
      return static_cast<nimble::generated::profile_4400::Side>(value);
  }
}

auto
GeneratedOrdType(char value) -> nimble::generated::profile_4400::OrdType
{
  switch (value) {
    case '1':
      return nimble::generated::profile_4400::OrdType::Market;
    case '2':
      return nimble::generated::profile_4400::OrdType::Limit;
    default:
      return static_cast<nimble::generated::profile_4400::OrdType>(value);
  }
}

auto
GeneratedPartyIdSource(char value) -> nimble::generated::profile_4400::PartyIdSource
{
  switch (value) {
    case 'D':
      return nimble::generated::profile_4400::PartyIdSource::Proprietary;
    case 'G':
      return nimble::generated::profile_4400::PartyIdSource::Mic;
    default:
      return static_cast<nimble::generated::profile_4400::PartyIdSource>(value);
  }
}

auto
GeneratedPartyRole(std::int64_t value) -> nimble::generated::profile_4400::PartyRole
{
  switch (value) {
    case 1:
      return nimble::generated::profile_4400::PartyRole::ExecutingFirm;
    case 3:
      return nimble::generated::profile_4400::PartyRole::ClientId;
    case 4:
      return nimble::generated::profile_4400::PartyRole::ClearingFirm;
    default:
      return static_cast<nimble::generated::profile_4400::PartyRole>(value);
  }
}

auto
PopulateGeneratedOrder(nimble::generated::profile_4400::NewOrderSingleBuilder* order,
                       const Fix44BusinessOrder& business_order) -> void
{
  if (order == nullptr) {
    return;
  }

  order->clear();
  order->cl_ord_id(business_order.cl_ord_id)
    .symbol(business_order.symbol)
    .side(GeneratedSide(business_order.side))
    .transact_time(business_order.transact_time.text)
    .order_qty(business_order.order_qty)
    .ord_type(GeneratedOrdType(business_order.ord_type));
  if (business_order.price.has_value()) {
    order->price(business_order.price.value());
  }
  order->add_party()
    .party_id(business_order.party_id)
    .party_id_source(GeneratedPartyIdSource(business_order.party_id_source))
    .party_role(GeneratedPartyRole(business_order.party_role));
}

auto
BuildFix44OrderAckFromNewOrder(nimble::message::MessageView order, std::uint32_t execution_id)
  -> nimble::base::Result<nimble::message::Message>
{
  auto inbound = fix44::NewOrderSingleView::Bind(order);
  if (!inbound.ok()) {
    return inbound.status();
  }

  const auto cl_ord_id = inbound.value().cl_ord_id();
  const auto side = inbound.value().side();
  const auto order_qty = inbound.value().order_qty();
  if (!cl_ord_id.has_value() || !order_qty.has_value()) {
    return nimble::base::Status::InvalidArgument(
      "loopback benchmark NewOrderSingle missing required ack source fields");
  }
  if (!side.ok()) {
    return side.status();
  }

  const auto order_id = std::string("ORDER-") + std::to_string(execution_id);
  const auto exec_id = std::string("EXEC-") + std::to_string(execution_id);

  fix44::ExecutionReportBuilder ack;
  ack.order_id(order_id)
    .cl_ord_id(cl_ord_id.value())
    .exec_id(exec_id)
    .exec_type(fix44::ExecType::New)
    .ord_status(fix44::OrdStatus::New)
    .side(side.value())
    .order_qty(order_qty.value())
    .leaves_qty(order_qty.value())
    .cum_qty(0.0)
    .avg_px(0.0);
  if (auto symbol = inbound.value().symbol(); symbol.has_value()) {
    ack.symbol(symbol.value());
  }
  return ack.ToMessage();
}

auto
BuildFix44OrderAckFromBusinessOrder(const Fix44BusinessOrder& order, std::uint32_t execution_id)
  -> nimble::base::Result<nimble::message::Message>
{
  const auto order_id = std::string("ORDER-") + std::to_string(execution_id);
  const auto exec_id = std::string("EXEC-") + std::to_string(execution_id);

  fix44::ExecutionReportBuilder ack;
  ack.order_id(order_id)
    .cl_ord_id(order.cl_ord_id)
    .exec_id(exec_id)
    .exec_type(fix44::ExecType::New)
    .ord_status(fix44::OrdStatus::New)
    .side(GeneratedSide(order.side))
    .order_qty(order.order_qty)
    .leaves_qty(order.order_qty)
    .cum_qty(0.0)
    .avg_px(0.0)
    .symbol(order.symbol);
  return ack.ToMessage();
}

auto
EncodeFullFrame(std::string_view msg_type,
                std::span<const std::byte> body,
                std::string_view begin_string,
                std::string_view sender_comp_id,
                std::string_view target_comp_id,
                std::uint32_t seq_num,
                std::string_view sending_time,
                nimble::codec::EncodeBuffer* buffer) -> nimble::base::Status
{
  if (buffer == nullptr) {
    return nimble::base::Status::InvalidArgument("encode buffer is null");
  }
  if (msg_type.empty()) {
    return nimble::base::Status::InvalidArgument("message type is empty");
  }

  constexpr char kDelimiter = '\x01';
  constexpr std::size_t kInitialEncodeBufferBytes = 256U;
  constexpr std::size_t kBodyLengthPlaceholderWidth = 7U;

  auto& out = buffer->storage;
  out.clear();
  out.reserve(kInitialEncodeBufferBytes + body.size());

  out.append("8=");
  out.append(begin_string.empty() ? std::string_view("FIX.4.4") : begin_string);
  out.push_back(kDelimiter);
  out.append("9=");
  const auto body_length_offset = out.size();
  out.append(kBodyLengthPlaceholderWidth, '0');
  out.push_back(kDelimiter);
  const auto body_start = out.size();

  auto append_string_field = [&](std::string_view prefix, std::string_view value) {
    out.append(prefix);
    out.append(value);
    out.push_back(kDelimiter);
  };
  auto append_uint_field = [&](std::string_view prefix, std::uint32_t value) {
    char buf[10];
    const auto len = nimble::codec::FormatUint32(buf, value);
    out.append(prefix);
    out.append(buf, len);
    out.push_back(kDelimiter);
  };

  append_string_field("35=", msg_type);
  append_uint_field("34=", seq_num == 0U ? 1U : seq_num);
  append_string_field("49=", sender_comp_id);
  append_string_field("56=", target_comp_id);
  append_string_field("52=", sending_time);
  out.append(reinterpret_cast<const char*>(body.data()), body.size());

  const auto body_length = static_cast<std::uint32_t>(out.size() - body_start);
  {
    char buf[10];
    const auto len = nimble::codec::FormatUint32(buf, body_length);
    if (len > kBodyLengthPlaceholderWidth) {
      return nimble::base::Status::FormatError("encoded body length exceeds BodyLength placeholder width");
    }
    out.replace(body_length_offset, kBodyLengthPlaceholderWidth, buf, len);
  }

  const auto checksum = nimble::codec::ComputeChecksumSIMD(out.data(), out.size()) % 256U;
  out.append("10=");
  std::array<char, 3> checksum_digits{};
  checksum_digits[0] = static_cast<char>('0' + ((checksum / 100U) % 10U));
  checksum_digits[1] = static_cast<char>('0' + ((checksum / 10U) % 10U));
  checksum_digits[2] = static_cast<char>('0' + (checksum % 10U));
  out.append(checksum_digits.data(), checksum_digits.size());
  out.push_back(kDelimiter);

  return nimble::base::Status::Ok();
}

auto
RunTypedSessionSendBenchmark(const Fix44BusinessOrder& business_order,
                             std::uint32_t iterations,
                             std::string_view begin_string) -> nimble::base::Result<BenchmarkResult>
{
  constexpr std::string_view kFixedSendingTime = "20260406-12:34:56.789";
  nimble::codec::EncodeBuffer encode_buffer;

  BenchmarkResult result;
  result.samples_ns.reserve(iterations);
  result.work_label = "messages";

  BenchmarkMeasurement measurement;
  for (std::uint32_t index = 0; index < iterations; ++index) {
    const auto sample_started = std::chrono::steady_clock::now();

    fix44::NewOrderSingleBuilder builder;
    PopulateGeneratedOrder(&builder, business_order);
    nimble::generated::detail::BodyEncodeBuffer body_buffer;
    auto encode_status = builder.EncodeBody(body_buffer);
    if (!encode_status.ok()) {
      return encode_status;
    }

    auto frame_status = EncodeFullFrame(fix44::NewOrderSingle::kMsgType,
                                        body_buffer.bytes(),
                                        begin_string,
                                        "BUY",
                                        "SELL",
                                        index + 1U,
                                        kFixedSendingTime,
                                        &encode_buffer);
    if (!frame_status.ok()) {
      return frame_status;
    }

    result.samples_ns.push_back(DurationNs(sample_started, std::chrono::steady_clock::now()));
    ++result.work_count;
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

  fix44::ResendRequestBuilder resend_request;
  resend_request.begin_seq_no(static_cast<std::int64_t>(2)).end_seq_no(static_cast<std::int64_t>(replay_span + 1U));
  const auto resend_request_message = resend_request.ToMessage().value();

  std::vector<std::vector<std::byte>> requests;
  requests.reserve(iterations);
  for (std::uint32_t index = 0; index < iterations; ++index) {
    auto encoded =
      BuildFrame(resend_request_message, dictionary, begin_string, "BUY", "SELL", default_appl_ver_id, index + 2U);
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

  const auto business_order = BuildFix44BusinessOrder();
  std::vector<std::vector<std::byte>> inbound_frames;
  inbound_frames.reserve(iterations);
  for (std::uint32_t index = 0; index < iterations; ++index) {
    auto bench_message = BuildFix44OrderAckFromBusinessOrder(business_order, index + 1U);
    if (!bench_message.ok()) {
      return bench_message.status();
    }
    auto frame =
      BuildFrame(bench_message.value(), dictionary, begin_string, "SELL", "BUY", default_appl_ver_id, index + 2U);
    if (!frame.ok()) {
      return frame.status();
    }
    inbound_frames.push_back(std::move(frame).value());
  }
  initiator_store.ReserveAdditionalSessionStorage(241U, inbound_frames.size(), 0U, TotalFrameBytes(inbound_frames));

  BenchmarkResult result;
  result.samples_ns.reserve(iterations);
  result.work_label = "messages";

  BenchmarkMeasurement measurement;
  nimble::codec::DecodedMessageView decoded;
  for (const auto& frame : inbound_frames) {
    const auto started = std::chrono::steady_clock::now();
    auto decode_status =
      nimble::codec::DecodeFixMessageView(std::span<const std::byte>(frame.data(), frame.size()), dictionary, &decoded);
    if (!decode_status.ok()) {
      return decode_status;
    }
    auto event = initiator.OnInbound(decoded, NowNs());
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
RunOutboundBenchmark(const nimble::profile::NormalizedDictionaryView& dictionary,
                     const Fix44BusinessOrder& business_order,
                     std::uint32_t iterations,
                     std::string begin_string,
                     std::string default_appl_ver_id) -> nimble::base::Result<BenchmarkResult>
{
  nimble::store::MemorySessionStore sender_store;
  nimble::store::MemorySessionStore peer_store;

  nimble::session::AdminProtocol sender(
    nimble::session::AdminProtocolConfig{
      .session =
        nimble::session::SessionConfig{
          .session_id = 501U,
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
    &sender_store);

  nimble::session::AdminProtocol peer(
    nimble::session::AdminProtocolConfig{
      .session =
        nimble::session::SessionConfig{
          .session_id = 502U,
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
    &peer_store);

  auto status = ActivateProtocolPair(sender, peer);
  if (!status.ok()) {
    return status;
  }

  const auto message_count = static_cast<std::size_t>(iterations);
  constexpr std::size_t kReserveAdminRecords = 8U;
  constexpr std::size_t kReserveAdminBytes = 4096U;
  constexpr std::size_t kEstimatedFrameSize = 256U;
  sender_store.ReserveAdditionalSessionStorage(
    501U, message_count + kReserveAdminRecords, 0U, (message_count * kEstimatedFrameSize) + kReserveAdminBytes);

  auto sink = std::make_shared<InlineProcessingSink>(&sender);
  nimble::session::SessionHandle raw_handle(sender.session().session_id(), 0U, sink);
  nimble::runtime::Session<fix44::Profile> session(raw_handle);

  BenchmarkResult result;
  result.samples_ns.reserve(iterations);
  result.work_label = "messages";

  BenchmarkMeasurement measurement;
  for (std::uint32_t index = 0; index < iterations; ++index) {
    (void)index;
    const auto sample_started = std::chrono::steady_clock::now();
    auto status =
      session.send<fix44::NewOrderSingle>([&](auto& message) { PopulateGeneratedOrder(&message, business_order); });
    if (!status.ok()) {
      return status;
    }
    result.samples_ns.push_back(DurationNs(sample_started, std::chrono::steady_clock::now()));
    ++result.work_count;
  }
  measurement.Finish(result);

  if (sink->processed() != iterations) {
    return nimble::base::Status::InvalidArgument("outbound benchmark did not process all messages");
  }
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
  nimble::codec::EncodeOptions options;
  options.begin_string = begin_string;
  options.sender_comp_id = "BUY";
  options.target_comp_id = "SELL";
  options.default_appl_ver_id = default_appl_ver_id;
  options.sending_time = "20260406-12:34:56.789";

  auto warmup = nimble::codec::EncodeFixMessage(
    BuildFix44MessageFromBusinessOrder(fix44_business_order), dictionary.value(), options);
  if (!warmup.ok()) {
    std::cerr << warmup.status().message() << '\n';
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

  auto typed_session_send = RunTypedSessionSendBenchmark(fix44_business_order, iterations, begin_string);
  if (!typed_session_send.ok()) {
    std::cerr << typed_session_send.status().message() << '\n';
    return 1;
  }

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

  auto outbound_benchmark =
    RunOutboundBenchmark(dictionary.value(), fix44_business_order, iterations, begin_string, default_appl_ver_id);
  if (!outbound_benchmark.ok()) {
    std::cerr << outbound_benchmark.status().message() << '\n';
    return 1;
  }
  std::vector<LabeledResult> results;
  nimble::bench_profile::GetInboundProfile().dump();
  results.push_back({ "encode", std::move(typed_session_send).value() });
  results.push_back({ "outbound", std::move(outbound_benchmark).value() });
  results.push_back({ "inbound", std::move(session_benchmark).value() });
  results.push_back({ "parse", std::move(parse_result) });

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
    results.push_back({ "loopback", std::move(loopback).value() });
  } else {
    std::cout << "loopback skipped: --loopback 0\n";
  }

  results.push_back({ "peek", std::move(peek_result) });

  bench_support::PrintResultTable(results);
  return 0;
}
