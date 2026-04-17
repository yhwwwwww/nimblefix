#include <filesystem>
#include <new>
#include <optional>
#include <string>
#include <string_view>

#include <arpa/inet.h>
#include <condition_variable>
#include <mutex>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

#include "bench_support.h"

#include "DataDictionary.h"
#include "Field.h"
#include "Message.h"
#include "fix44/NewOrderSingle.h"

#include "Application.h"
#include "DataDictionaryProvider.h"
#include "MessageStore.h"
#include "Responder.h"
#include "Session.h"
#include "SessionID.h"
#include "SessionSettings.h"
#include "ThreadedSocketAcceptor.h"
#include "ThreadedSocketInitiator.h"
#include "TimeRange.h"
#include "fix44/ExecutionReport.h"
#include "fix44/ResendRequest.h"

namespace {

using bench_support::BenchmarkMeasurement;
using bench_support::BenchmarkResult;
using bench_support::BuildFix44BusinessOrder;
using bench_support::Fix44BusinessOrder;
using bench_support::ReportMetric;

auto
PrintUsage() -> void
{
  std::cout << "usage: quickfix-cpp-bench --xml <FIX44.xml> [--iterations <count>] "
               "[--loopback <count>] [--replay <count>] [--replay-span <count>]\n";
}

auto
ToQuickFixTimestamp(const bench_support::BenchmarkTimestamp& timestamp) -> FIX::UtcTimeStamp
{
  return FIX::UtcTimeStamp(timestamp.hour,
                           timestamp.minute,
                           timestamp.second,
                           timestamp.millisecond,
                           timestamp.day,
                           timestamp.month,
                           timestamp.year);
}

auto
BuildOrderFromBusinessObject(const Fix44BusinessOrder& order_request) -> FIX44::NewOrderSingle
{
  FIX44::NewOrderSingle order(FIX::ClOrdID(std::string(order_request.cl_ord_id)),
                              FIX::Side(order_request.side),
                              FIX::TransactTime(ToQuickFixTimestamp(order_request.transact_time)),
                              FIX::OrdType(order_request.ord_type));
  order.set(FIX::Symbol(std::string(order_request.symbol)));
  order.set(FIX::OrderQty(static_cast<double>(order_request.order_qty)));
  if (order_request.price.has_value()) {
    order.set(FIX::Price(order_request.price.value()));
  }

  FIX44::NewOrderSingle::NoPartyIDs party;
  party.set(FIX::PartyID(std::string(order_request.party_id)));
  party.set(FIX::PartyIDSource(order_request.party_id_source));
  party.set(FIX::PartyRole(static_cast<int>(order_request.party_role)));
  order.addGroup(party);
  return order;
}

auto
ApplyStaticHeader(FIX44::NewOrderSingle* order) -> void
{
  if (order == nullptr) {
    return;
  }
  order->getHeader().setField(FIX::BeginString(std::string(bench_support::kDefaultBeginString)));
  order->getHeader().setField(FIX::SenderCompID(std::string(bench_support::kDefaultSenderCompId)));
  order->getHeader().setField(FIX::TargetCompID(std::string(bench_support::kDefaultTargetCompId)));
}

auto
ApplyBenchmarkHeader(FIX44::NewOrderSingle* order, std::uint32_t msg_seq_num, const FIX::UtcTimeStamp& sending_time)
  -> void
{
  ApplyStaticHeader(order);
  if (order == nullptr) {
    return;
  }
  order->getHeader().setField(FIX::MsgSeqNum(static_cast<int>(msg_seq_num)));
  order->getHeader().setField(FIX::SendingTime(sending_time));
}

auto
BuildSampleFrame(const Fix44BusinessOrder& business_order, const FIX::UtcTimeStamp& sending_time) -> std::string
{
  auto sample = BuildOrderFromBusinessObject(business_order);
  ApplyBenchmarkHeader(&sample, 1U, sending_time);
  return sample.toString();
}

auto
ReportQuickFixMetric(const std::string& label, const BenchmarkResult& result) -> void
{
  ReportMetric(label, result, 44);
}

using bench_support::LabeledResult;

auto
ExtractOrderFromQFMessage(FIX44::NewOrderSingle& order) -> bench_support::ParsedOrder
{
  bench_support::ParsedOrder parsed;

  FIX::ClOrdID cl_ord_id;
  if (order.isSet(cl_ord_id)) {
    order.get(cl_ord_id);
    parsed.cl_ord_id = std::string_view(cl_ord_id.getString());
  }

  FIX::Symbol symbol;
  if (order.isSet(symbol)) {
    order.get(symbol);
    parsed.symbol = std::string_view(symbol.getString());
  }

  FIX::Side side;
  if (order.isSet(side)) {
    order.get(side);
    parsed.side = side.getValue();
  }

  FIX::TransactTime transact_time;
  if (order.isSet(transact_time)) {
    order.get(transact_time);
    parsed.transact_time = std::string_view(transact_time.getString());
  }

  FIX::OrderQty order_qty;
  if (order.isSet(order_qty)) {
    order.get(order_qty);
    parsed.order_qty = order_qty.getValue();
  }

  FIX::OrdType ord_type;
  if (order.isSet(ord_type)) {
    order.get(ord_type);
    parsed.ord_type = ord_type.getValue();
  }

  FIX::Price price;
  if (order.isSet(price)) {
    order.get(price);
    parsed.price = price.getValue();
    parsed.has_price = true;
  }

  FIX::NoPartyIDs no_party_ids;
  if (order.isSet(no_party_ids)) {
    FIX44::NewOrderSingle::NoPartyIDs party;
    order.getGroup(1, party);

    FIX::PartyID pid;
    if (party.isSet(pid)) {
      party.get(pid);
      parsed.party_id = std::string_view(pid.getString());
    }
    FIX::PartyIDSource pid_src;
    if (party.isSet(pid_src)) {
      party.get(pid_src);
      parsed.party_id_source = pid_src.getValue();
    }
    FIX::PartyRole prole;
    if (party.isSet(prole)) {
      party.get(prole);
      parsed.party_role = prole.getValue();
    }
  }

  return parsed;
}

auto
RunParseBenchmark(const FIX::DataDictionary& dictionary, const std::string& sample_frame, std::uint32_t iterations)
  -> BenchmarkResult
{
  BenchmarkResult result;
  result.samples_ns.reserve(iterations);
  BenchmarkMeasurement measurement;
  double parse_sink = 0.0;
  for (std::uint32_t index = 0; index < iterations; ++index) {
    const auto started = std::chrono::steady_clock::now();
    FIX44::NewOrderSingle parsed;
    parsed.setString(sample_frame, true, &dictionary, &dictionary);
    const auto extracted = ExtractOrderFromQFMessage(parsed);
    result.samples_ns.push_back(bench_support::DurationNs(started, std::chrono::steady_clock::now()));
    parse_sink += extracted.order_qty;
  }
  measurement.Finish(result);
  static_cast<void>(parse_sink);
  return result;
}

auto
RunEncodeBenchmark(const Fix44BusinessOrder& business_order,
                   const FIX::UtcTimeStamp& sending_time,
                   std::uint32_t iterations) -> BenchmarkResult
{
  BenchmarkResult result;
  result.samples_ns.reserve(iterations);
  BenchmarkMeasurement measurement;
  for (std::uint32_t index = 0; index < iterations; ++index) {
    const auto started = std::chrono::steady_clock::now();
    auto order = BuildOrderFromBusinessObject(business_order);
    ApplyBenchmarkHeader(&order, index + 1U, sending_time);
    const auto encoded = order.toString();
    static_cast<void>(encoded);
    result.samples_ns.push_back(bench_support::DurationNs(started, std::chrono::steady_clock::now()));
  }
  measurement.Finish(result);
  return result;
}

auto
RunEncodeBufferBenchmark(const Fix44BusinessOrder& business_order,
                         const FIX::UtcTimeStamp& sending_time,
                         std::uint32_t iterations) -> BenchmarkResult
{
  BenchmarkResult result;
  result.samples_ns.reserve(iterations);
  std::string buffer;
  BenchmarkMeasurement measurement;
  for (std::uint32_t index = 0; index < iterations; ++index) {
    const auto started = std::chrono::steady_clock::now();
    auto order = BuildOrderFromBusinessObject(business_order);
    ApplyBenchmarkHeader(&order, index + 1U, sending_time);
    order.toString(buffer);
    result.samples_ns.push_back(bench_support::DurationNs(started, std::chrono::steady_clock::now()));
  }
  measurement.Finish(result);
  return result;
}

// ---------------------------------------------------------------------------
// In-process session benchmark helpers
// ---------------------------------------------------------------------------

class NullApplication : public FIX::Application
{
public:
  void onCreate(const FIX::SessionID&) override {}
  void onLogon(const FIX::SessionID&) override {}
  void onLogout(const FIX::SessionID&) override {}
  void toAdmin(FIX::Message&, const FIX::SessionID&) override {}
  void toApp(FIX::Message&, const FIX::SessionID&) EXCEPT(FIX::DoNotSend) override {}
  void fromAdmin(const FIX::Message&, const FIX::SessionID&)
    EXCEPT(FIX::FieldNotFound, FIX::IncorrectDataFormat, FIX::IncorrectTagValue, FIX::RejectLogon) override
  {
  }
  void fromApp(const FIX::Message&, const FIX::SessionID&)
    EXCEPT(FIX::FieldNotFound, FIX::IncorrectDataFormat, FIX::IncorrectTagValue, FIX::UnsupportedMessageType) override
  {
  }
};

class BufferedResponder : public FIX::Responder
{
public:
  std::vector<std::string> frames;
  bool send(const std::string& msg) override
  {
    frames.push_back(msg);
    return true;
  }
  void disconnect() override {}
  void clear() { frames.clear(); }
};

// Performs the logon handshake between two in-process sessions.
// Precondition: sessions have been constructed (registered); responders are
// valid.
auto
DoInProcessLogon(FIX::Session& initiator_session,
                 FIX::Session& acceptor_session,
                 BufferedResponder& initiator_resp,
                 BufferedResponder& acceptor_resp) -> bool
{
  initiator_session.setIsNonStopSession(true);
  initiator_session.setCheckLatency(false);
  acceptor_session.setIsNonStopSession(true);
  acceptor_session.setCheckLatency(false);

  initiator_session.logon();
  acceptor_session.logon();

  // setResponder may call checkSessionTime; isNonStopSession must be true
  // first.
  initiator_session.setResponder(&initiator_resp);
  acceptor_session.setResponder(&acceptor_resp);

  const auto now = FIX::UtcTimeStamp::now();

  // Initiator drives logon.
  initiator_session.next(now);

  // Feed initiator's logon frame to acceptor; acceptor sends logon-ack.
  for (auto& frame : initiator_resp.frames) {
    acceptor_session.next(frame, now);
  }
  initiator_resp.clear();

  // Feed acceptor's logon-ack to initiator.
  for (auto& frame : acceptor_resp.frames) {
    initiator_session.next(frame, now);
  }
  acceptor_resp.clear();

  return initiator_session.isLoggedOn() && acceptor_session.isLoggedOn();
}

auto
RunQFSessionInboundBenchmark(const FIX::DataDictionary& /*dictionary*/,
                             const std::filesystem::path& xml_path,
                             const Fix44BusinessOrder& business_order,
                             const FIX::UtcTimeStamp& sending_time,
                             std::uint32_t iterations) -> BenchmarkResult
{
  FIX::MemoryStoreFactory store_factory;
  NullApplication app;

  FIX::DataDictionaryProvider dict_provider;
  auto shared_dict = std::make_shared<FIX::DataDictionary>(xml_path.string());
  dict_provider.addTransportDataDictionary(FIX::BeginString("FIX.4.4"), shared_dict);

  FIX::UtcTimeOnly midnight(0, 0, 0);
  FIX::TimeRange always(midnight, midnight);

  // Declare responders before sessions so sessions (destroyed first in reverse
  // order) do not outlive their responders.
  BufferedResponder initiator_resp;
  BufferedResponder acceptor_resp;

  FIX::Session initiator_session([]() { return FIX::UtcTimeStamp::now(); },
                                 app,
                                 store_factory,
                                 FIX::SessionID("FIX.4.4", "BUY", "SELL"),
                                 dict_provider,
                                 always,
                                 30,
                                 nullptr);

  FIX::Session acceptor_session([]() { return FIX::UtcTimeStamp::now(); },
                                app,
                                store_factory,
                                FIX::SessionID("FIX.4.4", "SELL", "BUY"),
                                dict_provider,
                                always,
                                0,
                                nullptr);

  DoInProcessLogon(initiator_session, acceptor_session, initiator_resp, acceptor_resp);

  // Pre-build application message frames. Seqnums start at 2 (logon used seqnum
  // 1).
  std::vector<std::string> app_frames;
  app_frames.reserve(iterations);
  for (std::uint32_t i = 0; i < iterations; ++i) {
    auto order = BuildOrderFromBusinessObject(business_order);
    ApplyBenchmarkHeader(&order, i + 2U, sending_time);
    app_frames.push_back(order.toString());
  }

  const auto now = FIX::UtcTimeStamp::now();
  BenchmarkResult result;
  result.samples_ns.reserve(iterations);
  BenchmarkMeasurement measurement;
  for (auto& frame : app_frames) {
    const auto started = std::chrono::steady_clock::now();
    acceptor_session.next(frame, now);
    result.samples_ns.push_back(bench_support::DurationNs(started, std::chrono::steady_clock::now()));
    // Drain any incidental outbound admin frames outside timing.
    acceptor_resp.clear();
  }
  measurement.Finish(result);

  return result;
}

auto
RunQFReplayBenchmark(const FIX::DataDictionary& /*dictionary*/,
                     const std::filesystem::path& xml_path,
                     const Fix44BusinessOrder& business_order,
                     const FIX::UtcTimeStamp& sending_time,
                     std::uint32_t iterations,
                     std::uint32_t replay_span) -> BenchmarkResult
{
  FIX::MemoryStoreFactory store_factory;
  NullApplication app;

  FIX::DataDictionaryProvider dict_provider;
  auto shared_dict = std::make_shared<FIX::DataDictionary>(xml_path.string());
  dict_provider.addTransportDataDictionary(FIX::BeginString("FIX.4.4"), shared_dict);

  FIX::UtcTimeOnly midnight(0, 0, 0);
  FIX::TimeRange always(midnight, midnight);

  BufferedResponder initiator_resp;
  BufferedResponder acceptor_resp;

  FIX::Session initiator_session([]() { return FIX::UtcTimeStamp::now(); },
                                 app,
                                 store_factory,
                                 FIX::SessionID("FIX.4.4", "BUY", "SELL"),
                                 dict_provider,
                                 always,
                                 30,
                                 nullptr);

  FIX::Session acceptor_session([]() { return FIX::UtcTimeStamp::now(); },
                                app,
                                store_factory,
                                FIX::SessionID("FIX.4.4", "SELL", "BUY"),
                                dict_provider,
                                always,
                                0,
                                nullptr);

  DoInProcessLogon(initiator_session, acceptor_session, initiator_resp, acceptor_resp);

  // Acceptor sends replay_span messages. They are stored in its MessageStore at
  // outgoing seqnums 2..replay_span+1 (seqnum 1 was the logon-ack).
  FIX44::NewOrderSingle sample_msg = BuildOrderFromBusinessObject(business_order);
  for (std::uint32_t i = 0; i < replay_span; ++i) {
    acceptor_session.send(sample_msg);
    acceptor_resp.clear();
    initiator_resp.clear();
  }

  // Pre-build N ResendRequest frames from the initiator side.
  // The acceptor received initiator seqnum 1 (logon), so it expects seqnum 2
  // next. Each ResendRequest uses a consecutive seqnum.
  std::vector<std::string> resend_frames;
  resend_frames.reserve(iterations);
  for (std::uint32_t i = 0; i < iterations; ++i) {
    FIX44::ResendRequest resend_req(FIX::BeginSeqNo(2), FIX::EndSeqNo(static_cast<int>(replay_span) + 1));
    resend_req.getHeader().setField(FIX::BeginString("FIX.4.4"));
    resend_req.getHeader().setField(FIX::SenderCompID("BUY"));
    resend_req.getHeader().setField(FIX::TargetCompID("SELL"));
    resend_req.getHeader().setField(FIX::MsgSeqNum(static_cast<int>(i) + 2));
    resend_req.getHeader().setField(FIX::SendingTime(sending_time));
    resend_frames.push_back(resend_req.toString());
  }

  const auto now = FIX::UtcTimeStamp::now();
  BenchmarkResult result;
  result.work_label = "frames";
  result.samples_ns.reserve(iterations);
  BenchmarkMeasurement measurement;
  for (auto& frame : resend_frames) {
    const auto started = std::chrono::steady_clock::now();
    acceptor_session.next(frame, now);
    result.samples_ns.push_back(bench_support::DurationNs(started, std::chrono::steady_clock::now()));
    result.work_count += static_cast<std::uint64_t>(acceptor_resp.frames.size());
    acceptor_resp.clear();
    initiator_resp.clear();
  }
  measurement.Finish(result);

  return result;
}

// ---------------------------------------------------------------------------
// Loopback benchmark helpers
// ---------------------------------------------------------------------------

static auto
FindEphemeralPort() -> int
{
  const int sock = ::socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    return 19955;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = 0;
  addr.sin_addr.s_addr = INADDR_ANY;
  socklen_t addr_len = static_cast<socklen_t>(sizeof(addr));
  if (::bind(sock, reinterpret_cast<sockaddr*>(&addr), addr_len) < 0) {
    ::close(sock);
    return 19955;
  }
  if (::getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &addr_len) < 0) {
    ::close(sock);
    return 19955;
  }
  const int port = ntohs(addr.sin_port);
  ::close(sock);
  return port;
}

