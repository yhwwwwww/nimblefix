#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#include "nimblefix/profile/dictgen_input.h"

namespace {

auto
PrintUsage() -> void
{
  std::cout << "usage: nimblefix-fuzz-dictgen --input <file-or-directory>\n";
}

auto
CollectFiles(const std::filesystem::path& input) -> std::vector<std::filesystem::path>
{
  std::vector<std::filesystem::path> files;
  if (std::filesystem::is_regular_file(input)) {
    files.push_back(input);
    return files;
  }
  for (const auto& entry : std::filesystem::recursive_directory_iterator(input)) {
    if (entry.is_regular_file()) {
      files.push_back(entry.path());
    }
  }
  return files;
}

auto
ReadText(const std::filesystem::path& path) -> std::string
{
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

int
main(int argc, char** argv)
{
  std::filesystem::path input;
  for (int index = 1; index < argc; ++index) {
    const std::string_view arg(argv[index]);
    if (arg == "--input" && index + 1 < argc) {
      input = argv[++index];
      continue;
    }
    PrintUsage();
    return 1;
  }

  if (input.empty()) {
    PrintUsage();
    return 1;
  }

  const auto files = CollectFiles(input);
  std::size_t accepted = 0;
  for (const auto& file : files) {
    const auto text = ReadText(file);
    auto result = nimble::profile::LoadNormalizedDictionaryText(text);
    if (result.ok()) {
      ++accepted;
    }
  }

  std::cout << "processed " << files.size() << " dictgen inputs, accepted " << accepted << '\n';
  return 0;
}