#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nimblefix/base/result.h"
#include "nimblefix/base/status.h"
#include "nimblefix/runtime/acceptor.h"
#include "nimblefix/runtime/config.h"
#include "nimblefix/runtime/engine.h"
#include "nimblefix/runtime/initiator.h"
#include "nimblefix/runtime/profile_binding.h"
#include "nimblefix/session/session_key.h"
#include "nimblefix/session/transport_profile.h"

namespace nimble::runtime {

inline constexpr std::uint64_t kSimpleDefaultSessionId = 1U;
inline constexpr std::uint32_t kSimpleDefaultWorkerCount = 1U;
inline constexpr std::uint32_t kSimpleDefaultHeartbeatIntervalSeconds = 30U;
inline constexpr std::uint32_t kSimpleDefaultAcceptorWorkerCount = 2U;

namespace detail {

[[nodiscard]] inline auto
SimpleTransportProfile(session::TransportVersion version) -> session::TransportSessionProfile
{
  switch (version) {
    case session::TransportVersion::kFix40:
      return session::TransportSessionProfile::Fix40();
    case session::TransportVersion::kFix41:
      return session::TransportSessionProfile::Fix41();
    case session::TransportVersion::kFix42:
      return session::TransportSessionProfile::Fix42();
    case session::TransportVersion::kFix43:
      return session::TransportSessionProfile::Fix43();
    case session::TransportVersion::kFixT11:
      return session::TransportSessionProfile::FixT11();
    case session::TransportVersion::kFix44:
    default:
      return session::TransportSessionProfile::Fix44();
  }
}

} // namespace detail

struct SimpleRuntimeOptions
{
  std::uint32_t worker_count{ kSimpleDefaultWorkerCount };
  std::chrono::milliseconds poll_timeout{ kDefaultRuntimePollTimeout };
  std::chrono::milliseconds io_timeout{ kDefaultRuntimeIoTimeout };
  std::size_t command_queue_capacity{ kDefaultQueueEventCapacity };
  std::optional<std::uint32_t> front_door_cpu;
  std::vector<std::uint32_t> worker_cpu_affinity;
};

struct SimpleCounterpartyOptions
{
  StoreMode store_mode{ StoreMode::kMemory };
  std::filesystem::path store_path;
  session::RecoveryMode recovery_mode{ session::RecoveryMode::kMemoryOnly };
  session::ValidationPolicy validation_policy{ session::ValidationPolicy::Strict() };
  SessionScheduleConfig session_schedule;
};

namespace detail {

inline auto
ApplySimpleCounterpartyOptions(const SimpleCounterpartyOptions& options, CounterpartyConfig* counterparty) -> void
{
  counterparty->store_mode = options.store_mode;
  counterparty->store_path = options.store_path;
  counterparty->recovery_mode = options.recovery_mode;
  counterparty->validation_policy = options.validation_policy;
  counterparty->session_schedule = options.session_schedule;
}

} // namespace detail

template<class Profile, class ApplicationType = typename Profile::Application>
struct SimpleInitiatorSettings
{
  std::filesystem::path profile_artifact;
  std::string name{ "initiator" };
  std::uint64_t session_id{ kSimpleDefaultSessionId };
  std::string sender_comp_id;
  std::string target_comp_id;
  std::string host;
  std::uint16_t port{ 0 };
  session::TransportVersion transport_version{ session::TransportVersion::kFix44 };
  std::string default_appl_ver_id;
  std::uint32_t heartbeat_interval_seconds{ kSimpleDefaultHeartbeatIntervalSeconds };
  bool reconnect{ true };
  std::uint32_t reconnect_initial_ms{ kDefaultReconnectInitialMs };
  std::uint32_t reconnect_max_ms{ kDefaultReconnectMaxMs };
  std::uint32_t reconnect_max_retries{ kUnlimitedReconnectRetries };
  TlsClientConfig tls_client;
  SimpleCounterpartyOptions counterparty;
  std::shared_ptr<ApplicationType> application;
  SimpleRuntimeOptions runtime;
};

template<class Profile, class ApplicationType = typename Profile::Application>
struct SimpleAcceptorSettings
{
  std::filesystem::path profile_artifact;
  std::string listener_name{ "main" };
  std::string listener_host{ "0.0.0.0" };
  std::uint16_t listener_port{ 0 };
  std::string name{ "acceptor" };
  std::uint64_t session_id{ kSimpleDefaultSessionId };
  std::string local_comp_id;
  std::string remote_comp_id;
  bool accept_unknown_sessions{ false };
  session::TransportVersion transport_version{ session::TransportVersion::kFix44 };
  std::string default_appl_ver_id;
  std::uint32_t heartbeat_interval_seconds{ kSimpleDefaultHeartbeatIntervalSeconds };
  TlsServerConfig tls_server;
  TransportSecurityRequirement transport_security{ TransportSecurityRequirement::kAny };
  SimpleCounterpartyOptions counterparty;
  std::shared_ptr<ApplicationType> application;
  SimpleRuntimeOptions runtime{ .worker_count = kSimpleDefaultAcceptorWorkerCount };
};

template<class Profile, class ApplicationType = typename Profile::Application>
class SimpleInitiator
{
public:
  using Settings = SimpleInitiatorSettings<Profile, ApplicationType>;

