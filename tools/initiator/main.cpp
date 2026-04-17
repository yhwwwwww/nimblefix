#include <chrono>
#include <filesystem>
#include <iostream>

#include <atomic>
#include <memory>
#include <optional>
#include <vector>

#include "fastfix/codec/fix_tags.h"
#include "fastfix/message/message.h"
#include "fastfix/profile/profile_loader.h"
#include "fastfix/runtime/application.h"
#include "fastfix/runtime/engine.h"
#include "fastfix/runtime/live_initiator.h"

namespace {

using namespace fastfix::codec::tags;

auto
PrintUsage() -> void
{
  std::cout << "usage: fastfix-initiator --artifact <profile.art> --host "
               "<host> --port <port> --sender <sender> --target <target> "
               "[--begin-string <value>] [--default-appl-ver-id <value>] "
               "[--dispatch-mode inline|queue] [--validation-mode "
               "strict|compatible|permissive|raw-pass-through] [--worker-cpus "
               "<csv>] [--queue-runner-mode co-scheduled|threaded] [--app-cpus "
               "<csv>] [--reconnect] [--reconnect-initial-ms N] "
               "[--reconnect-max-ms N] [--reconnect-max-retries N]\n";
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
BuildInitiatorMessage() -> fastfix::message::Message
{
  fastfix::message::MessageBuilder builder("D");
  builder.set(kMsgType, "D");
  auto party = builder.add_group_entry(kNoPartyIDs);
  party.set(kPartyID, "INITIATOR-PARTY").set(kPartyIDSource, 'D').set(kPartyRole, static_cast<std::int64_t>(3));
  return std::move(builder).build();
}

auto
ValidateInitiatorEcho(fastfix::message::MessageView message) -> fastfix::base::Status
{
  const auto group = message.group(kNoPartyIDs);
  if (!group.has_value() || group->size() != 1U ||
      (*group)[0].get_string(kPartyID) != std::optional<std::string_view>{ "INITIATOR-PARTY" }) {
    return fastfix::base::Status::InvalidArgument("echoed application message missing repeating group");
  }

  std::cout << "received echo with PartyID=" << (*group)[0].get_string(kPartyID).value() << '\n';
  return fastfix::base::Status::Ok();
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

class EchoInitiatorApplication final : public fastfix::runtime::ApplicationCallbacks
{
public:
  EchoInitiatorApplication(fastfix::runtime::LiveInitiator* initiator, std::uint64_t session_id)
    : initiator_(initiator)
    , session_id_(session_id)
  {
  }

  auto BindInitiator(fastfix::runtime::LiveInitiator* initiator) -> void { initiator_ = initiator; }

  auto OnSessionEvent(const fastfix::runtime::RuntimeEvent& event) -> fastfix::base::Status override
  {
    if (event.handle.session_id() != session_id_ ||
        event.session_event != fastfix::runtime::SessionEventKind::kActive || sent_application_.exchange(true)) {
      return fastfix::base::Status::Ok();
    }
    return event.handle.Send(BuildInitiatorMessage());
  }

  auto OnAppMessage(const fastfix::runtime::RuntimeEvent& event) -> fastfix::base::Status override
  {
    if (event.handle.session_id() != session_id_) {
      return fastfix::base::Status::Ok();
    }

    auto status = ValidateInitiatorEcho(event.message.view());
    if (!status.ok()) {
      return status;
    }
    completed_.store(true);
    return initiator_->RequestLogout(session_id_);
  }

  [[nodiscard]] auto completed() const -> bool { return completed_.load(); }

private:
  fastfix::runtime::LiveInitiator* initiator_{ nullptr };
  std::uint64_t session_id_{ 0 };
  std::atomic<bool> sent_application_{ false };
  std::atomic<bool> completed_{ false };
};

struct QueueInitiatorState
{
  std::atomic<bool> sent_application{ false };
  std::atomic<bool> completed{ false };
};

class QueueInitiatorApplication final
  : public fastfix::runtime::ApplicationCallbacks
  , public fastfix::runtime::QueueApplicationProvider
{
public:
  QueueInitiatorApplication(std::uint64_t session_id,
                            std::uint32_t worker_count,
                            std::shared_ptr<QueueInitiatorState> state)
    : session_id_(session_id)
    , state_(std::move(state))
    , queue_(worker_count)
  {
  }

  auto OnSessionEvent(const fastfix::runtime::RuntimeEvent& event) -> fastfix::base::Status override
  {
    if (event.handle.session_id() != session_id_) {
      return fastfix::base::Status::Ok();
    }
    return queue_.OnSessionEvent(event);
  }

  auto OnAdminMessage(const fastfix::runtime::RuntimeEvent& event) -> fastfix::base::Status override
  {
    if (event.handle.session_id() != session_id_) {
      return fastfix::base::Status::Ok();
    }
    return queue_.OnAdminMessage(event);
  }

  auto OnAppMessage(const fastfix::runtime::RuntimeEvent& event) -> fastfix::base::Status override
  {
    if (event.handle.session_id() != session_id_) {
      return fastfix::base::Status::Ok();
    }
    return queue_.OnAppMessage(event);
  }

  [[nodiscard]] auto completed() const -> bool { return state_->completed.load(); }

  auto queue_application() -> fastfix::runtime::QueueApplication& override { return queue_; }

private:
  std::uint64_t session_id_{ 0 };
  std::shared_ptr<QueueInitiatorState> state_;
  fastfix::runtime::QueueApplication queue_;
};

class QueueInitiatorHandler final : public fastfix::runtime::QueueApplicationEventHandler
{
public:
  QueueInitiatorHandler(fastfix::runtime::LiveInitiator* initiator,
                        std::uint64_t session_id,
                        std::shared_ptr<QueueInitiatorState> state)
    : initiator_(initiator)
    , session_id_(session_id)
    , state_(std::move(state))
  {
  }

  auto BindInitiator(fastfix::runtime::LiveInitiator* initiator) -> void { initiator_ = initiator; }

  auto OnRuntimeEvent(const fastfix::runtime::RuntimeEvent& event) -> fastfix::base::Status override
  {
    if (event.handle.session_id() != session_id_) {
      return fastfix::base::Status::Ok();
    }

    if (event.kind == fastfix::runtime::RuntimeEventKind::kSession &&
        event.session_event == fastfix::runtime::SessionEventKind::kActive &&
        !state_->sent_application.exchange(true)) {
      return event.handle.Send(BuildInitiatorMessage());
    }

    if (event.kind != fastfix::runtime::RuntimeEventKind::kApplicationMessage) {
      return fastfix::base::Status::Ok();
    }

    auto status = ValidateInitiatorEcho(event.message.view());
    if (!status.ok()) {
      return status;
    }
    state_->completed.store(true);
    return initiator_->RequestLogout(session_id_);
  }

private:
  fastfix::runtime::LiveInitiator* initiator_{ nullptr };
  std::uint64_t session_id_{ 0 };
  std::shared_ptr<QueueInitiatorState> state_;
};

auto
ResolveProjectPath(const std::filesystem::path& path) -> std::filesystem::path
{
  if (path.empty() || path.is_absolute()) {
    return path;
  }
  return std::filesystem::path(FASTFIX_PROJECT_DIR) / path;
}

} // namespace

int
main(int argc, char** argv)
{
  std::filesystem::path artifact_path;
  std::string host{ "127.0.0.1" };
  std::string sender;
  std::string target;
  std::string begin_string{ "FIX.4.4" };
  std::string default_appl_ver_id;
  fastfix::runtime::AppDispatchMode dispatch_mode{ fastfix::runtime::AppDispatchMode::kInline };
  fastfix::session::ValidationMode validation_mode{ fastfix::session::ValidationMode::kStrict };
  std::vector<std::uint32_t> worker_cpu_affinity;
  fastfix::runtime::QueueAppThreadingMode queue_app_mode{ fastfix::runtime::QueueAppThreadingMode::kCoScheduled };
  std::vector<std::uint32_t> app_cpu_affinity;
  std::uint16_t port = 0;
  bool reconnect_enabled = false;
  std::uint32_t reconnect_initial_ms = 1000;
  std::uint32_t reconnect_max_ms = 30000;
  std::uint32_t reconnect_max_retries = 0;

  for (int index = 1; index < argc; ++index) {
    const std::string_view arg(argv[index]);
    if (arg == "--artifact" && index + 1 < argc) {
      artifact_path = argv[++index];
      continue;
    }
    if (arg == "--host" && index + 1 < argc) {
      host = argv[++index];
      continue;
    }
    if (arg == "--port" && index + 1 < argc) {
      port = static_cast<std::uint16_t>(std::stoul(argv[++index]));
      continue;
    }
    if (arg == "--sender" && index + 1 < argc) {
      sender = argv[++index];
      continue;
    }
    if (arg == "--target" && index + 1 < argc) {
      target = argv[++index];
      continue;
    }
    if (arg == "--begin-string" && index + 1 < argc) {
      begin_string = argv[++index];
      continue;
    }
    if (arg == "--default-appl-ver-id" && index + 1 < argc) {
      default_appl_ver_id = argv[++index];
      continue;
    }
    if (arg == "--dispatch-mode" && index + 1 < argc) {
      auto parsed = ParseDispatchMode(argv[++index]);
      if (!parsed.has_value()) {
        PrintUsage();
        return 1;
      }
      dispatch_mode = *parsed;
      continue;
    }
    if (arg == "--validation-mode" && index + 1 < argc) {
      auto parsed = ParseValidationMode(argv[++index]);
      if (!parsed.has_value()) {
        PrintUsage();
        return 1;
      }
      validation_mode = *parsed;
      continue;
    }
    if (arg == "--worker-cpus" && index + 1 < argc) {
      auto cpu_ids = ParseCpuList(argv[++index]);
      if (!cpu_ids.has_value()) {
        PrintUsage();
        return 1;
      }
      worker_cpu_affinity = std::move(*cpu_ids);
      continue;
    }
    if (arg == "--queue-runner-mode" && index + 1 < argc) {
      auto parsed = ParseQueueAppMode(argv[++index]);
      if (!parsed.has_value()) {
        PrintUsage();
        return 1;
      }
      queue_app_mode = *parsed;
      continue;
    }
    if (arg == "--app-cpus" && index + 1 < argc) {
      auto cpu_ids = ParseCpuList(argv[++index]);
      if (!cpu_ids.has_value()) {
        PrintUsage();
        return 1;
      }
      app_cpu_affinity = std::move(*cpu_ids);
      continue;
    }
    if (arg == "--reconnect") {
      reconnect_enabled = true;
      continue;
    }
    if (arg == "--reconnect-initial-ms" && index + 1 < argc) {
      reconnect_initial_ms = static_cast<std::uint32_t>(std::stoul(argv[++index]));
      continue;
    }
    if (arg == "--reconnect-max-ms" && index + 1 < argc) {
      reconnect_max_ms = static_cast<std::uint32_t>(std::stoul(argv[++index]));
      continue;
    }
    if (arg == "--reconnect-max-retries" && index + 1 < argc) {
      reconnect_max_retries = static_cast<std::uint32_t>(std::stoul(argv[++index]));
      continue;
    }
    PrintUsage();
    return 1;
  }

  if (artifact_path.empty() || sender.empty() || target.empty() || port == 0U) {
    PrintUsage();
    return 1;
  }

  artifact_path = ResolveProjectPath(artifact_path);

  auto profile = fastfix::profile::LoadProfileArtifact(artifact_path);
  if (!profile.ok()) {
    std::cerr << profile.status().message() << '\n';
    return 1;
  }

  fastfix::runtime::EngineConfig config;
  config.worker_count = 1U;
  config.enable_metrics = true;
  config.worker_cpu_affinity = worker_cpu_affinity;
  config.queue_app_mode = queue_app_mode;
  config.app_cpu_affinity = app_cpu_affinity;
  config.profile_artifacts.push_back(artifact_path);
  config.counterparties.push_back(fastfix::runtime::CounterpartyConfig{
    .name = "direct",
    .session =
      fastfix::session::SessionConfig{
        .session_id = 1U,
        .key = fastfix::session::SessionKey{ begin_string, sender, target },
        .profile_id = profile.value().profile_id(),
        .default_appl_ver_id = default_appl_ver_id,
        .heartbeat_interval_seconds = 5U,
        .is_initiator = true,
      },
    .store_path = {},
    .default_appl_ver_id = default_appl_ver_id,
    .store_mode = fastfix::runtime::StoreMode::kMemory,
    .recovery_mode = fastfix::session::RecoveryMode::kMemoryOnly,
    .dispatch_mode = dispatch_mode,
    .validation_policy = fastfix::session::MakeValidationPolicy(validation_mode),
    .reconnect_enabled = reconnect_enabled,
    .reconnect_initial_ms = reconnect_initial_ms,
    .reconnect_max_ms = reconnect_max_ms,
    .reconnect_max_retries = reconnect_max_retries,
  });

  fastfix::runtime::Engine engine;
  auto status = engine.Boot(config);
  if (!status.ok()) {
    std::cerr << status.message() << '\n';
    return 1;
  }

  std::shared_ptr<fastfix::runtime::ApplicationCallbacks> application;
  std::shared_ptr<EchoInitiatorApplication> inline_application;
  std::shared_ptr<QueueInitiatorApplication> queue_application;
  std::optional<fastfix::runtime::ManagedQueueApplicationRunnerOptions> managed_queue_runner;
  std::vector<QueueInitiatorHandler*> queue_handlers;
  if (dispatch_mode == fastfix::runtime::AppDispatchMode::kInline) {
    inline_application = std::make_shared<EchoInitiatorApplication>(nullptr, 1U);
    application = inline_application;
  } else {
    auto state = std::make_shared<QueueInitiatorState>();
    queue_application = std::make_shared<QueueInitiatorApplication>(1U, config.worker_count, state);
    managed_queue_runner.emplace();
    managed_queue_runner->mode = ResolveManagedQueueRunnerMode(config.queue_app_mode);
    managed_queue_runner->thread_options.cpu_affinity = config.app_cpu_affinity;
    auto& handlers = managed_queue_runner->handlers;
    handlers.reserve(queue_application->queue_application().worker_count());
    for (std::uint32_t worker_id = 0; worker_id < queue_application->queue_application().worker_count(); ++worker_id) {
      auto handler = std::make_unique<QueueInitiatorHandler>(nullptr, 1U, state);
      queue_handlers.push_back(handler.get());
      handlers.push_back(std::move(handler));
    }
    application = queue_application;
  }

  fastfix::runtime::LiveInitiator initiator(&engine,
                                            fastfix::runtime::LiveInitiator::Options{
                                              .poll_timeout = std::chrono::milliseconds(25),
                                              .io_timeout = std::chrono::seconds(5),
                                              .application = application,
                                              .managed_queue_runner = std::move(managed_queue_runner),
                                            });
  if (inline_application) {
    inline_application->BindInitiator(&initiator);
  }
  for (auto* handler : queue_handlers) {
    handler->BindInitiator(&initiator);
  }

  status = initiator.OpenSession(1U, host, port);
  if (!status.ok()) {
    std::cerr << status.message() << '\n';
    return 1;
  }

  status = initiator.Run(1U, std::chrono::seconds(15));
  if (!status.ok()) {
    std::cerr << status.message() << '\n';
    return 1;
  }
  const bool completed = inline_application ? inline_application->completed() : queue_application->completed();
  if (!completed) {
    std::cerr << "initiator did not receive the expected echo\n";
    return 1;
  }

  std::cout << "initiator session completed\n";
  return 0;
}