#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nimblefix/engine.hpp>
#include <nimblefix/settings.hpp>
#include <nimblefix/tls.hpp>

namespace nimblefix {

enum class ConfigDispatch : std::uint8_t { inline_callbacks, queued_callbacks };

struct ConfigDiagnostic {
    Status status{ErrorCode::invalid_config};
    std::filesystem::path source;
    std::uint32_t line{};
    std::uint32_t column{};
    std::string field;
    std::string received;
    std::string expected;
    std::string fix;
};

struct SessionSchemaConfig {
    std::string session_name;
    bool acceptor{};
    std::string schema{"none"};
    std::vector<std::filesystem::path> dictionaries;
};

struct ConfigDocument {
    std::uint32_t version{1};
    EngineConfig engine;
    ConfigDispatch dispatch{ConfigDispatch::inline_callbacks};
    std::string schema{"none"};
    std::vector<std::filesystem::path> dictionaries;
    std::vector<SessionSchemaConfig> session_schemas;
    std::vector<ListenerConfig> listeners;
    std::vector<InitiatorConfig> initiators;
    std::vector<AcceptorConfig> acceptors;
    std::size_t shards{1};
    std::vector<int> cpu_cores;
};

struct ConfigLoadResult {
    std::optional<ConfigDocument> document;
    std::vector<ConfigDiagnostic> diagnostics;
    [[nodiscard]] bool ok() const noexcept { return document.has_value() && diagnostics.empty(); }
};

// File references are resolved against this configuration file's directory.
// Only explicitly named password_env references read the environment.
[[nodiscard]] ConfigLoadResult load_config(const std::filesystem::path &path) noexcept;
[[nodiscard]] std::vector<ConfigDiagnostic> validate_config(const ConfigDocument &document) noexcept;
[[nodiscard]] std::string diagnostics_json(const std::vector<ConfigDiagnostic> &diagnostics);

} // namespace nimblefix
