#include "nimblefix/runtime/sharded_runtime.h"

#include <algorithm>

namespace nimble::runtime {

ShardedRuntime::ShardedRuntime(std::uint32_t worker_count)
{
  if (worker_count == 0) {
    worker_count = 1;
  }

  shards_.reserve(worker_count);
  for (std::uint32_t worker_id = 0; worker_id < worker_count; ++worker_id) {
    shards_.push_back(WorkerShard{ .worker_id = worker_id });
  }
}

auto
ShardedRuntime::RouteSession(const session::SessionKey& key) const -> std::uint32_t
{
  const auto hash = session::SessionKeyHash{}(key);
  return static_cast<std::uint32_t>(hash % shards_.size());
}

auto
ShardedRuntime::RegisterSession(const session::SessionCore& session) -> base::Status
{
  if (session_to_worker_.contains(session.session_id())) {
    return base::Status::AlreadyExists("session is already registered with a worker shard");
  }

  const auto worker_id = RouteSession(session.key());
  shards_[worker_id].session_ids.push_back(session.session_id());
  session_to_worker_.emplace(session.session_id(), worker_id);
  return base::Status::Ok();
}

auto
ShardedRuntime::RegisterPendingConnection(const PendingConnection& pending) -> base::Status
{
  if (pending_to_worker_.contains(pending.connection_id)) {
    return base::Status::AlreadyExists("pending connection is already registered with a worker shard");
  }

  const auto worker_id = RouteSession(pending.session_key);
  shards_[worker_id].pending_connection_ids.push_back(pending.connection_id);
  pending_to_worker_.emplace(pending.connection_id, worker_id);
  return base::Status::Ok();
}

auto
ShardedRuntime::UnregisterPendingConnection(std::uint64_t connection_id) -> base::Status
{
  const auto it = pending_to_worker_.find(connection_id);
  if (it == pending_to_worker_.end()) {
    return base::Status::NotFound("pending connection is not registered with a worker shard");
  }

  auto& ids = shards_[it->second].pending_connection_ids;
  ids.erase(std::remove(ids.begin(), ids.end(), connection_id), ids.end());
  pending_to_worker_.erase(it);
  return base::Status::Ok();
}

auto
ShardedRuntime::FindShard(std::uint32_t worker_id) const -> const WorkerShard*
{
  if (worker_id >= shards_.size()) {
    return nullptr;
  }
  return &shards_[worker_id];
}

auto
ShardedRuntime::FindSessionShard(std::uint64_t session_id) const -> const WorkerShard*
{
  const auto it = session_to_worker_.find(session_id);
  if (it == session_to_worker_.end()) {
    return nullptr;
  }
  return FindShard(it->second);
}

auto
ShardedRuntime::FindPendingConnectionShard(std::uint64_t connection_id) const -> const WorkerShard*
{
  const auto it = pending_to_worker_.find(connection_id);
  if (it == pending_to_worker_.end()) {
    return nullptr;
  }
  return FindShard(it->second);
}

} // namespace nimble::runtime
