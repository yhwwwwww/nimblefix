#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <limits>

#include "fastfix/profile/artifact_builder.h"
#include "fastfix/profile/dictgen_input.h"
#include "fastfix/runtime/soak.h"

#include "test_support.h"

namespace {

auto BuildSoakArtifact(const std::filesystem::path& artifact_path) -> fastfix::base::Status {
    const auto ffd_path = std::filesystem::path(FASTFIX_PROJECT_DIR) / "build" / "bench" / "quickfix_FIX44.ffd";
    auto dictionary = fastfix::profile::LoadNormalizedDictionaryFile(ffd_path);
    if (!dictionary.ok()) {
        return dictionary.status();
    }
    auto artifact = fastfix::profile::BuildProfileArtifact(dictionary.value());
    if (!artifact.ok()) {
        return artifact.status();
    }
    return fastfix::profile::WriteProfileArtifact(artifact_path, artifact.value());
}

auto RunSoakCase(
    const std::filesystem::path& artifact_path,
    const fastfix::runtime::SoakConfig& config,
    fastfix::runtime::SoakReport* out_report) -> void {
    REQUIRE(BuildSoakArtifact(artifact_path).ok());

    auto report = fastfix::runtime::RunSoak(config);
    REQUIRE(report.ok());
    REQUIRE(report.value().iterations_completed == config.iterations);
    REQUIRE(report.value().metrics.sessions.size() == config.session_count);
    REQUIRE(report.value().total_resend_requests > 0U);
    REQUIRE(report.value().total_gap_fills > 0U);
    REQUIRE(report.value().total_replay_requests > 0U);
    REQUIRE(report.value().total_reorder_events > 0U);
    REQUIRE(report.value().total_drop_events > 0U);
    REQUIRE(report.value().total_jitter_events > 0U);
    REQUIRE(report.value().total_reconnects > 0U);
    REQUIRE(report.value().total_duplicate_inbound_messages > 0U);
    REQUIRE(report.value().total_timer_events > 0U);
    REQUIRE(report.value().total_application_messages > 0U);

    for (const auto& session : report.value().metrics.sessions) {
        REQUIRE(session.inbound_messages > 0U);
        REQUIRE(session.outbound_messages > 0U);
        REQUIRE(session.resend_requests > 0U);
        REQUIRE(session.gap_fills > 0U);
    }

    if (out_report != nullptr) {
        *out_report = report.value();
    }
}

}  // namespace

TEST_CASE("soak-smoke", "[soak-smoke]") {    const auto artifact_path = std::filesystem::temp_directory_path() / "fastfix-soak-smoke.art";

    fastfix::runtime::SoakConfig config;
    config.profile_artifact = artifact_path;
    config.worker_count = 2U;
    config.session_count = 4U;
    config.iterations = 64U;
    config.gap_every = 16U;
    config.duplicate_every = 8U;
    config.replay_every = 16U;
    config.reorder_every = 14U;
    config.drop_every = 11U;
    config.jitter_every = 9U;
    config.jitter_step_millis = 250U;
    config.disconnect_every = 10U;
    config.timer_pulse_every = 12U;
    config.timer_step_seconds = 31U;
    config.enable_trace = true;

    fastfix::runtime::SoakReport report;
    RunSoakCase(artifact_path, config, &report);
    REQUIRE(report.total_inbound_messages > 256U);
    REQUIRE(report.total_outbound_messages > 256U);
    REQUIRE(report.trace_event_count > 0U);

    std::filesystem::remove(artifact_path);
}

