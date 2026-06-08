#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <ctime>
#include <future>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "fix44_api.h"
#include "nimblefix/base/status.h"
#include "nimblefix/runtime/simple.h"

using namespace nimble::generated::profile_4400;

namespace {

constexpr auto kProfileArtifactPath = "build/bench/quickfix_FIX44.nfa";
constexpr auto kVenueHost = "127.0.0.1";
constexpr auto kVenuePort = 9876U;
constexpr auto kSessionId = 1U;
constexpr auto kHeartbeatIntervalSeconds = 30U;
constexpr auto kFixTimestampMillisWidth = 3;
constexpr auto kUpstreamQueueCapacity = 1024U;
constexpr auto kShutdownRetryInterval = std::chrono::milliseconds{ 10 };
constexpr auto kShutdownRetryBudget = std::chrono::seconds{ 2 };

struct UpstreamOrder
{
  std::string cl_ord_id;
  std::string symbol;
  Side side{ Side::Buy };
  double quantity{ 0.0 };
  OrdType ord_type{ OrdType::Market };
  std::optional<double> limit_price;
};

enum class UpstreamCommandKind
{
  kOrder = 0,
  kShutdown,
};

struct UpstreamCommand
{
  UpstreamCommandKind kind{ UpstreamCommandKind::kOrder };
  UpstreamOrder order;
  std::string text;
};

template<typename T>
class SpscQueue
{
public:
  explicit SpscQueue(std::size_t capacity)
    : slots_((capacity == 0U ? 1U : capacity) + 1U)
    , capacity_(slots_.size())
  {
  }

  auto TryPush(const T& value) -> bool { return TryPushImpl(value); }
  auto TryPush(T&& value) -> bool { return TryPushImpl(std::move(value)); }

  auto TryPop() -> std::optional<T>
  {
    const auto tail = tail_.load(std::memory_order_relaxed);
    if (tail == head_.load(std::memory_order_acquire)) {
      return std::nullopt;
    }

    T value = std::move(slots_[tail]);
    tail_.store(Next(tail), std::memory_order_release);
    return value;
  }

private:
  template<typename U>
  auto TryPushImpl(U&& value) -> bool
  {
    const auto head = head_.load(std::memory_order_relaxed);
    const auto next = Next(head);
    if (next == tail_.load(std::memory_order_acquire)) {
      return false;
    }

    slots_[head] = std::forward<U>(value);
    head_.store(next, std::memory_order_release);
    return true;
  }

  [[nodiscard]] auto Next(std::size_t index) const -> std::size_t { return index + 1U == capacity_ ? 0U : index + 1U; }

  std::vector<T> slots_;
  std::size_t capacity_{ 0 };
  alignas(64) std::atomic<std::size_t> head_{ 0 };
  alignas(64) std::atomic<std::size_t> tail_{ 0 };
};

[[nodiscard]] auto
CurrentUtcFixTimestamp() -> std::string
{
  const auto now = std::chrono::system_clock::now();
  const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
  const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now - seconds).count();
  const auto time = std::chrono::system_clock::to_time_t(seconds);

  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &time);
#else
  gmtime_r(&time, &utc);
#endif

  std::ostringstream out;
  out << std::put_time(&utc, "%Y%m%d-%H:%M:%S") << '.' << std::setfill('0') << std::setw(kFixTimestampMillisWidth)
      << millis;
  return out.str();
}

[[nodiscard]] auto
TrimAscii(std::string_view value) -> std::string_view
{
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
    value.remove_prefix(1U);
  }
  while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
    value.remove_suffix(1U);
  }
  return value;
}

[[nodiscard]] auto
LowerAscii(std::string value) -> std::string
{
  for (auto& ch : value) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return value;
}

[[nodiscard]] auto
ParsePositiveDouble(const std::string& token, std::string_view field_name, double* value, std::string* error) -> bool
{
  char* end = nullptr;
  errno = 0;
  const auto parsed = std::strtod(token.c_str(), &end);
  if (errno != 0 || end == token.c_str() || *end != '\0' || parsed <= 0.0) {
    *error = std::string(field_name) + " must be a positive number";
    return false;
  }
  *value = parsed;
  return true;
}

[[nodiscard]] auto
ParseSideToken(const std::string& token, Side* side) -> bool
{
  const auto lower = LowerAscii(token);
  if (lower == "buy" || lower == "1") {
    *side = Side::Buy;
    return true;
  }
  if (lower == "sell" || lower == "2") {
    *side = Side::Sell;
    return true;
  }
  return false;
}

[[nodiscard]] auto
ParseOrdTypeToken(const std::string& token, OrdType* ord_type) -> bool
{
  const auto lower = LowerAscii(token);
  if (lower == "market" || lower == "1") {
    *ord_type = OrdType::Market;
    return true;
  }
  if (lower == "limit" || lower == "2") {
    *ord_type = OrdType::Limit;
    return true;
  }
  return false;
}

