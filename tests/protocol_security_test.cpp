#include <nimblefix/journal.hpp>
#include <nimblefix/message.hpp>
#include <nimblefix/session.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

#include <unistd.h>

namespace nf = nimblefix;

namespace {

int failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #cond << '\n'; \
        ++failures; \
    } \
} while (false)

constexpr std::string_view t0{"20261123-00:00:00.000"};
constexpr std::string_view t1{"20261123-00:00:01.000"};
constexpr std::string_view t2{"20261123-00:00:02.000"};
constexpr std::string_view t_ns{"20261123-00:00:02.123456789"};

struct TempDir {
    std::string path;
    TempDir() {
        char pat[] = "/tmp/nimblefix-sec-XXXXXX";
        if (char* p = ::mkdtemp(pat)) path = p;
    }
    ~TempDir() {
        if (!path.empty()) std::filesystem::remove_all(path);
    }
};

struct Capture {
    std::string bytes;
    Capture() { bytes.reserve(65536); }
};

nf::WriteResult write_capture(void* ctx, std::span<const char> data) noexcept {
    static_cast<Capture*>(ctx)->bytes.append(data.data(), data.size());
    return {data.size(), false, nf::Status::success()};
}

template <class Message>
std::string make_frame(const Message& msg, std::string_view sender, std::string_view target,
                       std::uint64_t seq, std::string_view wt, bool poss_dup = false,
                       std::string_view orig_time = {}, std::optional<std::uint64_t> ack = {}) {
    std::array<char, 4096> buf{};
    nf::SessionHeader h{sender, target, seq, wt, poss_dup, orig_time};
    h.last_msg_seq_num_processed = ack;
    const auto n = nf::encode_fix(msg, h, buf);
    CHECK(n.ok());
    return n.ok() ? std::string{buf.data(), n.value()} : std::string{};
}

bool pop_frame(std::string& wire, std::string& out) {
    nf::FrameView fv{};
    if (!nf::inspect_frame(wire, fv).ok()) return false;
    out.assign(fv.bytes);
    wire.erase(0, fv.consumed);
    return true;
}

std::string field(std::string_view frame, std::uint32_t tag) {
    std::array<nf::FieldView, 256> scratch{};
    nf::FrameView fv{};
    if (!nf::inspect_frame(frame, fv).ok()) return {};
    nf::FlatMessageView msg{};
    if (!nf::parse_flat(fv, scratch, msg).ok()) return {};
    const auto f = msg.find_unique(tag);
    return f.ok() ? std::string{f.value().value} : std::string{};
}

nf::SessionConfig make_cfg(const TempDir& dir, nf::SessionRole role) {
    nf::SessionConfig c{};
    c.role = role;
    c.storage_directory = dir.path;
    c.session_key = "FIX.4.4/BUY/SELL";
    c.local_comp_id = role == nf::SessionRole::initiator ? "BUY" : "SELL";
    c.remote_comp_id = role == nf::SessionRole::initiator ? "SELL" : "BUY";
    c.heartbeat_ms = 30'000;
    c.logon_timeout_ms = 10'000;
    c.logout_timeout_ms = 5'000;
    return c;
}

void flush_all(nf::SessionDriver& d, Capture& cap, std::uint64_t now) {
    for (int i = 0; i < 64 && d.needs_write(); ++i)
        CHECK(d.flush(&write_capture, &cap, now, 1U << 20).ok());
}

// 1. Pre-Logon auth bypass: unauthenticated acceptor must reject 0, 2, 4, 5 before Logon.
void test_pre_logon_auth_bypass_blocked() {
    for (const std::string_view bad_type : {"0", "2", "4", "5"}) {
        TempDir dir;
        auto opened = nf::SessionDriver::create(make_cfg(dir, nf::SessionRole::acceptor));
        CHECK(opened.ok());
        if (!opened.ok()) continue;
        auto drv = std::move(opened).value();
        CHECK(drv.connected(0, t0).ok());
        std::string frame;
        if (bad_type == "0") frame = make_frame(nf::message<"0">(), "BUY", "SELL", 1, t0);
        else if (bad_type == "2") frame = make_frame(nf::message<"2">().set<7>(1).set<16>(0), "BUY", "SELL", 1, t0);
        else if (bad_type == "4") frame = make_frame(nf::message<"4">().set<36>(5), "BUY", "SELL", 1, t0);
        else frame = make_frame(nf::message<"5">(), "BUY", "SELL", 1, t0);
        const auto st = drv.receive(frame, 1, t0);
        CHECK(!st.ok() && st.code == nf::ErrorCode::protocol_error);
        CHECK(drv.state() == nf::SessionState::failed);
    }
}

