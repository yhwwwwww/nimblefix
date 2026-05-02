#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "nimblefix/runtime/dynamic_config.h"
#include "nimblefix/runtime/engine.h"
#include "nimblefix/session/validation_callback.h"

namespace {

constexpr std::uint64_t kFix44ProfileId = 4400U;

class DynamicConfigValidationCallback final : public nimble::session::ValidationCallback
{};

auto
Fix44ArtifactPath() -> std::filesystem::path
{
  return std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.nfa";
}

auto
MakeCounterparty(std::uint64_t session_id, std::string name) -> nimble::runtime::CounterpartyConfig
{
  const auto id_text = std::to_string(session_id);
  return nimble::runtime::CounterpartyConfigBuilder::Initiator(
           std::move(name),
           session_id,
           nimble::session::SessionKey::ForInitiator("BUY" + id_text, "SELL" + id_text),
           kFix44ProfileId)
    .build();
}

auto
MakeEngineConfig(std::initializer_list<nimble::runtime::CounterpartyConfig> counterparties)
  -> nimble::runtime::EngineConfig
{
  nimble::runtime::EngineConfig config;
  config.worker_count = 1U;
  config.enable_metrics = true;
  config.profile_artifacts.push_back(Fix44ArtifactPath());
  config.counterparties.insert(config.counterparties.end(), counterparties.begin(), counterparties.end());
  return config;
}

auto
HasChange(const nimble::runtime::ConfigDelta& delta,
          nimble::runtime::ConfigChangeKind kind,
          std::uint64_t session_id = 0U,
          std::string_view name = {}) -> bool
{
  return std::any_of(delta.changes.begin(), delta.changes.end(), [&](const auto& change) {
    if (change.kind != kind || change.session_id != session_id) {
      return false;
    }
    return name.empty() || change.name == name;
  });
}

auto
HasApplied(const nimble::runtime::ApplyConfigResult& result,
           nimble::runtime::ConfigChangeKind kind,
           std::uint64_t session_id = 0U) -> bool
{
  return std::any_of(result.applied.begin(), result.applied.end(), [&](const auto& change) {
    return change.kind == kind && change.session_id == session_id;
  });
}

auto
HasSkipped(const nimble::runtime::ApplyConfigResult& result,
           nimble::runtime::ConfigChangeKind kind,
           std::string_view name) -> bool
{
  return std::any_of(result.skipped.begin(), result.skipped.end(), [&](const auto& change) {
    return change.kind == kind && change.name == name;
  });
}

auto
RequireFix44Artifact() -> void
{
  if (!std::filesystem::exists(Fix44ArtifactPath())) {
    SKIP("FIX44 artifact not available at: " << Fix44ArtifactPath().string());
  }
}

} // namespace

TEST_CASE("compute config delta detects added counterparty", "[dynamic-config]")
{
  auto current = MakeEngineConfig({ MakeCounterparty(1U, "venue-a") });
  auto proposed = current;
  proposed.counterparties.push_back(MakeCounterparty(2U, "venue-b"));

  const auto delta = nimble::runtime::ComputeConfigDelta(current, proposed);
  REQUIRE(HasChange(delta, nimble::runtime::ConfigChangeKind::kAddCounterparty, 2U));
}

TEST_CASE("compute config delta detects removed counterparty", "[dynamic-config]")
{
  auto current = MakeEngineConfig({ MakeCounterparty(1U, "venue-a"), MakeCounterparty(2U, "venue-b") });
  auto proposed = MakeEngineConfig({ MakeCounterparty(1U, "venue-a") });

  const auto delta = nimble::runtime::ComputeConfigDelta(current, proposed);
  REQUIRE(HasChange(delta, nimble::runtime::ConfigChangeKind::kRemoveCounterparty, 2U));
}

TEST_CASE("compute config delta detects modified counterparty", "[dynamic-config]")
{
  auto current = MakeEngineConfig({ MakeCounterparty(1U, "venue-a") });
  auto proposed = current;
  proposed.counterparties.front().name = "venue-a-renamed";

  const auto delta = nimble::runtime::ComputeConfigDelta(current, proposed);
  REQUIRE(HasChange(delta, nimble::runtime::ConfigChangeKind::kModifyCounterparty, 1U));
}

TEST_CASE("compute config delta detects worker_count requires restart", "[dynamic-config]")
{
  auto current = MakeEngineConfig({ MakeCounterparty(1U, "venue-a") });
  auto proposed = current;
  proposed.worker_count = 2U;

  const auto delta = nimble::runtime::ComputeConfigDelta(current, proposed);
  REQUIRE(delta.requires_restart);
  REQUIRE(HasChange(delta, nimble::runtime::ConfigChangeKind::kEngineFieldChanged, 0U, "worker_count"));
}

