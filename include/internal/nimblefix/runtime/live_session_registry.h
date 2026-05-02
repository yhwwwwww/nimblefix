#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "nimblefix/advanced/session_handle.h"
#include "nimblefix/base/result.h"
#include "nimblefix/base/status.h"

namespace nimble::runtime {

class LiveSessionRegistry
{
public:
  auto Clear() -> void;
  auto ClearTerminalStatus() -> void;

  [[nodiscard]] auto HasActiveSession(std::uint64_t session_id) const -> bool;
  auto RegisterActiveSession(std::uint64_t session_id) -> bool;
  auto UnregisterActiveSession(std::uint64_t session_id) -> void;

  auto SetTerminalStatus(base::Status status) -> void;
  [[nodiscard]] auto LoadTerminalStatus() const -> std::optional<base::Status>;

  auto UpdateSnapshot(session::SessionSnapshot snapshot) -> void;
  [[nodiscard]] auto LoadSnapshot(std::uint64_t session_id) const -> base::Result<session::SessionSnapshot>;
  [[nodiscard]] auto LoadAllSnapshots() const -> std::vector<session::SessionSnapshot>;

  auto RegisterSubscriber(std::uint64_t session_id, std::size_t queue_capacity)
    -> base::Result<session::SessionSubscription>;
  auto HasSubscribers(std::uint64_t session_id) -> bool;
  auto PublishNotification(const session::SessionNotification& notification) -> void;

private:
  mutable std::mutex mutex_;
  std::unordered_map<std::uint64_t, session::SessionSnapshot> snapshots_;
  std::unordered_map<std::uint64_t, std::vector<std::weak_ptr<session::SessionSubscriptionStream>>> subscribers_;
  std::unordered_set<std::uint64_t> active_session_ids_;
  std::optional<base::Status> terminal_status_;
};

} // namespace nimble::runtime
