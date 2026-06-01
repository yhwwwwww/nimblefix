#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "nimblefix/advanced/live_initiator.h"
#include "nimblefix/advanced/runtime_application.h"
#include "nimblefix/base/status.h"
#include "nimblefix/runtime/application.h"
#include "nimblefix/runtime/detail/typed_runtime_application.h"
#include "nimblefix/runtime/profile_binding.h"
#include "nimblefix/runtime/session.h"

namespace nimble::runtime {

template<class Profile, class ApplicationType = typename Profile::Application>
class Initiator
{
public:
  struct Options
  {
    std::shared_ptr<ApplicationType> application;
    std::chrono::milliseconds poll_timeout{ kDefaultRuntimePollTimeout };
    std::chrono::milliseconds io_timeout{ kDefaultRuntimeIoTimeout };
    std::size_t command_queue_capacity{ kDefaultQueueEventCapacity };
  };

  Initiator(Engine* engine, ProfileBinding<Profile>* binding, Options options)
    : adapter_(
        std::make_shared<detail::TypedRuntimeApplication<Profile, ApplicationType>>(binding, options.application))
    , runtime_(engine,
               LiveInitiator::Options{
                 .poll_timeout = options.poll_timeout,
                 .io_timeout = options.io_timeout,
                 .application = adapter_,
                 .managed_queue_runner = std::nullopt,
                 .command_queue_capacity = options.command_queue_capacity,
               })
  {
  }

  auto OpenSession(std::uint64_t session_id, std::string host, std::uint16_t port) -> base::Status
  {
    return runtime_.OpenSession(session_id, std::move(host), port);
  }

  auto OpenSessionAsync(std::uint64_t session_id, std::string host, std::uint16_t port) -> base::Status
  {
    return runtime_.OpenSessionAsync(session_id, std::move(host), port);
  }

  auto Run(std::size_t max_completed_sessions = 0,
           std::chrono::milliseconds idle_timeout = std::chrono::milliseconds{ 0 }) -> base::Status
  {
    return runtime_.Run(max_completed_sessions, idle_timeout);
  }

  auto Stop() -> void { runtime_.Stop(); }

  auto RequestLogout(std::uint64_t session_id, std::string text = {}) -> base::Status
  {
    return runtime_.RequestLogout(session_id, std::move(text));
  }

  [[nodiscard]] auto active_connection_count() const -> std::size_t { return runtime_.active_connection_count(); }
  [[nodiscard]] auto completed_session_count() const -> std::size_t { return runtime_.completed_session_count(); }
  [[nodiscard]] auto pending_reconnect_count() const -> std::size_t { return runtime_.pending_reconnect_count(); }

private:
  std::shared_ptr<ApplicationCallbacks> adapter_;
  LiveInitiator runtime_;
};

} // namespace nimble::runtime