// 2. ResendRequest storm deduplication (10 high-seq frames -> only 1 ResendRequest 16=0)
//    + out-of-order 35=2 mutual-gap deadlock prevention!
void test_resend_dedup_and_mutual_gap_deadlock() {
    TempDir dir;
    auto opened = nf::SessionDriver::create(make_cfg(dir, nf::SessionRole::initiator));
    CHECK(opened.ok());
    if (!opened.ok()) return;
    auto drv = std::move(opened).value();
    Capture cap;
    CHECK(drv.connected(0, t0).ok());
    flush_all(drv, cap, 0);
    cap.bytes.clear();
    CHECK(drv.receive(make_frame(nf::message<"A">().set<98>(0).set<108>(30), "SELL", "BUY", 1, t0), 1, t0).ok());
    CHECK(drv.state() == nf::SessionState::active);

    // Send 1 business order (seq 2) so peer can ask for replay.
    CHECK(drv.send(nf::message<"D">().set<11>("ORD-MUTUAL").set<55>("AAPL"), 2, t1).ok());
    flush_all(drv, cap, 2);
    cap.bytes.clear();

    // Receive 5 out-of-order orders (seq 5..9 while expecting seq 2): must emit ONLY ONE ResendRequest(7=2, 16=0)!
    for (std::uint64_t s = 5; s <= 9; ++s) {
        CHECK(drv.receive(make_frame(nf::message<"D">().set<11>("GAP").set<55>("AAPL"), "SELL", "BUY", s, t1), 3 + s, t1).ok());
    }
    flush_all(drv, cap, 20);
    std::string rr;
    int rr_count = 0;
    while (pop_frame(cap.bytes, rr)) {
        if (field(rr, 35) == "2") {
            ++rr_count;
            CHECK(field(rr, 7) == "2");
            CHECK(field(rr, 16) == "0");
        }
    }
    CHECK(rr_count == 1);

    // Now peer sends out-of-order ResendRequest(seq=10, 7=2, 16=2) while we STILL expect seq 2!
    // Must honor replay of our seq 2 despite the inbound gap (breaking mutual-gap deadlock).
    CHECK(drv.receive(make_frame(nf::message<"2">().set<7>(2).set<16>(2), "SELL", "BUY", 10, t2), 25, t2).ok());
    flush_all(drv, cap, 26);
    std::string replayed;
    CHECK(pop_frame(cap.bytes, replayed));
    CHECK(field(replayed, 35) == "D" && field(replayed, 34) == "2" && field(replayed, 43) == "Y");
}

