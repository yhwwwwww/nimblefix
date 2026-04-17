#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "fastfix/codec/fix_codec.h"
#include "fastfix/codec/fix_tags.h"
#include "fastfix/profile/normalized_dictionary.h"
#include "fastfix/profile/profile_loader.h"
#include "fastfix/session/admin_protocol.h"
#include "fastfix/store/memory_store.h"

namespace {

enum class FuzzMode
{
  kCodec,
  kAdmin,
};

struct FuzzStats
{
  std::size_t peeked{ 0 };
  std::size_t decoded{ 0 };
  std::size_t flagged{ 0 };
  std::size_t application_inputs{ 0 };
  std::size_t admin_events{ 0 };
  std::size_t admin_errors{ 0 };
  std::size_t outbound_frames{ 0 };
  std::size_t application_messages{ 0 };
  std::size_t timer_callbacks{ 0 };
};

auto
IsApplicationMsgType(std::string_view msg_type) -> bool
{
  return msg_type != "0" && msg_type != "1" && msg_type != "2" && msg_type != "3" && msg_type != "4" &&
         msg_type != "5" && msg_type != "A";
}

auto
PrintUsage() -> void
{
  std::cout << "usage: fastfix-fuzz-codec --artifact <profile.art> --input "
               "<file-or-directory>"
               " [--mode codec|admin]\n";
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

auto
ToBytes(std::string_view text) -> std::vector<std::byte>
{
  std::vector<std::byte> bytes;
  bytes.reserve(text.size());
  for (const auto ch : text) {
    bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
  }
  return bytes;
}

auto
WrapBodyFields(std::string_view body_fields) -> std::vector<std::byte>
{
  std::string body(body_fields);
  for (auto& ch : body) {
    if (ch == '|') {
      ch = '\x01';
    }
  }

  std::string full;
  full.append(fastfix::codec::tags::kBeginStringPrefix);
  full.append("FIX.4.4");
  full.push_back('\x01');
  full.append(fastfix::codec::tags::kBodyLengthPrefix);
  full.append(std::to_string(body.size()));
  full.push_back('\x01');
  full.append(body);

  std::uint32_t checksum = 0;
  for (const auto ch : full) {
    checksum += static_cast<unsigned char>(ch);
  }
  checksum %= 256U;

  std::array<char, 4> digits{};
  digits[0] = static_cast<char>('0' + ((checksum / 100U) % 10U));
  digits[1] = static_cast<char>('0' + ((checksum / 10U) % 10U));
  digits[2] = static_cast<char>('0' + (checksum % 10U));
  full.append(fastfix::codec::tags::kCheckSumPrefix);
  full.append(digits.data(), 3U);
  full.push_back('\x01');
  return ToBytes(full);
}

auto
NormalizeFrame(std::string text) -> std::vector<std::byte>
{
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
    text.pop_back();
  }
  if (text.rfind(fastfix::codec::tags::kBeginStringPrefix, 0) == 0) {
    for (auto& ch : text) {
      if (ch == '|') {
        ch = '\x01';
      }
    }
    return ToBytes(text);
  }
  return WrapBodyFields(text);
}

auto
TrimLine(std::string_view line) -> std::string_view
{
  while (!line.empty() && (line.front() == ' ' || line.front() == '\t' || line.front() == '\r')) {
    line.remove_prefix(1U);
  }
  while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
    line.remove_suffix(1U);
  }
  return line;
}

auto
ProcessCodecInput(std::string text, const fastfix::profile::NormalizedDictionaryView& dictionary, FuzzStats* stats)
  -> void
{
  const auto bytes = NormalizeFrame(std::move(text));
  if (fastfix::codec::PeekSessionHeader(bytes).ok()) {
    ++stats->peeked;
  }
  auto result = fastfix::codec::DecodeFixMessage(bytes, dictionary);
  if (!result.ok()) {
    return;
  }
  ++stats->decoded;
  if (IsApplicationMsgType(result.value().header.msg_type)) {
    ++stats->application_inputs;
  }
  if (result.value().validation_issue.present()) {
    ++stats->flagged;
  }
}