  SimpleInitiator() = default;
  SimpleInitiator(SimpleInitiator&&) noexcept = default;
  auto operator=(SimpleInitiator&&) noexcept -> SimpleInitiator& = default;

  SimpleInitiator(const SimpleInitiator&) = delete;
  auto operator=(const SimpleInitiator&) -> SimpleInitiator& = delete;

  [[nodiscard]] static auto Create(Settings settings) -> base::Result<SimpleInitiator>
  {
    if (settings.application == nullptr) {
      return base::Status::InvalidArgument("simple initiator requires an application instance");
    }
    if (settings.profile_artifact.empty()) {
      return base::Status::InvalidArgument("simple initiator requires profile_artifact");
    }
    if (settings.sender_comp_id.empty() || settings.target_comp_id.empty()) {
      return base::Status::InvalidArgument("simple initiator requires sender_comp_id and target_comp_id");
    }
    if (settings.host.empty() || settings.port == 0U) {
      return base::Status::InvalidArgument("simple initiator requires host and port");
    }

    EngineConfig config;
    config.worker_count = settings.runtime.worker_count;
    config.front_door_cpu = settings.runtime.front_door_cpu;
    config.worker_cpu_affinity = settings.runtime.worker_cpu_affinity;
    config.profile_artifacts.push_back(settings.profile_artifact);

    auto counterparty =
      CounterpartyConfigBuilder::Initiator(
        settings.name,
        settings.session_id,
        session::SessionKey::ForInitiator(settings.sender_comp_id,
                                          settings.target_comp_id,
                                          detail::SimpleTransportProfile(settings.transport_version).begin_string),
        Profile::kProfileId,
        settings.transport_version)
        .heartbeat_interval_seconds(settings.heartbeat_interval_seconds)
        .default_appl_ver_id(settings.default_appl_ver_id);
    if (settings.reconnect) {
      counterparty.reconnect(settings.reconnect_initial_ms, settings.reconnect_max_ms, settings.reconnect_max_retries);
    }
    counterparty.tls_client(settings.tls_client);
    auto counterparty_config = counterparty.build();
    detail::ApplySimpleCounterpartyOptions(settings.counterparty, &counterparty_config);
    config.counterparties.push_back(std::move(counterparty_config));

    SimpleInitiator result;
    result.engine_ = std::make_unique<Engine>();
    auto boot = result.engine_->Boot(config);
    if (!boot.ok()) {
      return boot;
    }

    auto binding = result.engine_->Bind<Profile>();
    if (!binding.ok()) {
      return binding.status();
    }
    result.binding_ = std::make_unique<ProfileBinding<Profile>>(std::move(binding).value());
    result.runtime_ = std::make_unique<Initiator<Profile, ApplicationType>>(
      result.engine_.get(),
      result.binding_.get(),
      typename Initiator<Profile, ApplicationType>::Options{
        .application = std::move(settings.application),
        .poll_timeout = settings.runtime.poll_timeout,
        .io_timeout = settings.runtime.io_timeout,
        .command_queue_capacity = settings.runtime.command_queue_capacity,
      });

    auto open = result.runtime_->OpenSession(settings.session_id, std::move(settings.host), settings.port);
    if (!open.ok()) {
      return open;
    }
    return std::move(result);
  }