[[nodiscard]] auto
ParseOrderLine(const std::string& line, UpstreamOrder* order, std::string* error) -> bool
{
  std::istringstream fields(line);
  std::string side_token;
  std::string quantity_token;
  std::string type_token;
  std::string price_token;

  if (!(fields >> order->cl_ord_id >> side_token >> order->symbol >> quantity_token >> type_token)) {
    *error = "expected: <clOrdId> <buy|sell> <symbol> <qty> <market|limit> [limitPrice]";
    return false;
  }
  if (!ParseSideToken(side_token, &order->side)) {
    *error = "side must be buy/sell or FIX 1/2";
    return false;
  }
  if (!ParsePositiveDouble(quantity_token, "quantity", &order->quantity, error)) {
    return false;
  }
  if (!ParseOrdTypeToken(type_token, &order->ord_type)) {
    *error = "order type must be market/limit or FIX 1/2";
    return false;
  }

  if (fields >> price_token) {
    std::string extra;
    if (fields >> extra) {
      *error = "too many fields";
      return false;
    }
    double parsed_price = 0.0;
    if (!ParsePositiveDouble(price_token, "limit price", &parsed_price, error)) {
      return false;
    }
    order->limit_price = parsed_price;
  } else {
    order->limit_price.reset();
  }

  if (order->ord_type == OrdType::Limit && !order->limit_price.has_value()) {
    *error = "limit orders require a limit price";
    return false;
  }
  if (order->ord_type == OrdType::Market && order->limit_price.has_value()) {
    *error = "market orders must not include a price";
    return false;
  }
  return true;
}

[[nodiscard]] auto
IsStopCommand(std::string_view line) -> bool
{
  const auto trimmed = TrimAscii(line);
  const auto command = LowerAscii(std::string(trimmed));
  return command == "quit" || command == "exit";
}

auto
PrintStatus(std::ostream& out, std::string_view prefix, const nimble::base::Status& status) -> void
{
  out << prefix;
  if (!status.message().empty()) {
    out << ": " << status.message();
  }
  out << '\n';
}

class OrderEntryApp final : public Handler
{
public:
  OrderEntryApp()
    : upstream_commands_(kUpstreamQueueCapacity)
  {
  }

  auto OnSessionActive(nimble::runtime::Session<Profile>& session) -> nimble::base::Status override
  {
    {
      std::lock_guard lock(session_mutex_);
      active_session_ = session;
    }
    std::cout << "FIX session active; order entry is enabled\n";
    return nimble::base::Status::Ok();
  }

  auto OnSessionClosed(nimble::runtime::Session<Profile>&, std::string_view text) -> nimble::base::Status override
  {
    {
      std::lock_guard lock(session_mutex_);
      active_session_ = {};
    }
    DropPendingCommands();
    std::cout << "FIX session closed";
    if (!text.empty()) {
      std::cout << ": " << text;
    }
    std::cout << '\n';
    return nimble::base::Status::Ok();
  }

  auto OnSessionPoll(nimble::runtime::InlineSession<Profile>& session) -> nimble::base::Status override
  {
    while (true) {
      auto command = upstream_commands_.TryPop();
      if (!command.has_value()) {
        return nimble::base::Status::Ok();
      }

      if (command->kind == UpstreamCommandKind::kShutdown) {
        return session.logout(command->text);
      }

      auto status = SendOrder(session, command->order);
      if (!status.ok()) {
        return status;
      }
      std::cout << "sent clOrdId=" << command->order.cl_ord_id << '\n';
    }
  }

  auto EnqueueFromUpstream(const UpstreamOrder& order) -> nimble::base::Status
  {
    return EnqueueCommand(UpstreamCommand{
      .kind = UpstreamCommandKind::kOrder,
      .order = order,
      .text = {},
    });
  }

  auto RequestShutdown(std::string text) -> nimble::base::Status
  {
    return EnqueueCommand(UpstreamCommand{
      .kind = UpstreamCommandKind::kShutdown,
      .order = {},
      .text = std::move(text),
    });
  }

  auto OnTypedMessage(nimble::runtime::InlineSession<Profile>&, ExecutionReportView exec) -> nimble::base::Status
  {
    auto ord_status = exec.ord_status();
    const auto ord_status_wire = ord_status.ok() ? ToWire(ord_status.value()) : '?';
    const auto cl_ord_id = exec.cl_ord_id().value_or("<missing>");
    const auto exec_id = exec.exec_id().value_or("<missing>");
    std::cout << "execution_report clOrdId=" << cl_ord_id << " execId=" << exec_id << " ordStatus=" << ord_status_wire
              << '\n';
    return nimble::base::Status::Ok();
  }

private:
  auto EnqueueCommand(UpstreamCommand command) -> nimble::base::Status
  {
    auto session = ActiveSession();
    if (!session.valid()) {
      return nimble::base::Status::Busy("FIX session is not active");
    }
    if (!upstream_commands_.TryPush(std::move(command))) {
      return nimble::base::Status::Busy("upstream input queue is full");
    }
    return session.wakeup();
  }

