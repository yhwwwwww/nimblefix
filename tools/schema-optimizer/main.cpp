#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "nimblefix/profile/dictgen_input.h"
#include "nimblefix/store/durable_batch_store.h"
#include "nimblefix/store/session_store.h"
#include "nimblefix/tools/schema_optimizer.h"

namespace {

struct Options
{
  std::filesystem::path store_path;
  std::optional<std::uint64_t> session_id;
  std::vector<std::filesystem::path> input_files;
  std::filesystem::path base_nfd_path;
  std::filesystem::path output_path;
  double min_frequency{ 0.0 };
  std::uint64_t profile_id{ 0 };
  bool report{ false };
  bool estimate{ false };
};

auto
PrintUsage() -> void
{
  std::cout << "Usage: nimblefix-schema-optimizer [options]\n"
               "  --store PATH       Read messages from durable store directory\n"
               "  --session-id ID    Filter to specific session\n"
               "  --input FILE       Read raw FIX messages from file (one per line, pipe-delimited)\n"
               "  --base-nfd FILE    Base dictionary to trim\n"
               "  --output FILE      Output trimmed .nfd\n"
               "  --min-freq N       Minimum tag frequency threshold (0.0-1.0, default 0.0)\n"
               "  --profile-id ID    Profile ID for output (default: from base dictionary)\n"
               "  --report           Print usage report to stdout\n"
               "  --estimate         Print layout size estimates to stdout\n";
}

[[nodiscard]] auto
ResolveProjectPath(const std::filesystem::path& path) -> std::filesystem::path
{
  if (path.empty() || path.is_absolute()) {
    return path;
  }
  return std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / path;
}

[[nodiscard]] auto
ParseU64(std::string_view token) -> std::optional<std::uint64_t>
{
  try {
    std::size_t consumed = 0U;
    const auto value = std::stoull(std::string(token), &consumed, 0);
    if (consumed != token.size()) {
      return std::nullopt;
    }
    return value;
  } catch (...) {
    return std::nullopt;
  }
}

[[nodiscard]] auto
ParseDouble(std::string_view token) -> std::optional<double>
{
  try {
    std::size_t consumed = 0U;
    const auto value = std::stod(std::string(token), &consumed);
    if (consumed != token.size()) {
      return std::nullopt;
    }
    return value;
  } catch (...) {
    return std::nullopt;
  }
}

[[nodiscard]] auto
ParseOptions(int argc, char** argv) -> std::optional<Options>
{
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view arg(argv[index]);
    if (arg == "--store" && index + 1 < argc) {
      options.store_path = argv[++index];
      continue;
    }
    if (arg == "--session-id" && index + 1 < argc) {
      auto parsed = ParseU64(argv[++index]);
      if (!parsed.has_value()) {
        return std::nullopt;
      }
      options.session_id = *parsed;
      continue;
    }
    if (arg == "--input" && index + 1 < argc) {
      options.input_files.emplace_back(argv[++index]);
      continue;
    }
    if (arg == "--base-nfd" && index + 1 < argc) {
      options.base_nfd_path = argv[++index];
      continue;
    }
    if (arg == "--output" && index + 1 < argc) {
      options.output_path = argv[++index];
      continue;
    }
    if (arg == "--min-freq" && index + 1 < argc) {
      auto parsed = ParseDouble(argv[++index]);
      if (!parsed.has_value() || *parsed < 0.0 || *parsed > 1.0) {
        return std::nullopt;
      }
      options.min_frequency = *parsed;
      continue;
    }
    if (arg == "--profile-id" && index + 1 < argc) {
      auto parsed = ParseU64(argv[++index]);
      if (!parsed.has_value()) {
        return std::nullopt;
      }
      options.profile_id = *parsed;
      continue;
    }
    if (arg == "--report") {
      options.report = true;
      continue;
    }
    if (arg == "--estimate") {
      options.estimate = true;
      continue;
    }
    return std::nullopt;
  }

  if (options.store_path.empty() && options.input_files.empty()) {
    return std::nullopt;
  }
  if (!options.store_path.empty() && !options.session_id.has_value()) {
    return std::nullopt;
  }
  if (!options.output_path.empty() && options.base_nfd_path.empty()) {
    return std::nullopt;
  }
  if (options.estimate && options.base_nfd_path.empty()) {
    return std::nullopt;
  }
  return options;
}

[[nodiscard]] auto
BytesToString(std::span<const std::byte> bytes) -> std::string
{
  if (bytes.empty()) {
    return {};
  }
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

auto
AppendRawInputFile(const std::filesystem::path& path, std::vector<std::string>* messages) -> bool
{
  std::ifstream input(path);
  if (!input) {
    std::cerr << "unable to open input file: " << path << '\n';
    return false;
  }

  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    messages->push_back(std::move(line));
  }
  return true;
}