class QFLoopbackApplication : public FIX::Application
{
public:
  std::mutex mutex;
  std::condition_variable cv;
  std::atomic<int> logon_count{ 0 };
  std::atomic<std::uint32_t> ack_count{ 0 };

  void waitForLogon()
  {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [this] { return logon_count.load(std::memory_order_acquire) >= 2; });
  }

  void waitForAck(std::uint32_t expected)
  {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait(lock, [this, expected] { return ack_count.load(std::memory_order_acquire) >= expected; });
  }

  void onCreate(const FIX::SessionID&) override {}

  void onLogon(const FIX::SessionID&) override
  {
    {
      std::lock_guard<std::mutex> lk(mutex);
      logon_count.fetch_add(1, std::memory_order_acq_rel);
    }
    cv.notify_all();
  }

  void onLogout(const FIX::SessionID&) override {}
  void toAdmin(FIX::Message&, const FIX::SessionID&) override {}
  void toApp(FIX::Message&, const FIX::SessionID&) EXCEPT(FIX::DoNotSend) override {}

  void fromAdmin(const FIX::Message&, const FIX::SessionID&)
    EXCEPT(FIX::FieldNotFound, FIX::IncorrectDataFormat, FIX::IncorrectTagValue, FIX::RejectLogon) override
  {
  }

  void fromApp(const FIX::Message& msg, const FIX::SessionID& session_id)
    EXCEPT(FIX::FieldNotFound, FIX::IncorrectDataFormat, FIX::IncorrectTagValue, FIX::UnsupportedMessageType) override
  {
    FIX::MsgType msg_type;
    msg.getHeader().getField(msg_type);

    if (msg_type.getValue() == "D") {
      // Acceptor received NewOrderSingle; send ExecutionReport back
      // through the same (acceptor) session identified by session_id.
      FIX::ClOrdID cl_ord_id;
      FIX::OrderQty order_qty;
      FIX::Side side;
      msg.getFieldIfSet(cl_ord_id);
      msg.getFieldIfSet(order_qty);
      msg.getFieldIfSet(side);

      FIX44::ExecutionReport exec_rpt(FIX::OrderID("ORDER-1"),
                                      FIX::ExecID("EXEC-1"),
                                      FIX::ExecType('0'),
                                      FIX::OrdStatus('0'),
                                      side,
                                      FIX::LeavesQty(0.0),
                                      FIX::CumQty(0.0),
                                      FIX::AvgPx(0.0));
      exec_rpt.set(cl_ord_id);
      FIX::Session::sendToTarget(exec_rpt, session_id);
      return;
    }

    if (msg_type.getValue() == "8") {
      // Initiator received ExecutionReport; signal measurement loop.
      {
        std::lock_guard<std::mutex> lk(mutex);
        ack_count.fetch_add(1, std::memory_order_acq_rel);
      }
      cv.notify_one();
    }
  }
};

