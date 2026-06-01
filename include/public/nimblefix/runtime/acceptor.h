#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

#include "nimblefix/advanced/live_acceptor.h"
#include "nimblefix/advanced/runtime_application.h"
#include "nimblefix/base/result.h"
#include "nimblefix/base/status.h"
#include "nimblefix/runtime/application.h"
#include "nimblefix/runtime/detail/typed_runtime_application.h"
#include "nimblefix/runtime/profile_binding.h"
#include "nimblefix/runtime/session.h"

namespace nimble::runtime {

template<class Profile, class ApplicationType = typename Profile::Application>
class Acceptor
{
public:
  struct Options
  {
    std::shared_ptr<ApplicationType> application;
    std::chrono::milliseconds poll_timeout{ kDefaultRuntimePollTimeout };
    std::chrono::milliseconds io_timeout{ kDefaultRuntimeIoTimeout };
    std::size_t command_queue_capacity{ kDefaultQueueEventCapacity };
  };

  Acceptor(Engine* engine, ProfileBinding<Profile>* binding, Options options)
    : adapter_(
        std::make_shared<detail::TypedRuntimeApplication<Profile, ApplicationType>>(binding, options.application))
    , runtime_(engine,
               LiveAcceptor::Options{
                 .poll_timeout = options.poll_timeout,
                 .io_timeout = options.io_timeout,
                 .application = adapter_,
                 .managed_queue_runner = std::nullopt,
                 .command_queue_capacity = options.command_queue_capacity,
               })
  {
  }

  auto OpenListeners(std::string_view listener_name = {}) -> base::Status
  {
    return runtime_.OpenListeners(listener_name);
  }

  auto Run(std::size_t max_completed_sessions = 0,
           std::chrono::milliseconds idle_timeout = std::chrono::milliseconds{ 0 }) -> base::Status
  {
    return runtime_.Run(max_completed_sessions, idle_timeout);
  }

  auto Stop() -> void { runtime_.Stop(); }

  [[nodiscard]] auto listener_port(std::string_view name) const -> base::Result<std::uint16_t>
  {
    return runtime_.listener_port(name);
  }

  [[nodiscard]] auto active_connection_count() const -> std::size_t { return runtime_.active_connection_count(); }
  [[nodiscard]] auto completed_session_count() const -> std::size_t { return runtime_.completed_session_count(); }

private:
  std::shared_ptr<ApplicationCallbacks> adapter_;
  LiveAcceptor runtime_;
};

} // namespace nimble::runtime
