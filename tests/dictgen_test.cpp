#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>

#include "nimblefix/codec/fix_codec.h"
#include "nimblefix/codec/fix_tags.h"
#include "nimblefix/message/message_builder.h"
#include "nimblefix/profile/artifact_builder.h"
#include "nimblefix/profile/dictgen_input.h"
#include "nimblefix/profile/normalized_dictionary.h"
#include "nimblefix/profile/profile_loader.h"

#include "test_support.h"

TEST_CASE("dictgen", "[dictgen]")
{
  const auto ToReadableFrame = [](std::span<const std::byte> bytes) {
    std::string text;
    text.reserve(bytes.size());
    for (const auto value : bytes) {
      const auto ch = static_cast<char>(std::to_integer<unsigned char>(value));
      text.push_back(ch == '\x01' ? '|' : ch);
    }
    return text;
  };

  const auto project_root = std::filesystem::path(NIMBLEFIX_PROJECT_DIR);
  const auto ffd_path = project_root / "build" / "bench" / "quickfix_FIX44.nfd";

  auto dictionary = nimble::profile::LoadNormalizedDictionaryFile(ffd_path);
  REQUIRE(dictionary.ok());
  REQUIRE(dictionary.value().profile_id == 4400U);
  REQUIRE(dictionary.value().fields.size() >= 100U);

  auto artifact = nimble::profile::BuildProfileArtifact(dictionary.value());
  REQUIRE(artifact.ok());

  const auto artifact_path = std::filesystem::temp_directory_path() / "nimblefix-dictgen-test.nfa";
  const auto write_status = nimble::profile::WriteProfileArtifact(artifact_path, artifact.value());
  REQUIRE(write_status.ok());

  auto loaded = nimble::profile::LoadProfileArtifact(artifact_path);
  REQUIRE(loaded.ok());
  auto view = nimble::profile::NormalizedDictionaryView::FromProfile(std::move(loaded).value());
  REQUIRE(view.ok());

  const auto* message = view.value().find_message("D");
  REQUIRE(message != nullptr);
  REQUIRE(view.value().message_field_rules(*message).size() >= 5U);

  // Verify NoPartyIDs is present in the NewOrderSingle message.
  const auto& field_rules = view.value().message_field_rules(*message);
  bool has_parties = false;
  for (const auto& rule : field_rules) {
    if (rule.tag == nimble::codec::tags::kNoPartyIDs) {
      has_parties = true;
      break;
    }
  }
  REQUIRE(has_parties);

  // Encode a NewOrderSingle with party group and verify roundtrip.
  nimble::codec::EncodeOptions options;
  options.begin_string = "FIX.4.4";
  options.sender_comp_id = "BUY";
  options.target_comp_id = "SELL";
  options.msg_seq_num = 1U;
  options.sending_time = "20260404-12:00:00.000";

  nimble::message::MessageBuilder builder{ "D" };
  builder.reserve_fields(4U).reserve_groups(1U).reserve_group_entries(nimble::codec::tags::kNoPartyIDs, 1U);
  builder.set_string(nimble::codec::tags::kSenderCompID, "BUY").set_string(nimble::codec::tags::kTargetCompID, "SELL");
  auto party = builder.add_group_entry(nimble::codec::tags::kNoPartyIDs);
  party.set_string(nimble::codec::tags::kPartyID, "PTY1")
    .set_char(nimble::codec::tags::kPartyIDSource, 'D')
    .set_int(nimble::codec::tags::kPartyRole, 7);

  nimble::codec::EncodeBuffer buffer;
  REQUIRE(builder.encode_to_buffer(view.value(), options, &buffer).ok());
  const auto readable = ToReadableFrame(buffer.bytes());
  const auto parties = readable.find("|453=1|");
  REQUIRE(parties != std::string::npos);

  std::filesystem::remove(artifact_path);
}
