#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "nimblefix/base/status.h"

namespace nimble::runtime {

struct SessionMetrics
{
  std::uint64_t session_id{ 0 };
  std::uint32_t worker_id{ 0 };
  std::atomic<std::uint64_t> inbound_messages{ 0 };
  std::atomic<std::uint64_t> outbound_messages{ 0 };
  std::atomic<std::uint64_t> admin_messages{ 0 };
  std::atomic<std::uint64_t> resend_requests{ 0 };
  std::atomic<std::uint64_t> gap_fills{ 0 };
  std::atomic<std::uint64_t> parse_failures{ 0 };
  std::atomic<std::uint64_t> checksum_failures{ 0 };
  std::atomic<std::uint32_t> outbound_queue_depth{ 0 };
  std::atomic<std::uint64_t> last_store_flush_latency_ns{ 0 };
};

struct WorkerMetrics
{
  std::uint32_t worker_id{ 0 };
  std::atomic<std::uint64_t> registered_sessions{ 0 };
  std::atomic<std::uint64_t> inbound_messages{ 0 };
  std::atomic<std::uint64_t> outbound_messages{ 0 };
  std::atomic<std::uint64_t> admin_messages{ 0 };
  std::atomic<std::uint64_t> resend_requests{ 0 };
  std::atomic<std::uint64_t> gap_fills{ 0 };
  std::atomic<std::uint64_t> parse_failures{ 0 };
  std::atomic<std::uint64_t> checksum_failures{ 0 };
  std::atomic<std::uint64_t> outbound_queue_depth{ 0 };
  std::atomic<std::uint64_t> last_store_flush_latency_ns{ 0 };

  // Steady-state breakdown timing (nanoseconds, relaxed stores from worker
  // thread).
  std::atomic<std::uint64_t> poll_wait_ns{ 0 };
  std::atomic<std::uint64_t> recv_dispatch_ns{ 0 };
  std::atomic<std::uint64_t> app_callback_ns{ 0 };
  std::atomic<std::uint64_t> timer_process_ns{ 0 };
  std::atomic<std::uint64_t> send_ns{ 0 };
  std::atomic<std::uint64_t> poll_iterations{ 0 };
};

struct RuntimeMetricsSnapshot
{
  struct SessionEntry
  {
    std::uint64_t session_id{ 0 };
    std::uint32_t worker_id{ 0 };
    std::uint64_t inbound_messages{ 0 };
    std::uint64_t outbound_messages{ 0 };
    std::uint64_t admin_messages{ 0 };
    std::uint64_t resend_requests{ 0 };
    std::uint64_t gap_fills{ 0 };
    std::uint64_t parse_failures{ 0 };
    std::uint64_t checksum_failures{ 0 };
    std::uint32_t outbound_queue_depth{ 0 };
    std::uint64_t last_store_flush_latency_ns{ 0 };
  };

  struct WorkerEntry
  {
    std::uint32_t worker_id{ 0 };
    std::uint64_t registered_sessions{ 0 };
    std::uint64_t inbound_messages{ 0 };
    std::uint64_t outbound_messages{ 0 };
    std::uint64_t admin_messages{ 0 };
    std::uint64_t resend_requests{ 0 };
    std::uint64_t gap_fills{ 0 };
    std::uint64_t parse_failures{ 0 };
    std::uint64_t checksum_failures{ 0 };
    std::uint64_t outbound_queue_depth{ 0 };
    std::uint64_t last_store_flush_latency_ns{ 0 };
    std::uint64_t poll_wait_ns{ 0 };
    std::uint64_t recv_dispatch_ns{ 0 };
    std::uint64_t app_callback_ns{ 0 };
    std::uint64_t timer_process_ns{ 0 };
    std::uint64_t send_ns{ 0 };
    std::uint64_t poll_iterations{ 0 };
  };

  std::vector<WorkerEntry> workers;
  std::vector<SessionEntry> sessions;
};

class MetricsRegistry
{
public:
  auto Reset(std::uint32_t worker_count) -> void;
  auto RegisterSession(std::uint64_t session_id, std::uint32_t worker_id) -> base::Status;

  auto RecordInbound(std::uint64_t session_id, bool is_admin) -> base::Status;
  auto RecordOutbound(std::uint64_t session_id, bool is_admin) -> base::Status;
  auto RecordResendRequest(std::uint64_t session_id) -> base::Status;
  auto RecordGapFill(std::uint64_t session_id, std::uint64_t count) -> base::Status;
  auto RecordParseFailure(std::uint64_t session_id) -> base::Status;
  auto RecordChecksumFailure(std::uint64_t session_id) -> base::Status;
  auto UpdateOutboundQueueDepth(std::uint64_t session_id, std::uint32_t depth) -> base::Status;
  auto ObserveStoreFlushLatency(std::uint64_t session_id, std::uint64_t latency_ns) -> base::Status;

  [[nodiscard]] auto Snapshot() const -> RuntimeMetricsSnapshot;
  [[nodiscard]] auto FindSession(std::uint64_t session_id) const -> const SessionMetrics*;
  // Workers are allocated during Reset()/Boot() and then only read
  // concurrently.
  [[nodiscard]] auto FindWorker(std::uint32_t worker_id) const -> const WorkerMetrics*;
  // Mutable worker pointers are stable after Reset(); callers only mutate the
  // atomics they own.
  [[nodiscard]] auto FindWorker(std::uint32_t worker_id) -> WorkerMetrics*;

private:
  auto FindMutableSession(std::uint64_t session_id) -> SessionMetrics*;
  auto FindMutableWorker(std::uint32_t worker_id) -> WorkerMetrics*;

  mutable std::shared_mutex sessions_mutex_;
  std::vector<std::unique_ptr<WorkerMetrics>> workers_;
  std::unordered_map<std::uint64_t, std::unique_ptr<SessionMetrics>> sessions_;
};

} // namespace nimble::runtime