TEST_CASE("compute config delta empty when configs identical", "[dynamic-config]")
{
  auto current = MakeEngineConfig({ MakeCounterparty(1U, "venue-a") });

  const auto delta = nimble::runtime::ComputeConfigDelta(current, current);
  REQUIRE(delta.empty());
  REQUIRE_FALSE(delta.requires_restart);
}

TEST_CASE("compute config delta detects listener changes", "[dynamic-config]")
{
  auto current = MakeEngineConfig({});
  current.listeners.push_back(nimble::runtime::ListenerConfig{ .name = "main", .host = "127.0.0.1", .port = 9000U });
  current.listeners.push_back(nimble::runtime::ListenerConfig{ .name = "old", .host = "127.0.0.1", .port = 9001U });

  auto proposed = current;
  proposed.listeners.front().port = 9002U;
  proposed.listeners.erase(proposed.listeners.begin() + 1);
  proposed.listeners.push_back(nimble::runtime::ListenerConfig{ .name = "new", .host = "127.0.0.1", .port = 9003U });

  const auto delta = nimble::runtime::ComputeConfigDelta(current, proposed);
  REQUIRE(HasChange(delta, nimble::runtime::ConfigChangeKind::kModifyListener, 0U, "main"));
  REQUIRE(HasChange(delta, nimble::runtime::ConfigChangeKind::kRemoveListener, 0U, "old"));
  REQUIRE(HasChange(delta, nimble::runtime::ConfigChangeKind::kAddListener, 0U, "new"));
}

TEST_CASE("engine apply config adds counterparty", "[dynamic-config]")
{
  RequireFix44Artifact();
  auto current = MakeEngineConfig({ MakeCounterparty(1U, "venue-a") });
  nimble::runtime::Engine engine;
  REQUIRE(engine.Boot(current).ok());

  auto proposed = current;
  proposed.counterparties.push_back(MakeCounterparty(2U, "venue-b"));
  auto applied = engine.ApplyConfig(proposed);

  REQUIRE(applied.ok());
  REQUIRE(HasApplied(applied.value(), nimble::runtime::ConfigChangeKind::kAddCounterparty, 2U));
  REQUIRE(engine.FindCounterpartyConfig(2U) != nullptr);
  REQUIRE(engine.runtime()->FindSessionShard(2U) != nullptr);
}

TEST_CASE("engine apply config removes counterparty", "[dynamic-config]")
{
  RequireFix44Artifact();
  auto current = MakeEngineConfig({ MakeCounterparty(1U, "venue-a"), MakeCounterparty(2U, "venue-b") });
  nimble::runtime::Engine engine;
  REQUIRE(engine.Boot(current).ok());

  auto proposed = MakeEngineConfig({ MakeCounterparty(1U, "venue-a") });
  auto applied = engine.ApplyConfig(proposed);

  REQUIRE(applied.ok());
  REQUIRE(HasApplied(applied.value(), nimble::runtime::ConfigChangeKind::kRemoveCounterparty, 2U));
  REQUIRE(engine.FindCounterpartyConfig(2U) == nullptr);
  REQUIRE(engine.runtime()->FindSessionShard(2U) == nullptr);
}

TEST_CASE("engine apply config modifies counterparty", "[dynamic-config]")
{
  RequireFix44Artifact();
  auto current = MakeEngineConfig({ MakeCounterparty(1U, "venue-a") });
  nimble::runtime::Engine engine;
  REQUIRE(engine.Boot(current).ok());

  auto proposed = current;
  proposed.counterparties.front().name = "venue-a-renamed";
  auto applied = engine.ApplyConfig(proposed);

  REQUIRE(applied.ok());
  REQUIRE(HasApplied(applied.value(), nimble::runtime::ConfigChangeKind::kModifyCounterparty, 1U));
  REQUIRE(engine.FindCounterpartyConfig(1U) != nullptr);
  REQUIRE(engine.FindCounterpartyConfig(1U)->name == "venue-a-renamed");
}

TEST_CASE("compute config delta detects timestamp resolution change", "[dynamic-config]")
{
  auto current = MakeEngineConfig({ MakeCounterparty(1U, "venue-a") });
  auto proposed = current;
  proposed.counterparties.front().timestamp_resolution = nimble::codec::TimestampResolution::kMicroseconds;

  const auto delta = nimble::runtime::ComputeConfigDelta(current, proposed);
  REQUIRE(HasChange(delta, nimble::runtime::ConfigChangeKind::kModifyCounterparty, 1U));
  REQUIRE_FALSE(delta.requires_restart);
  const auto it = std::find_if(delta.changes.begin(), delta.changes.end(), [](const auto& change) {
    return change.kind == nimble::runtime::ConfigChangeKind::kModifyCounterparty && change.session_id == 1U;
  });
  REQUIRE(it != delta.changes.end());
  REQUIRE(it->description.find("timestamp_resolution") != std::string::npos);
}

