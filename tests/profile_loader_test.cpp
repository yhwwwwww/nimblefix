#include <catch2/catch_test_macros.hpp>

#include <filesystem>

#include "nimblefix/profile/artifact.h"
#include "nimblefix/profile/profile_loader.h"
#include "nimblefix/runtime/engine.h"

#include "test_support.h"

TEST_CASE("profile-loader", "[profile-loader]")
{
  const auto artifact_path = std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.nfa";
  if (!std::filesystem::exists(artifact_path)) {
    SKIP("FIX44 artifact not available at: " << artifact_path.string());
  }

  auto loaded = nimble::profile::LoadProfileArtifact(artifact_path);
  REQUIRE(loaded.ok());
  REQUIRE(loaded.value().valid());
  REQUIRE(loaded.value().profile_id() > 0ULL);
  REQUIRE(loaded.value().schema_hash() != 0ULL);
  REQUIRE(loaded.value().sections().size() > 0U);

  const auto string_table = loaded.value().string_table();
  REQUIRE(string_table.has_value());

  nimble::runtime::Engine engine;
  nimble::runtime::EngineConfig config;
  config.profile_artifacts.push_back(artifact_path);

  const auto status = engine.LoadProfiles(config);
  REQUIRE(status.ok());
  REQUIRE(engine.profiles().size() == 1U);
  REQUIRE(engine.profiles().Find(loaded.value().profile_id()) != nullptr);
}

TEST_CASE("profile madvise warming", "[profile-loader]")
{
  const auto artifact_path = std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.nfa";
  if (!std::filesystem::exists(artifact_path)) {
    SKIP("FIX44 artifact not available at: " << artifact_path.string());
  }

  nimble::profile::ProfileLoadOptions options;
  options.madvise = true;

  auto loaded = nimble::profile::LoadProfileArtifact(artifact_path, options);
  REQUIRE(loaded.ok());
  REQUIRE(loaded.value().valid());
  REQUIRE(loaded.value().profile_id() > 0ULL);
}

TEST_CASE("profile mlock warming", "[profile-loader]")
{
  const auto artifact_path = std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.nfa";
  if (!std::filesystem::exists(artifact_path)) {
    SKIP("FIX44 artifact not available at: " << artifact_path.string());
  }

  nimble::profile::ProfileLoadOptions options;
  options.mlock = true;

  // mlock may fail due to RLIMIT_MEMLOCK but must not abort the load
  auto loaded = nimble::profile::LoadProfileArtifact(artifact_path, options);
  REQUIRE(loaded.ok());
  REQUIRE(loaded.value().valid());
  REQUIRE(loaded.value().profile_id() > 0ULL);
}

TEST_CASE("profile warming disabled by default", "[profile-loader]")
{
  nimble::runtime::EngineConfig config;
  REQUIRE(config.profile_madvise == false);
  REQUIRE(config.profile_mlock == false);

  nimble::profile::ProfileLoadOptions options;
  REQUIRE(options.madvise == false);
  REQUIRE(options.mlock == false);
}

TEST_CASE("schema_hash validation match", "[profile-loader]")
{
  const auto artifact_path = std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.nfa";
  if (!std::filesystem::exists(artifact_path)) {
    SKIP("FIX44 artifact not available at: " << artifact_path.string());
  }

  auto loaded = nimble::profile::LoadProfileArtifact(artifact_path);
  REQUIRE(loaded.ok());

  // Matching hash succeeds
  auto status = nimble::profile::ValidateSchemaHash(loaded.value(), loaded.value().schema_hash());
  REQUIRE(status.ok());
}

TEST_CASE("schema_hash validation mismatch", "[profile-loader]")
{
  const auto artifact_path = std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.nfa";
  if (!std::filesystem::exists(artifact_path)) {
    SKIP("FIX44 artifact not available at: " << artifact_path.string());
  }

  auto loaded = nimble::profile::LoadProfileArtifact(artifact_path);
  REQUIRE(loaded.ok());

  // Wrong hash fails with VersionMismatch
  auto status = nimble::profile::ValidateSchemaHash(loaded.value(), 0xDEADBEEFULL);
  REQUIRE_FALSE(status.ok());
  REQUIRE(status.code() == nimble::base::ErrorCode::kVersionMismatch);
  REQUIRE(status.message().find("schema_hash mismatch") != std::string::npos);
}

TEST_CASE("schema_hash validation invalid profile", "[profile-loader]")
{
  nimble::profile::LoadedProfile empty_profile;
  auto status = nimble::profile::ValidateSchemaHash(empty_profile, 42ULL);
  REQUIRE_FALSE(status.ok());
  REQUIRE(status.code() == nimble::base::ErrorCode::kInvalidArgument);
}
