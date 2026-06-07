#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>

#include "fix44_api.h"
#include "nimblefix/base/status.h"
#include "nimblefix/profile/artifact.h"
#include "nimblefix/profile/profile_loader.h"
#include "nimblefix/runtime/acceptor.h"
#include "nimblefix/runtime/engine.h"
#include "nimblefix/runtime/initiator.h"
#include "nimblefix/runtime/simple.h"

namespace {

class TypedRuntimeSmokeApp final : public nimble::generated::profile_4400::Handler
{};

struct StubDispatcher
{};

struct SchemaMismatchProfile
{
  using Application = TypedRuntimeSmokeApp;
  using Dispatcher = StubDispatcher;
  static constexpr std::uint64_t kProfileId = nimble::generated::profile_4400::Profile::kProfileId;
  static constexpr std::uint64_t kSchemaHash = nimble::generated::profile_4400::Profile::kSchemaHash ^ 0x1ULL;
};

struct MissingProfile
{
  using Application = TypedRuntimeSmokeApp;
  using Dispatcher = StubDispatcher;
  static constexpr std::uint64_t kProfileId = 0xDEAD'BEEF'0000'4400ULL;
  static constexpr std::uint64_t kSchemaHash = 1ULL;
};

} // namespace

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

TEST_CASE("profile artifact magic uses NimbleFIX magic", "[profile-loader]")
{
  REQUIRE(nimble::profile::kArtifactMagic.size() == 4U);
  REQUIRE(nimble::profile::kArtifactMagic[0] == static_cast<std::uint8_t>('N'));
  REQUIRE(nimble::profile::kArtifactMagic[1] == static_cast<std::uint8_t>('F'));
  REQUIRE(nimble::profile::kArtifactMagic[2] == static_cast<std::uint8_t>('P'));
  REQUIRE(nimble::profile::kArtifactMagic[3] == static_cast<std::uint8_t>('F'));
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

TEST_CASE("engine bind returns typed profile binding", "[profile-loader][typed-runtime]")
{
  const auto artifact_path = std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.nfa";
  if (!std::filesystem::exists(artifact_path)) {
    SKIP("FIX44 artifact not available at: " << artifact_path.string());
  }

  nimble::runtime::Engine engine;
  nimble::runtime::EngineConfig config;
  config.profile_artifacts.push_back(artifact_path);

  REQUIRE(engine.LoadProfiles(config).ok());

  auto binding = engine.Bind<nimble::generated::profile_4400::Profile>();
  REQUIRE(binding.ok());
  REQUIRE(binding.value().profile_id() == nimble::generated::profile_4400::Profile::kProfileId);
  REQUIRE(binding.value().schema_hash() == nimble::generated::profile_4400::Profile::kSchemaHash);
  REQUIRE(binding.value().dictionary().profile().profile_id() == nimble::generated::profile_4400::Profile::kProfileId);
}

TEST_CASE("typed runtime wrappers instantiate against generated profile", "[profile-loader][typed-runtime]")
{
  const auto artifact_path = std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.nfa";
  if (!std::filesystem::exists(artifact_path)) {
    SKIP("FIX44 artifact not available at: " << artifact_path.string());
  }

  nimble::runtime::Engine engine;
  nimble::runtime::EngineConfig config;
  config.profile_artifacts.push_back(artifact_path);

  REQUIRE(engine.LoadProfiles(config).ok());

  auto binding = engine.Bind<nimble::generated::profile_4400::Profile>();
  REQUIRE(binding.ok());

  auto app = std::make_shared<TypedRuntimeSmokeApp>();
  nimble::runtime::Initiator<nimble::generated::profile_4400::Profile> initiator(
    &engine, &binding.value(), { .application = app });
  nimble::runtime::Acceptor<nimble::generated::profile_4400::Profile> acceptor(
    &engine, &binding.value(), { .application = app });

  CHECK(initiator.active_connection_count() == 0U);
  CHECK(initiator.completed_session_count() == 0U);
  CHECK(initiator.pending_reconnect_count() == 0U);
  CHECK(acceptor.active_connection_count() == 0U);
  CHECK(acceptor.completed_session_count() == 0U);
}

TEST_CASE("simple runtime facade instantiates against generated profile", "[profile-loader][typed-runtime]")
{
  using Profile = nimble::generated::profile_4400::Profile;

  auto initiator = nimble::runtime::CreateInitiator<Profile>(nimble::runtime::SimpleInitiatorSettings<Profile>{});
  REQUIRE_FALSE(initiator.ok());
  CHECK(initiator.status().code() == nimble::base::ErrorCode::kInvalidArgument);
  CHECK(initiator.status().message().find("application instance") != std::string::npos);

  auto acceptor = nimble::runtime::CreateAcceptor<Profile>(nimble::runtime::SimpleAcceptorSettings<Profile>{});
  REQUIRE_FALSE(acceptor.ok());
  CHECK(acceptor.status().code() == nimble::base::ErrorCode::kInvalidArgument);
  CHECK(acceptor.status().message().find("application instance") != std::string::npos);
}

TEST_CASE("engine bind rejects schema hash mismatch", "[profile-loader][typed-runtime]")
{
  const auto artifact_path = std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.nfa";
  if (!std::filesystem::exists(artifact_path)) {
    SKIP("FIX44 artifact not available at: " << artifact_path.string());
  }

  nimble::runtime::Engine engine;
  nimble::runtime::EngineConfig config;
  config.profile_artifacts.push_back(artifact_path);

  REQUIRE(engine.LoadProfiles(config).ok());

  auto binding = engine.Bind<SchemaMismatchProfile>();
  REQUIRE_FALSE(binding.ok());
  REQUIRE(binding.status().code() == nimble::base::ErrorCode::kVersionMismatch);
  REQUIRE(binding.status().message().find("schema_hash mismatch") != std::string_view::npos);
}

TEST_CASE("engine bind rejects missing profile", "[profile-loader][typed-runtime]")
{
  const auto artifact_path = std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.nfa";
  if (!std::filesystem::exists(artifact_path)) {
    SKIP("FIX44 artifact not available at: " << artifact_path.string());
  }

  nimble::runtime::Engine engine;
  nimble::runtime::EngineConfig config;
  config.profile_artifacts.push_back(artifact_path);

  REQUIRE(engine.LoadProfiles(config).ok());

  auto binding = engine.Bind<MissingProfile>();
  REQUIRE_FALSE(binding.ok());
  REQUIRE(binding.status().code() == nimble::base::ErrorCode::kNotFound);
}
