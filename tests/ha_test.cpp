#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <vector>

#include "nimblefix/runtime/engine.h"
#include "nimblefix/runtime/ha.h"
#include "nimblefix/runtime/live_session_registry.h"

namespace {

constexpr std::uint64_t kFix44ProfileId = 4400U;

auto
Fix44ArtifactPath() -> std::filesystem::path
{
  return std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.nfa";
}

auto
MakeCounterparty(std::uint64_t session_id) -> nimble::runtime::CounterpartyConfig
{
  const auto suffix = std::to_string(session_id);
  return nimble::runtime::CounterpartyConfigBuilder::Initiator(
           "ha-session-" + suffix,
           session_id,
           nimble::session::SessionKey::ForInitiator("BUY" + suffix, "SELL" + suffix),
           kFix44ProfileId)
    .build();
}

auto
MakeEngineConfig(std::initializer_list<std::uint64_t> session_ids) -> nimble::runtime::EngineConfig
{
  if (!std::filesystem::exists(Fix44ArtifactPath())) {
    SKIP("FIX44 artifact not available at: " << Fix44ArtifactPath().string());
  }
  nimble::runtime::EngineConfig config;
  config.worker_count = 1U;
  config.profile_artifacts.push_back(Fix44ArtifactPath());
  for (const auto session_id : session_ids) {
    config.counterparties.push_back(MakeCounterparty(session_id));
  }
  return config;
}

} // namespace

TEST_CASE("ha controller starts in configured role", "[ha]")
{
  nimble::runtime::HaController controller;
  REQUIRE(controller.Configure(nimble::runtime::HaConfig{ .initial_role = nimble::runtime::HaRole::kStandby }).ok());
  REQUIRE(controller.Start().ok());

  CHECK(controller.running());
  CHECK(controller.role() == nimble::runtime::HaRole::kStandby);
}

TEST_CASE("ha controller promote to primary", "[ha]")
{
  nimble::runtime::HaController controller;
  REQUIRE(controller.Configure(nimble::runtime::HaConfig{ .initial_role = nimble::runtime::HaRole::kStandby }).ok());

  REQUIRE(controller.PromoteToPrimary().ok());
  CHECK(controller.role() == nimble::runtime::HaRole::kPrimary);
  CHECK(controller.generation() == 1U);
}

TEST_CASE("ha controller demote to standby", "[ha]")
{
  nimble::runtime::HaController controller;
  REQUIRE(controller.Configure(nimble::runtime::HaConfig{ .initial_role = nimble::runtime::HaRole::kPrimary }).ok());

  REQUIRE(controller.DemoteToStandby().ok());
  CHECK(controller.role() == nimble::runtime::HaRole::kStandby);
  CHECK(controller.generation() == 1U);
}

TEST_CASE("ha controller role change callback", "[ha]")
{
  std::vector<nimble::runtime::HaRole> roles;
  nimble::runtime::HaController controller;
  REQUIRE(controller
            .Configure(nimble::runtime::HaConfig{
              .initial_role = nimble::runtime::HaRole::kStandby,
              .on_role_change =
                [&](nimble::runtime::HaRole old_role, nimble::runtime::HaRole new_role) {
                  roles.push_back(old_role);
                  roles.push_back(new_role);
                },
            })
            .ok());

  REQUIRE(controller.PromoteToPrimary().ok());
  REQUIRE(roles.size() == 2U);
  CHECK(roles[0] == nimble::runtime::HaRole::kStandby);
  CHECK(roles[1] == nimble::runtime::HaRole::kPrimary);
}