// 3. PossDup duplicate with older 369 + SequenceReset optional tag 123 + duplicate GapFill + 27-char ns wall_time.
void test_possdup_369_sequence_reset_and_ns_timestamp() {
    TempDir dir;
    auto cfg = make_cfg(dir, nf::SessionRole::initiator);
    nf::SessionSettings s{};
    s.check_sending_time = false;
    cfg.settings = s;
    auto opened = nf::SessionDriver::create(cfg);
    CHECK(opened.ok());
    if (!opened.ok()) return;
    auto drv = std::move(opened).value();
    Capture cap;
    CHECK(drv.connected(0, t0).ok());
    flush_all(drv, cap, 0);
    std::string logon_out;
    CHECK(pop_frame(cap.bytes, logon_out));
    // Must NOT emit illegal 369=0 before any inbound message is processed!
    CHECK(field(logon_out, 369).empty());

    CHECK(drv.receive(make_frame(nf::message<"A">().set<98>(0).set<108>(30), "SELL", "BUY", 1, t0, false, {}, 1), 1, t0).ok());
    CHECK(drv.send(nf::message<"D">().set<11>("O2").set<55>("AAPL"), 2, t1).ok());
    flush_all(drv, cap, 2);
    cap.bytes.clear();

    // Peer acknowledges seq 2 via 369=2 on inbound seq 2.
    CHECK(drv.receive(make_frame(nf::message<"0">(), "SELL", "BUY", 2, t1, false, {}, 2), 3, t1).ok());
    // Replayed PossDup duplicate of seq 1 carrying older 369=1 must be ignored (NOT fail!).
    CHECK(drv.receive(make_frame(nf::message<"0">(), "SELL", "BUY", 1, t2, true, t0, 1), 4, t_ns).ok());
    CHECK(drv.state() == nf::SessionState::active);

    // Duplicate GapFill (123=Y, seq=1, NewSeqNo=2 when expected is 3) must be ignored cleanly!
    CHECK(drv.receive(make_frame(nf::message<"4">().set<123>('Y').set<36>(2), "SELL", "BUY", 1, t2, true, t0), 5, t_ns).ok());
    CHECK(drv.state() == nf::SessionState::active && drv.next_inbound_seq() == 3);

    // SequenceReset with omitted optional tag 123 (defaults to Reset mode 'N') advances to 7!
    CHECK(drv.receive(make_frame(nf::message<"4">().set<36>(7), "SELL", "BUY", 99, t_ns), 6, t_ns).ok());
    CHECK(drv.state() == nf::SessionState::active && drv.next_inbound_seq() == 7);
}

// 4. 141=Y reconnect resets local_epoch_reset_started and accepts peer 34=1,141=Y,369=1.
void test_141_reconnect_and_369_one() {
    TempDir dir;
    auto cfg = make_cfg(dir, nf::SessionRole::initiator);
    nf::SessionSettings s{};
    s.check_sending_time = false;
    s.allow_sequence_reset = true;
    s.logon.request_sequence_reset = true;
    cfg.settings = s;
    cfg.journal_capacity_bytes = 2U * 1024U * 1024U;
    cfg.journal_segment_bytes = 16U * 1024U;
    cfg.journal_max_segments = 16;

    auto opened = nf::SessionDriver::create(cfg);
    CHECK(opened.ok());
    if (!opened.ok()) return;
    auto drv = std::move(opened).value();
    Capture cap;
    CHECK(drv.connected(0, t0).ok());
    flush_all(drv, cap, 0);
    std::string l1;
    CHECK(pop_frame(cap.bytes, l1) && field(l1, 34) == "1" && field(l1, 141) == "Y");

    // Peer replies with 34=1, 141=Y, 369=1 (already processed our Logon seq 1).
    CHECK(drv.receive(make_frame(nf::message<"A">().set<98>(0).set<108>(30).set<141>('Y'),
                                 "SELL", "BUY", 1, t0, false, {}, 1), 1, t0).ok());
    CHECK(drv.state() == nf::SessionState::active);

    // Disconnect and reconnect on the SAME driver instance -> must start new epoch with 34=1, 141=Y!
    CHECK(drv.disconnected(nf::Status{nf::ErrorCode::connection_closed}, 2).ok());
    cap.bytes.clear();
    CHECK(drv.connected(3, t1).ok());
    flush_all(drv, cap, 3);
    std::string l2;
    CHECK(pop_frame(cap.bytes, l2) && field(l2, 34) == "1" && field(l2, 141) == "Y");
}

