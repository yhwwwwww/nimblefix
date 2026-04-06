#include <catch2/catch_test_macros.hpp>

#include "fastfix/runtime/metrics.h"
#include "fastfix/runtime/trace.h"

#include "test_support.h"

TEST_CASE("metrics-trace", "[metrics-trace]") {
    fastfix::runtime::MetricsRegistry metrics;
    metrics.Reset(2U);
    REQUIRE(metrics.RegisterSession(1001U, 1U).ok());
    REQUIRE(metrics.RecordInbound(1001U, false).ok());
    REQUIRE(metrics.RecordOutbound(1001U, true).ok());
    REQUIRE(metrics.RecordResendRequest(1001U).ok());
    REQUIRE(metrics.RecordGapFill(1001U, 3U).ok());
    REQUIRE(metrics.RecordParseFailure(1001U).ok());
    REQUIRE(metrics.RecordChecksumFailure(1001U).ok());
    REQUIRE(metrics.UpdateOutboundQueueDepth(1001U, 4U).ok());
    REQUIRE(metrics.ObserveStoreFlushLatency(1001U, 1234U).ok());

    const auto snapshot = metrics.Snapshot();
    REQUIRE(snapshot.workers.size() == 2U);
    REQUIRE(snapshot.sessions.size() == 1U);
    REQUIRE(snapshot.sessions[0].inbound_messages == 1U);
    REQUIRE(snapshot.sessions[0].outbound_messages == 1U);
    REQUIRE(snapshot.sessions[0].admin_messages == 1U);
    REQUIRE(snapshot.sessions[0].resend_requests == 1U);
    REQUIRE(snapshot.sessions[0].gap_fills == 3U);
    REQUIRE(snapshot.sessions[0].parse_failures == 1U);
    REQUIRE(snapshot.sessions[0].checksum_failures == 1U);
    REQUIRE(snapshot.sessions[0].outbound_queue_depth == 4U);
    REQUIRE(snapshot.sessions[0].last_store_flush_latency_ns == 1234U);
    REQUIRE(snapshot.workers[1].registered_sessions == 1U);
    REQUIRE(snapshot.workers[1].outbound_queue_depth == 4U);

    fastfix::runtime::TraceRecorder trace;
    trace.Configure(fastfix::runtime::TraceMode::kRing, 3U);
    trace.Record(fastfix::runtime::TraceEventKind::kConfigLoaded, 0U, 0U, 1U, 1U, 0U, "boot");
    trace.Record(fastfix::runtime::TraceEventKind::kProfileLoaded, 1001U, 0U, 2U, 1001U, 0U, "profile");
    trace.Record(fastfix::runtime::TraceEventKind::kSessionRegistered, 1001U, 1U, 3U, 1001U, 0U, "session-a");
    trace.Record(fastfix::runtime::TraceEventKind::kSessionEvent, 1002U, 1U, 4U, 7U, 8U, "session-b");

    const auto events = trace.Snapshot();
    REQUIRE(events.size() == 3U);
    REQUIRE(events[0].sequence == 2U);
    REQUIRE(events[0].kind == fastfix::runtime::TraceEventKind::kProfileLoaded);
    REQUIRE(std::string_view(events[2].text.data()) == "session-b");
}