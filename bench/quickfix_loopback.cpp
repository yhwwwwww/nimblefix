// bench/quickfix_loopback.cpp
//
// Two-process FIX loopback latency benchmark: QuickFIX acceptor process vs.
// initiator process. Forks the current binary: the child becomes the
// ThreadedSocketAcceptor,
//                           the parent becomes the ThreadedSocketInitiator.
//
// Sends both 35=D (NewOrderSingle) and 35=F (OrderCancelRequest) with the full
// standard FIX44 field set (Account, Currency, ExecInst, HandlInst,
// SecurityID/Source, OrderQty, OrdType, Price, Side, Symbol, TimeInForce,
// TransactTime, ExDestination, SecurityExchange, NoPartyIDs group) and matches
// acks to originating requests by ClOrdID.
//
// QuickFIX is inherently multi-threaded: send via FIX::Session::sendToTarget()
// from any thread; acks arrive on the QuickFIX IO thread via
// Application::fromApp().
//
// Measurement boundary:
//   T0 = before struct → FIX44::NewOrderSingle or FIX44::OrderCancelRequest
//   build T1 = inside fromApp() callback, after ExecutionReport → struct
//   extraction
//
// Usage:
//   quickfix-loopback --xml <path/to/FIX44.xml>
//                     [--iterations <N>]  default 1000 per message type
//                     [--warmup <W>]      default 50  per message type

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bench_support.h"

#include "Application.h"
#include "DataDictionary.h"
#include "DataDictionaryProvider.h"
#include "Field.h"
#include "Message.h"
#include "MessageStore.h"
#include "Session.h"
#include "SessionID.h"
#include "SessionSettings.h"
#include "ThreadedSocketAcceptor.h"
#include "ThreadedSocketInitiator.h"
#include "TimeRange.h"
#include "fix44/ExecutionReport.h"
#include "fix44/NewOrderSingle.h"
#include "fix44/OrderCancelRequest.h"

