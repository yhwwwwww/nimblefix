#include "nimblefix/runtime/live_session_registry.h"

#include <memory>
#include <optional>
#include <utility>

#include "nimblefix/base/spsc_queue.h"

namespace nimble::runtime {

namespace {

class SubscriberStream final : public session::SessionSubscriptionStream
{
public:
  explicit SubscriberStream(std::size_t queue_capacity)
    : queue_(queue_capacity)
  {
  }

  auto TryPop() -> base::Result<std::optional<session::SessionNotification>> override { return queue_.TryPop(); }

  auto TryPush(const session::SessionNotification& notification) -> bool { return queue_.TryPush(notification); }

private:
  base::SpscQueue<session::SessionNotification> queue_;
};

} // namespace

auto
LiveSessionRegistry::Clear() -> void
{
  std::lock_guard lock(mutex_);
  snapshots_.clear();
  subscribers_.clear();
  active_session_ids_.clear();
  terminal_status_.reset();
}

auto
LiveSessionRegistry::ClearTerminalStatus() -> void
{
  std::lock_guard lock(mutex_);
  terminal_status_.reset();
}

auto
LiveSessionRegistry::HasActiveSession(std::uint64_t session_id) const -> bool
{
  std::lock_guard lock(mutex_);
  return active_session_ids_.contains(session_id);
}

auto
LiveSessionRegistry::RegisterActiveSession(std::uint64_t session_id) -> bool
{
  std::lock_guard lock(mutex_);
  return active_session_ids_.emplace(session_id).second;
}

auto
LiveSessionRegistry::UnregisterActiveSession(std::uint64_t session_id) -> void
{
  std::lock_guard lock(mutex_);
  active_session_ids_.erase(session_id);
}

auto
LiveSessionRegistry::SetTerminalStatus(base::Status status) -> void
{
  std::lock_guard lock(mutex_);
  if (!terminal_status_.has_value()) {
    terminal_status_ = std::move(status);
  }
}

auto
LiveSessionRegistry::LoadTerminalStatus() const -> std::optional<base::Status>
{
  std::lock_guard lock(mutex_);
  return terminal_status_;
}

auto
LiveSessionRegistry::UpdateSnapshot(session::SessionSnapshot snapshot) -> void
{
  std::lock_guard lock(mutex_);
  snapshots_[snapshot.session_id] = std::move(snapshot);
}

auto
LiveSessionRegistry::LoadSnapshot(std::uint64_t session_id) const -> base::Result<session::SessionSnapshot>
{
  std::lock_guard lock(mutex_);
  const auto it = snapshots_.find(session_id);
  if (it == snapshots_.end()) {
    return base::Status::NotFound("session snapshot was not found");
  }
  return it->second;
}

auto
LiveSessionRegistry::RegisterSubscriber(std::uint64_t session_id, std::size_t queue_capacity)
  -> base::Result<session::SessionSubscription>
{
  auto stream = std::make_shared<SubscriberStream>(queue_capacity);

  std::lock_guard lock(mutex_);
  if (!snapshots_.contains(session_id)) {
    return base::Status::NotFound("session subscription target was not found");
  }
  subscribers_[session_id].push_back(stream);
  return session::SessionSubscription(std::move(stream));
}

auto
LiveSessionRegistry::HasSubscribers(std::uint64_t session_id) -> bool
{
  std::lock_guard lock(mutex_);
  const auto it = subscribers_.find(session_id);
  if (it == subscribers_.end()) {
    return false;
  }

  auto& weak_subscribers = it->second;
  for (auto weak_it = weak_subscribers.begin(); weak_it != weak_subscribers.end();) {
    if (weak_it->expired()) {
      weak_it = weak_subscribers.erase(weak_it);
      continue;
    }
    return true;
  }

  subscribers_.erase(it);
  return false;
}

auto
LiveSessionRegistry::PublishNotification(const session::SessionNotification& notification) -> void
{
  std::vector<std::shared_ptr<session::SessionSubscriptionStream>> subscribers;
  {
    std::lock_guard lock(mutex_);
    snapshots_[notification.snapshot.session_id] = notification.snapshot;

    auto it = subscribers_.find(notification.snapshot.session_id);
    if (it == subscribers_.end()) {
      return;
    }

    auto& weak_subscribers = it->second;
    for (auto weak_it = weak_subscribers.begin(); weak_it != weak_subscribers.end();) {
      auto subscriber = weak_it->lock();
      if (subscriber == nullptr) {
        weak_it = weak_subscribers.erase(weak_it);
        continue;
      }
      subscribers.push_back(std::move(subscriber));
      ++weak_it;
    }
  }

  for (const auto& subscriber : subscribers) {
    auto* stream = dynamic_cast<SubscriberStream*>(subscriber.get());
    if (stream != nullptr) {
      static_cast<void>(stream->TryPush(notification));
    }
  }
}

} // namespace nimble::runtime