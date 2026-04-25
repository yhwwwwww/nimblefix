#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>       // IWYU pragma: keep
#include <stop_token> // IWYU pragma: keep
#include <string>
#include <string_view> // IWYU pragma: keep

#include "nimblefix/base/result.h" // IWYU pragma: keep
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

// Minimal initiator bring-up:
//
//   auto app = std::make_shared<MyApp>();
//   EngineConfig config;
//   config.profile_artifacts.push_back("fix44.nfa");
//   config.counterparties.push_back(
//     CounterpartyConfigBuilder::Initiator(
//       "buy-side",
//       1001U,
//       session::SessionKey{ .sender_comp_id = "BUY1", .target_comp_id = "SELL1" },
//       4400U,
//       session::TransportVersion::kFix44)
//       .reconnect()
//       .build());
//
//   Engine engine;
//   auto status = engine.Boot(config);
//   if (!status.ok()) {
//     return status;
//   }
//
//   LiveInitiator initiator(&engine, LiveInitiator::Options{ .application = app });
//   status = initiator.OpenSession(1001U, "127.0.0.1", 9876);
//   if (!status.ok()) {
//     return status;
//   }
//   return initiator.Run();
//
// Send the first business message after OnSessionEvent(kActive). OpenSession()
// may block until the TCP dial succeeds or times out; use OpenSessionAsync()
// when the caller cannot afford that blocking path.
class LiveInitiator
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

  explicit LiveInitiator(Engine* engine);
  explicit LiveInitiator(Engine* engine, Options options);
  ~LiveInitiator();

  LiveInitiator(const LiveInitiator&) = delete;
  auto operator=(const LiveInitiator&) -> LiveInitiator& = delete;
  LiveInitiator(LiveInitiator&&) = delete;
  auto operator=(LiveInitiator&&) -> LiveInitiator& = delete;

  auto OpenSession(std::uint64_t session_id, std::string host, std::uint16_t port) -> base::Status;
  auto OpenSessionAsync(std::uint64_t session_id, std::string host, std::uint16_t port) -> base::Status;
  auto Run(std::size_t max_completed_sessions = 0,
           std::chrono::milliseconds idle_timeout = std::chrono::milliseconds{ 0 }) -> base::Status;
  auto Stop() -> void;
  auto RequestLogout(std::uint64_t session_id, std::string text = {}) -> base::Status;

  [[nodiscard]] auto active_connection_count() const -> std::size_t;
  [[nodiscard]] auto completed_session_count() const -> std::size_t;
  [[nodiscard]] auto pending_reconnect_count() const -> std::size_t;

private:
#include "nimblefix/runtime/detail/live_initiator_private.inc"

  std::unique_ptr<Impl> impl_;
};

} // namespace nimble::runtime