namespace {

// ---------------------------------------------------------------------------
// CPU affinity (best-effort)
// ---------------------------------------------------------------------------

static void
pin_to_core(int core)
{
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(core, &set);
  if (::sched_setaffinity(0, sizeof(set), &set) != 0) {
    std::fprintf(stderr, "warning: sched_setaffinity(core %d) failed: %s\n", core, std::strerror(errno));
  }
}

// ---------------------------------------------------------------------------
// Business-level structs
// ---------------------------------------------------------------------------

struct NewOrderFields
{
  std::string cl_ord_id;
  std::string account{ "ACC001" };
  std::string currency{ "USD" };
  char exec_inst{ 'G' };
  char hand_l_inst{ '1' };
  std::string security_id{ "US0231351067" };
  char security_id_source{ '4' }; // ISIN
  double order_qty{ 100.0 };
  char ord_type{ '2' }; // Limit
  double price{ 150.25 };
  char side{ '1' }; // Buy
  std::string symbol{ "AAPL" };
  char time_in_force{ '0' }; // Day
  std::string transact_time;
  std::string ex_destination{ "XNYS" };
  std::string security_exchange{ "XNYS" };
  std::string party_id{ "CUSTODIAN" };
  char party_id_source{ 'D' };
  int party_role{ 3 };
};

struct CancelOrderFields
{
  std::string cl_ord_id;
  std::string orig_cl_ord_id;
  std::string account{ "ACC001" };
  std::string security_id{ "US0231351067" };
  char security_id_source{ '4' };
  double order_qty{ 100.0 };
  char side{ '1' };
  std::string symbol{ "AAPL" };
  std::string transact_time;
  std::string security_exchange{ "XNYS" };
  std::string party_id{ "CUSTODIAN" };
  char party_id_source{ 'D' };
  int party_role{ 3 };
};

struct ParsedAck
{
  std::string cl_ord_id;
  char exec_type{ '\0' };
  char ord_status{ '\0' };
  double leaves_qty{ 0.0 };
  double cum_qty{ 0.0 };
  double avg_px{ 0.0 };
};

// ---------------------------------------------------------------------------
// QuickFIX message builders
// ---------------------------------------------------------------------------

auto
BuildQFNewOrder(const NewOrderFields& req) -> FIX44::NewOrderSingle
{
  FIX44::NewOrderSingle order(FIX::ClOrdID(req.cl_ord_id),
                              FIX::Side(req.side),
                              FIX::TransactTime(FIX::UtcTimeStamp::now()),
                              FIX::OrdType(req.ord_type));

  order.set(FIX::Account(req.account));
  order.set(FIX::Currency(req.currency));
  order.set(FIX::ExecInst(std::string(1, req.exec_inst)));
  order.set(FIX::HandlInst(req.hand_l_inst));
  order.set(FIX::SecurityIDSource(std::string(1, req.security_id_source)));
  order.set(FIX::SecurityID(req.security_id));
  order.set(FIX::OrderQty(req.order_qty));
  order.set(FIX::Price(req.price));
  order.set(FIX::Symbol(req.symbol));
  order.set(FIX::TimeInForce(req.time_in_force));
  order.set(FIX::ExDestination(req.ex_destination));
  order.set(FIX::SecurityExchange(req.security_exchange));

  FIX44::NewOrderSingle::NoPartyIDs party;
  party.set(FIX::PartyID(req.party_id));
  party.set(FIX::PartyIDSource(req.party_id_source));
  party.set(FIX::PartyRole(req.party_role));
  order.addGroup(party);

  return order;
}

auto
BuildQFCancelOrder(const CancelOrderFields& req) -> FIX44::OrderCancelRequest
{
  FIX44::OrderCancelRequest cancel(FIX::OrigClOrdID(req.orig_cl_ord_id),
                                   FIX::ClOrdID(req.cl_ord_id),
                                   FIX::Side(req.side),
                                   FIX::TransactTime(FIX::UtcTimeStamp::now()));

  cancel.set(FIX::Account(req.account));
  cancel.set(FIX::SecurityIDSource(std::string(1, req.security_id_source)));
  cancel.set(FIX::SecurityID(req.security_id));
  cancel.set(FIX::OrderQty(req.order_qty));
  cancel.set(FIX::Symbol(req.symbol));
  // SecurityExchange is in the Instrument component which is valid for
  // OrderCancelRequest.
  cancel.setField(FIX::SecurityExchange(req.security_exchange));

  FIX44::OrderCancelRequest::NoPartyIDs party;
  party.set(FIX::PartyID(req.party_id));
  party.set(FIX::PartyIDSource(req.party_id_source));
  party.set(FIX::PartyRole(req.party_role));
  cancel.addGroup(party);

  return cancel;
}

auto
ParseQFExecReport(const FIX::Message& msg) -> ParsedAck
{
  ParsedAck ack;
  FIX::ClOrdID cl_ord_id;
  if (msg.isSetField(FIX::FIELD::ClOrdID)) {
    msg.getField(cl_ord_id);
    ack.cl_ord_id = cl_ord_id.getString();
  }
  FIX::ExecType exec_type;
  if (msg.isSetField(FIX::FIELD::ExecType)) {
    msg.getField(exec_type);
    ack.exec_type = exec_type.getValue();
  }
  FIX::OrdStatus ord_status;
  if (msg.isSetField(FIX::FIELD::OrdStatus)) {
    msg.getField(ord_status);
    ack.ord_status = ord_status.getValue();
  }
  FIX::LeavesQty leaves_qty;
  if (msg.isSetField(FIX::FIELD::LeavesQty)) {
    msg.getField(leaves_qty);
    ack.leaves_qty = leaves_qty.getValue();
  }
  FIX::CumQty cum_qty;
  if (msg.isSetField(FIX::FIELD::CumQty)) {
    msg.getField(cum_qty);
    ack.cum_qty = cum_qty.getValue();
  }
  FIX::AvgPx avg_px;
  if (msg.isSetField(FIX::FIELD::AvgPx)) {
    msg.getField(avg_px);
    ack.avg_px = avg_px.getValue();
  }
  return ack;
}

// ---------------------------------------------------------------------------
// Acceptor application (child process): echoes acks for 35=D and 35=F
// ---------------------------------------------------------------------------

class QFAcceptorApp final : public FIX::Application
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