// 5. Persisted inbound Logon keeps real wire direction (49=remote, 56=local).
void test_inbound_logon_persisted_comp_ids() {
    TempDir dir;
    auto cfg = make_cfg(dir, nf::SessionRole::acceptor);
    {
        auto opened = nf::SessionDriver::create(cfg);
        CHECK(opened.ok());
        if (!opened.ok()) return;
        auto drv = std::move(opened).value();
        CHECK(drv.connected(0, t0).ok());
        CHECK(drv.receive(make_frame(nf::message<"A">().set<98>(0).set<108>(30), "BUY", "SELL", 1, t0), 1, t0).ok());
    }
    std::filesystem::path jpath;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir.path))
        if (entry.path().extension() == ".nfxj") jpath = entry.path();
    CHECK(!jpath.empty());
    auto j = nf::Journal::open({.path = jpath.string(), .session_key = cfg.session_key});
    CHECK(j.ok());
    if (!j.ok()) return;
    bool checked = false;
    for (const auto& rec : j.value().records()) {
        if (rec.kind == nf::RecordKind::inbound) {
            CHECK(field(rec.payload, 49) == "BUY");
            CHECK(field(rec.payload, 56) == "SELL");
            checked = true;
        }
    }
    CHECK(checked);
}

// 6. N01: Retention floor must never prune outbound records still queued in TX or active replay.
void test_n01_retention_protects_unflushed_tx_and_replay() {
    TempDir dir;
    auto cfg = make_cfg(dir, nf::SessionRole::initiator);
    cfg.max_frame_bytes = 1024;
    cfg.max_pending_messages = 512;
    cfg.max_pending_bytes = 1U << 20;
    cfg.admin_reserve_messages = 16;
    cfg.admin_reserve_bytes = 16'384;
    cfg.receipt_capacity = 1024;
    cfg.journal_capacity_bytes = 4U << 20;
    cfg.journal_segment_bytes = 8'192;
    cfg.journal_max_segments = 256;
    cfg.retain_outbound_messages = 32; // Intentionally much smaller than queued backlog (200)
    nf::SessionSettings s{};
    s.check_sending_time = false;
    s.emit_last_processed_seq = false;
    cfg.settings = s;

    auto opened = nf::SessionDriver::create(cfg);
    CHECK(opened.ok());
    if (!opened.ok()) return;
    auto drv = std::move(opened).value();
    Capture cap;
    CHECK(drv.connected(0, t0).ok());
    flush_all(drv, cap, 0);
    cap.bytes.clear();
    CHECK(drv.receive(make_frame(nf::message<"A">().set<98>(0).set<108>(30),
                                 "SELL", "BUY", 1, t0), 1, t0).ok());
    CHECK(drv.state() == nf::SessionState::active);

    // Queue 200 orders across many 8 KiB journal segments WITHOUT flushing to transport!
    const std::string text_pad(64, 'P');
    for (int i = 0; i < 200; ++i) {
        CHECK(drv.send(nf::message<"D">()
                           .set<11>("N01-" + std::to_string(i))
                           .set<55>("AAPL")
                           .set<58>(text_pad),
                       static_cast<std::uint64_t>(2 + i), t1).ok());
    }
    CHECK(drv.pending_messages() == 200);

    // Peer sends high 369=201 while all 200 messages are still sitting unflushed in tx_slots!
    CHECK(drv.receive(make_frame(nf::message<"0">(), "SELL", "BUY", 2, t1, false, {}, 201), 300, t1).ok());
    CHECK(drv.state() == nf::SessionState::active);

    // Flush everything: must NOT fail with corrupt_store; all 200 orders must arrive in strict order!
    flush_all(drv, cap, 301);
    CHECK(drv.state() == nf::SessionState::active);
    CHECK(drv.pending_messages() == 0);
    int flushed_orders = 0;
    std::string wire;
    while (pop_frame(cap.bytes, wire)) {
        if (field(wire, 35) == "D") {
            CHECK(field(wire, 11) == "N01-" + std::to_string(flushed_orders));
            CHECK(field(wire, 34) == std::to_string(2 + flushed_orders));
            ++flushed_orders;
        }
    }
    CHECK(flushed_orders == 200);

    // Verify deferred retention caught up after TX drained (old segments reclaimed cleanly).
    std::filesystem::path jpath;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir.path))
        if (entry.path().extension() == ".nfxj") jpath = entry.path();
    CHECK(!jpath.empty());
    const auto snap = nf::Journal::inspect_snapshot({.path = jpath.string(), .session_key = cfg.session_key});
    CHECK(snap.diagnostic.ok() && snap.lowest_retained_outbound_seq > 1);
}