  auto SendOrder(nimble::runtime::InlineSession<Profile>& session, const UpstreamOrder& order) -> nimble::base::Status
  {
    const auto transact_time = CurrentUtcFixTimestamp();
    return session.send<NewOrderSingle>([&](auto& fix_order) {
      fix_order.cl_ord_id(order.cl_ord_id)
        .symbol(order.symbol)
        .side(order.side)
        .transact_time(transact_time)
        .order_qty(order.quantity)
        .ord_type(order.ord_type);

      if (order.limit_price.has_value()) {
        fix_order.price(*order.limit_price);
      }
    });
  }

  auto DropPendingCommands() -> void
  {
    while (upstream_commands_.TryPop().has_value()) {
    }
  }

  [[nodiscard]] auto ActiveSession() const -> nimble::runtime::Session<Profile>
  {
    std::lock_guard lock(session_mutex_);
    return active_session_;
  }

  mutable std::mutex session_mutex_;
  nimble::runtime::Session<Profile> active_session_;
  SpscQueue<UpstreamCommand> upstream_commands_;
};

auto
RunOrderInput(OrderEntryApp& app, std::istream& input, std::ostream& output, std::ostream& errors)
  -> nimble::base::Status
{
  output << "order input: <clOrdId> <buy|sell> <symbol> <qty> <market|limit> [limitPrice]\n"
         << "example: ORD-1001 buy AAPL 100 limit 150.25\n"
         << "type quit to stop\n";

  std::string line;
  while (std::getline(input, line)) {
    if (TrimAscii(line).empty()) {
      continue;
    }
    if (IsStopCommand(line)) {
      break;
    }

    UpstreamOrder order;
    std::string error;
    if (!ParseOrderLine(line, &order, &error)) {
      errors << "input rejected: " << error << '\n';
      continue;
    }

    const auto enqueue_status = app.EnqueueFromUpstream(order);
    if (!enqueue_status.ok()) {
      PrintStatus(errors, "input rejected", enqueue_status);
      continue;
    }
    output << "queued clOrdId=" << order.cl_ord_id << '\n';
  }
  return nimble::base::Status::Ok();
}

auto
RequestShutdownWithRetry(OrderEntryApp& app, std::string_view text) -> nimble::base::Status
{
  const auto deadline = std::chrono::steady_clock::now() + kShutdownRetryBudget;
  auto last_status = nimble::base::Status::Busy("FIX session is not active");
  while (std::chrono::steady_clock::now() < deadline) {
    last_status = app.RequestShutdown(std::string(text));
    if (last_status.ok()) {
      return last_status;
    }
    std::this_thread::sleep_for(kShutdownRetryInterval);
  }
  return last_status;
}

} // namespace

int
main()
{
  auto app = std::make_shared<OrderEntryApp>();
  auto initiator_result = nimble::runtime::CreateInitiator<Profile, OrderEntryApp>(
    nimble::runtime::SimpleInitiatorSettings<Profile, OrderEntryApp>{
      .profile_artifact = kProfileArtifactPath,
      .name = "venue-a",
      .session_id = kSessionId,
      .sender_comp_id = "MY_FIRM",
      .target_comp_id = "VENUE_A",
      .host = kVenueHost,
      .port = kVenuePort,
      .heartbeat_interval_seconds = kHeartbeatIntervalSeconds,
      .application = app,
    });
  if (!initiator_result.ok()) {
    PrintStatus(std::cerr, "failed to create initiator", initiator_result.status());
    return 1;
  }

  auto initiator = std::move(initiator_result).value();
  std::promise<void> runtime_started;
  auto started = runtime_started.get_future();
  std::promise<nimble::base::Status> runtime_done;
  auto runtime_status = runtime_done.get_future();
  std::jthread fix_thread([&initiator, started = std::move(runtime_started), done = std::move(runtime_done)]() mutable {
    started.set_value();
    done.set_value(initiator.Run());
  });
  started.wait();

  const auto input_status = RunOrderInput(*app, std::cin, std::cout, std::cerr);
  const auto runtime_finished_before_shutdown =
    runtime_status.wait_for(std::chrono::milliseconds{ 0 }) == std::future_status::ready;
  auto logout_status = nimble::base::Status::Ok();
  if (!runtime_finished_before_shutdown) {
    logout_status = RequestShutdownWithRetry(*app, "operator requested shutdown");
    if (!logout_status.ok()) {
      initiator.Stop();
    }
  }
  fix_thread.join();

  const auto run_status = runtime_status.get();
  if (!input_status.ok()) {
    PrintStatus(std::cerr, "input failed", input_status);
    return 1;
  }
  if (!logout_status.ok()) {
    PrintStatus(std::cerr, "logout request failed", logout_status);
    return 1;
  }
  if (!run_status.ok()) {
    PrintStatus(std::cerr, "initiator failed", run_status);
    return 1;
  }
  return 0;
}