  void fromApp(const FIX::Message& msg, const FIX::SessionID& session_id)
    EXCEPT(FIX::FieldNotFound, FIX::IncorrectDataFormat, FIX::IncorrectTagValue, FIX::UnsupportedMessageType) override
  {

    FIX::MsgType msg_type;
    msg.getHeader().getField(msg_type);

    if (msg_type.getValue() == "D") {
      // NewOrderSingle → ExecutionReport ExecType=0 (New)
      FIX::ClOrdID cl_ord_id;
      FIX::Side side;
      FIX::OrderQty order_qty;
      if (msg.isSetField(FIX::FIELD::ClOrdID))
        msg.getField(cl_ord_id);
      if (msg.isSetField(FIX::FIELD::Side))
        msg.getField(side);
      if (msg.isSetField(FIX::FIELD::OrderQty))
        msg.getField(order_qty);

      const auto exec_counter = exec_id_.fetch_add(1U) + 1U;
      const std::string exec_id_str = "E" + std::to_string(exec_counter);
      const std::string order_id_str = "O" + std::to_string(exec_counter);
      const double qty = order_qty.getValue();

      FIX44::ExecutionReport er(FIX::OrderID(order_id_str),
                                FIX::ExecID(exec_id_str),
                                FIX::ExecType('0'),
                                FIX::OrdStatus('0'),
                                side,
                                FIX::LeavesQty(qty),
                                FIX::CumQty(0.0),
                                FIX::AvgPx(0.0));
      er.set(cl_ord_id);
      FIX::Session::sendToTarget(er, session_id);

    } else if (msg_type.getValue() == "F") {
      // OrderCancelRequest → ExecutionReport ExecType=4 (Canceled)
      FIX::ClOrdID cl_ord_id;
      FIX::OrigClOrdID orig_cl_ord_id;
      FIX::Side side;
      FIX::OrderQty order_qty;
      if (msg.isSetField(FIX::FIELD::ClOrdID))
        msg.getField(cl_ord_id);
      if (msg.isSetField(FIX::FIELD::OrigClOrdID))
        msg.getField(orig_cl_ord_id);
      if (msg.isSetField(FIX::FIELD::Side))
        msg.getField(side);
      if (msg.isSetField(FIX::FIELD::OrderQty))
        msg.getField(order_qty);

      const auto exec_counter = exec_id_.fetch_add(1U) + 1U;
      const std::string exec_id_str = "E" + std::to_string(exec_counter);
      const std::string order_id_str = "O" + std::to_string(exec_counter);
      const double qty = order_qty.getValue();

      FIX44::ExecutionReport er(FIX::OrderID(order_id_str),
                                FIX::ExecID(exec_id_str),
                                FIX::ExecType('4'),
                                FIX::OrdStatus('4'),
                                side,
                                FIX::LeavesQty(0.0),
                                FIX::CumQty(qty),
                                FIX::AvgPx(150.25));
      er.set(cl_ord_id);
      er.set(orig_cl_ord_id);
      FIX::Session::sendToTarget(er, session_id);
    }
  }

private:
  std::atomic<std::uint32_t> exec_id_{ 0 };
};

// ---------------------------------------------------------------------------
// Initiator application (parent process)
// ---------------------------------------------------------------------------

struct TimingRecord
{
  std::chrono::steady_clock::time_point sent_at;
};

class QFInitiatorApp final : public FIX::Application
{
public:
  // Populated before measurements
  std::unordered_map<std::string, TimingRecord> pending_map;
  std::mutex pending_mutex;

