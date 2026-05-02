#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "nimblefix/codec/fix_tags.h"
#include "nimblefix/store/durable_batch_store.h"
#include "nimblefix/store/session_store.h"
#include "nimblefix/tools/message_dump.h"

namespace {

enum class DirectionFilter : std::uint8_t
{
  kBoth = 0,
  kInbound,
  kOutbound,
};

struct Options
{
  std::filesystem::path store_path;
  nimble::tools::DumpOptions dump;
  DirectionFilter direction{ DirectionFilter::kBoth };
};

struct DumpCandidate
{
  std::uint64_t session_id{ 0 };
  std::uint32_t sequence_number{ 0 };
  std::uint64_t timestamp_ns{ 0 };
  std::uint16_t flags{ 0 };
  std::string direction;
  std::string raw_fix;
};

auto
PrintUsage() -> void
{
  std::cout << "Usage: nimblefix-msgdump [options]\n"
               "  --store <path>         Path to durable store directory\n"
               "  --format <fix|json>    Output format (default: fix)\n"
               "  --msg-type <type>      Filter by MsgType\n"
               "  --session-id <id>      Filter by runtime session ID\n"
               "  --tag <tag>            Filter by tag presence\n"
               "  --tag-value <t=v>      Filter by tag=value\n"
               "  --seq-from <n>         Start sequence number\n"
               "  --seq-to <n>           End sequence number\n"
               "  --include-admin        Include admin messages\n"
               "  --limit <n>            Max messages to output\n"
               "  --direction <in|out>   Filter by direction\n";
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
BytesToString(std::span<const std::byte> bytes) -> std::string
{
  if (bytes.empty()) {
    return {};
  }
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

[[nodiscard]] auto
ValidateDurableStoreRoot(const std::filesystem::path& store_path) -> bool
{
  std::error_code error;
  if (!std::filesystem::is_directory(store_path, error) || error) {
    return false;
  }
  return std::filesystem::is_regular_file(store_path / "active.log", error) && !error &&
         std::filesystem::is_regular_file(store_path / "active.out.idx", error) && !error &&
         std::filesystem::is_regular_file(store_path / "recovery.log", error) && !error;
}

[[nodiscard]] auto
IsAdminMsgType(std::string_view msg_type) -> bool
{
  return msg_type == nimble::codec::msg_types::kHeartbeat || msg_type == nimble::codec::msg_types::kTestRequest ||
         msg_type == nimble::codec::msg_types::kResendRequest || msg_type == nimble::codec::msg_types::kReject ||
         msg_type == nimble::codec::msg_types::kSequenceReset || msg_type == nimble::codec::msg_types::kLogout ||
         msg_type == nimble::codec::msg_types::kLogon;
}

[[nodiscard]] auto
ExtractTag(std::string_view raw_fix, std::uint32_t wanted_tag) -> std::optional<std::string>
{
  constexpr char kSoh = '\x01';
  constexpr char kReadableDelimiter = '|';
  std::size_t begin = 0U;
  while (begin < raw_fix.size()) {
    std::size_t end = begin;
    while (end < raw_fix.size() && raw_fix[end] != kSoh && raw_fix[end] != kReadableDelimiter) {
      ++end;
    }

    const auto field = raw_fix.substr(begin, end - begin);
    const auto equals = field.find('=');
    if (equals != std::string_view::npos && equals > 0U) {
      std::uint32_t tag = 0U;
      try {
        tag = static_cast<std::uint32_t>(std::stoul(std::string(field.substr(0U, equals))));
      } catch (...) {
        tag = 0U;
      }
      if (tag == wanted_tag) {
        return std::string(field.substr(equals + 1U));
      }
    }

    if (end == raw_fix.size()) {
      break;
    }
    begin = end + 1U;
  }
  return std::nullopt;
}

[[nodiscard]] auto
ParseFormat(std::string_view token) -> std::optional<nimble::tools::DumpFormat>
{
  if (token == "fix") {
    return nimble::tools::DumpFormat::kFixReadable;
  }
  if (token == "json") {
    return nimble::tools::DumpFormat::kJson;
  }
  return std::nullopt;
}

[[nodiscard]] auto
ParseDirection(std::string_view token) -> std::optional<DirectionFilter>
{
  if (token == "in" || token == "inbound") {
    return DirectionFilter::kInbound;
  }
  if (token == "out" || token == "outbound") {
    return DirectionFilter::kOutbound;
  }
  return std::nullopt;
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
    if (arg == "--format" && index + 1 < argc) {
      auto parsed = ParseFormat(argv[++index]);
      if (!parsed.has_value()) {
        return std::nullopt;
      }
      options.dump.format = *parsed;
      continue;
    }
    if (arg == "--msg-type" && index + 1 < argc) {
      options.dump.filter.msg_type = argv[++index];
      continue;
    }
    if (arg == "--session-id" && index + 1 < argc) {
      options.dump.filter.session_id = static_cast<std::uint64_t>(std::stoull(argv[++index]));
      continue;
    }
    if (arg == "--tag" && index + 1 < argc) {
      options.dump.filter.tag = static_cast<std::uint32_t>(std::stoul(argv[++index]));
      continue;
    }
    if (arg == "--tag-value" && index + 1 < argc) {
      options.dump.filter.tag_value = argv[++index];
      continue;
    }
    if (arg == "--seq-from" && index + 1 < argc) {
      options.dump.filter.seq_from = static_cast<std::uint32_t>(std::stoul(argv[++index]));
      continue;
    }
    if (arg == "--seq-to" && index + 1 < argc) {
      options.dump.filter.seq_to = static_cast<std::uint32_t>(std::stoul(argv[++index]));
      continue;
    }
    if (arg == "--include-admin") {
      options.dump.include_admin = true;
      continue;
    }
    if (arg == "--limit" && index + 1 < argc) {
      options.dump.limit = static_cast<std::uint32_t>(std::stoul(argv[++index]));
      continue;
    }
    if (arg == "--direction" && index + 1 < argc) {
      auto direction = ParseDirection(argv[++index]);
      if (!direction.has_value()) {
        return std::nullopt;
      }
      options.direction = *direction;
      continue;
    }
    return std::nullopt;
  }

  if (options.store_path.empty()) {
    return std::nullopt;
  }
  if (options.dump.filter.tag_value.has_value() && !options.dump.filter.tag.has_value()) {
    return std::nullopt;
  }
  return options;
}

auto
AppendCandidates(std::vector<DumpCandidate>* candidates,
                 const std::vector<nimble::store::MessageRecord>& records,
                 std::string_view direction) -> void
{
  candidates->reserve(candidates->size() + records.size());
  for (const auto& record : records) {
    candidates->push_back(DumpCandidate{
      .session_id = record.session_id,
      .sequence_number = record.seq_num,
      .timestamp_ns = record.timestamp_ns,
      .flags = record.flags,
      .direction = std::string(direction),
      .raw_fix = BytesToString(record.payload),
    });
  }
}

[[nodiscard]] auto
BuildCandidateFilter(const Options& options) -> nimble::tools::DumpFilter
{
  nimble::tools::DumpFilter filter = options.dump.filter;
  filter.session_id.reset();
  filter.seq_from.reset();
  filter.seq_to.reset();
  return filter;
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

  if (!ValidateDurableStoreRoot(options.store_path)) {
    std::cerr << "--store must point to an existing durable batch store root\n";
    return 1;
  }

  if (!options.dump.filter.session_id.has_value()) {
    std::cerr << "--session-id is required because SessionStore does not expose session enumeration\n";
    return 1;
  }

  nimble::store::DurableBatchSessionStore store(options.store_path,
                                                nimble::store::DurableBatchStoreOptions{
                                                  .flush_threshold = 1U,
                                                  .rollover_mode = nimble::store::DurableStoreRolloverMode::kExternal,
                                                });
  auto status = store.Open();
  if (!status.ok()) {
    std::cerr << status.message() << '\n';
    return 1;
  }

  const auto session_id = options.dump.filter.session_id.value();

  std::vector<DumpCandidate> candidates;
  auto recovery = store.LoadRecoveryState(session_id);
  if (!recovery.ok()) {
    std::cerr << recovery.status().message() << '\n';
    return 1;
  }

  const auto seq_from = options.dump.filter.seq_from.value_or(1U);
  const auto out_seq_to = options.dump.filter.seq_to.value_or(
    recovery.value().next_out_seq == 0U ? 0U : recovery.value().next_out_seq - 1U);
  const auto in_seq_to =
    options.dump.filter.seq_to.value_or(recovery.value().next_in_seq == 0U ? 0U : recovery.value().next_in_seq - 1U);

  if (options.direction != DirectionFilter::kInbound && out_seq_to != 0U && seq_from <= out_seq_to) {
    auto outbound = store.LoadOutboundRange(session_id, seq_from, out_seq_to);
    if (!outbound.ok()) {
      std::cerr << outbound.status().message() << '\n';
      return 1;
    }
    AppendCandidates(&candidates, outbound.value(), "outbound");
  }
  if (options.direction != DirectionFilter::kOutbound && in_seq_to != 0U && seq_from <= in_seq_to) {
    auto inbound = store.LoadInboundRange(session_id, seq_from, in_seq_to);
    if (!inbound.ok()) {
      std::cerr << inbound.status().message() << '\n';
      return 1;
    }
    AppendCandidates(&candidates, inbound.value(), "inbound");
  }

  std::stable_sort(candidates.begin(), candidates.end(), [](const DumpCandidate& lhs, const DumpCandidate& rhs) {
    if (lhs.timestamp_ns != rhs.timestamp_ns) {
      return lhs.timestamp_ns < rhs.timestamp_ns;
    }
    if (lhs.sequence_number != rhs.sequence_number) {
      return lhs.sequence_number < rhs.sequence_number;
    }
    return lhs.direction < rhs.direction;
  });

  const auto candidate_filter = BuildCandidateFilter(options);
  std::uint32_t emitted = 0U;
  for (const auto& candidate : candidates) {
    if (options.dump.limit != 0U && emitted >= options.dump.limit) {
      break;
    }

    const auto msg_type = ExtractTag(candidate.raw_fix, nimble::codec::tags::kMsgType).value_or(std::string{});
    if (!options.dump.include_admin && IsAdminMsgType(msg_type)) {
      continue;
    }
    if (!nimble::tools::MatchesFilter(candidate.raw_fix, candidate_filter)) {
      continue;
    }

    std::cout << nimble::tools::FormatFixMessage(candidate.raw_fix, options.dump.format) << '\n';
    ++emitted;
  }

  return 0;
}
