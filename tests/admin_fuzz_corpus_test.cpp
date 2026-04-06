#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "fastfix/codec/fix_codec.h"
#include "fastfix/profile/artifact_builder.h"
#include "fastfix/profile/dictgen_input.h"
#include "fastfix/profile/normalized_dictionary.h"
#include "fastfix/profile/overlay.h"
#include "fastfix/profile/profile_loader.h"
#include "fastfix/session/admin_protocol.h"
#include "fastfix/store/memory_store.h"

#include "test_support.h"

namespace {

struct AdminCorpusStats {
    std::size_t corpus_count{0};
    std::size_t peeked{0};
    std::size_t decoded{0};
    std::size_t flagged{0};
    std::size_t application_inputs{0};
    std::size_t admin_events{0};
    std::size_t admin_errors{0};
    std::size_t outbound_frames{0};
    std::size_t application_messages{0};
    std::size_t timer_callbacks{0};
};

auto IsApplicationMsgType(std::string_view msg_type) -> bool {
    return msg_type != "0" && msg_type != "1" && msg_type != "2" && msg_type != "3" &&
           msg_type != "4" && msg_type != "5" && msg_type != "A";
}

auto BuildAdminCorpusArtifact(const std::filesystem::path& artifact_path) -> fastfix::base::Status {
    const auto project_root = std::filesystem::path(FASTFIX_PROJECT_DIR);
    auto dictionary = fastfix::profile::LoadNormalizedDictionaryFile(project_root / "samples" / "basic_profile.ffd");
    if (!dictionary.ok()) {
        return dictionary.status();
    }
    auto overlay = fastfix::profile::LoadNormalizedDictionaryFile(project_root / "samples" / "basic_overlay.ffd");
    if (!overlay.ok()) {
        return overlay.status();
    }
    auto merged = fastfix::profile::ApplyOverlay(dictionary.value(), overlay.value());
    if (!merged.ok()) {
        return merged.status();
    }
    auto artifact = fastfix::profile::BuildProfileArtifact(merged.value());
    if (!artifact.ok()) {
        return artifact.status();
    }
    return fastfix::profile::WriteProfileArtifact(artifact_path, artifact.value());
}

auto ReadText(const std::filesystem::path& path) -> std::string {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

auto ToBytes(std::string_view text) -> std::vector<std::byte> {
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (const auto ch : text) {
        bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
    }
    return bytes;
}

auto WrapBodyFields(std::string_view body_fields) -> std::vector<std::byte> {
    std::string body(body_fields);
    for (auto& ch : body) {
        if (ch == '|') {
            ch = '\x01';
        }
    }

    std::string full;
    full.append("8=FIX.4.4");
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

    full.append("10=");
    full.push_back(static_cast<char>('0' + ((checksum / 100U) % 10U)));
    full.push_back(static_cast<char>('0' + ((checksum / 10U) % 10U)));
    full.push_back(static_cast<char>('0' + (checksum % 10U)));
    full.push_back('\x01');
    return ToBytes(full);
}

auto NormalizeFrame(std::string text) -> std::vector<std::byte> {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.pop_back();
    }
    if (text.rfind("8=", 0) == 0) {
        for (auto& ch : text) {
            if (ch == '|') {
                ch = '\x01';
            }
        }
        return ToBytes(text);
    }
    return WrapBodyFields(text);
}

auto TrimLine(std::string_view line) -> std::string_view {
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t' || line.front() == '\r')) {
        line.remove_prefix(1U);
    }
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) {
        line.remove_suffix(1U);
    }
    return line;
}

auto RunAdminCorpus(
    std::string text,
    const fastfix::profile::NormalizedDictionaryView& dictionary,
    AdminCorpusStats* stats) -> fastfix::base::Status {
    fastfix::store::MemorySessionStore store;
    fastfix::session::AdminProtocol protocol(
        fastfix::session::AdminProtocolConfig{
            .session = fastfix::session::SessionConfig{
                .session_id = 90'001U,
                .key = fastfix::session::SessionKey{"FIX.4.4", "SELL", "BUY"},
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
            continue;
        }

        const auto bytes = NormalizeFrame(std::string(line));
        if (fastfix::codec::PeekSessionHeader(bytes).ok()) {
            ++stats->peeked;
        }

        auto decoded = fastfix::codec::DecodeFixMessage(bytes, dictionary);
        if (decoded.ok()) {
            ++stats->decoded;
            if (decoded.value().validation_issue.present()) {
                ++stats->flagged;
            }
            if (IsApplicationMsgType(decoded.value().header.msg_type)) {
                ++stats->application_inputs;
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

}  // namespace

TEST_CASE("admin-fuzz-corpus", "[admin-fuzz-corpus]") {    const auto artifact_path = std::filesystem::temp_directory_path() / "fastfix-admin-fuzz-corpus.art";
    REQUIRE(BuildAdminCorpusArtifact(artifact_path).ok());

    auto loaded = fastfix::profile::LoadProfileArtifact(artifact_path);
    REQUIRE(loaded.ok());

    auto dictionary = fastfix::profile::NormalizedDictionaryView::FromProfile(std::move(loaded).value());
    REQUIRE(dictionary.ok());

    const auto root = std::filesystem::path(FASTFIX_PROJECT_DIR) / "tests" / "data" / "fuzz" / "admin_protocol";
    std::vector<std::filesystem::path> corpora;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (entry.is_regular_file()) {
            corpora.push_back(entry.path());
        }
    }
    std::sort(corpora.begin(), corpora.end());

    AdminCorpusStats stats;
    for (const auto& corpus : corpora) {
        auto status = RunAdminCorpus(ReadText(corpus), dictionary.value(), &stats);
        REQUIRE(status.ok());
        ++stats.corpus_count;
    }

    REQUIRE(stats.corpus_count >= 3U);
    REQUIRE(stats.peeked >= stats.corpus_count);
    REQUIRE(stats.decoded >= stats.corpus_count);
    REQUIRE(stats.admin_events > stats.corpus_count);
    REQUIRE(stats.admin_errors == 0U);
    REQUIRE(stats.outbound_frames > 0U);
    REQUIRE(stats.application_inputs > 0U);
    REQUIRE(stats.timer_callbacks > 0U);

    std::filesystem::remove(artifact_path);
}