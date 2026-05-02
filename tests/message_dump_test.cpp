#include <catch2/catch_test_macros.hpp>

#include <string>

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