static auto
MakeAcceptorSettings(const std::filesystem::path& xml_path, int port) -> FIX::SessionSettings
{
  const std::string cfg = "[DEFAULT]\n"
                          "NonStopSession=Y\n"
                          "PersistMessages=N\n"
                          "CheckLatency=N\n"
                          "ResetOnLogon=Y\n"
                          "DataDictionary=" +
                          xml_path.string() +
                          "\n"
                          "[SESSION]\n"
                          "ConnectionType=acceptor\n"
                          "BeginString=FIX.4.4\n"
                          "SenderCompID=SELL\n"
                          "TargetCompID=BUY\n"
                          "HeartBtInt=30\n"
                          "SocketAcceptPort=" +
                          std::to_string(port) + "\n";
  std::istringstream stream(cfg);
  return FIX::SessionSettings(stream);
}

static auto
MakeInitiatorSettings(const std::filesystem::path& xml_path, int port) -> FIX::SessionSettings
{
  const std::string cfg = "[DEFAULT]\n"
                          "NonStopSession=Y\n"
                          "PersistMessages=N\n"
                          "CheckLatency=N\n"
                          "ResetOnLogon=Y\n"
                          "DataDictionary=" +
                          xml_path.string() +
                          "\n"
                          "[SESSION]\n"
                          "ConnectionType=initiator\n"
                          "BeginString=FIX.4.4\n"
                          "SenderCompID=BUY\n"
                          "TargetCompID=SELL\n"
                          "HeartBtInt=30\n"
                          "SocketConnectHost=127.0.0.1\n"
                          "SocketConnectPort=" +
                          std::to_string(port) +
                          "\n"
                          "ReconnectInterval=1\n";
  std::istringstream stream(cfg);
  return FIX::SessionSettings(stream);
}

