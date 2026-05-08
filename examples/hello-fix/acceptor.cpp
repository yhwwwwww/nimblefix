#include <memory>
#include <string>

#include "fix44_api.h"
#include "nimblefix/base/result.h"
#include "nimblefix/base/status.h"
#include "nimblefix/runtime/acceptor.h"
#include "nimblefix/runtime/config.h"
#include "nimblefix/runtime/engine.h"
#include "nimblefix/session/session_key.h"
#include "nimblefix/session/transport_profile.h"

using namespace nimble::generated::profile_4400;

namespace {

constexpr auto kProfileArtifactPath = "build/bench/quickfix_FIX44.nfa";
constexpr auto kListenerName = "main";
constexpr auto kListenerHost = "127.0.0.1";
constexpr auto kListenerPort = 9876U;
constexpr auto kHeartbeatIntervalSeconds = 30U;

// Demonstrates a generated-first acceptor: sessions are onboarded dynamically
// from inbound Logon CompIDs, and NewOrderSingle messages are handled through the
// generated typed callback before echoing a typed ExecutionReport.
class EchoExecutionReportApp final : public Handler
{
public:
  auto OnNewOrderSingle(nimble::runtime::InlineSession<Profile>& session, NewOrderSingleView order)
    -> nimble::base::Status override
  {
    auto side = order.side();
    if (!side.ok()) {
      return side.status();
    }

    return session.send<ExecutionReport>([&](auto& report) {
      report.order_id("HELLO-FIX-ORDER")
        .exec_id("HELLO-FIX-EXEC")
        .exec_type(ExecType::New)
        .ord_status(OrdStatus::New)
        .side(side.value())
        .leaves_qty(order.order_qty().value_or(0.0))
        .cum_qty(0.0)
        .avg_px(0.0);

      if (auto cl_ord_id = order.cl_ord_id(); cl_ord_id.has_value()) {
        report.cl_ord_id(*cl_ord_id);
      }
      if (auto symbol = order.symbol(); symbol.has_value()) {
        report.symbol(*symbol);
      }
    });
  }
};

} // namespace

int
main()
{
  nimble::runtime::EngineConfig config;
  config.worker_count = 2;
  config.profile_artifacts = { kProfileArtifactPath };
  config.listeners.push_back(nimble::runtime::ListenerConfig{
    .name = kListenerName,
    .host = kListenerHost,
    .port = kListenerPort,
  });
  config.accept_unknown_sessions = true;

  nimble::runtime::Engine engine;
  auto boot = engine.Boot(config);
  if (!boot.ok()) {
    return 1;
  }

  // Accept unknown inbound sessions dynamically. The key is already normalized
  // to the local acceptor perspective by the runtime.
  engine.SetSessionFactory(
    [](const nimble::session::SessionKey& key) -> nimble::base::Result<nimble::runtime::CounterpartyConfig> {
      return nimble::runtime::CounterpartyConfig{
        .name = std::string(key.sender_comp_id),
        .session = {
          .key = key,
          .profile_id = Profile::kProfileId,
          .heartbeat_interval_seconds = kHeartbeatIntervalSeconds,
          .is_initiator = false,
        },
        .transport_profile = nimble::session::TransportSessionProfile::Fix44(),
      };
    });

  auto binding = engine.Bind<Profile>();
  if (!binding.ok()) {
    return 1;
  }

  auto app = std::make_shared<EchoExecutionReportApp>();
  nimble::runtime::Acceptor<Profile> acceptor(&engine, &binding.value(), { .application = app });

  auto open = acceptor.OpenListeners(kListenerName);
  if (!open.ok()) {
    return 1;
  }
  return acceptor.Run().ok() ? 0 : 1;
}
