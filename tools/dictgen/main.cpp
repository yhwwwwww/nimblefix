#include <filesystem>
#include <iostream>
#include <vector>

#include "nimblefix/profile/artifact_builder.h"
#include "nimblefix/profile/builder_codegen.h"
#include "nimblefix/profile/dictgen_input.h"
#include "nimblefix/profile/overlay.h"

namespace {

auto
PrintUsage() -> void
{
  std::cout << "usage: nimblefix-dictgen --input <dictionary.ffd> [--merge "
               "<overlay.ffd> ...] --output <profile.art> [--cpp-builders "
               "<generated.hpp>] [--cpp-readers <generated.hpp>]\n";
}

auto
ResolveProjectPath(const std::filesystem::path& path) -> std::filesystem::path
{
  if (path.is_absolute()) {
    return path;
  }

  return std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / path;
}

} // namespace

int
main(int argc, char** argv)
{
  std::filesystem::path input_path;
  std::filesystem::path output_path;
  std::filesystem::path builder_output_path;
  std::filesystem::path reader_output_path;
  std::vector<std::filesystem::path> merge_paths;

  for (int index = 1; index < argc; ++index) {
    const std::string_view arg(argv[index]);
    if (arg == "--input" && index + 1 < argc) {
      input_path = argv[++index];
      continue;
    }
    if ((arg == "--merge" || arg == "--overlay") && index + 1 < argc) {
      merge_paths.emplace_back(argv[++index]);
      continue;
    }
    if (arg == "--output" && index + 1 < argc) {
      output_path = argv[++index];
      continue;
    }
    if (arg == "--cpp-builders" && index + 1 < argc) {
      builder_output_path = argv[++index];
      continue;
    }
    if (arg == "--cpp-readers" && index + 1 < argc) {
      reader_output_path = argv[++index];
      continue;
    }
    PrintUsage();
    return 1;
  }

  if (input_path.empty() || output_path.empty()) {
    PrintUsage();
    return 1;
  }

  input_path = ResolveProjectPath(input_path);
  output_path = ResolveProjectPath(output_path);
  if (!builder_output_path.empty()) {
    builder_output_path = ResolveProjectPath(builder_output_path);
  }
  if (!reader_output_path.empty()) {
    reader_output_path = ResolveProjectPath(reader_output_path);
  }
  for (auto& merge_path : merge_paths) {
    merge_path = ResolveProjectPath(merge_path);
  }

  if (const auto parent = output_path.parent_path(); !parent.empty()) {
    std::filesystem::create_directories(parent);
  }

  auto dictionary = nimble::profile::LoadNormalizedDictionaryFile(input_path);
  if (!dictionary.ok()) {
    std::cerr << dictionary.status().message() << '\n';
    return 1;
  }

  auto merged = std::move(dictionary).value();
  for (const auto& merge_path : merge_paths) {
    auto additional = nimble::profile::LoadNormalizedDictionaryFile(merge_path);
    if (!additional.ok()) {
      std::cerr << additional.status().message() << '\n';
      return 1;
    }

    auto applied = nimble::profile::ApplyOverlay(merged, additional.value());
    if (!applied.ok()) {
      std::cerr << applied.status().message() << '\n';
      return 1;
    }
    merged = std::move(applied).value();
  }

  auto artifact = nimble::profile::BuildProfileArtifact(merged);
  if (!artifact.ok()) {
    std::cerr << artifact.status().message() << '\n';
    return 1;
  }

  const auto write_status = nimble::profile::WriteProfileArtifact(output_path, artifact.value());
  if (!write_status.ok()) {
    std::cerr << write_status.message() << '\n';
    return 1;
  }

  if (!builder_output_path.empty()) {
    const auto builder_status = nimble::profile::WriteCppBuildersHeader(builder_output_path, merged);
    if (!builder_status.ok()) {
      std::cerr << builder_status.message() << '\n';
      return 1;
    }
  }

  if (!reader_output_path.empty()) {
    const auto reader_status = nimble::profile::WriteCppReadersHeader(reader_output_path, merged);
    if (!reader_status.ok()) {
      std::cerr << reader_status.message() << '\n';
      return 1;
    }
  }

  std::cout << "generated artifact '" << output_path.string() << "' with " << merged.fields.size() << " fields, "
            << merged.messages.size() << " messages, and " << merged.groups.size() << " groups";
  if (!builder_output_path.empty()) {
    std::cout << "; generated builder header '" << builder_output_path.string() << "'";
  }
  if (!reader_output_path.empty()) {
    std::cout << "; generated reader header '" << reader_output_path.string() << "'";
  }
  std::cout << '\n';
  return 0;
}