auto
AppendStoreMessages(const std::filesystem::path& store_path,
                    std::uint64_t session_id,
                    std::vector<std::string>* messages) -> bool
{
  nimble::store::DurableBatchSessionStore store(store_path,
                                                nimble::store::DurableBatchStoreOptions{
                                                  .flush_threshold = 1U,
                                                  .rollover_mode = nimble::store::DurableStoreRolloverMode::kExternal,
                                                });
  auto status = store.Open();
  if (!status.ok()) {
    std::cerr << status.message() << '\n';
    return false;
  }

  auto recovery = store.LoadRecoveryState(session_id);
  if (!recovery.ok()) {
    std::cerr << recovery.status().message() << '\n';
    return false;
  }

  const auto out_seq_to = recovery.value().next_out_seq == 0U ? 0U : recovery.value().next_out_seq - 1U;
  const auto in_seq_to = recovery.value().next_in_seq == 0U ? 0U : recovery.value().next_in_seq - 1U;

  if (out_seq_to != 0U) {
    auto outbound = store.LoadOutboundRange(session_id, 1U, out_seq_to);
    if (!outbound.ok()) {
      std::cerr << outbound.status().message() << '\n';
      return false;
    }
    for (const auto& record : outbound.value()) {
      messages->push_back(BytesToString(record.payload));
    }
  }
  if (in_seq_to != 0U) {
    auto inbound = store.LoadInboundRange(session_id, 1U, in_seq_to);
    if (!inbound.ok()) {
      std::cerr << inbound.status().message() << '\n';
      return false;
    }
    for (const auto& record : inbound.value()) {
      messages->push_back(BytesToString(record.payload));
    }
  }
  return true;
}

[[nodiscard]] auto
BuildMessageViews(const std::vector<std::string>& messages) -> std::vector<std::string_view>
{
  std::vector<std::string_view> views;
  views.reserve(messages.size());
  for (const auto& message : messages) {
    views.push_back(message);
  }
  return views;
}

auto
WriteTextFile(const std::filesystem::path& path, std::string_view text) -> bool
{
  std::ofstream output(path);
  if (!output) {
    std::cerr << "unable to open output file: " << path << '\n';
    return false;
  }
  output << text;
  return output.good();
}

auto
LoadDictionary(const std::filesystem::path& path) -> std::optional<nimble::profile::NormalizedDictionary>
{
  auto dictionary = nimble::profile::LoadNormalizedDictionaryFile(path);
  if (!dictionary.ok()) {
    std::cerr << dictionary.status().message() << '\n';
    return std::nullopt;
  }
  return std::move(dictionary).value();
}

} // namespace

int
main(int argc, char** argv)
{
  std::optional<Options> parsed;
  try {
    parsed = ParseOptions(argc, argv);
  } catch (...) {
    parsed.reset();
  }
  if (!parsed.has_value()) {
    PrintUsage();
    return 1;
  }

  auto options = std::move(parsed).value();
  options.store_path = ResolveProjectPath(options.store_path);
  options.base_nfd_path = ResolveProjectPath(options.base_nfd_path);
  options.output_path = ResolveProjectPath(options.output_path);
  for (auto& input : options.input_files) {
    input = ResolveProjectPath(input);
  }

  std::vector<std::string> messages;
  for (const auto& input : options.input_files) {
    if (!AppendRawInputFile(input, &messages)) {
      return 1;
    }
  }
  if (!options.store_path.empty() && !AppendStoreMessages(options.store_path, options.session_id.value(), &messages)) {
    return 1;
  }

  const auto views = BuildMessageViews(messages);
  auto usage_report = nimble::tools::AnalyzeMessages(views, '\x01');

  std::optional<nimble::profile::NormalizedDictionary> base_dictionary;
  std::optional<nimble::tools::SchemaOptimizationResult> optimization;
  if (!options.base_nfd_path.empty()) {
    base_dictionary = LoadDictionary(options.base_nfd_path);
    if (!base_dictionary.has_value()) {
      return 1;
    }

    nimble::tools::SchemaOptimizerConfig config;
    config.min_frequency = options.min_frequency;
    config.profile_id = options.profile_id == 0U ? base_dictionary->profile_id : options.profile_id;
    optimization = nimble::tools::GenerateTrimmedNfd(usage_report, *base_dictionary, config);
  }

  if (options.report) {
    std::cout << nimble::tools::FormatUsageReport(usage_report);
  }

  if (options.output_path.empty() && optimization.has_value() && !options.report && !options.estimate) {
    std::cout << optimization->trimmed_nfd_text;
  }

  if (!options.output_path.empty()) {
    if (!optimization.has_value()) {
      std::cerr << "--base-nfd is required when --output is used\n";
      return 1;
    }
    if (!WriteTextFile(options.output_path, optimization->trimmed_nfd_text)) {
      return 1;
    }
  }

  if (options.estimate) {
    if (!optimization.has_value()) {
      std::cerr << "--base-nfd is required when --estimate is used\n";
      return 1;
    }
    std::cout << nimble::tools::FormatOptimizationResult(*optimization);
  }

  return 0;
}
