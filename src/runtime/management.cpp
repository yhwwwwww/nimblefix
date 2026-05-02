#include "nimblefix/runtime/management.h"

#include <algorithm>
#include <chrono>
#include <span>
#include <sstream>
#include <utility>

#include "nimblefix/runtime/engine.h"
#include "nimblefix/runtime/profile_registry.h"

namespace nimble::runtime {

namespace {

auto
NowNs() -> std::uint64_t
{
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

auto
BuildSessionStatus(const CounterpartyConfig& counterparty, std::span<const session::SessionSnapshot> snapshots)
  -> ManagedSessionStatus
{
  ManagedSessionStatus status{
    .session_id = counterparty.session.session_id,
    .name = counterparty.name,
    .state = session::SessionState::kDisconnected,
    .next_inbound_seq = 0U,
    .next_outbound_seq = 0U,
    .last_activity_ns = 0U,
    .is_initiator = counterparty.session.is_initiator,
    .profile_id = counterparty.session.profile_id,
    .begin_string = counterparty.session.key.begin_string,
    .sender_comp_id = counterparty.session.key.sender_comp_id,
    .target_comp_id = counterparty.session.key.target_comp_id,
    .store_mode = counterparty.store_mode,
    .dispatch_mode = counterparty.dispatch_mode,
    .reconnect_enabled = counterparty.reconnect_enabled,
  };
  const auto it = std::find_if(snapshots.begin(), snapshots.end(), [&](const auto& snapshot) {
    return snapshot.session_id == counterparty.session.session_id;
  });
  if (it != snapshots.end()) {
    status.state = it->state;
    status.next_inbound_seq = it->next_in_seq;
    status.next_outbound_seq = it->next_out_seq;
    status.last_activity_ns = std::max(it->last_inbound_ns, it->last_outbound_ns);
  }
  return status;
}

} // namespace

struct ManagementPlane::Impl
{
  explicit Impl(Engine* engine_arg)
    : engine(engine_arg)
    , boot_timestamp_ns(NowNs())
  {
  }

  Engine* engine{ nullptr };
  std::uint64_t boot_timestamp_ns{ 0 };
};

ManagementPlane::ManagementPlane(Engine* engine)
  : impl_(std::make_unique<Impl>(engine))
{
}

ManagementPlane::~ManagementPlane() = default;

ManagementPlane::ManagementPlane(ManagementPlane&&) noexcept = default;

auto
ManagementPlane::operator=(ManagementPlane&&) noexcept -> ManagementPlane& = default;

auto
ManagementPlane::QueryEngineStatus() const -> base::Result<EngineManagementStatus>
{
  if (impl_->engine == nullptr) {
    return base::Status::InvalidArgument("management plane requires an engine");
  }

  const auto* config = impl_->engine->config();
  if (config == nullptr) {
    return base::Status::InvalidArgument("engine must be booted before querying management status");
  }

  EngineManagementStatus status;
  status.timestamp_ns = NowNs();
  status.booted = true;
  status.worker_count = config->worker_count;
  status.total_sessions = static_cast<std::uint32_t>(config->counterparties.size());
  status.loaded_profiles = static_cast<std::uint32_t>(impl_->engine->profiles().size());
  status.listener_count = static_cast<std::uint32_t>(config->listeners.size());
  status.uptime = std::chrono::nanoseconds(status.timestamp_ns - impl_->boot_timestamp_ns);
  const auto snapshots = impl_->engine->QuerySessionSnapshots();
  status.sessions.reserve(config->counterparties.size());
  for (const auto& counterparty : config->counterparties) {
    status.sessions.push_back(BuildSessionStatus(counterparty, snapshots));
  }
  return status;
}

auto
ManagementPlane::QuerySessionStatus(std::uint64_t session_id) const -> base::Result<ManagedSessionStatus>
{
  if (impl_->engine == nullptr) {
    return base::Status::InvalidArgument("management plane requires an engine");
  }
  if (impl_->engine->config() == nullptr) {
    return base::Status::InvalidArgument("engine must be booted before querying session status");
  }
  const auto* counterparty = impl_->engine->FindCounterpartyConfig(session_id);
  if (counterparty == nullptr) {
    return base::Status::NotFound("session was not found");
  }
  const auto snapshots = impl_->engine->QuerySessionSnapshots();
  return BuildSessionStatus(*counterparty, snapshots);
}

auto
ManagementPlane::QueryAllSessions() const -> base::Result<std::vector<ManagedSessionStatus>>
{
  auto engine_status = QueryEngineStatus();
  if (!engine_status.ok()) {
    return engine_status.status();
  }
  return std::move(engine_status).value().sessions;
}

auto
ManagementPlane::IsHealthy() const -> bool
{
  return impl_->engine != nullptr && impl_->engine->config() != nullptr;
}

auto
ManagementPlane::HealthSummary() const -> std::string
{
  auto status = QueryEngineStatus();
  if (!status.ok()) {
    return std::string("engine unhealthy: ").append(status.status().message());
  }

  std::ostringstream out;
  out << "engine healthy: sessions=" << status.value().total_sessions << ", workers=" << status.value().worker_count
      << ", profiles=" << status.value().loaded_profiles << ", listeners=" << status.value().listener_count;
  return out.str();
}

auto
ManagementPlane::SetApplicationMessagesAvailable(std::uint64_t session_id, bool available) -> base::Status
{
  if (impl_->engine == nullptr || impl_->engine->config() == nullptr) {
    return base::Status::InvalidArgument("engine must be booted before changing application availability");
  }
  if (impl_->engine->FindCounterpartyConfig(session_id) == nullptr) {
    return base::Status::NotFound("session was not found");
  }
  (void)available;
  return base::Status::InvalidArgument("application message availability changes are not yet supported at runtime");
}

auto
ManagementPlane::boot_timestamp_ns() const -> std::uint64_t
{
  return impl_->boot_timestamp_ns;
}

} // namespace nimble::runtime
