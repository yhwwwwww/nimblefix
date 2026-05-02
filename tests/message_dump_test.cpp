#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "nimblefix/store/durable_batch_store.h"
#include "nimblefix/tools/message_dump.h"

namespace {

[[nodiscard]] auto
SampleOrder() -> std::string
{
  return "8=FIX.4.4\x01"
         "9=75\x01"
         "35=D\x01"
         "34=7\x01"
         "49=BUY\x01"
         "56=SELL\x01"
         "52=20260429-09:30:00.000\x01"
         "11=ABC-1\x01"
         "55=AAPL\x01"
         "336=42\x01"
         "10=123\x01";
}

[[nodiscard]] auto
Payload(std::string_view text) -> std::vector<std::byte>
{
  std::vector<std::byte> bytes;
  bytes.reserve(text.size());
  for (const auto ch : text) {
    bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
  }
  return bytes;
}

[[nodiscard]] auto
QuoteShellPath(const std::filesystem::path& path) -> std::string
{
  std::string quoted = "'";
  for (const auto ch : path.string()) {
    if (ch == '\'') {
      quoted.append("'\\''");
    } else {
      quoted.push_back(ch);
    }
  }
  quoted.push_back('\'');
  return quoted;
}

[[nodiscard]] auto
ReadTextFile(const std::filesystem::path& path) -> std::string
{
  std::ifstream input(path);
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

} // namespace

TEST_CASE("message dump formats FIX readable output", "[message-dump]")
{
  const auto raw_fix = SampleOrder();
  const auto readable = nimble::tools::FormatFixReadable(raw_fix);

  REQUIRE(readable.find("8=FIX.4.4|") != std::string::npos);
  REQUIRE(readable.find("35=D|") != std::string::npos);
  REQUIRE(readable.find("11=ABC-1|") != std::string::npos);
  REQUIRE(readable.find('\x01') == std::string::npos);
}

TEST_CASE("message dump formats FIX JSON output", "[message-dump]")
{
  const auto raw_fix = SampleOrder();
  const auto json = nimble::tools::FormatFixJson(raw_fix);

  REQUIRE(json.front() == '{');
  REQUIRE(json.back() == '}');
  REQUIRE(json.find("\"BeginString\":\"FIX.4.4\"") != std::string::npos);
  REQUIRE(json.find("\"MsgType\":\"D\"") != std::string::npos);
  REQUIRE(json.find("\"ClOrdID\":\"ABC-1\"") != std::string::npos);
  REQUIRE(json.find("\"336\":\"42\"") != std::string::npos);
}

TEST_CASE("message dump escapes JSON values", "[message-dump]")
{
  const std::string raw_fix = "35=D\x01"
                              "58=quote \" and slash \\\x01";
  const auto json = nimble::tools::FormatFixJson(raw_fix);

  REQUIRE(json.find("\"Text\":\"quote \\\" and slash \\\\\"") != std::string::npos);
}

TEST_CASE("message dump matches filter combinations", "[message-dump]")
{
  const auto raw_fix = SampleOrder();

  nimble::tools::DumpFilter filter;
  filter.msg_type = "D";
  filter.session_id = 42U;
  filter.tag = 11U;
  filter.tag_value = "ABC-1";
  filter.seq_from = 7U;
  filter.seq_to = 9U;
  REQUIRE(nimble::tools::MatchesFilter(raw_fix, filter));

  filter.msg_type = "8";
  REQUIRE_FALSE(nimble::tools::MatchesFilter(raw_fix, filter));

  filter.msg_type = "D";
  filter.session_id = 43U;
  REQUIRE(nimble::tools::MatchesFilter(raw_fix, filter));

  filter.session_id = 42U;
  filter.tag_value = "NOPE";
  REQUIRE_FALSE(nimble::tools::MatchesFilter(raw_fix, filter));

  filter.tag_value = "ABC-1";
  filter.seq_from = 8U;
  REQUIRE_FALSE(nimble::tools::MatchesFilter(raw_fix, filter));
}

TEST_CASE("message dump handles tag presence and malformed filters", "[message-dump]")
{
  const auto raw_fix = SampleOrder();

  nimble::tools::DumpFilter has_symbol;
  has_symbol.tag = 55U;
  REQUIRE(nimble::tools::MatchesFilter(raw_fix, has_symbol));

  nimble::tools::DumpFilter missing_tag;
  missing_tag.tag = 9999U;
  REQUIRE_FALSE(nimble::tools::MatchesFilter(raw_fix, missing_tag));

  nimble::tools::DumpFilter tag_value_without_tag;
  tag_value_without_tag.tag_value = "AAPL";
  REQUIRE_FALSE(nimble::tools::MatchesFilter(raw_fix, tag_value_without_tag));
}

TEST_CASE("message dump edge cases", "[message-dump]")
{
  REQUIRE(nimble::tools::FormatFixReadable("").empty());
  REQUIRE(nimble::tools::FormatFixJson("") == "{}");
  REQUIRE(nimble::tools::FormatFixReadable("not-a-fix-message") == "not-a-fix-message");
  REQUIRE(nimble::tools::FormatFixJson("not-a-fix-message") == "{}");

  nimble::tools::DumpFilter msg_type_filter;
  msg_type_filter.msg_type = "D";
  REQUIRE_FALSE(nimble::tools::MatchesFilter("not-a-fix-message", msg_type_filter));

  nimble::tools::DumpFilter seq_filter;
  seq_filter.seq_from = 1U;
  REQUIRE_FALSE(nimble::tools::MatchesFilter("35=D|34=bad|", seq_filter));
}

TEST_CASE("msgdump CLI reads durable store and applies tag-value filter", "[message-dump]")
{
  const auto root = std::filesystem::temp_directory_path() / "nimblefix-msgdump-cli-test";
  const auto output_path = std::filesystem::temp_directory_path() / "nimblefix-msgdump-cli-test.out";
  const auto error_path = std::filesystem::temp_directory_path() / "nimblefix-msgdump-cli-test.err";
  std::filesystem::remove_all(root);
  std::filesystem::remove(output_path);
  std::filesystem::remove(error_path);

  {
    nimble::store::DurableBatchSessionStore store(root,
                                                  nimble::store::DurableBatchStoreOptions{
                                                    .flush_threshold = 1U,
                                                    .rollover_mode = nimble::store::DurableStoreRolloverMode::kExternal,
                                                  });
    REQUIRE(store.Open().ok());
    REQUIRE(store
              .SaveOutbound(nimble::store::MessageRecord{
                .session_id = 7001U,
                .seq_num = 1U,
                .timestamp_ns = 10U,
                .flags = 0U,
                .payload = Payload("8=FIX.4.4\x01"
                                   "35=D\x01"
                                   "34=1\x01"
                                   "11=ABC\x01"
                                   "10=000\x01"),
              })
              .ok());
    REQUIRE(store
              .SaveOutbound(nimble::store::MessageRecord{
                .session_id = 7001U,
                .seq_num = 2U,
                .timestamp_ns = 20U,
                .flags = 0U,
                .payload = Payload("8=FIX.4.4\x01"
                                   "35=D\x01"
                                   "34=2\x01"
                                   "11=NOPE\x01"
                                   "10=000\x01"),
              })
              .ok());
    REQUIRE(store
              .SaveInbound(nimble::store::MessageRecord{
                .session_id = 7001U,
                .seq_num = 1U,
                .timestamp_ns = 30U,
                .flags = 0U,
                .payload = Payload("8=FIX.4.4\x01"
                                   "35=D\x01"
                                   "34=1\x01"
                                   "11=ABC\x01"
                                   "55=MSFT\x01"
                                   "10=000\x01"),
              })
              .ok());
    REQUIRE(store
              .SaveRecoveryState(nimble::store::SessionRecoveryState{
                .session_id = 7001U,
                .next_in_seq = 2U,
                .next_out_seq = 3U,
                .last_inbound_ns = 30U,
                .last_outbound_ns = 20U,
                .active = true,
              })
              .ok());
    REQUIRE(store.Flush().ok());
  }

  const auto self = std::filesystem::read_symlink("/proc/self/exe");
  const auto msgdump = self.parent_path() / "nimblefix-msgdump";
  REQUIRE(std::filesystem::exists(msgdump));

  const auto command = QuoteShellPath(msgdump) + " --store " + QuoteShellPath(root) +
                       " --session-id 7001 --include-admin --format json --tag-value 11=ABC --direction out > " +
                       QuoteShellPath(output_path) + " 2> " + QuoteShellPath(error_path);
  REQUIRE(std::system(command.c_str()) == 0);

  const auto output = ReadTextFile(output_path);
  REQUIRE(output.find("\"ClOrdID\":\"ABC\"") != std::string::npos);
  REQUIRE(output.find("\"ClOrdID\":\"NOPE\"") == std::string::npos);
  REQUIRE(output.find("\"Symbol\":\"MSFT\"") == std::string::npos);

  std::filesystem::remove_all(root);
  std::filesystem::remove(output_path);
  std::filesystem::remove(error_path);
}
