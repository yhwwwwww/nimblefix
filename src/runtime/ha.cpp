#include "nimblefix/runtime/ha.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <utility>

#include "nimblefix/runtime/engine.h"

namespace nimble::runtime {

namespace {

auto
NowNs() -> std::uint64_t
{
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

} // namespace

struct HaController::Impl
{
  mutable std::mutex mutex;
  HaConfig config;
  std::atomic<HaRole> role{ HaRole::kSolo };
  std::atomic<HaPeerState> peer_state{ HaPeerState::kUnknown };
  std::atomic<std::uint64_t> generation{ 0 };
  std::atomic<std::uint64_t> last_peer_heartbeat_ns{ 0 };
  std::atomic<bool> running{ false };
  bool configured{ false };
  std::optional<HaStateSnapshot> last_applied_snapshot;
  std::uint64_t last_applied_generation{ 0 };

  auto ChangeRole(HaRole expected, HaRole desired) -> base::Status
  {
    std::lock_guard lock(mutex);
    const auto current = role.load(std::memory_order_acquire);
    if (current != expected) {
      return base::Status::InvalidArgument("HA role transition is not valid from the current role");
    }
    role.store(desired, std::memory_order_release);
    generation.fetch_add(1U, std::memory_order_acq_rel);
    if (config.on_role_change) {
      config.on_role_change(current, desired);
    }
    return base::Status::Ok();
  }
};

HaController::HaController()
  : impl_(std::make_unique<Impl>())
{
}

HaController::~HaController() = default;

HaController::HaController(HaController&&) noexcept = default;

auto
HaController::operator=(HaController&&) noexcept -> HaController& = default;

auto
HaController::Configure(HaConfig config) -> base::Status
{
  if (impl_->running.load(std::memory_order_acquire)) {
    return base::Status::Busy("cannot configure HA controller while running");
  }
  if (config.heartbeat_interval.count() <= 0) {
    return base::Status::InvalidArgument("HA heartbeat_interval must be positive");
  }
  if (config.replication_interval.count() <= 0) {
    return base::Status::InvalidArgument("HA replication_interval must be positive");
  }
  if (config.suspect_threshold == 0U) {
    return base::Status::InvalidArgument("HA suspect_threshold must be positive");
  }
  if (config.dead_threshold < config.suspect_threshold) {
    return base::Status::InvalidArgument("HA dead_threshold must be greater than or equal to suspect_threshold");
  }

  std::lock_guard lock(impl_->mutex);
  impl_->config = std::move(config);
  impl_->role.store(impl_->config.initial_role, std::memory_order_release);
  impl_->peer_state.store(HaPeerState::kUnknown, std::memory_order_release);
  impl_->generation.store(0U, std::memory_order_release);
  impl_->last_peer_heartbeat_ns.store(0U, std::memory_order_release);
  impl_->configured = true;
  return base::Status::Ok();
}

auto
HaController::Start() -> base::Status
{
  if (!impl_->configured) {
    return base::Status::InvalidArgument("HA controller must be configured before Start");
  }
  impl_->running.store(true, std::memory_order_release);
  return base::Status::Ok();
}

auto
HaController::Stop() -> void
{
  impl_->running.store(false, std::memory_order_release);
}

auto
HaController::role() const -> HaRole
{
  return impl_->role.load(std::memory_order_acquire);
}

auto
HaController::peer_state() const -> HaPeerState
{
  return impl_->peer_state.load(std::memory_order_acquire);
}

auto
HaController::generation() const -> std::uint64_t
{
  return impl_->generation.load(std::memory_order_acquire);
}

auto
HaController::running() const -> bool
{
  return impl_->running.load(std::memory_order_acquire);
}

auto
HaController::PromoteToPrimary() -> base::Status
{
  return impl_->ChangeRole(HaRole::kStandby, HaRole::kPrimary);
}

auto
HaController::DemoteToStandby() -> base::Status
{
  return impl_->ChangeRole(HaRole::kPrimary, HaRole::kStandby);
}

auto
HaController::RecordPeerHeartbeat(std::uint64_t timestamp_ns) -> void
{
  const auto heartbeat = timestamp_ns == 0U ? NowNs() : timestamp_ns;
  impl_->last_peer_heartbeat_ns.store(heartbeat, std::memory_order_release);
  impl_->peer_state.store(HaPeerState::kAlive, std::memory_order_release);
}

auto
HaController::TakeSnapshot(const Engine& engine) -> base::Result<HaStateSnapshot>
{
  const auto* config = engine.config();
  if (config == nullptr) {
    return base::Status::InvalidArgument("engine must be booted before taking HA snapshot");
  }

  HaStateSnapshot snapshot;
  snapshot.snapshot_timestamp_ns = NowNs();
  snapshot.generation = generation();

  auto session_snapshots = engine.QuerySessionSnapshots();
  snapshot.sessions.reserve(std::max(session_snapshots.size(), config->counterparties.size()));
  for (const auto& ss : session_snapshots) {
    snapshot.sessions.push_back(SessionSequenceState{
      .session_id = ss.session_id,
      .next_inbound_seq = ss.next_in_seq,
      .next_outbound_seq = ss.next_out_seq,
      .last_activity_ns = std::max(ss.last_inbound_ns, ss.last_outbound_ns),
    });
  }

  for (const auto& counterparty : config->counterparties) {
    const auto found = std::any_of(snapshot.sessions.begin(), snapshot.sessions.end(), [&](const auto& state) {
      return state.session_id == counterparty.session.session_id;
    });
    if (found) {
      continue;
    }
    snapshot.sessions.push_back(SessionSequenceState{
      .session_id = counterparty.session.session_id,
      .next_inbound_seq = 1U,
      .next_outbound_seq = 1U,
      .last_activity_ns = 0U,
    });
  }
  return snapshot;
}

auto
HaController::ApplySnapshot(Engine& engine, const HaStateSnapshot& snapshot) -> base::Status
{
  if (engine.config() == nullptr) {
    return base::Status::InvalidArgument("engine must be booted before applying HA snapshot");
  }

  std::lock_guard lock(impl_->mutex);
  impl_->last_applied_snapshot = snapshot;
  impl_->last_applied_generation = snapshot.generation;
  engine.SetLastAppliedHaSnapshot(snapshot);
  return base::Status::Ok();
}

auto
HaController::last_applied_snapshot() const -> const std::optional<HaStateSnapshot>&
{
  return impl_->last_applied_snapshot;
}

auto
HaController::CheckHealth(std::uint64_t current_time_ns) -> void
{
  if (!running()) {
    return;
  }

  const auto last_heartbeat = impl_->last_peer_heartbeat_ns.load(std::memory_order_acquire);
  if (last_heartbeat == 0U) {
    return;
  }

  const auto now = current_time_ns == 0U ? NowNs() : current_time_ns;
  if (now <= last_heartbeat) {
    impl_->peer_state.store(HaPeerState::kAlive, std::memory_order_release);
    return;
  }

  const auto heartbeat_ns = static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::nanoseconds>(impl_->config.heartbeat_interval).count());
  if (heartbeat_ns == 0U) {
    return;
  }

  const auto missed = (now - last_heartbeat) / heartbeat_ns;
  if (missed >= impl_->config.dead_threshold) {
    impl_->peer_state.store(HaPeerState::kDead, std::memory_order_release);
    if (impl_->config.auto_failover && role() == HaRole::kStandby) {
      (void)PromoteToPrimary();
    }
  } else if (missed >= impl_->config.suspect_threshold) {
    impl_->peer_state.store(HaPeerState::kSuspect, std::memory_order_release);
  } else {
    impl_->peer_state.store(HaPeerState::kAlive, std::memory_order_release);
  }
}

auto
InMemoryHaTransport::replicator() -> HaStateReplicator
{
  return [this](const HaStateSnapshot& snapshot) -> base::Status {
    std::lock_guard lock(mutex_);
    latest_ = snapshot;
    return base::Status::Ok();
  };
}

auto
InMemoryHaTransport::receiver() -> HaStateReceiver
{
  return [this]() -> base::Result<HaStateSnapshot> {
    std::lock_guard lock(mutex_);
    if (!latest_.has_value()) {
      return base::Status::NotFound("no snapshot available");
    }
    return *latest_;
  };
}

} // namespace nimble::runtime
