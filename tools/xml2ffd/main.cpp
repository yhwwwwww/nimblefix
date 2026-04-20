#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "nimblefix/profile/builder_codegen.h"
#include "nimblefix/profile/dictgen_input.h"
#include "nimblefix/profile/overlay.h"
#include "xml2ffd.h"

namespace {

auto
PrintUsage() -> void
{
  std::cerr << "usage:\n"
            << "  nimblefix-xml2ffd --xml <FIX44.xml> --output <output.ffd> "
               "--profile-id <uint64> [--cpp-builders <output.h>] [--cpp-readers "
               "<output.h>]\n"
            << "  nimblefix-xml2ffd --input <baseline.ffd> [--input <overlay.ffd> ...] "
               "--cpp-builders <output.h> [--cpp-readers <output.h>]\n";
}

auto
ResolveProjectPath(const std::filesystem::path& path) -> std::filesystem::path
{
  if (path.is_absolute()) {
    return path;
  }
  return std::filesystem::path(NIMBLEFIX_PROJECT_DIR) / path;
}

auto
ReadFile(const std::filesystem::path& path) -> std::string
{
  std::ifstream stream(path);
  if (!stream) {
    std::cerr << "error: cannot open file: " << path << '\n';
    std::exit(1);
  }
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

} // namespace

int
main(int argc, char** argv)
{
  std::filesystem::path xml_path;
  std::filesystem::path output_path;
  std::filesystem::path builder_output_path;
  std::filesystem::path reader_output_path;
  std::vector<std::filesystem::path> input_paths;
  std::uint64_t profile_id = 0;

  for (int index = 1; index < argc; ++index) {
    const std::string_view arg(argv[index]);
    if (arg == "--xml" && index + 1 < argc) {
      xml_path = argv[++index];
      continue;
    }
    if (arg == "--output" && index + 1 < argc) {
      output_path = argv[++index];
      continue;
    }
    if (arg == "--profile-id" && index + 1 < argc) {
      profile_id = std::stoull(argv[++index], nullptr, 0);
      continue;
    }
    if (arg == "--input" && index + 1 < argc) {
      input_paths.emplace_back(argv[++index]);
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

  // Validate: either --xml mode or --input mode
  const bool xml_mode = !xml_path.empty();
  const bool input_mode = !input_paths.empty();

  if (xml_mode && input_mode) {
    std::cerr << "error: --xml and --input are mutually exclusive\n";
    return 1;
  }

  if (!xml_mode && !input_mode) {
    PrintUsage();
    return 1;
  }

  if (xml_mode && (output_path.empty() || profile_id == 0)) {
    std::cerr << "error: --xml mode requires --output and --profile-id\n";
    return 1;
  }

  if (input_mode && builder_output_path.empty() && reader_output_path.empty()) {
    std::cerr << "error: --input mode requires --cpp-builders or --cpp-readers\n";
    return 1;
  }

  // Phase 1: XML → .ffd (if xml mode)
  std::string ffd_text;

  if (xml_mode) {
    xml_path = ResolveProjectPath(xml_path);
    output_path = ResolveProjectPath(output_path);

    const auto xml_content = ReadFile(xml_path);
    try {
      ffd_text = nimble::tools::ConvertXmlToFfd(xml_content, profile_id);
    } catch (const std::exception& e) {
      std::cerr << "error: " << e.what() << '\n';
      return 1;
    }

    if (const auto parent = output_path.parent_path(); !parent.empty()) {
      std::filesystem::create_directories(parent);
    }

    std::ofstream out(output_path);
    if (!out) {
      std::cerr << "error: cannot write to: " << output_path << '\n';
      return 1;
    }
    out << ffd_text;
    std::cout << "generated dictionary '" << output_path.string() << "'\n";
  }

  // Phase 2: Build normalized dictionary (if codegen is requested)
  nimble::profile::NormalizedDictionary dictionary;
  const bool need_dictionary = !builder_output_path.empty() || !reader_output_path.empty();

  if (need_dictionary) {
    if (xml_mode) {
      // Use the .ffd text we just generated
      auto parsed = nimble::profile::LoadNormalizedDictionaryText(ffd_text);
      if (!parsed.ok()) {
        std::cerr << "error: " << parsed.status().message() << '\n';
        return 1;
      }
      dictionary = std::move(parsed).value();
    } else {
      // Load and merge input .ffd files
      for (auto& p : input_paths) {
        p = ResolveProjectPath(p);
      }

      auto baseline = nimble::profile::LoadNormalizedDictionaryFile(input_paths[0]);
      if (!baseline.ok()) {
        std::cerr << "error: " << baseline.status().message() << '\n';
        return 1;
      }
      dictionary = std::move(baseline).value();

      for (std::size_t i = 1; i < input_paths.size(); ++i) {
        auto additional = nimble::profile::LoadNormalizedDictionaryFile(input_paths[i]);
        if (!additional.ok()) {
          std::cerr << "error: " << additional.status().message() << '\n';
          return 1;
        }
        auto merged = nimble::profile::ApplyOverlay(dictionary, additional.value());
        if (!merged.ok()) {
          std::cerr << "error: " << merged.status().message() << '\n';
          return 1;
        }
        dictionary = std::move(merged).value();
      }
    }
  }

  // Phase 3: Generate C++ builder header (if requested)
  if (!builder_output_path.empty()) {
    builder_output_path = ResolveProjectPath(builder_output_path);

    if (const auto parent = builder_output_path.parent_path(); !parent.empty()) {
      std::filesystem::create_directories(parent);
    }

    const auto status = nimble::profile::WriteCppBuildersHeader(builder_output_path, dictionary);
    if (!status.ok()) {
      std::cerr << "error: " << status.message() << '\n';
      return 1;
    }
    std::cout << "generated builder header '" << builder_output_path.string() << "'\n";
  }

  // Phase 4: Generate C++ reader header (if requested)
  if (!reader_output_path.empty()) {
    reader_output_path = ResolveProjectPath(reader_output_path);

    if (const auto parent = reader_output_path.parent_path(); !parent.empty()) {
      std::filesystem::create_directories(parent);
    }

    const auto status = nimble::profile::WriteCppReadersHeader(reader_output_path, dictionary);
    if (!status.ok()) {
      std::cerr << "error: " << status.message() << '\n';
      return 1;
    }
    std::cout << "generated reader header '" << reader_output_path.string() << "'\n";
  }

  return 0;
}