  [[nodiscard]] auto engine() -> Engine& { return *engine_; }
  [[nodiscard]] auto engine() const -> const Engine& { return *engine_; }
  [[nodiscard]] auto binding() -> ProfileBinding<Profile>& { return *binding_; }
  [[nodiscard]] auto binding() const -> const ProfileBinding<Profile>& { return *binding_; }
  [[nodiscard]] auto runtime() -> Initiator<Profile, ApplicationType>& { return *runtime_; }
  [[nodiscard]] auto runtime() const -> const Initiator<Profile, ApplicationType>& { return *runtime_; }

  auto Run(std::size_t max_completed_sessions = 0,
           std::chrono::milliseconds idle_timeout = std::chrono::milliseconds{ 0 }) -> base::Status
  {
    return runtime_->Run(max_completed_sessions, idle_timeout);
  }

  auto Stop() -> void { runtime_->Stop(); }

private:
  std::unique_ptr<Engine> engine_;
  std::unique_ptr<ProfileBinding<Profile>> binding_;
  std::unique_ptr<Initiator<Profile, ApplicationType>> runtime_;
};

template<class Profile, class ApplicationType = typename Profile::Application>
class SimpleAcceptor
{
public:
  using Settings = SimpleAcceptorSettings<Profile, ApplicationType>;

  SimpleAcceptor() = default;
  SimpleAcceptor(SimpleAcceptor&&) noexcept = default;
  auto operator=(SimpleAcceptor&&) noexcept -> SimpleAcceptor& = default;

  SimpleAcceptor(const SimpleAcceptor&) = delete;
  auto operator=(const SimpleAcceptor&) -> SimpleAcceptor& = delete;

  [[nodiscard]] static auto Create(Settings settings) -> base::Result<SimpleAcceptor>
  {
    if (settings.application == nullptr) {
      return base::Status::InvalidArgument("simple acceptor requires an application instance");
    }
    if (settings.profile_artifact.empty()) {
      return base::Status::InvalidArgument("simple acceptor requires profile_artifact");
    }
    if (settings.listener_name.empty()) {
      return base::Status::InvalidArgument("simple acceptor requires listener_name");
    }
    if (!settings.accept_unknown_sessions && (settings.local_comp_id.empty() || settings.remote_comp_id.empty())) {
      return base::Status::InvalidArgument("static simple acceptor requires local_comp_id and remote_comp_id");
    }

    EngineConfig config;
    config.worker_count = settings.runtime.worker_count;
    config.front_door_cpu = settings.runtime.front_door_cpu;
    config.worker_cpu_affinity = settings.runtime.worker_cpu_affinity;
    config.profile_artifacts.push_back(settings.profile_artifact);
    config.listeners.push_back(ListenerConfig{
      .name = settings.listener_name,
      .host = settings.listener_host,
      .port = settings.listener_port,
      .tls_server = settings.tls_server,
    });
    config.accept_unknown_sessions = settings.accept_unknown_sessions;

    const auto transport_profile = detail::SimpleTransportProfile(settings.transport_version);
    if (!settings.accept_unknown_sessions) {
      auto counterparty = CounterpartyConfigBuilder::Acceptor(
                            settings.name,
                            settings.session_id,
                            session::SessionKey::ForAcceptor(
                              settings.local_comp_id, settings.remote_comp_id, transport_profile.begin_string),
                            Profile::kProfileId,
                            settings.transport_version)
                            .heartbeat_interval_seconds(settings.heartbeat_interval_seconds)
                            .default_appl_ver_id(settings.default_appl_ver_id)
                            .acceptor_transport_security(settings.transport_security)
                            .build();
      detail::ApplySimpleCounterpartyOptions(settings.counterparty, &counterparty);
      config.counterparties.push_back(std::move(counterparty));
    }

    SimpleAcceptor result;
    result.engine_ = std::make_unique<Engine>();
    auto boot = result.engine_->Boot(config);
    if (!boot.ok()) {
      return boot;
    }

    if (settings.accept_unknown_sessions) {
      const auto name = settings.name;
      const auto transport_version = settings.transport_version;
      const auto default_appl_ver_id = settings.default_appl_ver_id;
      const auto heartbeat_interval_seconds = settings.heartbeat_interval_seconds;
      const auto transport_security = settings.transport_security;
      const auto counterparty_options = settings.counterparty;
      result.engine_->SetSessionFactory(
        [name,
         transport_version,
         default_appl_ver_id,
         heartbeat_interval_seconds,
         transport_security,
         counterparty_options](const session::SessionKey& key) -> base::Result<CounterpartyConfig> {
          auto counterparty = CounterpartyConfigBuilder::Acceptor(name, 0U, key, Profile::kProfileId, transport_version)
                                .heartbeat_interval_seconds(heartbeat_interval_seconds)
                                .default_appl_ver_id(default_appl_ver_id)
                                .acceptor_transport_security(transport_security)
                                .build();
          detail::ApplySimpleCounterpartyOptions(counterparty_options, &counterparty);
          return counterparty;
        });
    }

    auto binding = result.engine_->Bind<Profile>();
    if (!binding.ok()) {
      return binding.status();
    }
    result.binding_ = std::make_unique<ProfileBinding<Profile>>(std::move(binding).value());
    result.runtime_ = std::make_unique<Acceptor<Profile, ApplicationType>>(
      result.engine_.get(),
      result.binding_.get(),
      typename Acceptor<Profile, ApplicationType>::Options{
        .application = std::move(settings.application),
        .poll_timeout = settings.runtime.poll_timeout,
        .io_timeout = settings.runtime.io_timeout,
        .command_queue_capacity = settings.runtime.command_queue_capacity,
      });

    auto open = result.runtime_->OpenListeners(settings.listener_name);
    if (!open.ok()) {
      return open;
    }
    return std::move(result);
  }

