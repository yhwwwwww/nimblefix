#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "nimblefix/base/status.h"

namespace nimble::runtime {

template<class Profile>
class Session;

template<class Profile>
class InlineSession;

/// Default per-worker queue capacity for queue-decoupled application delivery.
inline constexpr std::size_t kDefaultQueueEventCapacity = 1024U;
/// Sentinel meaning control queue capacity should reuse queue_capacity.
inline constexpr std::size_t kUseApplicationQueueCapacity = 0U;
/// Default upper bound of events drained by one poll step.
inline constexpr std::size_t kDefaultQueuePollerBatchLimit = 256U;
/// Sentinel meaning drain until the selected worker queue is empty.
inline constexpr std::size_t kDrainAllQueueEvents = 0U;
/// Default sleep-oriented runtime poll timeout.
inline constexpr auto kDefaultRuntimePollTimeout = std::chrono::milliseconds{ 50 };
/// Default transport I/O timeout used by live runtimes.
inline constexpr auto kDefaultRuntimeIoTimeout = std::chrono::milliseconds{ 5'000 };

/// Profile-agnostic typed lifecycle callback base.
///
/// Generated profile handlers derive from this template and add the typed
/// business-message callbacks for one concrete Profile.
template<class Profile>
class Application
{
public:
  virtual ~Application() = default;

  virtual auto OnSessionBound(Session<Profile>& session) -> base::Status
  {
    (void)session;
    return base::Status::Ok();
  }

  virtual auto OnSessionActive(Session<Profile>& session) -> base::Status
  {
    (void)session;
    return base::Status::Ok();
  }

  virtual auto OnSessionPoll(InlineSession<Profile>& session) -> base::Status
  {
    (void)session;
    return base::Status::Ok();
  }

  virtual auto OnSessionClosed(Session<Profile>& session, std::string_view text) -> base::Status
  {
    (void)session;
    (void)text;
    return base::Status::Ok();
  }
};

} // namespace nimble::runtime
