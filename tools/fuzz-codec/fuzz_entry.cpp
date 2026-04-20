// libFuzzer entry point for codec mutation-based fuzzing.
//
// Build:
//   xmake build nimblefix-fuzz-codec-libfuzzer
//
// Run:
//   ./build/linux/x86_64/release/nimblefix-fuzz-codec-libfuzzer corpus_dir/
//
// The entry point feeds arbitrary bytes through PeekSessionHeaderView() and
// DecodeFixMessageView() to find crashes, hangs, or assertion failures in the
// codec parsing paths.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include "nimblefix/codec/fix_codec.h"
#include "nimblefix/codec/fix_tags.h"
#include "nimblefix/profile/artifact_builder.h"
#include "nimblefix/profile/normalized_dictionary.h"
#include "nimblefix/profile/profile_loader.h"

namespace {

using namespace nimble::codec::tags;

// Lazily initialized dictionary shared across all fuzz iterations.
struct FuzzState
{
  nimble::profile::LoadedProfile profile;
  std::optional<nimble::profile::NormalizedDictionaryView> dictionary;
  bool initialized{ false };
};

auto
GetFuzzState() -> FuzzState&
{
  static FuzzState state;
  if (!state.initialized) {
    // Build a minimal dictionary inline — no external artifact file needed.
    nimble::profile::NormalizedDictionary dict;
    dict.profile_id = 9001U;
    dict.schema_hash = 0x9001900190019001ULL;
    dict.fields = {
      { kMsgType, "MsgType", nimble::profile::ValueType::kString, 0U },
      { kSenderCompID, "SenderCompID", nimble::profile::ValueType::kString, 0U },
      { kTargetCompID, "TargetCompID", nimble::profile::ValueType::kString, 0U },
      { kClOrdID, "ClOrdID", nimble::profile::ValueType::kString, 0U },
      { kSymbol, "Symbol", nimble::profile::ValueType::kString, 0U },
      { kNoSides, "NoSides", nimble::profile::ValueType::kInt, 0U },
      { kSide, "Side", nimble::profile::ValueType::kChar, 0U },
      { kNoPartyIDs, "NoPartyIDs", nimble::profile::ValueType::kInt, 0U },
      { kPartyID, "PartyID", nimble::profile::ValueType::kString, 0U },
      { kEncryptMethod, "EncryptMethod", nimble::profile::ValueType::kInt, 0U },
      { kHeartBtInt, "HeartBtInt", nimble::profile::ValueType::kInt, 0U },
    };
    dict.messages = {
        nimble::profile::MessageDef{
            .msg_type = "D",
            .name = "NewOrderSingle",
            .field_rules =
                {
                    {kMsgType, static_cast<std::uint32_t>(nimble::profile::FieldRuleFlags::kRequired)},
                    {kClOrdID, static_cast<std::uint32_t>(nimble::profile::FieldRuleFlags::kRequired)},
                    {kSymbol, static_cast<std::uint32_t>(nimble::profile::FieldRuleFlags::kRequired)},
                    {kNoSides, 0U},
                },
            .flags = 0U,
        },
        nimble::profile::MessageDef{
            .msg_type = "0",
            .name = "Heartbeat",
            .field_rules = {},
            .flags = 0U,
        },
        nimble::profile::MessageDef{
            .msg_type = "A",
            .name = "Logon",
            .field_rules = {},
            .flags = 0U,
        },
    };
    dict.groups = {
        nimble::profile::GroupDef{
            .count_tag = kNoSides,
            .delimiter_tag = kSide,
            .name = "Sides",
            .field_rules =
                {
                    {kSide, static_cast<std::uint32_t>(nimble::profile::FieldRuleFlags::kRequired)},
                    {kNoPartyIDs, 0U},
                },
            .flags = 0U,
        },
        nimble::profile::GroupDef{
            .count_tag = kNoPartyIDs,
            .delimiter_tag = kPartyID,
            .name = "Parties",
            .field_rules =
                {
                    {kPartyID, static_cast<std::uint32_t>(nimble::profile::FieldRuleFlags::kRequired)},
                },
            .flags = 0U,
        },
    };

    auto artifact = nimble::profile::BuildProfileArtifact(dict);
    if (!artifact.ok()) {
      __builtin_trap();
    }
    const auto artifact_path = std::filesystem::temp_directory_path() / "nimblefix-fuzz-codec-libfuzzer.art";
    auto write_status = nimble::profile::WriteProfileArtifact(artifact_path, artifact.value());
    if (!write_status.ok()) {
      __builtin_trap();
    }
    auto loaded = nimble::profile::LoadProfileArtifact(artifact_path);
    std::filesystem::remove(artifact_path);
    if (!loaded.ok()) {
      __builtin_trap();
    }
    state.profile = std::move(loaded).value();
    auto dv = nimble::profile::NormalizedDictionaryView::FromProfile(state.profile);
    if (!dv.ok()) {
      __builtin_trap();
    }
    state.dictionary.emplace(std::move(dv).value());
    state.initialized = true;
  }
  return state;
}

} // namespace

extern "C" int
LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  auto& fuzz = GetFuzzState();
  if (!fuzz.initialized) {
    return 0;
  }

  std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(data), size);

  // Exercise the peek path (zero-copy header scanner)
  (void)nimble::codec::PeekSessionHeaderView(bytes);

  // Exercise the full decode-view path (parsed field slots + groups)
  (void)nimble::codec::DecodeFixMessageView(bytes, *fuzz.dictionary);

  // Exercise the owning decode path
  std::vector<std::byte> owned(bytes.begin(), bytes.end());
  (void)nimble::codec::DecodeFixMessage(owned, *fuzz.dictionary);

  return 0;
}