auto
RunQFLoopbackBenchmark(const FIX::DataDictionary& /*dictionary*/,
                       const std::filesystem::path& xml_path,
                       const Fix44BusinessOrder& business_order,
                       std::uint32_t iterations) -> BenchmarkResult
{
  const int port = FindEphemeralPort();
  QFLoopbackApplication app;
  FIX::MemoryStoreFactory store_factory;

  auto acceptor_settings = MakeAcceptorSettings(xml_path, port);
  auto initiator_settings = MakeInitiatorSettings(xml_path, port);

  FIX::ThreadedSocketAcceptor acceptor(app, store_factory, acceptor_settings);
  FIX::ThreadedSocketInitiator initiator(app, store_factory, initiator_settings);

  acceptor.start();
  initiator.start();

  app.waitForLogon();

  const FIX::SessionID initiator_id("FIX.4.4", "BUY", "SELL");

  // Pre-warm: a few round-trips outside measurement.
  for (int i = 0; i < 5; ++i) {
    const std::uint32_t before = app.ack_count.load(std::memory_order_acquire);
    FIX44::NewOrderSingle warm_order = BuildOrderFromBusinessObject(business_order);
    FIX::Session::sendToTarget(warm_order, initiator_id);
    app.waitForAck(before + 1U);
  }

  BenchmarkResult result;
  result.samples_ns.reserve(iterations);
  BenchmarkMeasurement measurement;
  for (std::uint32_t i = 0; i < iterations; ++i) {
    const std::uint32_t before = app.ack_count.load(std::memory_order_acquire);
    const auto started = std::chrono::steady_clock::now();
    FIX44::NewOrderSingle order = BuildOrderFromBusinessObject(business_order);
    FIX::Session::sendToTarget(order, initiator_id);
    app.waitForAck(before + 1U);
    result.samples_ns.push_back(bench_support::DurationNs(started, std::chrono::steady_clock::now()));
  }
  measurement.Finish(result);

  initiator.stop();
  acceptor.stop();
  return result;
}

} // namespace

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
  std::free(memory);
}

