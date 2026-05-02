#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "nimblefix/profile/dictgen_input.h"
#include "nimblefix/tools/schema_optimizer.h"

namespace {

[[nodiscard]] auto
MakeBaseDictionaryText() -> std::string
{
  return "profile_id=9001\n"
         "schema_hash=0x1\n"
         "\n"
         "field|35|MsgType|string|0\n"
         "field|11|ClOrdID|string|0\n"
         "field|14|CumQty|float|0\n"
         "field|17|ExecID|string|0\n"
         "field|31|LastPx|float|0\n"
         "field|32|LastQty|float|0\n"
         "field|37|OrderID|string|0\n"
         "field|38|OrderQty|float|0\n"
         "field|39|OrdStatus|char|0\n"
         "field|40|OrdType|char|0\n"
         "field|54|Side|char|0\n"
         "field|55|Symbol|string|0\n"
         "field|150|ExecType|char|0\n"
         "field|5000|VenueTag|string|1\n"
         "enum|54|1|Buy\n"
         "enum|54|2|Sell\n"
         "\n"
         "message|D|NewOrderSingle|0|35:r,11:r,38:r,40:r,54:r,55:r,5000:o\n"
         "message|8|ExecutionReport|0|35:r,11:o,14:r,17:r,31:o,32:o,37:r,39:r,54:r,55:o,150:r,5000:o\n";
}

[[nodiscard]] auto
MakeBaseDictionary() -> nimble::profile::NormalizedDictionary
{
  auto dictionary = nimble::profile::LoadNormalizedDictionaryText(MakeBaseDictionaryText());
  REQUIRE(dictionary.ok());
  return std::move(dictionary).value();
}

[[nodiscard]] auto
LoadGenerated(std::string_view nfd_text) -> nimble::profile::NormalizedDictionary
{
  auto dictionary = nimble::profile::LoadNormalizedDictionaryText(nfd_text);
  REQUIRE(dictionary.ok());
  return std::move(dictionary).value();
}

[[nodiscard]] auto
FindMessage(const nimble::profile::NormalizedDictionary& dictionary, std::string_view msg_type)
  -> const nimble::profile::MessageDef*
{
  const auto iterator =
    std::find_if(dictionary.messages.begin(),
                 dictionary.messages.end(),
                 [&](const nimble::profile::MessageDef& message) { return message.msg_type == msg_type; });
  return iterator == dictionary.messages.end() ? nullptr : &*iterator;
}

[[nodiscard]] auto
MessageHasTag(const nimble::profile::NormalizedDictionary& dictionary, std::string_view msg_type, std::uint32_t tag)
  -> bool
{
  const auto* message = FindMessage(dictionary, msg_type);
  if (message == nullptr) {
    return false;
  }
  return std::any_of(
    message->field_rules.begin(), message->field_rules.end(), [&](const auto& rule) { return rule.tag == tag; });
}

[[nodiscard]] auto
HasField(const nimble::profile::NormalizedDictionary& dictionary, std::uint32_t tag) -> bool
{
  return std::any_of(
    dictionary.fields.begin(), dictionary.fields.end(), [&](const auto& field) { return field.tag == tag; });
}

[[nodiscard]] auto
MakeViews(const std::vector<std::string>& messages) -> std::vector<std::string_view>
{
  std::vector<std::string_view> views;
  views.reserve(messages.size());
  for (const auto& message : messages) {
    views.push_back(message);
  }
  return views;
}

} // namespace

TEST_CASE("analyze messages counts tags per msg type", "[schema-optimizer]")
{
  const std::vector<std::string> messages{
    "8=FIX.4.4|35=D|11=ORD-1|55=AAPL|38=100|54=1|",
    "8=FIX.4.4|35=D|11=ORD-2|55=MSFT|38=200|54=2|",
    "8=FIX.4.4|35=8|11=ORD-1|17=EX-1|39=0|150=0|",
  };

  const auto report = nimble::tools::AnalyzeMessages(MakeViews(messages), '|');

  REQUIRE(report.total_messages == 3U);
  REQUIRE(report.by_msg_type.at("D").message_count == 2U);
  REQUIRE(report.by_msg_type.at("8").message_count == 1U);
  REQUIRE(report.by_msg_type.at("D").tag_counts.at(11U) == 2U);
  REQUIRE(report.by_msg_type.at("D").tag_counts.at(55U) == 2U);
  REQUIRE(report.by_msg_type.at("8").tag_counts.at(17U) == 1U);
}

TEST_CASE("analyze messages tracks max tag", "[schema-optimizer]")
{
  const std::vector<std::string> messages{ "35=D|11=ORD-1|9001=custom|55=AAPL|" };

  const auto report = nimble::tools::AnalyzeMessages(MakeViews(messages), '|');

  REQUIRE(report.global_max_tag == 9001U);
  REQUIRE(report.by_msg_type.at("D").max_tag == 9001U);
}

