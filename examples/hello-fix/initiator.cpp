#include <memory>

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

// Demonstrates the generated-first initiator path: lifecycle callbacks receive a
// typed Session<Profile>, outbound orders are populated through the generated
// send<Msg> API, and no raw builders or tag-level code are needed.
class OrderSender final : public Handler
{
public:
  auto OnSessionActive(nimble::runtime::Session<Profile>& session) -> nimble::base::Status override
  {
    auto status = session.send<NewOrderSingle>([](auto& buy_order) {
      buy_order.cl_ord_id("HELLO-FIX-001")
        .symbol("AAPL")
        .side(Side::Buy)
        .transact_time("20260429-09:30:00.000")
        .order_qty(100)
        .ord_type(OrdType::Limit)
        .price(150.25);
    });
    if (!status.ok()) {
      return status;
    }

    return session.send<NewOrderSingle>([](auto& sell_order) {
      sell_order.cl_ord_id("HELLO-FIX-002")
        .symbol("MSFT")
        .side(Side::Sell)
        .transact_time("20260429-09:30:01.000")
        .order_qty(50)
        .ord_type(OrdType::Market);
    });
  }

  auto OnExecutionReport(nimble::runtime::InlineSession<Profile>&, ExecutionReportView exec)
    -> nimble::base::Status override
  {
    // Typed inbound views expose business fields by name; no application tag
    // numbers or raw MessageView traversal are needed in normal business code.
    const auto cl_ord_id = exec.cl_ord_id();
    const auto exec_id = exec.exec_id();
    const auto ord_status = exec.ord_status();
    (void)cl_ord_id;
    (void)exec_id;
    (void)ord_status;
    return nimble::base::Status::Ok();
  }
};

} // namespace

int
main()
{
  auto app = std::make_shared<OrderSender>();
  auto initiator = nimble::runtime::CreateInitiator<Profile>(nimble::runtime::SimpleInitiatorSettings<Profile>{
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
  if (!initiator.ok()) {
    return 1;
  }
  return initiator.value().Run().ok() ? 0 : 1;
}
