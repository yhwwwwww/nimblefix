#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <unordered_set>
#include <vector>

#include "fastfix/profile/profile_loader.h"
#include "fastfix/runtime/application.h"
#include "fastfix/runtime/config.h"
#include "fastfix/runtime/config_io.h"
#include "fastfix/runtime/engine.h"
#include "fastfix/runtime/live_acceptor.h"

namespace {

struct Options
{
  std::filesystem::path artifact_path;
  std::filesystem::path config_path;
  std::string bind_host{ "127.0.0.1" };
  std::string listener_name;
  std::string sender;
  std::string target;
  std::string begin_string{ "FIX.4.4" };
  std::string default_appl_ver_id;
  fastfix::runtime::AppDispatchMode dispatch_mode{ fastfix::runtime::AppDispatchMode::kInline };
  fastfix::session::ValidationMode validation_mode{ fastfix::session::ValidationMode::kStrict };
  std::optional<std::uint32_t> front_door_cpu;
  std::vector<std::uint32_t> worker_cpu_affinity;
  std::optional<fastfix::runtime::QueueAppThreadingMode> queue_app_mode;
  std::vector<std::uint32_t> app_cpu_affinity;
  std::size_t max_sessions{ 1 };
  std::uint32_t idle_timeout_ms{ 30000 };
  std::uint16_t port{ 0 };
};

auto
PrintUsage() -> void
{
  std::cout << "usage: fastfix-acceptor (--artifact <profile.art> --bind <host> "
               "--port <port> --sender <sender> --target <target> [--begin-string "
               "<value>] [--default-appl-ver-id <value>] [--dispatch-mode "
               "inline|queue] [--validation-mode "
               "strict|compatible|permissive|raw-pass-through] [--front-door-cpu "
               "<id>] [--worker-cpus <csv>] [--queue-runner-mode "
               "co-scheduled|threaded] [--app-cpus <csv>] | --config <engine.ffcfg> "
               "[--listener <name>] [--front-door-cpu <id>] [--worker-cpus <csv>] "
               "[--queue-runner-mode co-scheduled|threaded] [--app-cpus <csv>]) "
               "[--max-sessions <count>] [--idle-timeout-ms <value>]\n";
}

auto
ResolveProjectPath(const std::filesystem::path& path) -> std::filesystem::path
{
  if (path.empty() || path.is_absolute()) {
    return path;
  }
  return std::filesystem::path(FASTFIX_PROJECT_DIR) / path;
}

auto
MakeDirectCounterparty(const Options& options, std::uint64_t profile_id) -> fastfix::runtime::CounterpartyConfig
{
  fastfix::runtime::CounterpartyConfig counterparty;
  counterparty.name = "direct";
  counterparty.session.session_id = 1U;
  counterparty.session.key = fastfix::session::SessionKey{ options.begin_string, options.sender, options.target };
  counterparty.session.profile_id = profile_id;
  counterparty.session.default_appl_ver_id = options.default_appl_ver_id;
  counterparty.session.heartbeat_interval_seconds = 5U;
  counterparty.session.is_initiator = false;
  counterparty.default_appl_ver_id = options.default_appl_ver_id;
  counterparty.dispatch_mode = options.dispatch_mode;
  counterparty.validation_policy = fastfix::session::MakeValidationPolicy(options.validation_mode);
  return counterparty;
}

auto
ParseDispatchMode(std::string_view token) -> std::optional<fastfix::runtime::AppDispatchMode>
{
  if (token == "inline") {
    return fastfix::runtime::AppDispatchMode::kInline;
  }
  if (token == "queue" || token == "queue-decoupled") {
    return fastfix::runtime::AppDispatchMode::kQueueDecoupled;
  }
  return std::nullopt;
}

auto
ParseValidationMode(std::string_view token) -> std::optional<fastfix::session::ValidationMode>
{
  if (token == "strict") {
    return fastfix::session::ValidationMode::kStrict;
  }
  if (token == "compatible") {
    return fastfix::session::ValidationMode::kCompatible;
  }
  if (token == "permissive") {
    return fastfix::session::ValidationMode::kPermissive;
  }
  if (token == "raw-pass-through") {
    return fastfix::session::ValidationMode::kRawPassThrough;
  }
  return std::nullopt;
}

auto
ParseCpuList(std::string_view token) -> std::optional<std::vector<std::uint32_t>>
{
  std::vector<std::uint32_t> cpu_ids;
  if (token.empty()) {
    return cpu_ids;
  }

  std::size_t begin = 0U;
  while (begin <= token.size()) {
    const auto end = token.find(',', begin);
    const auto part = token.substr(begin, end == std::string_view::npos ? token.size() - begin : end - begin);
    if (part.empty()) {
      return std::nullopt;
    }

    try {
      cpu_ids.push_back(static_cast<std::uint32_t>(std::stoul(std::string(part))));
    } catch (...) {
      return std::nullopt;
    }

    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1U;
  }

  return cpu_ids;
}

auto
ParseQueueAppMode(std::string_view token) -> std::optional<fastfix::runtime::QueueAppThreadingMode>
{
  if (token == "co-scheduled" || token == "co_scheduled") {
    return fastfix::runtime::QueueAppThreadingMode::kCoScheduled;
  }
  if (token == "threaded") {
    return fastfix::runtime::QueueAppThreadingMode::kThreaded;
  }
  return std::nullopt;
}

auto
ResolveManagedQueueRunnerMode(fastfix::runtime::QueueAppThreadingMode mode)
  -> fastfix::runtime::ManagedQueueApplicationRunnerMode
{
  return mode == fastfix::runtime::QueueAppThreadingMode::kThreaded
           ? fastfix::runtime::ManagedQueueApplicationRunnerMode::kThreaded
           : fastfix::runtime::ManagedQueueApplicationRunnerMode::kCoScheduled;
}

auto
CollectQueueSessions(const fastfix::runtime::EngineConfig& config) -> std::unordered_set<std::uint64_t>
{
  std::unordered_set<std::uint64_t> session_ids;
  for (const auto& counterparty : config.counterparties) {
    if (counterparty.dispatch_mode == fastfix::runtime::AppDispatchMode::kQueueDecoupled) {
      session_ids.emplace(counterparty.session.session_id);
    }
  }
  return session_ids;
}

class ToolApplication final
  : public fastfix::runtime::ApplicationCallbacks
  , public fastfix::runtime::QueueApplicationProvider
{
public:
  ToolApplication(std::unordered_set<std::uint64_t> queue_sessions, std::uint32_t worker_count)
    : queue_sessions_(std::move(queue_sessions))
    , queue_(worker_count)
  {
  }

  auto OnSessionEvent(const fastfix::runtime::RuntimeEvent& event) -> fastfix::base::Status override
  {
    if (queue_sessions_.contains(event.handle.session_id())) {
      return queue_.OnSessionEvent(event);
    }
    return fastfix::base::Status::Ok();
  }

  auto OnAdminMessage(const fastfix::runtime::RuntimeEvent& event) -> fastfix::base::Status override
  {
    if (queue_sessions_.contains(event.handle.session_id())) {
      return queue_.OnAdminMessage(event);
    }
    return fastfix::base::Status::Ok();
  }

  auto OnAppMessage(const fastfix::runtime::RuntimeEvent& event) -> fastfix::base::Status override
  {
    if (queue_sessions_.contains(event.handle.session_id())) {
      return queue_.OnAppMessage(event);
    }
    return event.handle.Send(event.message);
  }

  auto queue_application() -> fastfix::runtime::QueueApplication& override { return queue_; }

private:
  std::unordered_set<std::uint64_t> queue_sessions_;
  fastfix::runtime::QueueApplication queue_;
};

class EchoQueueHandler final : public fastfix::runtime::QueueApplicationEventHandler
{
public:
  auto OnRuntimeEvent(const fastfix::runtime::RuntimeEvent& event) -> fastfix::base::Status override
  {
    if (event.kind != fastfix::runtime::RuntimeEventKind::kApplicationMessage) {
      return fastfix::base::Status::Ok();
    }
    return event.handle.Send(event.message);
  }
};

} // namespace