TEST_CASE("compute config delta compares validation callback identity", "[dynamic-config]")
{
  auto current = MakeEngineConfig({ MakeCounterparty(1U, "venue-a") });
  auto proposed = current;
  proposed.counterparties.front().validation_callback = std::make_shared<DynamicConfigValidationCallback>();

  const auto delta = nimble::runtime::ComputeConfigDelta(current, proposed);
  REQUIRE(HasChange(delta, nimble::runtime::ConfigChangeKind::kModifyCounterparty, 1U));
  const auto it = std::find_if(delta.changes.begin(), delta.changes.end(), [](const auto& change) {
    return change.kind == nimble::runtime::ConfigChangeKind::kModifyCounterparty && change.session_id == 1U;
  });
  REQUIRE(it != delta.changes.end());
  REQUIRE(it->description.find("validation_callback") != std::string::npos);
}

TEST_CASE("engine apply config skips restart-required changes", "[dynamic-config]")
{
  RequireFix44Artifact();
  auto current = MakeEngineConfig({ MakeCounterparty(1U, "venue-a") });
  nimble::runtime::Engine engine;
  REQUIRE(engine.Boot(current).ok());

  auto proposed = current;
  proposed.worker_count = 2U;
  auto applied = engine.ApplyConfig(proposed);

  REQUIRE(applied.ok());
  REQUIRE_FALSE(applied.value().fully_applied());
  REQUIRE(HasSkipped(applied.value(), nimble::runtime::ConfigChangeKind::kEngineFieldChanged, "worker_count"));
  REQUIRE(engine.config() != nullptr);
  REQUIRE(engine.config()->worker_count == 1U);
  REQUIRE(engine.runtime()->worker_count() == 1U);
}

TEST_CASE("engine apply config rejects invalid new config", "[dynamic-config]")
{
  RequireFix44Artifact();
  auto current = MakeEngineConfig({ MakeCounterparty(1U, "venue-a") });
  nimble::runtime::Engine engine;
  REQUIRE(engine.Boot(current).ok());

  auto proposed = current;
  proposed.counterparties.front().name.clear();
  auto applied = engine.ApplyConfig(proposed);

  REQUIRE_FALSE(applied.ok());
}

TEST_CASE("engine apply config updates trace configuration", "[dynamic-config]")
{
  RequireFix44Artifact();
  auto current = MakeEngineConfig({ MakeCounterparty(1U, "venue-a") });
  current.trace_mode = nimble::runtime::TraceMode::kDisabled;
  current.trace_capacity = 0U;
  nimble::runtime::Engine engine;
  REQUIRE(engine.Boot(current).ok());
  REQUIRE_FALSE(engine.trace().enabled());

  auto proposed = current;
  proposed.trace_mode = nimble::runtime::TraceMode::kRing;
  proposed.trace_capacity = 4U;
  auto applied = engine.ApplyConfig(proposed);

  REQUIRE(applied.ok());
  REQUIRE(engine.trace().enabled());
  REQUIRE(engine.trace().capacity() == 4U);
}

TEST_CASE("engine apply config before boot fails", "[dynamic-config]")
{
  auto config = MakeEngineConfig({ MakeCounterparty(1U, "venue-a") });
  nimble::runtime::Engine engine;

  auto applied = engine.ApplyConfig(config);
  REQUIRE_FALSE(applied.ok());
}

TEST_CASE("config delta detects counterparty fields requiring remove add", "[dynamic-config]")
{
  auto current = MakeEngineConfig({ MakeCounterparty(1U, "venue-a") });
  auto proposed = current;
  proposed.counterparties.front().store_mode = nimble::runtime::StoreMode::kMmap;
  proposed.counterparties.front().store_path = "dynamic-config-store";

  const auto delta = nimble::runtime::ComputeConfigDelta(current, proposed);
  REQUIRE_FALSE(delta.requires_restart);
  const auto it = std::find_if(delta.changes.begin(), delta.changes.end(), [](const auto& change) {
    return change.kind == nimble::runtime::ConfigChangeKind::kModifyCounterparty && change.session_id == 1U;
  });
  REQUIRE(it != delta.changes.end());
  REQUIRE(it->description.find("requires remove+add") != std::string::npos);
  REQUIRE(it->description.find("store_mode") != std::string::npos);
}

TEST_CASE("config delta detects warmup message count changes", "[dynamic-config]")
{
  auto current = MakeEngineConfig({ MakeCounterparty(1U, "venue-a") });
  auto proposed = current;
  proposed.counterparties.front().warmup_message_count = 2U;

  const auto delta = nimble::runtime::ComputeConfigDelta(current, proposed);
  REQUIRE_FALSE(delta.requires_restart);
  const auto it = std::find_if(delta.changes.begin(), delta.changes.end(), [](const auto& change) {
    return change.kind == nimble::runtime::ConfigChangeKind::kModifyCounterparty && change.session_id == 1U;
  });
  REQUIRE(it != delta.changes.end());
  REQUIRE(it->description.find("warmup_message_count") != std::string::npos);
}
