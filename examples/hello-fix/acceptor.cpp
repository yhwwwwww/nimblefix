#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include "fix44_api.h"
#include "nimblefix/base/status.h"
#include "nimblefix/runtime/simple.h"

using namespace nimble::generated::profile_4400;

namespace {

constexpr auto kProfileArtifactPath = "build/bench/quickfix_FIX44.nfa";
constexpr auto kListenerName = "main";
constexpr auto kListenerHost = "127.0.0.1";
constexpr auto kListenerPort = 9876U;
constexpr auto kHeartbeatIntervalSeconds = 30U;

auto
PrintStatus(std::ostream& out, std::string_view prefix, const nimble::base::Status& status) -> void
{
  out << prefix;
  if (!status.message().empty()) {
    out << ": " << status.message();
  }
  out << '\n';
}

// Demonstrates a generated-first acceptor: sessions are onboarded dynamically
// from inbound Logon CompIDs, and NewOrderSingle messages are handled through the
// generated typed callback before echoing a typed ExecutionReport.
class EchoExecutionReportApp final : public Handler
{
public:
  auto OnTypedMessage(nimble::runtime::InlineSession<Profile>& session, NewOrderSingleView order)
    -> nimble::base::Status
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
  auto app = std::make_shared<EchoExecutionReportApp>();
  auto acceptor = nimble::runtime::CreateAcceptor<Profile, EchoExecutionReportApp>(
    nimble::runtime::SimpleAcceptorSettings<Profile, EchoExecutionReportApp>{
      .profile_artifact = kProfileArtifactPath,
      .listener_name = kListenerName,
      .listener_host = kListenerHost,
      .listener_port = kListenerPort,
      .name = "hello-fix-dynamic",
      .accept_unknown_sessions = true,
      .heartbeat_interval_seconds = kHeartbeatIntervalSeconds,
      .application = app,
    });
  if (!acceptor.ok()) {
    PrintStatus(std::cerr, "failed to create acceptor", acceptor.status());
    return 1;
  }
  auto status = acceptor.value().Run();
  if (!status.ok()) {
    PrintStatus(std::cerr, "acceptor failed", status);
    return 1;
  }
  return 0;
}
