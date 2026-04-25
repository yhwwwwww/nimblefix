#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

#include "nimblefix/profile/contract_sidecar.h"
#include "orchestra_import.h"

namespace {

auto
PrintUsage() -> void
{
  std::cout << "usage:\n"
            << "  nimblefix-orchestra-import --xml <repository.xml> --profile-id <uint64> "
               "[--output-nfd <dictionary.nfd>] [--output-contract <contract.nfct>] [--cache-root <dir>]\n"
            << "  nimblefix-orchestra-import --contract <contract.nfct> [--dump] "
               "[--markdown <contract.md>] [--interop-dir <outdir>]\n";
}

auto
ReadTextFile(const std::filesystem::path& path) -> std::optional<std::string>
{
  std::ifstream stream(path);
  if (!stream.is_open()) {
    return std::nullopt;
  }
  return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

auto
WriteTextFile(const std::filesystem::path& path, std::string_view text) -> bool
{
  if (const auto parent = path.parent_path(); !parent.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      return false;
    }
  }
  std::ofstream stream(path, std::ios::trunc);
  if (!stream.is_open()) {
    return false;
  }
  stream << text;
  return true;
}

} // namespace

int
main(int argc, char** argv)
{
  std::filesystem::path xml_path;
  std::filesystem::path output_nfd_path;
  std::filesystem::path output_contract_path;
  std::filesystem::path cache_root;
  std::filesystem::path contract_path;
  std::filesystem::path markdown_path;
  std::filesystem::path interop_dir;
  bool dump_contract = false;
  std::uint64_t profile_id = 0U;

  for (int index = 1; index < argc; ++index) {
    const std::string_view arg(argv[index]);
    if (arg == "--xml" && index + 1 < argc) {
      xml_path = argv[++index];
      continue;
    }
    if (arg == "--profile-id" && index + 1 < argc) {
      profile_id = std::stoull(argv[++index], nullptr, 0);
      continue;
    }
    if (arg == "--output-nfd" && index + 1 < argc) {
      output_nfd_path = argv[++index];
      continue;
    }
    if (arg == "--output-contract" && index + 1 < argc) {
      output_contract_path = argv[++index];
      continue;
    }
    if (arg == "--cache-root" && index + 1 < argc) {
      cache_root = argv[++index];
      continue;
    }
    if (arg == "--contract" && index + 1 < argc) {
      contract_path = argv[++index];
      continue;
    }
    if (arg == "--dump") {
      dump_contract = true;
      continue;
    }
    if (arg == "--markdown" && index + 1 < argc) {
      markdown_path = argv[++index];
      continue;
    }
    if (arg == "--interop-dir" && index + 1 < argc) {
      interop_dir = argv[++index];
      continue;
    }
    PrintUsage();
    return 1;
  }

  if (!xml_path.empty()) {
    if (profile_id == 0U) {
      PrintUsage();
      return 1;
    }

    if (!cache_root.empty()) {
      const auto layout = nimble::tools::ResolveOrchestraCacheLayout(cache_root, profile_id);
      if (output_nfd_path.empty()) {
        output_nfd_path = layout.dictionary_nfd;
      }
      if (output_contract_path.empty()) {
        output_contract_path = layout.contract_sidecar;
      }
    }

    if (output_nfd_path.empty() || output_contract_path.empty()) {
      PrintUsage();
      return 1;
    }

    const auto xml_text = ReadTextFile(xml_path);
    if (!xml_text.has_value()) {
      std::cerr << "unable to open Orchestra XML: " << xml_path.string() << '\n';
      return 1;
    }

    auto imported = nimble::tools::ImportOrchestraXml(xml_text.value(), profile_id, xml_path.filename().string());
    if (!imported.ok()) {
      std::cerr << imported.status().message() << '\n';
      return 1;
    }

    const auto nfd_text = nimble::tools::SerializeDictionaryAsNfd(imported.value().dictionary);
    if (!WriteTextFile(output_nfd_path, nfd_text)) {
      std::cerr << "unable to write NFD output: " << output_nfd_path.string() << '\n';
      return 1;
    }
    const auto contract_status = nimble::profile::WriteContractSidecar(output_contract_path, imported.value().contract);
    if (!contract_status.ok()) {
      std::cerr << contract_status.message() << '\n';
      return 1;
    }

    for (const auto& warning : imported.value().warnings) {
      std::cerr << "warning: " << warning << '\n';
    }

    std::cout << "generated dictionary '" << output_nfd_path.string() << "'\n";
    std::cout << "generated contract sidecar '" << output_contract_path.string() << "'\n";
    return 0;
  }

  if (!contract_path.empty()) {
    auto contract = nimble::profile::LoadContractSidecarFile(contract_path);
    if (!contract.ok()) {
      std::cerr << contract.status().message() << '\n';
      return 1;
    }

    if (dump_contract) {
      std::cout << nimble::profile::RenderContractSummary(contract.value());
    }
    if (!markdown_path.empty()) {
      if (!WriteTextFile(markdown_path, nimble::profile::RenderContractMarkdown(contract.value()))) {
        std::cerr << "unable to write markdown output: " << markdown_path.string() << '\n';
        return 1;
      }
    }
    if (!interop_dir.empty()) {
      const auto generated = nimble::profile::GenerateInteropScenarioAugmentations(contract.value());
      for (const auto& scenario : generated) {
        if (!WriteTextFile(interop_dir / scenario.file_name, scenario.text)) {
          std::cerr << "unable to write generated scenario: " << (interop_dir / scenario.file_name).string() << '\n';
          return 1;
        }
      }
    }
    if (!dump_contract && markdown_path.empty() && interop_dir.empty()) {
      PrintUsage();
      return 1;
    }
    return 0;
  }

  PrintUsage();
  return 1;
}