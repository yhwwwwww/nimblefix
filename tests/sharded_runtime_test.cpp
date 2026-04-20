#include <catch2/catch_test_macros.hpp>

#include "nimblefix/runtime/sharded_runtime.h"

#include "test_support.h"

TEST_CASE("sharded-runtime", "[sharded-runtime]")
{
  nimble::runtime::ShardedRuntime runtime(4U);
  REQUIRE(runtime.worker_count() == 4U);

  nimble::session::SessionConfig first_config;
  first_config.session_id = 100U;
  first_config.profile_id = 1U;
  first_config.key.begin_string = "FIX.4.4";
  first_config.key.sender_comp_id = "A";
  first_config.key.target_comp_id = "B";

  nimble::session::SessionConfig second_config;
  second_config.session_id = 200U;
  second_config.profile_id = 2U;
  second_config.key.begin_string = "FIX.4.4";
  second_config.key.sender_comp_id = "C";
  second_config.key.target_comp_id = "D";

  nimble::session::SessionCore first(first_config);
  nimble::session::SessionCore second(second_config);

  REQUIRE(runtime.RegisterSession(first).ok());
  REQUIRE(runtime.RegisterSession(second).ok());

  const auto* first_shard = runtime.FindSessionShard(first.session_id());
  const auto* second_shard = runtime.FindSessionShard(second.session_id());
  REQUIRE(first_shard != nullptr);
  REQUIRE(second_shard != nullptr);

  nimble::runtime::PendingConnection pending;
  pending.connection_id = 300U;
  pending.profile_id = 1U;
  pending.session_key = first.key();

  REQUIRE(runtime.RegisterPendingConnection(pending).ok());
  const auto* pending_shard = runtime.FindPendingConnectionShard(pending.connection_id);
  REQUIRE(pending_shard != nullptr);
  REQUIRE(pending_shard->worker_id == runtime.RouteSession(first.key()));
  REQUIRE(runtime.pending_connection_count() == 1U);
  REQUIRE(runtime.UnregisterPendingConnection(pending.connection_id).ok());
  REQUIRE(runtime.pending_connection_count() == 0U);
  REQUIRE(runtime.FindPendingConnectionShard(pending.connection_id) == nullptr);
}
