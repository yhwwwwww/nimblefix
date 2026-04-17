#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "fastfix/profile/artifact_builder.h"
#include "fastfix/profile/normalized_dictionary.h"
#include "fastfix/profile/profile_loader.h"

namespace fastfix::tests {

inline auto
Bytes(std::string_view text) -> std::vector<std::byte>
{
  std::vector<std::byte> bytes;
  bytes.reserve(text.size());
  for (const auto ch : text) {
    bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
  }
  return bytes;
}

inline auto
EncodeFixFrame(std::string_view body_fields, std::string_view begin_string = "FIX.4.4", char readable_delimiter = '|')
  -> std::vector<std::byte>
{
  std::string body(body_fields);
  for (auto& ch : body) {
    if (ch == readable_delimiter) {
      ch = '\x01';
    }
  }

  std::string full;
  full.append("8=");
  full.append(begin_string);
  full.push_back('\x01');
  full.append("9=");
  full.append(std::to_string(body.size()));
  full.push_back('\x01');
  full.append(body);

  std::uint32_t checksum = 0;
  for (const auto ch : full) {
    checksum += static_cast<unsigned char>(ch);
  }
  checksum %= 256U;

  std::ostringstream stream;
  stream << "10=" << std::setw(3) << std::setfill('0') << checksum << '\x01';
  full.append(stream.str());
  return Bytes(full);
}

inline auto
LoadFix44DictionaryView() -> base::Result<profile::NormalizedDictionaryView>
{
  const auto path = std::filesystem::path(FASTFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.art";
  auto profile = profile::LoadProfileArtifact(path);
  if (!profile.ok()) {
    return profile.status();
  }
  return profile::NormalizedDictionaryView::FromProfile(std::move(profile).value());
}

inline auto
LoadFix44DictionaryViewOrSkip() -> profile::NormalizedDictionaryView
{
  auto result = LoadFix44DictionaryView();
  if (!result.ok()) {
    SKIP("FIX44 artifact not available: " << result.status().message());
  }
  return std::move(result).value();
}

inline auto
BuildDictionaryViewFromDictionary(profile::NormalizedDictionary dict) -> base::Result<profile::NormalizedDictionaryView>
{
  auto artifact = profile::BuildProfileArtifact(dict);
  if (!artifact.ok()) {
    return artifact.status();
  }
  const auto artifact_path = std::filesystem::temp_directory_path() / "fastfix-test-support.art";
  const auto write_status = profile::WriteProfileArtifact(artifact_path, artifact.value());
  if (!write_status.ok()) {
    return write_status;
  }
  auto loaded = profile::LoadProfileArtifact(artifact_path);
  std::filesystem::remove(artifact_path);
  if (!loaded.ok()) {
    return loaded.status();
  }
  return profile::NormalizedDictionaryView::FromProfile(std::move(loaded).value());
}

} // namespace fastfix::tests
