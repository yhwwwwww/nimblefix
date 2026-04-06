#include "fastfix/runtime/metrics.h"

#include <algorithm>

namespace fastfix::runtime {

auto MetricsRegistry::Reset(std::uint32_t worker_count) -> void {
    workers_.clear();
    workers_.reserve(worker_count);
    for (std::uint32_t worker_id = 0; worker_id < worker_count; ++worker_id) {
        workers_.push_back(WorkerMetrics{.worker_id = worker_id});
    }
    sessions_.clear();
}

auto MetricsRegistry::FindMutableSession(std::uint64_t session_id) -> SessionMetrics* {
    const auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        return nullptr;
    }
    return &it->second;
}

auto MetricsRegistry::FindMutableWorker(std::uint32_t worker_id) -> WorkerMetrics* {
    if (worker_id >= workers_.size()) {
        return nullptr;
    }
    return &workers_[worker_id];
}

auto MetricsRegistry::RegisterSession(std::uint64_t session_id, std::uint32_t worker_id) -> base::Status {
    if (session_id == 0) {
        return base::Status::InvalidArgument("metrics session_id must be positive");
    }
    auto* worker = FindMutableWorker(worker_id);
    if (worker == nullptr) {
        return base::Status::NotFound("metrics worker_id is not registered");
    }
    if (!sessions_.emplace(session_id, SessionMetrics{.session_id = session_id, .worker_id = worker_id}).second) {
        return base::Status::AlreadyExists("metrics session already registered");
    }
    ++worker->registered_sessions;
    return base::Status::Ok();
}

auto MetricsRegistry::RecordInbound(std::uint64_t session_id, bool is_admin) -> base::Status {
    auto* session = FindMutableSession(session_id);
    if (session == nullptr) {
        return base::Status::NotFound("metrics session not found");
    }
    auto* worker = FindMutableWorker(session->worker_id);
    ++session->inbound_messages;
    ++worker->inbound_messages;
    if (is_admin) {
        ++session->admin_messages;
        ++worker->admin_messages;
    }
    return base::Status::Ok();
}

auto MetricsRegistry::RecordOutbound(std::uint64_t session_id, bool is_admin) -> base::Status {
    auto* session = FindMutableSession(session_id);
    if (session == nullptr) {
        return base::Status::NotFound("metrics session not found");
    }
    auto* worker = FindMutableWorker(session->worker_id);
    ++session->outbound_messages;
    ++worker->outbound_messages;
    if (is_admin) {
        ++session->admin_messages;
        ++worker->admin_messages;
    }
    return base::Status::Ok();
}

auto MetricsRegistry::RecordResendRequest(std::uint64_t session_id) -> base::Status {
    auto* session = FindMutableSession(session_id);
    if (session == nullptr) {
        return base::Status::NotFound("metrics session not found");
    }
    auto* worker = FindMutableWorker(session->worker_id);
    ++session->resend_requests;
    ++worker->resend_requests;
    return base::Status::Ok();
}

auto MetricsRegistry::RecordGapFill(std::uint64_t session_id, std::uint64_t count) -> base::Status {
    auto* session = FindMutableSession(session_id);
    if (session == nullptr) {
        return base::Status::NotFound("metrics session not found");
    }
    auto* worker = FindMutableWorker(session->worker_id);
    session->gap_fills += count;
    worker->gap_fills += count;
    return base::Status::Ok();
}

auto MetricsRegistry::RecordParseFailure(std::uint64_t session_id) -> base::Status {
    auto* session = FindMutableSession(session_id);
    if (session == nullptr) {
        return base::Status::NotFound("metrics session not found");
    }
    auto* worker = FindMutableWorker(session->worker_id);
    ++session->parse_failures;
    ++worker->parse_failures;
    return base::Status::Ok();
}

auto MetricsRegistry::RecordChecksumFailure(std::uint64_t session_id) -> base::Status {
    auto* session = FindMutableSession(session_id);
    if (session == nullptr) {
        return base::Status::NotFound("metrics session not found");
    }
    auto* worker = FindMutableWorker(session->worker_id);
    ++session->checksum_failures;
    ++worker->checksum_failures;
    return base::Status::Ok();
}

auto MetricsRegistry::UpdateOutboundQueueDepth(std::uint64_t session_id, std::uint32_t depth) -> base::Status {
    auto* session = FindMutableSession(session_id);
    if (session == nullptr) {
        return base::Status::NotFound("metrics session not found");
    }
    auto* worker = FindMutableWorker(session->worker_id);
    worker->outbound_queue_depth -= session->outbound_queue_depth;
    session->outbound_queue_depth = depth;
    worker->outbound_queue_depth += depth;
    return base::Status::Ok();
}

auto MetricsRegistry::ObserveStoreFlushLatency(std::uint64_t session_id, std::uint64_t latency_ns) -> base::Status {
    auto* session = FindMutableSession(session_id);
    if (session == nullptr) {
        return base::Status::NotFound("metrics session not found");
    }
    auto* worker = FindMutableWorker(session->worker_id);
    session->last_store_flush_latency_ns = latency_ns;
    worker->last_store_flush_latency_ns = latency_ns;
    return base::Status::Ok();
}

auto MetricsRegistry::Snapshot() const -> RuntimeMetricsSnapshot {
    RuntimeMetricsSnapshot snapshot;
    snapshot.workers = workers_;
    snapshot.sessions.reserve(sessions_.size());
    for (const auto& [_, session] : sessions_) {
        snapshot.sessions.push_back(session);
    }
    std::sort(snapshot.sessions.begin(), snapshot.sessions.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.session_id < rhs.session_id;
    });
    return snapshot;
}

auto MetricsRegistry::FindSession(std::uint64_t session_id) const -> const SessionMetrics* {
    const auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
        return nullptr;
    }
    return &it->second;
}

auto MetricsRegistry::FindWorker(std::uint32_t worker_id) const -> const WorkerMetrics* {
    if (worker_id >= workers_.size()) {
        return nullptr;
    }
    return &workers_[worker_id];
}

}  // namespace fastfix::runtime