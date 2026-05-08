#include <memory>

#include "fix44_api.h"
#include "nimblefix/base/status.h"
#include "nimblefix/runtime/acceptor.h"
#include "nimblefix/runtime/config.h"
#include "nimblefix/runtime/engine.h"
#include "nimblefix/session/session_key.h"
#include "nimblefix/session/transport_profile.h"

using namespace nimble::generated::profile_4400;

namespace {

constexpr auto kProfileArtifactPath = "build/bench/quickfix_FIX44.nfa";
constexpr auto kListenerName = "queue-main";
constexpr auto kListenerHost = "127.0.0.1";
constexpr auto kListenerPort = 9877U;
constexpr auto kSessionId = 1U;
constexpr auto kHeartbeatIntervalSeconds = 30U;

struct MatchResult
{
  OrdStatus status{ OrdStatus::New };
  ExecType exec_type{ ExecType::New };
  double leaves_qty{ 0.0 };
  double cum_qty{ 0.0 };
  double avg_px{ 0.0 };
};

// Represents business work that can run away from the I/O worker when the
// session uses kQueueDecoupled and the engine uses kThreaded queue app mode.
class SimpleMatcher
{
public:
  [[nodiscard]] auto Match(NewOrderSingleView order) const -> MatchResult
  {
    const auto order_qty = order.order_qty().value_or(0.0);
    const auto limit_price = order.price().value_or(0.0);

    MatchResult result;
    result.leaves_qty = order_qty;
    result.avg_px = limit_price;
    return result;
  }
};

// Demonstrates queue-decoupled typed callbacks. With kQueueDecoupled + kThreaded,
// this handler is invoked on the app thread, while FIX I/O and session state stay
// on the runtime worker thread.
class QueuedOrderApp final : public Handler
{
public:
  auto OnNewOrderSingle(nimble::runtime::InlineSession<Profile>& session, NewOrderSingleView order)
    -> nimble::base::Status override
  {
    auto side = order.side();
    if (!side.ok()) {
      return side.status();
    }

    const auto match = matcher_.Match(order);

    return session.send<ExecutionReport>([&](auto& report) {
      report.order_id("QUEUE-ORDER")
        .exec_id("QUEUE-EXEC")
        .exec_type(match.exec_type)
        .ord_status(match.status)
        .side(side.value())
        .leaves_qty(match.leaves_qty)
        .cum_qty(match.cum_qty)
        .avg_px(match.avg_px);

      if (auto cl_ord_id = order.cl_ord_id(); cl_ord_id.has_value()) {
        report.cl_ord_id(*cl_ord_id);
      }
      if (auto symbol = order.symbol(); symbol.has_value()) {
        report.symbol(*symbol);
      }
    });
  }

private:
  SimpleMatcher matcher_;
};

} // namespace

int
main()
{
  nimble::runtime::EngineConfig config;
  config.worker_count = 1;
  config.queue_app_mode = nimble::runtime::QueueAppThreadingMode::kThreaded;
  config.profile_artifacts = { kProfileArtifactPath };
  config.listeners.push_back(nimble::runtime::ListenerConfig{
    .name = kListenerName,
    .host = kListenerHost,
    .port = kListenerPort,
  });
  config.counterparties.push_back(nimble::runtime::CounterpartyConfig{
    .name = "queue-client-a",
    .session = {
      .session_id = kSessionId,
      .key = nimble::session::SessionKey::ForAcceptor("SERVER", "CLIENT_A"),
      .profile_id = Profile::kProfileId,
      .heartbeat_interval_seconds = kHeartbeatIntervalSeconds,
      .is_initiator = false,
    },
    .transport_profile = nimble::session::TransportSessionProfile::Fix44(),
    .dispatch_mode = nimble::runtime::AppDispatchMode::kQueueDecoupled,
  });

  nimble::runtime::Engine engine;
  auto boot = engine.Boot(config);
  if (!boot.ok()) {
    return 1;
  }

  auto binding = engine.Bind<Profile>();
  if (!binding.ok()) {
    return 1;
  }

  auto app = std::make_shared<QueuedOrderApp>();
  nimble::runtime::Acceptor<Profile> acceptor(&engine, &binding.value(), { .application = app });

  auto open = acceptor.OpenListeners(kListenerName);
  if (!open.ok()) {
    return 1;
  }
  return acceptor.Run().ok() ? 0 : 1;
}