TEST_CASE("ha controller peer heartbeat tracking", "[ha]")
{
  nimble::runtime::HaController controller;
  REQUIRE(controller.Configure(nimble::runtime::HaConfig{ .initial_role = nimble::runtime::HaRole::kPrimary }).ok());
  REQUIRE(controller.Start().ok());

  controller.RecordPeerHeartbeat(1'000U);
  controller.CheckHealth(1'500U);
  CHECK(controller.peer_state() == nimble::runtime::HaPeerState::kAlive);
}

TEST_CASE("ha controller health check detects dead peer", "[ha]")
{
  nimble::runtime::HaController controller;
  REQUIRE(controller
            .Configure(nimble::runtime::HaConfig{
              .initial_role = nimble::runtime::HaRole::kPrimary,
              .heartbeat_interval = std::chrono::milliseconds{ 1 },
              .suspect_threshold = 2U,
              .dead_threshold = 4U,
            })
            .ok());
  REQUIRE(controller.Start().ok());
  controller.RecordPeerHeartbeat(1'000'000U);

  controller.CheckHealth(6'000'000U);
  CHECK(controller.peer_state() == nimble::runtime::HaPeerState::kDead);
}

TEST_CASE("ha controller auto failover on dead peer", "[ha]")
{
  nimble::runtime::HaController controller;
  REQUIRE(controller
            .Configure(nimble::runtime::HaConfig{
              .initial_role = nimble::runtime::HaRole::kStandby,
              .heartbeat_interval = std::chrono::milliseconds{ 1 },
              .suspect_threshold = 1U,
              .dead_threshold = 2U,
              .auto_failover = true,
            })
            .ok());
  REQUIRE(controller.Start().ok());
  controller.RecordPeerHeartbeat(1'000'000U);

  controller.CheckHealth(4'000'000U);
  CHECK(controller.peer_state() == nimble::runtime::HaPeerState::kDead);
  CHECK(controller.role() == nimble::runtime::HaRole::kPrimary);
}

TEST_CASE("ha in-memory transport replicates and receives snapshots", "[ha]")
{
  nimble::runtime::InMemoryHaTransport transport;
  auto receiver = transport.receiver();
  CHECK_FALSE(receiver().ok());

  nimble::runtime::HaStateSnapshot snapshot;
  snapshot.snapshot_timestamp_ns = 10U;
  snapshot.generation = 2U;
  snapshot.sessions.push_back(nimble::runtime::SessionSequenceState{
    .session_id = 42U,
    .next_inbound_seq = 7U,
    .next_outbound_seq = 9U,
    .last_activity_ns = 11U,
  });

  auto replicator = transport.replicator();
  REQUIRE(replicator(snapshot).ok());
  auto received = receiver();
  REQUIRE(received.ok());
  CHECK(received.value().generation == 2U);
  REQUIRE(received.value().sessions.size() == 1U);
  CHECK(received.value().sessions[0].session_id == 42U);
  CHECK(received.value().sessions[0].next_inbound_seq == 7U);
  CHECK(received.value().sessions[0].next_outbound_seq == 9U);
}

TEST_CASE("live session registry loads all snapshots", "[ha]")
{
  nimble::runtime::LiveSessionRegistry registry;
  registry.UpdateSnapshot(nimble::session::SessionSnapshot{
    .session_id = 1U,
    .state = nimble::session::SessionState::kActive,
    .profile_id = kFix44ProfileId,
    .next_in_seq = 3U,
    .next_out_seq = 4U,
    .last_inbound_ns = 5U,
    .last_outbound_ns = 6U,
  });
  registry.UpdateSnapshot(nimble::session::SessionSnapshot{
    .session_id = 2U,
    .state = nimble::session::SessionState::kPendingLogon,
    .profile_id = kFix44ProfileId,
    .next_in_seq = 7U,
    .next_out_seq = 8U,
    .last_inbound_ns = 9U,
    .last_outbound_ns = 10U,
  });

  const auto snapshots = registry.LoadAllSnapshots();
  REQUIRE(snapshots.size() == 2U);
  CHECK((snapshots[0].session_id == 1U || snapshots[1].session_id == 1U));
  CHECK((snapshots[0].session_id == 2U || snapshots[1].session_id == 2U));
}

TEST_CASE("ha controller take snapshot", "[ha]")
{
  nimble::runtime::Engine engine;
  REQUIRE(engine.Boot(MakeEngineConfig({ 1U, 2U })).ok());
  engine.SetSessionSnapshotProvider([]() {
    return std::vector<nimble::session::SessionSnapshot>{
      nimble::session::SessionSnapshot{
        .session_id = 1U,
        .state = nimble::session::SessionState::kActive,
        .profile_id = kFix44ProfileId,
        .next_in_seq = 5U,
        .next_out_seq = 8U,
        .last_inbound_ns = 100U,
        .last_outbound_ns = 200U,
      },
    };
  });
  nimble::runtime::HaController controller;
  REQUIRE(controller.Configure(nimble::runtime::HaConfig{ .initial_role = nimble::runtime::HaRole::kPrimary }).ok());

  auto snapshot = controller.TakeSnapshot(engine);
  REQUIRE(snapshot.ok());
  CHECK(snapshot.value().sessions.size() == 2U);
  CHECK(snapshot.value().sessions[0].session_id == 1U);
  CHECK(snapshot.value().sessions[0].next_inbound_seq == 5U);
  CHECK(snapshot.value().sessions[0].next_outbound_seq == 8U);
  CHECK(snapshot.value().sessions[0].last_activity_ns == 200U);
  CHECK(snapshot.value().sessions[1].session_id == 2U);
  CHECK(snapshot.value().sessions[1].next_inbound_seq == 1U);
  CHECK(snapshot.value().sessions[1].next_outbound_seq == 1U);
}

TEST_CASE("ha controller snapshot provider round-trip", "[ha]")
{
  nimble::runtime::Engine primary;
  REQUIRE(primary.Boot(MakeEngineConfig({ 1U })).ok());
  primary.SetSessionSnapshotProvider([]() {
    return std::vector<nimble::session::SessionSnapshot>{
      nimble::session::SessionSnapshot{
        .session_id = 1U,
        .state = nimble::session::SessionState::kActive,
        .profile_id = kFix44ProfileId,
        .next_in_seq = 11U,
        .next_out_seq = 13U,
        .last_inbound_ns = 17U,
        .last_outbound_ns = 19U,
      },
    };
  });

  nimble::runtime::HaController primary_controller;
  REQUIRE(
    primary_controller.Configure(nimble::runtime::HaConfig{ .initial_role = nimble::runtime::HaRole::kPrimary }).ok());
  auto snapshot = primary_controller.TakeSnapshot(primary);
  REQUIRE(snapshot.ok());

  nimble::runtime::InMemoryHaTransport transport;
  REQUIRE(transport.replicator()(snapshot.value()).ok());
  auto received = transport.receiver()();
  REQUIRE(received.ok());

  nimble::runtime::Engine standby;
  REQUIRE(standby.Boot(MakeEngineConfig({ 1U })).ok());
  nimble::runtime::HaController standby_controller;
  REQUIRE(
    standby_controller.Configure(nimble::runtime::HaConfig{ .initial_role = nimble::runtime::HaRole::kStandby }).ok());
  REQUIRE(standby_controller.ApplySnapshot(standby, received.value()).ok());

  REQUIRE(standby_controller.last_applied_snapshot().has_value());
  REQUIRE(standby.last_applied_ha_snapshot() != nullptr);
  CHECK(standby_controller.last_applied_snapshot()->sessions[0].next_inbound_seq == 11U);
  CHECK(standby.last_applied_ha_snapshot()->sessions[0].next_outbound_seq == 13U);
}

TEST_CASE("ha controller apply snapshot", "[ha]")
{
  nimble::runtime::Engine engine;
  REQUIRE(engine.Boot(MakeEngineConfig({ 1U })).ok());
  nimble::runtime::HaController controller;
  REQUIRE(controller.Configure(nimble::runtime::HaConfig{ .initial_role = nimble::runtime::HaRole::kStandby }).ok());

  nimble::runtime::HaStateSnapshot snapshot;
  snapshot.snapshot_timestamp_ns = 1U;
  snapshot.generation = 1U;
  snapshot.sessions.push_back(nimble::runtime::SessionSequenceState{
    .session_id = 1U,
    .next_inbound_seq = 7U,
    .next_outbound_seq = 9U,
    .last_activity_ns = 11U,
  });

  const auto status = controller.ApplySnapshot(engine, snapshot);
  REQUIRE(status.ok());
  REQUIRE(controller.last_applied_snapshot().has_value());
  CHECK(controller.last_applied_snapshot()->generation == 1U);
  CHECK(controller.last_applied_snapshot()->sessions[0].next_inbound_seq == 7U);
  REQUIRE(engine.last_applied_ha_snapshot() != nullptr);
  CHECK(engine.last_applied_ha_snapshot()->sessions[0].next_outbound_seq == 9U);
}

TEST_CASE("ha controller cannot promote when already primary", "[ha]")
{
  nimble::runtime::HaController controller;
  REQUIRE(controller.Configure(nimble::runtime::HaConfig{ .initial_role = nimble::runtime::HaRole::kPrimary }).ok());

  const auto promoted = controller.PromoteToPrimary();
  CHECK_FALSE(promoted.ok());
}