void
operator delete[](void* memory) noexcept
{
  std::free(memory);
}

void
operator delete(void* memory, std::size_t) noexcept
{
  std::free(memory);
}

void
operator delete[](void* memory, std::size_t) noexcept
{
  std::free(memory);
}

void
operator delete(void* memory, std::align_val_t) noexcept
{
  std::free(memory);
}

void
operator delete[](void* memory, std::align_val_t) noexcept
{
  std::free(memory);
}

void
operator delete(void* memory, std::size_t, std::align_val_t) noexcept
{
  std::free(memory);
}

void
operator delete[](void* memory, std::size_t, std::align_val_t) noexcept
{
  std::free(memory);
}

int
main(int argc, char** argv)
{
  std::filesystem::path xml_path;
  std::uint32_t iterations = 100000U;
  std::uint32_t loopback_iterations = 1000U;
  std::uint32_t replay_iterations = 1000U;
  std::uint32_t replay_span = 128U;

  for (int index = 1; index < argc; ++index) {
    const std::string_view arg(argv[index]);
    if (arg == "--xml" && index + 1 < argc) {
      xml_path = argv[++index];
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
    PrintUsage();
    return 1;
  }

  if (xml_path.empty()) {
    PrintUsage();
    return 1;
  }

  FIX::DataDictionary dictionary(xml_path.string());
  const auto business_order = BuildFix44BusinessOrder();
  const auto fixed_sending_time = ToQuickFixTimestamp(business_order.transact_time);
  const auto sample_frame = BuildSampleFrame(business_order, fixed_sending_time);

  std::cout << "quickfix encode uses fixed SendingTime; quickfix-encode and "
               "quickfix-encode-buffer share the same serializer\n";

  const auto encode_result = RunEncodeBenchmark(business_order, fixed_sending_time, iterations);

  const auto parse_result = RunParseBenchmark(dictionary, sample_frame, iterations);

  auto session_inbound =
    RunQFSessionInboundBenchmark(dictionary, xml_path, business_order, fixed_sending_time, iterations);

  // --- Shared benchmarks (aligned with FastFix compare order) ---
  std::vector<LabeledResult> results;
  results.push_back({ "quickfix-encode", encode_result });
  results.push_back({ "quickfix-parse", parse_result });
  results.push_back({ "quickfix-session-inbound", session_inbound });

  if (replay_iterations > 0U) {
    auto replay =
      RunQFReplayBenchmark(dictionary, xml_path, business_order, fixed_sending_time, replay_iterations, replay_span);
    results.push_back({ "quickfix-replay", replay });
  } else {
    std::cout << "quickfix-replay skipped: --replay 0\n";
  }

  if (loopback_iterations > 0U) {
    auto loopback = RunQFLoopbackBenchmark(dictionary, xml_path, business_order, loopback_iterations);
    results.push_back({ "quickfix-loopback", loopback });
  } else {
    std::cout << "quickfix-loopback skipped: --loopback 0\n";
  }

  // --- QuickFIX-only benchmarks ---
  const auto encode_buffer_result = RunEncodeBufferBenchmark(business_order, fixed_sending_time, iterations);
  results.push_back({ "quickfix-encode-buffer", encode_buffer_result });

  bench_support::PrintResultTable(results);
  return 0;
}