TEST_CASE("soak-long", "[soak-long]") {    const auto artifact_path = std::filesystem::temp_directory_path() / "fastfix-soak-long.art";

    fastfix::runtime::SoakConfig config;
    config.profile_artifact = artifact_path;
    config.worker_count = 2U;
    config.session_count = 6U;
    config.iterations = 256U;
    config.gap_every = 12U;
    config.duplicate_every = 6U;
    config.replay_every = 12U;
    config.reorder_every = 9U;
    config.drop_every = 7U;
    config.jitter_every = 5U;
    config.jitter_step_millis = 125U;
    config.disconnect_every = 12U;
    config.timer_pulse_every = 9U;
    config.timer_step_seconds = 31U;
    config.enable_trace = false;

    fastfix::runtime::SoakReport report;
    RunSoakCase(artifact_path, config, &report);
    REQUIRE(report.total_inbound_messages > 1500U);
    REQUIRE(report.total_outbound_messages > 1200U);
    REQUIRE(report.total_application_messages > 1000U);
    REQUIRE(report.total_reorder_events >= 100U);
    REQUIRE(report.total_drop_events >= 150U);
    REQUIRE(report.total_jitter_events >= 250U);
    REQUIRE(report.trace_event_count == 0U);

    std::filesystem::remove(artifact_path);
}

TEST_CASE("soak-multihour", "[soak-multihour]") {    const auto artifact_path = std::filesystem::temp_directory_path() / "fastfix-soak-multihour.art";

    fastfix::runtime::SoakConfig config;
    config.profile_artifact = artifact_path;
    config.worker_count = 2U;
    config.session_count = 8U;
    config.iterations = 2048U;
    config.gap_every = 10U;
    config.duplicate_every = 5U;
    config.replay_every = 10U;
    config.reorder_every = 7U;
    config.drop_every = 5U;
    config.jitter_every = 4U;
    config.jitter_step_millis = 100U;
    config.disconnect_every = 9U;
    config.timer_pulse_every = 8U;
    config.timer_step_seconds = 31U;
    config.enable_trace = false;

    fastfix::runtime::SoakReport report;
    RunSoakCase(artifact_path, config, &report);
    REQUIRE(report.total_inbound_messages > 20'000U);
    REQUIRE(report.total_outbound_messages > 20'000U);
    REQUIRE(report.total_application_messages > 16'000U);
    REQUIRE(report.total_reorder_events >= 1'500U);
    REQUIRE(report.total_drop_events >= 3'000U);
    REQUIRE(report.total_jitter_events >= 4'000U);
    REQUIRE(report.total_reconnects >= 1'500U);
    REQUIRE(report.trace_event_count == 0U);

    std::filesystem::remove(artifact_path);
}

TEST_CASE("soak-multiworker", "[soak-multiworker]") {    const auto artifact_path = std::filesystem::temp_directory_path() / "fastfix-soak-multiworker.art";

    fastfix::runtime::SoakConfig config;
    config.profile_artifact = artifact_path;
    config.worker_count = 4U;
    config.session_count = 16U;
    config.iterations = 512U;
    config.gap_every = 12U;
    config.duplicate_every = 6U;
    config.replay_every = 12U;
    config.reorder_every = 8U;
    config.drop_every = 6U;
    config.jitter_every = 5U;
    config.jitter_step_millis = 125U;
    config.disconnect_every = 10U;
    config.timer_pulse_every = 9U;
    config.timer_step_seconds = 31U;
    config.enable_trace = false;

    fastfix::runtime::SoakReport report;
    RunSoakCase(artifact_path, config, &report);

    REQUIRE(report.metrics.workers.size() == config.worker_count);
    REQUIRE(report.total_inbound_messages > 10'000U);
    REQUIRE(report.total_outbound_messages > 10'000U);
    REQUIRE(report.total_application_messages > 8'000U);

    std::uint64_t min_registered = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t max_registered = 0U;
    std::size_t workers_with_sessions = 0U;
    for (const auto& worker : report.metrics.workers) {
        if (worker.registered_sessions > 0U) {
            ++workers_with_sessions;
        }
        min_registered = std::min(min_registered, worker.registered_sessions);
        max_registered = std::max(max_registered, worker.registered_sessions);
        REQUIRE(worker.inbound_messages > 0U);
        REQUIRE(worker.outbound_messages > 0U);
    }
    REQUIRE(workers_with_sessions == config.worker_count);
    REQUIRE(max_registered - min_registered <= 4U);

    std::filesystem::remove(artifact_path);
}