#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>       // IWYU pragma: keep
#include <stop_token> // IWYU pragma: keep
#include <string>     // IWYU pragma: keep
#include <string_view>

#include "nimblefix/base/result.h"
#include "nimblefix/base/status.h"
#include "nimblefix/profile/normalized_dictionary.h" // IWYU pragma: keep
#include "nimblefix/runtime/application.h"
#include "nimblefix/runtime/config.h" // IWYU pragma: keep

namespace nimble::codec {

struct SessionHeaderView;

} // namespace nimble::codec

namespace nimble::runtime {

class Engine;
enum class TraceEventKind : std::uint32_t;

} // namespace nimble::runtime

namespace nimble::session {

struct EncodedFrame;
struct ProtocolEvent;
class ProtocolFrameCollection;

} // namespace nimble::session

namespace nimble::store {

class SessionStore;

} // namespace nimble::store

namespace nimble::runtime {

// Minimal acceptor bring-up:
//
//   auto app = std::make_shared<MyApp>();
//   EngineConfig config;
//   config.profile_artifacts.push_back("fix44.art");
//   config.listeners.push_back(
//     ListenerConfigBuilder::Named("main").bind("0.0.0.0", 9876).build());
//   config.counterparties.push_back(
//     CounterpartyConfigBuilder::Acceptor(
//       "sell-side",
//       2001U,
//       session::SessionKey{ .sender_comp_id = "SELL1", .target_comp_id = "BUY1" },
//       4400U,
//       session::TransportVersion::kFix44)
//       .store(StoreMode::kDurableBatch, "/var/lib/nimblefix/sell-side")
//       .build());
//
//   Engine engine;
//   auto status = engine.Boot(config);
//   if (!status.ok()) {
//     return status;
//   }
//
//   // Optional dynamic onboarding for unknown Logons:
//   // config.accept_unknown_sessions = true;
//   // engine.SetSessionFactory(...);
//
//   LiveAcceptor acceptor(&engine, LiveAcceptor::Options{ .application = app });
//   status = acceptor.OpenListeners("main");
//   if (!status.ok()) {
//     return status;
//   }
//   return acceptor.Run();
//
// Static counterparties always match before SessionFactory. SessionFactory sees
// only the local-perspective SessionKey; listener name and local port are not
// part of the callback input.
// LiveAcceptor keeps socket accept on a front-door thread. In multi-worker
// mode, listener worker_hint only seeds initial accept-side placement; once
// Logon binds the session, the bound worker owns steady-state protocol and
// application work.
class LiveAcceptor
{
public:
  struct Options
  {
    std::chrono::milliseconds poll_timeout{ kDefaultRuntimePollTimeout };
    std::chrono::milliseconds io_timeout{ kDefaultRuntimeIoTimeout };
    std::shared_ptr<ApplicationCallbacks> application;
    std::optional<ManagedQueueApplicationRunnerOptions> managed_queue_runner;
    std::size_t command_queue_capacity{ kDefaultQueueEventCapacity };
  };

  explicit LiveAcceptor(Engine* engine);
  explicit LiveAcceptor(Engine* engine, Options options);
  ~LiveAcceptor();

  LiveAcceptor(const LiveAcceptor&) = delete;
  auto operator=(const LiveAcceptor&) -> LiveAcceptor& = delete;
  LiveAcceptor(LiveAcceptor&&) = delete;
  auto operator=(LiveAcceptor&&) -> LiveAcceptor& = delete;

  auto OpenListeners(std::string_view listener_name = {}) -> base::Status;
  auto Run(std::size_t max_completed_sessions = 0,
           std::chrono::milliseconds idle_timeout = std::chrono::milliseconds{ 0 }) -> base::Status;
  auto Stop() -> void;

  [[nodiscard]] auto listener_port(std::string_view name) const -> base::Result<std::uint16_t>;
  [[nodiscard]] auto active_connection_count() const -> std::size_t;
  [[nodiscard]] auto completed_session_count() const -> std::size_t;

private:
#include "nimblefix/runtime/detail/live_acceptor_private.inc"

  std::unique_ptr<Impl> impl_;
};

} // namespace nimble::runtime