  // Collected RTTs
  std::vector<std::uint64_t> nos_rtts;
  std::vector<std::uint64_t> ocr_rtts;
  std::mutex results_mutex;

  std::atomic<int> logon_count{ 0 };
  std::atomic<std::uint32_t> acks_count{ 0 };
  std::condition_variable cv;
  std::mutex cv_mutex;

  void waitForLogon()
  {
    std::unique_lock<std::mutex> lk(cv_mutex);
    cv.wait(lk, [this] { return logon_count.load(std::memory_order_acquire) >= 1; });
  }

  void waitForAcks(std::uint32_t expected)
  {
    std::unique_lock<std::mutex> lk(cv_mutex);
    cv.wait_for(lk, std::chrono::seconds(60), [this, expected] {
      return acks_count.load(std::memory_order_acquire) >= expected;
    });
  }

  // Record send time before FIX::Session::sendToTarget
  void recordSend(const std::string& cl_ord_id, std::chrono::steady_clock::time_point t0)
  {
    std::lock_guard<std::mutex> lk(pending_mutex);
    pending_map[cl_ord_id] = TimingRecord{ t0 };
  }

  void onCreate(const FIX::SessionID&) override {}

  void onLogon(const FIX::SessionID&) override
  {
    {
      std::lock_guard<std::mutex> lk(cv_mutex);
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

  void fromApp(const FIX::Message& msg, const FIX::SessionID&)
    EXCEPT(FIX::FieldNotFound, FIX::IncorrectDataFormat, FIX::IncorrectTagValue, FIX::UnsupportedMessageType) override
  {

    FIX::MsgType msg_type;
    msg.getHeader().getField(msg_type);
    if (msg_type.getValue() != "8")
      return;

    // T1: parse ack to struct, then take timestamp
    const auto ack = ParseQFExecReport(msg);
    const auto t1 = std::chrono::steady_clock::now();

    std::chrono::steady_clock::time_point t0{};
    bool is_cancel = false;
    {
      std::lock_guard<std::mutex> lk(pending_mutex);
      auto it = pending_map.find(ack.cl_ord_id);
      if (it == pending_map.end())
        return;
      t0 = it->second.sent_at;
      pending_map.erase(it);
    }

    is_cancel = (!ack.cl_ord_id.empty() && ack.cl_ord_id[0] == 'C');

    const std::uint64_t rtt =
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    {
      std::lock_guard<std::mutex> lk(results_mutex);
      if (is_cancel)
        ocr_rtts.push_back(rtt);
      else
        nos_rtts.push_back(rtt);
    }

    acks_count.fetch_add(1U, std::memory_order_acq_rel);
    cv.notify_one();
  }
};

// ---------------------------------------------------------------------------
// SessionSettings helpers
// ---------------------------------------------------------------------------

static auto
FindEphemeralPort() -> int
{
  const int sock = ::socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0)
    return 19956;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = 0;
  addr.sin_addr.s_addr = INADDR_ANY;
  socklen_t len = static_cast<socklen_t>(sizeof(addr));
  if (::bind(sock, reinterpret_cast<sockaddr*>(&addr), len) < 0) {
    ::close(sock);
    return 19956;
  }
  if (::getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
    ::close(sock);
    return 19956;
  }
  const int port = ntohs(addr.sin_port);
  ::close(sock);
  return port;
}

static auto
MakeAcceptorSettings(const std::filesystem::path& xml_path, int port) -> FIX::SessionSettings
{
  const std::string cfg = "[DEFAULT]\n"
                          "NonStopSession=Y\n"
                          "PersistMessages=N\n"
                          "CheckLatency=N\n"
                          "ResetOnLogon=Y\n"
                          "ValidateFieldsOutOfOrder=N\n"
                          "ValidateUserDefinedFields=N\n"
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
                          "ValidateFieldsOutOfOrder=N\n"
                          "ValidateUserDefinedFields=N\n"
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

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

struct LatencyStats
{
  std::size_t count{ 0 };
  std::uint64_t min_ns{ 0 };
  std::uint64_t p50_ns{ 0 };
  std::uint64_t p95_ns{ 0 };
  std::uint64_t p99_ns{ 0 };
  std::uint64_t p999_ns{ 0 };
  std::uint64_t max_ns{ 0 };
  double avg_ns{ 0.0 };
};

auto
ComputeStats(std::vector<std::uint64_t> samples) -> LatencyStats
{
  if (samples.empty())
    return {};
  std::sort(samples.begin(), samples.end());
  const auto n = samples.size();
  LatencyStats s;
  s.count = n;
  s.min_ns = samples.front();
  s.max_ns = samples.back();
  s.p50_ns = samples[n * 50 / 100];
  s.p95_ns = samples[n * 95 / 100];
  s.p99_ns = samples[n * 99 / 100];
  s.p999_ns = samples[std::max(std::size_t{ 0 }, n * 999 / 1000)];
  double sum = 0.0;
  for (auto v : samples)
    sum += static_cast<double>(v);
  s.avg_ns = sum / static_cast<double>(n);
  return s;
}

auto
PrintStats(std::string_view label, const LatencyStats& s) -> void
{
  if (s.count == 0) {
    std::printf("  %-30s  no data\n", std::string(label).c_str());
    return;
  }
  std::printf("  %-30s  count=%5zu  avg=%8.0f ns  p50=%8.0f ns  p95=%8.0f ns  "
              "p99=%8.0f ns  p999=%8.0f ns\n",
              std::string(label).c_str(),
              s.count,
              s.avg_ns,
              static_cast<double>(s.p50_ns),
              static_cast<double>(s.p95_ns),
              static_cast<double>(s.p99_ns),
              static_cast<double>(s.p999_ns));
}

} // namespace

// ---------------------------------------------------------------------------
// operator new overrides for allocation tracking
// ---------------------------------------------------------------------------

void*
operator new(std::size_t size)
{
  auto* p = bench_support::AllocateRaw(size, alignof(std::max_align_t));
  if (!p)
    throw std::bad_alloc();
  bench_support::RecordAllocation(size);
  return p;
}
void*
operator new[](std::size_t size)
{
  auto* p = bench_support::AllocateRaw(size, alignof(std::max_align_t));
  if (!p)
    throw std::bad_alloc();
  bench_support::RecordAllocation(size);
  return p;
}
void*
operator new(std::size_t s, std::align_val_t a)
{
  auto* p = bench_support::AllocateRaw(s, static_cast<std::size_t>(a));
  if (!p)
    throw std::bad_alloc();
  bench_support::RecordAllocation(s);
  return p;
}
void*
operator new[](std::size_t s, std::align_val_t a)
{
  auto* p = bench_support::AllocateRaw(s, static_cast<std::size_t>(a));
  if (!p)
    throw std::bad_alloc();
  bench_support::RecordAllocation(s);
  return p;
}
void*
operator new(std::size_t s, const std::nothrow_t&) noexcept
{
  auto* p = bench_support::AllocateRaw(s, alignof(std::max_align_t));
  if (p)
    bench_support::RecordAllocation(s);
  return p;
}
void*
operator new[](std::size_t s, const std::nothrow_t&) noexcept
{
  auto* p = bench_support::AllocateRaw(s, alignof(std::max_align_t));
  if (p)
    bench_support::RecordAllocation(s);
  return p;
}
void
operator delete(void* p) noexcept
{
  std::free(p);
}
void
operator delete[](void* p) noexcept
{
  std::free(p);
}
void
operator delete(void* p, std::size_t) noexcept
{
  std::free(p);
}
void
operator delete[](void* p, std::size_t) noexcept
{
  std::free(p);
}
void
operator delete(void* p, std::align_val_t) noexcept
{
  std::free(p);
}
void
operator delete[](void* p, std::align_val_t) noexcept
{
  std::free(p);
}
void
operator delete(void* p, std::size_t, std::align_val_t) noexcept
{
  std::free(p);
}
void
operator delete[](void* p, std::size_t, std::align_val_t) noexcept
{
  std::free(p);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int
main(int argc, char** argv)
{
  std::filesystem::path xml_path;
  std::uint32_t iterations = 1000U;
  std::uint32_t warmup = 50U;
  std::uint32_t send_interval_us = 0U;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg{ argv[i] };
    if (arg == "--xml" && i + 1 < argc) {
      xml_path = argv[++i];
    } else if (arg == "--iterations" && i + 1 < argc) {
      iterations = static_cast<std::uint32_t>(std::stoul(argv[++i]));
    } else if (arg == "--warmup" && i + 1 < argc) {
      warmup = static_cast<std::uint32_t>(std::stoul(argv[++i]));
    } else if (arg == "--send-interval-us" && i + 1 < argc) {
      send_interval_us = static_cast<std::uint32_t>(std::stoul(argv[++i]));
    } else {
      std::fprintf(stderr,
                   "usage: quickfix-loopback --xml <FIX44.xml> [--iterations "
                   "N] [--warmup N] [--send-interval-us N]\n");
      return 1;
    }
  }

  if (xml_path.empty()) {
    std::fprintf(stderr,
                 "usage: quickfix-loopback --xml <FIX44.xml> [--iterations N] "
                 "[--warmup N] [--send-interval-us N]\n");
    return 1;
  }

  const int port = FindEphemeralPort();

  // Pipe: acceptor writes 1 byte when started, initiator reads before
  // connecting.
  int pipe_ready[2];
  if (::pipe(pipe_ready) != 0) {
    std::perror("pipe");
    return 1;
  }

  const pid_t pid = ::fork();
  if (pid < 0) {
    std::perror("fork");
    return 1;
  }

  if (pid == 0) {
    // -------- Child: acceptor --------
    pin_to_core(1); // Acceptor on core 1
    ::close(pipe_ready[0]);

    QFAcceptorApp acceptor_app;
    FIX::MemoryStoreFactory store_factory;
    auto acceptor_settings = MakeAcceptorSettings(xml_path, port);
    FIX::ThreadedSocketAcceptor acceptor(acceptor_app, store_factory, acceptor_settings);
    acceptor.start();

    // Signal parent that acceptor is started.
    const std::uint8_t ready = 1;
    ::write(pipe_ready[1], &ready, 1);
    ::close(pipe_ready[1]);

    // Run until parent kills us.
    while (true) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    _exit(0);
  }

  // -------- Parent: initiator --------
  pin_to_core(0); // Initiator on core 0
  ::close(pipe_ready[1]);
  {
    std::uint8_t ready = 0;
    if (::read(pipe_ready[0], &ready, 1) != 1) {
      std::fprintf(stderr, "initiator: failed to read acceptor ready signal\n");
      ::kill(pid, SIGTERM);
      return 1;
    }
  }
  ::close(pipe_ready[0]);

  // Small extra delay to ensure acceptor socket is accepting.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  QFInitiatorApp initiator_app;
  FIX::MemoryStoreFactory store_factory;
  auto initiator_settings = MakeInitiatorSettings(xml_path, port);
  FIX::ThreadedSocketInitiator initiator(initiator_app, store_factory, initiator_settings);
  initiator.start();

  // Wait for both sides to log on.
  initiator_app.waitForLogon();

  const FIX::SessionID initiator_id("FIX.4.4", "BUY", "SELL");

  auto make_nos_id = [](std::uint32_t i) -> std::string {
    char buf[12];
    std::snprintf(buf, sizeof(buf), "N%07u", i + 1U);
    return { buf };
  };
  auto make_ocr_id = [](std::uint32_t i) -> std::string {
    char buf[12];
    std::snprintf(buf, sizeof(buf), "C%07u", i + 1U);
    return { buf };
  };

  const std::uint32_t total_pairs = warmup + iterations;

  std::printf("QuickFIX two-process loopback: %u iterations per message type, "
              "%u warmup  send-interval=%u us\n",
              iterations,
              warmup,
              send_interval_us);
  std::printf("  Messages: 35=D (NewOrderSingle, 16 fields + NoPartyIDs) "
              "and 35=F (OrderCancelRequest, 10 fields + NoPartyIDs)\n");
  std::printf("  Async: FIX::Session::sendToTarget() + fromApp() callback\n");
  std::printf("  T0 = before struct→FIX build   T1 = after ack→struct extraction\n\n");

  // Sender fires all messages at fixed rate; acks arrive independently via
  // fromApp() on the QuickFIX IO thread.  Never wait for acks between sends.
  const auto wall_start = std::chrono::steady_clock::now();
  for (std::uint32_t i = 0; i < total_pairs; ++i) {
    // --- NewOrderSingle (35=D) ---
    {
      NewOrderFields nos;
      nos.cl_ord_id = make_nos_id(i);

      const auto t0 = std::chrono::steady_clock::now(); // T0
      auto order = BuildQFNewOrder(nos);
      initiator_app.recordSend(nos.cl_ord_id, t0);
      FIX::Session::sendToTarget(order, initiator_id);
    }

    if (send_interval_us > 0) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(send_interval_us);
      while (std::chrono::steady_clock::now() < deadline) {
      }
    }

    // --- OrderCancelRequest (35=F) ---
    {
      CancelOrderFields ocr;
      ocr.cl_ord_id = make_ocr_id(i);
      ocr.orig_cl_ord_id = make_nos_id(i);

      const auto t0 = std::chrono::steady_clock::now(); // T0
      auto cancel = BuildQFCancelOrder(ocr);
      initiator_app.recordSend(ocr.cl_ord_id, t0);
      FIX::Session::sendToTarget(cancel, initiator_id);
    }

    if (send_interval_us > 0) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(send_interval_us);
      while (std::chrono::steady_clock::now() < deadline) {
      }
    }
  }

  // Wait for all acks (fromApp callback runs on the QuickFIX IO thread
  // independently).
  initiator_app.waitForAcks(2U * total_pairs);
  const auto wall_end = std::chrono::steady_clock::now();

  initiator.stop();

  // Terminate acceptor child.
  ::kill(pid, SIGTERM);
  int child_status = 0;
  ::waitpid(pid, &child_status, 0);

  // Trim warmup samples from collected RTTs (they were mixed in via fromApp, so
  // slice)
  auto trim_warmup = [warmup](std::vector<std::uint64_t>& v) {
    // warmup samples are the first 'warmup' entries recorded; we recorded in
    // order, so erase the first min(warmup, v.size()) entries.
    if (v.size() > warmup) {
      v.erase(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(warmup));
    } else {
      v.clear();
    }
  };

  std::vector<std::uint64_t> nos_rtts, ocr_rtts;
  {
    std::lock_guard<std::mutex> lk(initiator_app.results_mutex);
    nos_rtts = initiator_app.nos_rtts;
    ocr_rtts = initiator_app.ocr_rtts;
  }
  trim_warmup(nos_rtts);
  trim_warmup(ocr_rtts);

  if (nos_rtts.empty() && ocr_rtts.empty()) {
    std::fprintf(stderr, "error: no timing results collected\n");
    return 1;
  }

  auto combined = nos_rtts;
  combined.insert(combined.end(), ocr_rtts.begin(), ocr_rtts.end());

  std::printf("=== QuickFIX two-process loopback latency ===\n");
  PrintStats("35=D  NewOrderSingle", ComputeStats(nos_rtts));
  PrintStats("35=F  OrderCancelRequest", ComputeStats(ocr_rtts));
  PrintStats("Combined", ComputeStats(combined));
  std::printf("  Total wall time: %.3f ms  (%zu messages, send-interval=%u us)\n",
              static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(wall_end - wall_start).count()) /
                1e6,
              nos_rtts.size() + ocr_rtts.size(),
              send_interval_us);
  std::printf("\n");

  return 0;
}