// 7. N02: SequenceReset-Reset mode (123=N or omitted) matches FIX 4.4 / QuickFIX semantics.
void test_n02_sequence_reset_idempotency_and_reject() {
    TempDir dir;
    auto cfg = make_cfg(dir, nf::SessionRole::initiator);
    nf::SessionSettings s{};
    s.check_sending_time = false;
    cfg.settings = s;

    int err_calls = 0;
    nf::Status last_err{};
    nf::SessionCallbacks cbs{};
    cbs.on_error = [&](nf::Status st) {
        ++err_calls;
        last_err = st;
    };
    auto opened = nf::SessionDriver::create(cfg, std::move(cbs));
    CHECK(opened.ok());
    if (!opened.ok()) return;
    auto drv = std::move(opened).value();
    Capture cap;
    CHECK(drv.connected(0, t0).ok());
    flush_all(drv, cap, 0);
    cap.bytes.clear();
    CHECK(drv.receive(make_frame(nf::message<"A">().set<98>(0).set<108>(30),
                                 "SELL", "BUY", 1, t0), 1, t0).ok());
    CHECK(drv.state() == nf::SessionState::active && drv.next_inbound_seq() == 2);

    // Advance to 5 via forward SequenceReset (omitted 123 defaults to Reset mode 'N').
    CHECK(drv.receive(make_frame(nf::message<"4">().set<36>(5), "SELL", "BUY", 2, t0), 2, t0).ok());
    CHECK(drv.state() == nf::SessionState::active && drv.next_inbound_seq() == 5);

    // No-op Reset (NewSeqNo == expected == 5): must succeed cleanly without Reject or disconnect.
    CHECK(drv.receive(make_frame(nf::message<"4">().set<36>(5), "SELL", "BUY", 5, t1), 3, t1).ok());
    CHECK(drv.state() == nf::SessionState::active && drv.next_inbound_seq() == 5);
    CHECK(!drv.needs_write() && err_calls == 0);

    // Advance to 10, then replay an already-applied Reset with PossDupFlag=Y (34=2, 36=10 and 36=5).
    CHECK(drv.receive(make_frame(nf::message<"4">().set<36>(10), "SELL", "BUY", 6, t1), 4, t1).ok());
    CHECK(drv.next_inbound_seq() == 10);
    CHECK(drv.receive(make_frame(nf::message<"4">().set<36>(10), "SELL", "BUY", 2, t2, true, t0), 5, t2).ok());
    CHECK(drv.receive(make_frame(nf::message<"4">().set<36>(5), "SELL", "BUY", 2, t2, true, t0), 6, t2).ok());
    CHECK(drv.state() == nf::SessionState::active && drv.next_inbound_seq() == 10);
    CHECK(!drv.needs_write() && err_calls == 0);

    // Live attempt to lower expected sequence (34=10, 36=4, non-PossDup):
    // must emit Session Reject (35=3, 45=10, 371=36, 372=4, 373=5) and keep session active!
    CHECK(drv.receive(make_frame(nf::message<"4">().set<123>('N').set<36>(4), "SELL", "BUY", 10, t2), 7, t2).ok());
    CHECK(drv.state() == nf::SessionState::active && drv.next_inbound_seq() == 10);
    CHECK(err_calls == 1 && last_err.code == nf::ErrorCode::invalid_value && last_err.tag == 36);
    flush_all(drv, cap, 8);
    std::string rej;
    CHECK(pop_frame(cap.bytes, rej));
    CHECK(field(rej, 35) == "3");
    CHECK(field(rej, 45) == "10");
    CHECK(field(rej, 371) == "36");
    CHECK(field(rej, 372) == "4");
    CHECK(field(rej, 373) == "5");

    // Subsequent normal traffic at seq=10 continues uninterrupted.
    CHECK(drv.receive(make_frame(nf::message<"0">(), "SELL", "BUY", 10, t2), 9, t2).ok());
    CHECK(drv.state() == nf::SessionState::active && drv.next_inbound_seq() == 11);
}