TEST_CASE("generate trimmed nfd excludes unobserved tags", "[schema-optimizer]")
{
  const auto base_dictionary = MakeBaseDictionary();
  const std::vector<std::string> messages{
    "35=D|11=ORD-1|38=100|54=1|55=AAPL|",
    "35=8|11=ORD-1|17=EX-1|37=O-1|39=0|150=0|",
  };
  const auto report = nimble::tools::AnalyzeMessages(MakeViews(messages), '|');

  const auto result = nimble::tools::GenerateTrimmedNfd(report, base_dictionary, {});
  const auto trimmed = LoadGenerated(result.trimmed_nfd_text);

  REQUIRE(MessageHasTag(trimmed, "D", 11U));
  REQUIRE(MessageHasTag(trimmed, "D", 38U));
  REQUIRE_FALSE(MessageHasTag(trimmed, "D", 40U));
  REQUIRE_FALSE(MessageHasTag(trimmed, "D", 5000U));
  REQUIRE_FALSE(HasField(trimmed, 40U));
  REQUIRE_FALSE(HasField(trimmed, 5000U));
}

TEST_CASE("generate trimmed nfd respects min frequency", "[schema-optimizer]")
{
  const auto base_dictionary = MakeBaseDictionary();
  std::vector<std::string> messages;
  messages.reserve(10U);
  messages.push_back("35=D|11=ORD-1|38=100|54=1|55=AAPL|5000=X|");
  for (std::uint32_t index = 2U; index <= 10U; ++index) {
    messages.push_back("35=D|11=ORD-" + std::to_string(index) + "|38=100|54=1|55=AAPL|");
  }
  const auto report = nimble::tools::AnalyzeMessages(MakeViews(messages), '|');

  nimble::tools::SchemaOptimizerConfig config;
  config.min_frequency = 0.5;
  const auto result = nimble::tools::GenerateTrimmedNfd(report, base_dictionary, config);
  const auto trimmed = LoadGenerated(result.trimmed_nfd_text);

  REQUIRE(MessageHasTag(trimmed, "D", 11U));
  REQUIRE_FALSE(MessageHasTag(trimmed, "D", 5000U));
  REQUIRE_FALSE(HasField(trimmed, 5000U));
}

TEST_CASE("generate trimmed nfd always includes required tags", "[schema-optimizer]")
{
  const auto base_dictionary = MakeBaseDictionary();
  const std::vector<std::string> messages{ "35=D|11=ORD-1|38=100|54=1|55=AAPL|" };
  const auto report = nimble::tools::AnalyzeMessages(MakeViews(messages), '|');

  nimble::tools::SchemaOptimizerConfig config;
  config.always_include_tags.insert(40U);
  const auto result = nimble::tools::GenerateTrimmedNfd(report, base_dictionary, config);
  const auto trimmed = LoadGenerated(result.trimmed_nfd_text);

  REQUIRE(MessageHasTag(trimmed, "D", 40U));
  REQUIRE(HasField(trimmed, 40U));
}

TEST_CASE("estimate layout sizes computes savings", "[schema-optimizer]")
{
  auto original = nimble::profile::LoadNormalizedDictionaryText("profile_id=1\n"
                                                                "schema_hash=0x1\n"
                                                                "field|35|MsgType|string|0\n"
                                                                "field|500|LowTag|string|0\n"
                                                                "field|5000|HighTag|string|0\n"
                                                                "message|Z|Synthetic|0|35:r,500:o,5000:o\n");
  auto trimmed = nimble::profile::LoadNormalizedDictionaryText("profile_id=1\n"
                                                               "schema_hash=0x2\n"
                                                               "field|35|MsgType|string|0\n"
                                                               "field|500|LowTag|string|0\n"
                                                               "message|Z|Synthetic|0|35:r,500:o\n");
  REQUIRE(original.ok());
  REQUIRE(trimmed.ok());

  const auto estimates = nimble::tools::EstimateLayoutSizes(original.value(), trimmed.value());

  REQUIRE(estimates.size() == 1U);
  REQUIRE(estimates[0].original_max_tag == 5000U);
  REQUIRE(estimates[0].trimmed_max_tag == 500U);
  REQUIRE(estimates[0].tag_to_slot_bytes_saved == 4500U * sizeof(int));
}

TEST_CASE("generated trimmed nfd is valid", "[schema-optimizer]")
{
  const auto base_dictionary = MakeBaseDictionary();
  const std::vector<std::string> messages{ "35=D|11=ORD-1|38=100|54=1|55=AAPL|" };
  const auto report = nimble::tools::AnalyzeMessages(MakeViews(messages), '|');

  const auto result = nimble::tools::GenerateTrimmedNfd(report, base_dictionary, {});
  auto parsed = nimble::profile::LoadNormalizedDictionaryText(result.trimmed_nfd_text);

  REQUIRE(parsed.ok());
  REQUIRE(parsed.value().profile_id == base_dictionary.profile_id);
  REQUIRE(parsed.value().schema_hash != 0U);
}

TEST_CASE("format usage report produces readable output", "[schema-optimizer]")
{
  const std::vector<std::string> messages{ "35=D|11=ORD-1|55=AAPL|" };
  const auto report = nimble::tools::AnalyzeMessages(MakeViews(messages), '|');

  const auto formatted = nimble::tools::FormatUsageReport(report);

  REQUIRE_FALSE(formatted.empty());
  REQUIRE(formatted.find("Schema Usage Report") != std::string::npos);
  REQUIRE(formatted.find("D") != std::string::npos);
}
