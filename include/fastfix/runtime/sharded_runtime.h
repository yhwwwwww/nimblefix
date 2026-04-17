#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "fastfix/base/status.h"
#include "fastfix/session/session_core.h"

namespace fastfix::runtime {

struct PendingConnection
{
  std::uint64_t connection_id{ 0 };
  session::SessionKey session_key;
  std::uint64_t profile_id{ 0 };
};

struct WorkerShard
{
  std::uint32_t worker_id{ 0 };
  std::vector<std::uint64_t> session_ids;
  std::vector<std::uint64_t> pending_connection_ids;
};

class ShardedRuntime
{
public:
  explicit ShardedRuntime(std::uint32_t worker_count);

  [[nodiscard]] std::uint32_t worker_count() const { return static_cast<std::uint32_t>(shards_.size()); }

  [[nodiscard]] std::size_t session_count() const { return session_to_worker_.size(); }

  [[nodiscard]] std::size_t pending_connection_count() const { return pending_to_worker_.size(); }

  [[nodiscard]] auto RouteSession(const session::SessionKey& key) const -> std::uint32_t;
  auto RegisterSession(const session::SessionCore& session) -> base::Status;
  auto RegisterPendingConnection(const PendingConnection& pending) -> base::Status;
  auto UnregisterPendingConnection(std::uint64_t connection_id) -> base::Status;

  [[nodiscard]] auto FindShard(std::uint32_t worker_id) const -> const WorkerShard*;
  [[nodiscard]] auto FindSessionShard(std::uint64_t session_id) const -> const WorkerShard*;
  [[nodiscard]] auto FindPendingConnectionShard(std::uint64_t connection_id) const -> const WorkerShard*;

private:
  std::vector<WorkerShard> shards_;
  std::unordered_map<std::uint64_t, std::uint32_t> session_to_worker_;
  std::unordered_map<std::uint64_t, std::uint32_t> pending_to_worker_;
};

} // namespace fastfix::runtime