auto
ProcessAdminInput(std::string text, const fastfix::profile::NormalizedDictionaryView& dictionary, FuzzStats* stats)
  -> fastfix::base::Status
{
  fastfix::store::MemorySessionStore store;
  fastfix::session::AdminProtocol protocol(
    fastfix::session::AdminProtocolConfig{
      .session =
        fastfix::session::SessionConfig{
          .session_id = 90'001U,
          .key = fastfix::session::SessionKey{ "FIX.4.4", "SELL", "BUY" },
          .profile_id = dictionary.profile().header().profile_id,
          .heartbeat_interval_seconds = 30U,
          .is_initiator = false,
        },
      .begin_string = "FIX.4.4",
      .sender_comp_id = "SELL",
      .target_comp_id = "BUY",
      .heartbeat_interval_seconds = 30U,
    },
    dictionary,
    &store);

  auto connected = protocol.OnTransportConnected(1U);
  if (!connected.ok()) {
    return connected.status();
  }

  std::uint64_t timestamp_ns = 1U;
  std::size_t line_begin = 0U;
  while (line_begin <= text.size()) {
    auto line_end = text.find('\n', line_begin);
    if (line_end == std::string::npos) {
      line_end = text.size();
    }
    auto line = TrimLine(std::string_view(text).substr(line_begin, line_end - line_begin));
    line_begin = line_end + 1U;

    if (line.empty() || line.front() == '#') {
      continue;
    }

    if (line == "CONNECT") {
      timestamp_ns += 1U;
      auto event = protocol.OnTransportConnected(timestamp_ns);
      if (!event.ok()) {
        ++stats->admin_errors;
        continue;
      }
      ++stats->admin_events;
      stats->outbound_frames += event.value().outbound_frames.size();
      continue;
    }

    if (line == "DISCONNECT") {
      auto status = protocol.OnTransportClosed();
      if (!status.ok()) {
        ++stats->admin_errors;
      }
      continue;
    }

    if (line.rfind("TIMER ", 0) == 0) {
      std::uint64_t timer_ns = 0U;
      try {
        timer_ns = static_cast<std::uint64_t>(std::stoull(std::string(line.substr(6U))));
      } catch (...) {
        ++stats->admin_errors;
        continue;
      }
      timestamp_ns = std::max(timestamp_ns + 1U, timer_ns);
      auto event = protocol.OnTimer(timestamp_ns);
      ++stats->timer_callbacks;
      if (!event.ok()) {
        ++stats->admin_errors;
        continue;
      }
      ++stats->admin_events;
      stats->outbound_frames += event.value().outbound_frames.size();
      for (const auto& outbound : event.value().outbound_frames) {
        auto decoded = fastfix::codec::DecodeFixMessage(outbound.bytes, dictionary);
        if (!decoded.ok()) {
          return decoded.status();
        }
      }
      continue;
    }

    const auto bytes = NormalizeFrame(std::string(line));
    if (fastfix::codec::PeekSessionHeader(bytes).ok()) {
      ++stats->peeked;
    }

    auto decoded = fastfix::codec::DecodeFixMessage(bytes, dictionary);
    if (decoded.ok()) {
      ++stats->decoded;
      if (IsApplicationMsgType(decoded.value().header.msg_type)) {
        ++stats->application_inputs;
      }
      if (decoded.value().validation_issue.present()) {
        ++stats->flagged;
      }
    }

    timestamp_ns += 1U;
    auto event = protocol.OnInbound(bytes, timestamp_ns);
    if (!event.ok()) {
      ++stats->admin_errors;
      continue;
    }

    ++stats->admin_events;
    stats->application_messages += event.value().application_messages.size();
    stats->outbound_frames += event.value().outbound_frames.size();
    for (const auto& outbound : event.value().outbound_frames) {
      auto outbound_decoded = fastfix::codec::DecodeFixMessage(outbound.bytes, dictionary);
      if (!outbound_decoded.ok()) {
        return outbound_decoded.status();
      }
    }
  }

  return fastfix::base::Status::Ok();
}

} // namespace

int
main(int argc, char** argv)
{
  std::filesystem::path artifact;
  std::filesystem::path input;
  auto mode = FuzzMode::kCodec;
  for (int index = 1; index < argc; ++index) {
    const std::string_view arg(argv[index]);
    if (arg == "--artifact" && index + 1 < argc) {
      artifact = argv[++index];
      continue;
    }
    if (arg == "--input" && index + 1 < argc) {
      input = argv[++index];
      continue;
    }
    if (arg == "--mode" && index + 1 < argc) {
      const std::string_view value(argv[++index]);
      if (value == "codec") {
        mode = FuzzMode::kCodec;
        continue;
      }
      if (value == "admin") {
        mode = FuzzMode::kAdmin;
        continue;
      }
      PrintUsage();
      return 1;
    }
    PrintUsage();
    return 1;
  }

  if (artifact.empty() || input.empty()) {
    PrintUsage();
    return 1;
  }

  auto profile = fastfix::profile::LoadProfileArtifact(artifact);
  if (!profile.ok()) {
    std::cerr << profile.status().message() << '\n';
    return 1;
  }
  auto dictionary = fastfix::profile::NormalizedDictionaryView::FromProfile(std::move(profile).value());
  if (!dictionary.ok()) {
    std::cerr << dictionary.status().message() << '\n';
    return 1;
  }

  const auto files = CollectFiles(input);
  FuzzStats stats;
  for (const auto& file : files) {
    if (mode == FuzzMode::kCodec) {
      ProcessCodecInput(ReadText(file), dictionary.value(), &stats);
      continue;
    }

    auto status = ProcessAdminInput(ReadText(file), dictionary.value(), &stats);
    if (!status.ok()) {
      std::cerr << file << ": " << status.message() << '\n';
      return 1;
    }
  }

  if (mode == FuzzMode::kCodec) {
    std::cout << "processed " << files.size() << " wire inputs, peeked " << stats.peeked << ", decoded "
              << stats.decoded << ", flagged " << stats.flagged << '\n';
    return 0;
  }

  std::cout << "processed " << files.size() << " admin corpora, peeked " << stats.peeked << ", decoded "
            << stats.decoded << ", flagged " << stats.flagged << ", app-input " << stats.application_inputs
            << ", events " << stats.admin_events << ", app " << stats.application_messages << ", outbound "
            << stats.outbound_frames << ", timers " << stats.timer_callbacks << ", errors " << stats.admin_errors
            << '\n';
  return 0;
}