// 8. N05: Overlapping/duplicate ResendRequests merge across distinct inbound dependencies without
//    overflowing replay_slots or duplicating frames, and mark all merged ResendRequests delivered.
void test_n05_overlapping_resend_request_dedup_and_delivery() {
    TempDir dir;
    auto cfg = make_cfg(dir, nf::SessionRole::initiator);
    cfg.max_pending_messages = 16; // Small replay_slots ring
    cfg.admin_reserve_messages = 4;
    cfg.max_pending_inbound_messages = 64;
    nf::SessionSettings s{};
    s.check_sending_time = false;
    cfg.settings = s;

    auto opened = nf::SessionDriver::create(cfg);
    CHECK(opened.ok());
    if (!opened.ok()) return;
    auto drv = std::move(opened).value();
    Capture cap;
    CHECK(drv.connected(0, t0).ok());
    flush_all(drv, cap, 0);
    cap.bytes.clear();
    CHECK(drv.receive(make_frame(nf::message<"A">().set<98>(0).set<108>(30),
                                 "SELL", "BUY", 1, t0), 1, t0).ok());

    for (int i = 0; i < 12; ++i) {
        CHECK(drv.send(nf::message<"D">().set<11>("R-" + std::to_string(i)).set<55>("AAPL"),
                       static_cast<std::uint64_t>(2 + i), t1).ok());
        flush_all(drv, cap, static_cast<std::uint64_t>(2 + i));
    }
    cap.bytes.clear();

    // Without flushing, flood 24 overlapping ResendRequests (exceeding replay_slots capacity=16!).
    // First requests 2..8, next 23 overlap/extend to 2..13 with distinct MsgSeqNums (2..25).
    CHECK(drv.receive(make_frame(nf::message<"2">().set<7>(2).set<16>(8), "SELL", "BUY", 2, t1), 20, t1).ok());
    for (int i = 0; i < 23; ++i) {
        const std::uint64_t end_seq = static_cast<std::uint64_t>(8 + (i % 6)); // extends up to 13
        CHECK(drv.receive(make_frame(nf::message<"2">().set<7>(2).set<16>(end_seq),
                                     "SELL", "BUY", static_cast<std::uint64_t>(3 + i), t2),
                          static_cast<std::uint64_t>(21 + i), t2).ok());
    }
    CHECK(drv.state() == nf::SessionState::active);

    // Now flush: each seq 2..13 must be replayed EXACTLY ONCE (12 frames, not 24x!).
    flush_all(drv, cap, 60);
    CHECK(drv.state() == nf::SessionState::active);
    std::vector<std::string> replayed_seqs;
    std::string wire;
    while (pop_frame(cap.bytes, wire)) {
        if (field(wire, 35) == "D") {
            CHECK(field(wire, 43) == "Y");
            replayed_seqs.push_back(field(wire, 34));
        }
    }
    CHECK(replayed_seqs.size() == 12);
    for (std::size_t i = 0; i < replayed_seqs.size(); ++i) {
        CHECK(replayed_seqs[i] == std::to_string(2 + i));
    }

    // Send one more heartbeat after replay completes and verify tag 369 reflects ALL 24 merged ResendRequests!
    CHECK(drv.receive(make_frame(nf::message<"1">().set<112>("CHK369"), "SELL", "BUY", 26, t2), 61, t2).ok());
    flush_all(drv, cap, 62);
    std::string hb;
    CHECK(pop_frame(cap.bytes, hb) && field(hb, 35) == "0");
    CHECK(field(hb, 369) == "25"); // All ResendRequests (2..25) completed & delivered!
}

} // namespace

int main() {
    test_pre_logon_auth_bypass_blocked();
    test_resend_dedup_and_mutual_gap_deadlock();
    test_possdup_369_sequence_reset_and_ns_timestamp();
    test_141_reconnect_and_369_one();
    test_inbound_logon_persisted_comp_ids();
    test_n01_retention_protects_unflushed_tx_and_replay();
    test_n02_sequence_reset_idempotency_and_reject();
    test_n05_overlapping_resend_request_dedup_and_delivery();
    if (failures != 0) {
        std::cerr << failures << " protocol security test(s) failed\n";
        return 1;
    }
    std::cout << "protocol security and FIX compliance regression tests passed\n";
    return 0;
}