int
main(int argc, char** argv)
{
  Options options;

  for (int index = 1; index < argc; ++index) {
    const std::string_view arg(argv[index]);
    if (arg == "--artifact" && index + 1 < argc) {
      options.artifact_path = argv[++index];
      continue;
    }
    if (arg == "--config" && index + 1 < argc) {
      options.config_path = argv[++index];
      continue;
    }
    if (arg == "--bind" && index + 1 < argc) {
      options.bind_host = argv[++index];
      continue;
    }
    if (arg == "--listener" && index + 1 < argc) {
      options.listener_name = argv[++index];
      continue;
    }
    if (arg == "--port" && index + 1 < argc) {
      options.port = static_cast<std::uint16_t>(std::stoul(argv[++index]));
      continue;
    }
    if (arg == "--sender" && index + 1 < argc) {
      options.sender = argv[++index];
      continue;
    }
    if (arg == "--target" && index + 1 < argc) {
      options.target = argv[++index];
      continue;
    }
    if (arg == "--begin-string" && index + 1 < argc) {
      options.begin_string = argv[++index];
      continue;
    }
    if (arg == "--default-appl-ver-id" && index + 1 < argc) {
      options.default_appl_ver_id = argv[++index];
      continue;
    }
    if (arg == "--dispatch-mode" && index + 1 < argc) {
      auto dispatch_mode = ParseDispatchMode(argv[++index]);
      if (!dispatch_mode.has_value()) {
        PrintUsage();
        return 1;
      }
      options.dispatch_mode = *dispatch_mode;
      continue;
    }
    if (arg == "--validation-mode" && index + 1 < argc) {
      auto validation_mode = ParseValidationMode(argv[++index]);
      if (!validation_mode.has_value()) {
        PrintUsage();
        return 1;
      }
      options.validation_mode = *validation_mode;
      continue;
    }
    if (arg == "--front-door-cpu" && index + 1 < argc) {
      options.front_door_cpu = static_cast<std::uint32_t>(std::stoul(argv[++index]));
      continue;
    }
    if (arg == "--worker-cpus" && index + 1 < argc) {
      auto cpu_ids = ParseCpuList(argv[++index]);
      if (!cpu_ids.has_value()) {
        PrintUsage();
        return 1;
      }
      options.worker_cpu_affinity = std::move(*cpu_ids);
      continue;
    }
    if (arg == "--queue-runner-mode" && index + 1 < argc) {
      auto parsed = ParseQueueAppMode(argv[++index]);
      if (!parsed.has_value()) {
        PrintUsage();
        return 1;
      }
      options.queue_app_mode = *parsed;
      continue;
    }
    if (arg == "--app-cpus" && index + 1 < argc) {
      auto cpu_ids = ParseCpuList(argv[++index]);
      if (!cpu_ids.has_value()) {
        PrintUsage();
        return 1;
      }
      options.app_cpu_affinity = std::move(*cpu_ids);
      continue;
    }
    if (arg == "--max-sessions" && index + 1 < argc) {
      options.max_sessions = static_cast<std::size_t>(std::stoull(argv[++index]));
      continue;
    }
    if (arg == "--idle-timeout-ms" && index + 1 < argc) {
      options.idle_timeout_ms = static_cast<std::uint32_t>(std::stoul(argv[++index]));
      continue;
    }
    PrintUsage();
    return 1;
  }

  fastfix::runtime::Engine engine;
  std::string listener_name;
  fastfix::runtime::EngineConfig resolved_config;

  if (!options.config_path.empty()) {
    options.config_path = ResolveProjectPath(options.config_path);
    auto config = fastfix::runtime::LoadEngineConfigFile(options.config_path);
    if (!config.ok()) {
      std::cerr << config.status().message() << '\n';
      return 1;
    }

    if (options.listener_name.empty()) {
      if (config.value().listeners.empty()) {
        std::cerr << "runtime config did not provide a listener for the acceptor\n";
        return 1;
      }
      listener_name = config.value().listeners.front().name;
    } else {
      listener_name = options.listener_name;
    }

    resolved_config = config.value();
    if (options.front_door_cpu.has_value()) {
      resolved_config.front_door_cpu = options.front_door_cpu;
    }
    if (!options.worker_cpu_affinity.empty()) {
      resolved_config.worker_cpu_affinity = options.worker_cpu_affinity;
    }
    if (options.queue_app_mode.has_value()) {
      resolved_config.queue_app_mode = *options.queue_app_mode;
    }
    if (!options.app_cpu_affinity.empty()) {
      resolved_config.app_cpu_affinity = options.app_cpu_affinity;
    }

    auto status = engine.Boot(resolved_config);
    if (!status.ok()) {
      std::cerr << status.message() << '\n';
      return 1;
    }
  } else {
    if (options.artifact_path.empty() || options.sender.empty() || options.target.empty() || options.port == 0U) {
      PrintUsage();
      return 1;
    }

    options.artifact_path = ResolveProjectPath(options.artifact_path);
    fastfix::runtime::EngineConfig config;
    config.worker_count = 1U;
    config.enable_metrics = true;
    config.front_door_cpu = options.front_door_cpu;
    config.worker_cpu_affinity = options.worker_cpu_affinity;
    if (options.queue_app_mode.has_value()) {
      config.queue_app_mode = *options.queue_app_mode;
    }
    config.app_cpu_affinity = options.app_cpu_affinity;
    config.profile_artifacts.push_back(options.artifact_path);
    config.listeners.push_back(fastfix::runtime::ListenerConfig{
      .name = "direct",
      .host = options.bind_host,
      .port = options.port,
      .worker_hint = 0U,
    });

    auto profile = fastfix::profile::LoadProfileArtifact(options.artifact_path);
    if (!profile.ok()) {
      std::cerr << profile.status().message() << '\n';
      return 1;
    }

    config.counterparties.push_back(MakeDirectCounterparty(options, profile.value().profile_id()));
    listener_name = "direct";
    resolved_config = config;

    auto status = engine.Boot(resolved_config);
    if (!status.ok()) {
      std::cerr << status.message() << '\n';
      return 1;
    }
  }

  auto queue_sessions = CollectQueueSessions(resolved_config);
  std::shared_ptr<fastfix::runtime::ApplicationCallbacks> application;
  std::shared_ptr<ToolApplication> tool_application;
  std::optional<fastfix::runtime::ManagedQueueApplicationRunnerOptions> managed_queue_runner;
  if (queue_sessions.empty()) {
    application = std::make_shared<fastfix::runtime::EchoApplication>();
  } else {
    tool_application = std::make_shared<ToolApplication>(std::move(queue_sessions), resolved_config.worker_count);
    managed_queue_runner.emplace();
    managed_queue_runner->mode = ResolveManagedQueueRunnerMode(resolved_config.queue_app_mode);
    managed_queue_runner->thread_options.cpu_affinity = resolved_config.app_cpu_affinity;
    auto& handlers = managed_queue_runner->handlers;
    handlers.reserve(tool_application->queue_application().worker_count());
    for (std::uint32_t worker_id = 0; worker_id < tool_application->queue_application().worker_count(); ++worker_id) {
      (void)worker_id;
      handlers.push_back(std::make_unique<EchoQueueHandler>());
    }
    application = tool_application;
  }

  fastfix::runtime::LiveAcceptor acceptor(&engine,
                                          fastfix::runtime::LiveAcceptor::Options{
                                            .poll_timeout = std::chrono::milliseconds(50),
                                            .io_timeout = std::chrono::seconds(5),
                                            .application = application,
                                            .managed_queue_runner = std::move(managed_queue_runner),
                                          });

  auto status = acceptor.OpenListeners(listener_name);
  if (!status.ok()) {
    std::cerr << status.message() << '\n';
    return 1;
  }

  auto port = acceptor.listener_port(listener_name);
  if (!port.ok()) {
    std::cerr << port.status().message() << '\n';
    return 1;
  }

  std::cout << "acceptor listening on " << listener_name << ':' << port.value() << '\n';

  status = acceptor.Run(options.max_sessions, std::chrono::milliseconds(options.idle_timeout_ms));
  if (!status.ok()) {
    std::cerr << status.message() << '\n';
    return 1;
  }

  std::cout << "acceptor completed " << acceptor.completed_session_count() << " session(s)\n";
  return 0;
}