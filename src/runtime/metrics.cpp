#include "fastfix/runtime/metrics.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <shared_mutex>

namespace fastfix::runtime {

auto
MetricsRegistry::Reset(std::uint32_t worker_count) -> void
{
  std::unique_lock lock(sessions_mutex_);
  workers_.clear();
  workers_.reserve(worker_count);
  for (std::uint32_t worker_id = 0; worker_id < worker_count; ++worker_id) {
    auto w = std::make_unique<WorkerMetrics>();
    w->worker_id = worker_id;
    workers_.push_back(std::move(w));
  }
  sessions_.clear();
}

auto
MetricsRegistry::FindMutableSession(std::uint64_t session_id) -> SessionMetrics*
{
  const auto it = sessions_.find(session_id);
  if (it == sessions_.end()) {
    return nullptr;
  }
  return it->second.get();
}

auto
MetricsRegistry::FindMutableWorker(std::uint32_t worker_id) -> WorkerMetrics*
{
  if (worker_id >= workers_.size()) {
    return nullptr;
  }
  return workers_[worker_id].get();
}

auto
MetricsRegistry::RegisterSession(std::uint64_t session_id, std::uint32_t worker_id) -> base::Status
{
  if (session_id == 0) {
    return base::Status::InvalidArgument("metrics session_id must be positive");
  }
  std::unique_lock lock(sessions_mutex_);
  auto* worker = FindMutableWorker(worker_id);
  if (worker == nullptr) {
    return base::Status::NotFound("metrics worker_id is not registered");
  }
  auto s = std::make_unique<SessionMetrics>();
  s->session_id = session_id;
  s->worker_id = worker_id;
  if (!sessions_.emplace(session_id, std::move(s)).second) {
    return base::Status::AlreadyExists("metrics session already registered");
  }
  worker->registered_sessions.fetch_add(1, std::memory_order_relaxed);
  return base::Status::Ok();
}

auto
MetricsRegistry::RecordInbound(std::uint64_t session_id, bool is_admin) -> base::Status
{
  auto* session = FindMutableSession(session_id);
  if (session == nullptr) {
    return base::Status::NotFound("metrics session not found");
  }
  auto* worker = FindMutableWorker(session->worker_id);
  session->inbound_messages.fetch_add(1, std::memory_order_relaxed);
  worker->inbound_messages.fetch_add(1, std::memory_order_relaxed);
  if (is_admin) {
    session->admin_messages.fetch_add(1, std::memory_order_relaxed);
    worker->admin_messages.fetch_add(1, std::memory_order_relaxed);
  }
  return base::Status::Ok();
}

auto
MetricsRegistry::RecordOutbound(std::uint64_t session_id, bool is_admin) -> base::Status
{
  auto* session = FindMutableSession(session_id);
  if (session == nullptr) {
    return base::Status::NotFound("metrics session not found");
  }
  auto* worker = FindMutableWorker(session->worker_id);
  session->outbound_messages.fetch_add(1, std::memory_order_relaxed);
  worker->outbound_messages.fetch_add(1, std::memory_order_relaxed);
  if (is_admin) {
    session->admin_messages.fetch_add(1, std::memory_order_relaxed);
    worker->admin_messages.fetch_add(1, std::memory_order_relaxed);
  }
  return base::Status::Ok();
}

auto
MetricsRegistry::RecordResendRequest(std::uint64_t session_id) -> base::Status
{
  auto* session = FindMutableSession(session_id);
  if (session == nullptr) {
    return base::Status::NotFound("metrics session not found");
  }
  auto* worker = FindMutableWorker(session->worker_id);
  session->resend_requests.fetch_add(1, std::memory_order_relaxed);
  worker->resend_requests.fetch_add(1, std::memory_order_relaxed);
  return base::Status::Ok();
}

auto
MetricsRegistry::RecordGapFill(std::uint64_t session_id, std::uint64_t count) -> base::Status
{
  auto* session = FindMutableSession(session_id);
  if (session == nullptr) {
    return base::Status::NotFound("metrics session not found");
  }
  auto* worker = FindMutableWorker(session->worker_id);
  session->gap_fills.fetch_add(count, std::memory_order_relaxed);
  worker->gap_fills.fetch_add(count, std::memory_order_relaxed);
  return base::Status::Ok();
}

auto
MetricsRegistry::RecordParseFailure(std::uint64_t session_id) -> base::Status
{
  auto* session = FindMutableSession(session_id);
  if (session == nullptr) {
    return base::Status::NotFound("metrics session not found");
  }
  auto* worker = FindMutableWorker(session->worker_id);
  session->parse_failures.fetch_add(1, std::memory_order_relaxed);
  worker->parse_failures.fetch_add(1, std::memory_order_relaxed);
  return base::Status::Ok();
}

auto
MetricsRegistry::RecordChecksumFailure(std::uint64_t session_id) -> base::Status
{
  auto* session = FindMutableSession(session_id);
  if (session == nullptr) {
    return base::Status::NotFound("metrics session not found");
  }
  auto* worker = FindMutableWorker(session->worker_id);
  session->checksum_failures.fetch_add(1, std::memory_order_relaxed);
  worker->checksum_failures.fetch_add(1, std::memory_order_relaxed);
  return base::Status::Ok();
}

auto
MetricsRegistry::UpdateOutboundQueueDepth(std::uint64_t session_id, std::uint32_t depth) -> base::Status
{
  auto* session = FindMutableSession(session_id);
  if (session == nullptr) {
    return base::Status::NotFound("metrics session not found");
  }
  auto* worker = FindMutableWorker(session->worker_id);
  const auto old_depth = session->outbound_queue_depth.exchange(depth, std::memory_order_relaxed);
  worker->outbound_queue_depth.fetch_sub(old_depth, std::memory_order_relaxed);
  worker->outbound_queue_depth.fetch_add(depth, std::memory_order_relaxed);
  return base::Status::Ok();
}

auto
MetricsRegistry::ObserveStoreFlushLatency(std::uint64_t session_id, std::uint64_t latency_ns) -> base::Status
{
  auto* session = FindMutableSession(session_id);
  if (session == nullptr) {
    return base::Status::NotFound("metrics session not found");
  }
  auto* worker = FindMutableWorker(session->worker_id);
  session->last_store_flush_latency_ns.store(latency_ns, std::memory_order_relaxed);
  worker->last_store_flush_latency_ns.store(latency_ns, std::memory_order_relaxed);
  return base::Status::Ok();
}

auto
MetricsRegistry::Snapshot() const -> RuntimeMetricsSnapshot
{
  std::shared_lock lock(sessions_mutex_);
  RuntimeMetricsSnapshot snapshot;
  snapshot.workers.reserve(workers_.size());
  for (const auto& wp : workers_) {
    const auto& w = *wp;
    snapshot.workers.push_back(RuntimeMetricsSnapshot::WorkerEntry{
      .worker_id = w.worker_id,
      .registered_sessions = w.registered_sessions.load(std::memory_order_relaxed),
      .inbound_messages = w.inbound_messages.load(std::memory_order_relaxed),
      .outbound_messages = w.outbound_messages.load(std::memory_order_relaxed),
      .admin_messages = w.admin_messages.load(std::memory_order_relaxed),
      .resend_requests = w.resend_requests.load(std::memory_order_relaxed),
      .gap_fills = w.gap_fills.load(std::memory_order_relaxed),
      .parse_failures = w.parse_failures.load(std::memory_order_relaxed),
      .checksum_failures = w.checksum_failures.load(std::memory_order_relaxed),
      .outbound_queue_depth = w.outbound_queue_depth.load(std::memory_order_relaxed),
      .last_store_flush_latency_ns = w.last_store_flush_latency_ns.load(std::memory_order_relaxed),
      .poll_wait_ns = w.poll_wait_ns.load(std::memory_order_relaxed),
      .recv_dispatch_ns = w.recv_dispatch_ns.load(std::memory_order_relaxed),
      .app_callback_ns = w.app_callback_ns.load(std::memory_order_relaxed),
      .timer_process_ns = w.timer_process_ns.load(std::memory_order_relaxed),
      .send_ns = w.send_ns.load(std::memory_order_relaxed),
      .poll_iterations = w.poll_iterations.load(std::memory_order_relaxed),
    });
  }
  snapshot.sessions.reserve(sessions_.size());
  for (const auto& [_, sp] : sessions_) {
    const auto& s = *sp;
    snapshot.sessions.push_back(RuntimeMetricsSnapshot::SessionEntry{
      .session_id = s.session_id,
      .worker_id = s.worker_id,
      .inbound_messages = s.inbound_messages.load(std::memory_order_relaxed),
      .outbound_messages = s.outbound_messages.load(std::memory_order_relaxed),
      .admin_messages = s.admin_messages.load(std::memory_order_relaxed),
      .resend_requests = s.resend_requests.load(std::memory_order_relaxed),
      .gap_fills = s.gap_fills.load(std::memory_order_relaxed),
      .parse_failures = s.parse_failures.load(std::memory_order_relaxed),
      .checksum_failures = s.checksum_failures.load(std::memory_order_relaxed),
      .outbound_queue_depth = static_cast<std::uint32_t>(s.outbound_queue_depth.load(std::memory_order_relaxed)),
      .last_store_flush_latency_ns = s.last_store_flush_latency_ns.load(std::memory_order_relaxed),
    });
  }
  std::sort(snapshot.sessions.begin(), snapshot.sessions.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.session_id < rhs.session_id;
  });
  return snapshot;
}

auto
MetricsRegistry::FindSession(std::uint64_t session_id) const -> const SessionMetrics*
{
  std::shared_lock lock(sessions_mutex_);
  const auto it = sessions_.find(session_id);
  if (it == sessions_.end()) {
    return nullptr;
  }
  return it->second.get();
}

auto
MetricsRegistry::FindWorker(std::uint32_t worker_id) const -> const WorkerMetrics*
{
  if (worker_id >= workers_.size()) {
    return nullptr;
  }
  return workers_[worker_id].get();
}

auto
MetricsRegistry::FindWorker(std::uint32_t worker_id) -> WorkerMetrics*
{
  return FindMutableWorker(worker_id);
}

} // namespace fastfix::runtime