  [[nodiscard]] auto engine() -> Engine& { return *engine_; }
  [[nodiscard]] auto engine() const -> const Engine& { return *engine_; }
  [[nodiscard]] auto binding() -> ProfileBinding<Profile>& { return *binding_; }
  [[nodiscard]] auto binding() const -> const ProfileBinding<Profile>& { return *binding_; }
  [[nodiscard]] auto runtime() -> Acceptor<Profile, ApplicationType>& { return *runtime_; }
  [[nodiscard]] auto runtime() const -> const Acceptor<Profile, ApplicationType>& { return *runtime_; }

  auto Run(std::size_t max_completed_sessions = 0,
           std::chrono::milliseconds idle_timeout = std::chrono::milliseconds{ 0 }) -> base::Status
  {
    return runtime_->Run(max_completed_sessions, idle_timeout);
  }

  auto Stop() -> void { runtime_->Stop(); }

  [[nodiscard]] auto listener_port(std::string_view name) const -> base::Result<std::uint16_t>
  {
    return runtime_->listener_port(name);
  }

private:
  std::unique_ptr<Engine> engine_;
  std::unique_ptr<ProfileBinding<Profile>> binding_;
  std::unique_ptr<Acceptor<Profile, ApplicationType>> runtime_;
};

template<class Profile, class ApplicationType = typename Profile::Application>
[[nodiscard]] auto
CreateInitiator(SimpleInitiatorSettings<Profile, ApplicationType> settings)
  -> base::Result<SimpleInitiator<Profile, ApplicationType>>
{
  return SimpleInitiator<Profile, ApplicationType>::Create(std::move(settings));
}

template<class Profile, class ApplicationType = typename Profile::Application>
[[nodiscard]] auto
CreateAcceptor(SimpleAcceptorSettings<Profile, ApplicationType> settings)
  -> base::Result<SimpleAcceptor<Profile, ApplicationType>>
{
  return SimpleAcceptor<Profile, ApplicationType>::Create(std::move(settings));
}

} // namespace nimble::runtime
