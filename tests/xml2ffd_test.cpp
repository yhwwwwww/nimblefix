#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "nimblefix/codec/fix_tags.h"
#include "nimblefix/profile/dictgen_input.h"
#include "nimblefix/profile/normalized_dictionary.h"

#include "../tools/xml2ffd/xml2ffd.h"

namespace {

using namespace nimble::codec::tags;

auto
ReadFileContent(const std::filesystem::path& path) -> std::string
{
  std::ifstream stream(path);
  REQUIRE(stream.good());
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

} // namespace

TEST_CASE("xml2ffd converts QuickFIX XML to valid ffd", "[xml2ffd]")
{
  const auto project_root = std::filesystem::path(NIMBLEFIX_PROJECT_DIR);
  const auto xml_path = project_root / "tests" / "data" / "test_fix44.xml";

  const auto xml_content = ReadFileContent(xml_path);
  REQUIRE(!xml_content.empty());

  const auto ffd_text = nimble::tools::ConvertXmlToFfd(xml_content, 2001);
  REQUIRE(!ffd_text.empty());

  // Verify the output is valid ffd by loading it.
  auto dictionary = nimble::profile::LoadNormalizedDictionaryText(ffd_text);
  REQUIRE(dictionary.ok());

  const auto& dict = dictionary.value();

  SECTION("header values")
  {
    CHECK(dict.profile_id == 2001U);
    CHECK(dict.schema_hash != 0U); // auto-computed
  }

  SECTION("field definitions")
  {
    // The test XML has 37 fields.
    CHECK(dict.fields.size() == 37U);

    // Check specific field: MsgType (type string).
    auto msgtype_it =
      std::find_if(dict.fields.begin(), dict.fields.end(), [](const auto& f) { return f.tag == kMsgType; });
    REQUIRE(msgtype_it != dict.fields.end());
    CHECK(msgtype_it->name == "MsgType");
    CHECK(msgtype_it->value_type == nimble::profile::ValueType::kString);
    CHECK(msgtype_it->flags == 0U);

    // Check Price (type float).
    auto price_it = std::find_if(dict.fields.begin(), dict.fields.end(), [](const auto& f) { return f.tag == kPrice; });
    REQUIRE(price_it != dict.fields.end());
    CHECK(price_it->name == "Price");
    CHECK(price_it->value_type == nimble::profile::ValueType::kFloat);

    // Check TransactTime (type timestamp).
    auto tt_it =
      std::find_if(dict.fields.begin(), dict.fields.end(), [](const auto& f) { return f.tag == kTransactTime; });
    REQUIRE(tt_it != dict.fields.end());
    CHECK(tt_it->name == "TransactTime");
    CHECK(tt_it->value_type == nimble::profile::ValueType::kTimestamp);

    // Check GapFillFlag (type boolean).
    auto gff_it =
      std::find_if(dict.fields.begin(), dict.fields.end(), [](const auto& f) { return f.tag == kGapFillFlag; });
    REQUIRE(gff_it != dict.fields.end());
    CHECK(gff_it->name == "GapFillFlag");
    CHECK(gff_it->value_type == nimble::profile::ValueType::kBoolean);

    // Check Side (type char).
    auto side_it = std::find_if(dict.fields.begin(), dict.fields.end(), [](const auto& f) { return f.tag == kSide; });
    REQUIRE(side_it != dict.fields.end());
    CHECK(side_it->name == "Side");
    CHECK(side_it->value_type == nimble::profile::ValueType::kChar);
  }

  SECTION("message definitions")
  {
    // 9 messages: Heartbeat, TestRequest, ResendRequest, Reject, SequenceReset,
    // Logout, Logon, ExecutionReport, NewOrderSingle.
    CHECK(dict.messages.size() == 9U);

    // Check Heartbeat is admin.
    auto hb_it =
      std::find_if(dict.messages.begin(), dict.messages.end(), [](const auto& m) { return m.msg_type == "0"; });
    REQUIRE(hb_it != dict.messages.end());
    CHECK(hb_it->name == "Heartbeat");
    CHECK((hb_it->flags & 1U) == 1U); // kAdmin

    // Check Logon is admin.
    auto logon_it =
      std::find_if(dict.messages.begin(), dict.messages.end(), [](const auto& m) { return m.msg_type == "A"; });
    REQUIRE(logon_it != dict.messages.end());
    CHECK(logon_it->name == "Logon");
    CHECK((logon_it->flags & 1U) == 1U);

    // Check NewOrderSingle is app.
    auto nos_it =
      std::find_if(dict.messages.begin(), dict.messages.end(), [](const auto& m) { return m.msg_type == "D"; });
    REQUIRE(nos_it != dict.messages.end());
    CHECK(nos_it->name == "NewOrderSingle");
    CHECK((nos_it->flags & 1U) == 0U);

    // NewOrderSingle should have field rules for:
    // ClOrdID(11), Symbol(55), SecurityID(48), SecurityIDSource(22),
    // Side(54), TransactTime(60), OrderQty(38), OrdType(40), Price(44),
    // NoPartyIDs(453), NoAllocs(78).
    CHECK(nos_it->field_rules.size() >= 11U);

    // Check ClOrdID is required.
    auto clord_rule = std::find_if(
      nos_it->field_rules.begin(), nos_it->field_rules.end(), [](const auto& r) { return r.tag == kClOrdID; });
    REQUIRE(clord_rule != nos_it->field_rules.end());
    CHECK(clord_rule->required());

    // Check Price is optional.
    auto price_rule = std::find_if(
      nos_it->field_rules.begin(), nos_it->field_rules.end(), [](const auto& r) { return r.tag == kPrice; });
    REQUIRE(price_rule != nos_it->field_rules.end());
    CHECK(!price_rule->required());

    // Check ExecutionReport is app.
    auto er_it =
      std::find_if(dict.messages.begin(), dict.messages.end(), [](const auto& m) { return m.msg_type == "8"; });
    REQUIRE(er_it != dict.messages.end());
    CHECK(er_it->name == "ExecutionReport");
    CHECK((er_it->flags & 1U) == 0U);
  }

  SECTION("group definitions")
  {
    // NoPartyIDs (453) and NoAllocs (78).
    CHECK(dict.groups.size() == 2U);

    // Check Parties group.
    auto parties_it =
      std::find_if(dict.groups.begin(), dict.groups.end(), [](const auto& g) { return g.count_tag == kNoPartyIDs; });
    REQUIRE(parties_it != dict.groups.end());
    CHECK(parties_it->delimiter_tag == kPartyID);
    CHECK(parties_it->name == "NoPartyIDs");
    CHECK(parties_it->field_rules.size() == 3U);

    // Check Allocs group.
    auto allocs_it =
      std::find_if(dict.groups.begin(), dict.groups.end(), [](const auto& g) { return g.count_tag == 78U; });
    REQUIRE(allocs_it != dict.groups.end());
    CHECK(allocs_it->delimiter_tag == 79U);
    CHECK(allocs_it->name == "NoAllocs");
    CHECK(allocs_it->field_rules.size() == 2U);
  }
}
