#include <nimblefix/session.hpp>
#include <nimblefix/message.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include <unistd.h>

namespace nf = nimblefix;

namespace {

int failures = 0;

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #condition << '\n'; \
            ++failures;                                                                     \
        }                                                                                   \
    } while (false)

constexpr std::string_view t0{"20261027-00:00:00.000"};
constexpr std::string_view t1{"20261027-00:00:01.000"};
constexpr std::string_view t2{"20261027-00:00:02.000"};
constexpr std::string_view t3{"20261027-00:00:03.000"};
constexpr std::string_view t4{"20261027-00:00:04.000"};
constexpr std::string_view t5{"20261027-00:00:05.000"};
constexpr std::string_view t6{"20261027-00:00:06.000"};
constexpr std::string_view t7{"20261027-00:00:07.000"};

struct TempDirectory {
    std::string path;
    TempDirectory() {
        char pattern[] = "/tmp/nimblefix-session-XXXXXX";
        if (char* created = ::mkdtemp(pattern); created != nullptr) path = created;
    }
    ~TempDirectory() {
        if (!path.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(path, ignored);
        }
    }
};

struct Capture {
    std::string bytes;
    std::size_t max_write{std::numeric_limits<std::size_t>::max()};
    bool block_once{};
    bool fail_once{};

    Capture() { bytes.reserve(1024U * 1024U); }
};

nf::WriteResult capture_write(void* opaque, std::span<const char> bytes) noexcept {
    auto& capture = *static_cast<Capture*>(opaque);
    if (capture.fail_once) {
        capture.fail_once = false;
        return {0, false, nf::Status{nf::ErrorCode::io_error}};
    }
    const auto count = std::min(bytes.size(), capture.max_write);
    try {
        capture.bytes.append(bytes.data(), count);
    } catch (...) {
        return {0, false, nf::Status{nf::ErrorCode::capacity_exceeded}};
    }
    const bool would_block = capture.block_once;
    capture.block_once = false;
    return {count, would_block, nf::Status::success()};
}

template <class Message>
std::string encode(Message&& message, nf::SessionHeader header) {
    std::array<char, 4096> output{};
    const auto encoded = nf::encode_fix44(message, header, output);
    if (!encoded.ok()) return {};
    return {output.data(), encoded.value()};
}

std::string logon(std::string_view sender, std::string_view target,
    std::uint64_t sequence, std::string_view sending_time, std::uint32_t heartbeat_seconds = 30) {
    auto message = nf::message<"A">().set<98>(0).set<108>(heartbeat_seconds);
    return encode(message, {sender, target, sequence, sending_time});
}

std::string resend_request(std::string_view sender, std::string_view target,
    std::uint64_t sequence, std::string_view sending_time,
    std::uint64_t begin, std::uint64_t end) {
    auto message = nf::message<"2">().set<7>(begin).set<16>(end);
    return encode(message, {sender, target, sequence, sending_time});
}

std::string order(std::string_view sender, std::string_view target,
    std::uint64_t sequence, std::string_view sending_time, std::string_view cl_ord_id,
    bool poss_dup = false, std::string_view original_time = {}) {
    auto message = nf::message<"D">().set<11>(cl_ord_id).set<55>("AAPL");
    return encode(message, {sender, target, sequence, sending_time, poss_dup, original_time});
}

std::string_view local_comp(nf::SessionRole role) noexcept {
    return role == nf::SessionRole::initiator ? "BUY" : "SELL";
}

std::string_view remote_comp(nf::SessionRole role) noexcept {
    return role == nf::SessionRole::initiator ? "SELL" : "BUY";
}

bool take_frame(std::string& stream, std::string& output) {
    nf::FrameView frame{};
    const auto status = nf::inspect_frame(stream, frame);
    if (!status.ok()) return false;
    output.assign(frame.bytes);
    stream.erase(0, frame.consumed);
    return true;
}

std::string field(std::string_view frame, std::uint32_t wanted_tag) {
    std::array<nf::FieldView, 2048> scratch{};
    nf::FrameView framed{frame, frame.size()};
    nf::FlatMessageView parsed{};
    const auto status = nf::parse_flat(framed, scratch, parsed);
    if (!status.ok()) return {};
    const auto found = parsed.find_unique(wanted_tag);
    return found.ok() ? std::string{found.value().value} : std::string{};
}

bool flush_all(nf::SessionDriver& driver, Capture& capture, std::uint64_t now_ms,
    std::size_t budget = 1024U * 1024U) {
    for (int attempt = 0; attempt != 16 && driver.needs_write(); ++attempt) {
        const auto status = driver.flush(&capture_write, &capture, now_ms, budget);
        if (!status.ok()) return false;
        if (capture.block_once) return true;
        if (capture.max_write == 0) return true;
    }
    return !driver.needs_write();
}

nf::SessionConfig config_for(const TempDirectory& directory, nf::SessionRole role,
    std::string_view local = {}, std::string_view remote = {}) {
    nf::SessionConfig config;
    config.role = role;
    config.name = "unit-session";
    config.storage_directory = directory.path;
    config.session_key = "FIX.4.4/BUY/SELL";
    config.local_comp_id = local.empty() ? local_comp(role) : local;
    config.remote_comp_id = remote.empty() ? remote_comp(role) : remote;
    config.heartbeat_ms = 60'000;
    config.logon_timeout_ms = 20'000;
    config.write_stall_ms = 10'000;
    config.logout_timeout_ms = 5'000;
    return config;
}

bool start_initiator(nf::SessionDriver& driver, Capture& capture) {
    CHECK(driver.connected(0, t0).ok());
    CHECK(flush_all(driver, capture, 0));
    std::string frame;
    CHECK(take_frame(capture.bytes, frame));
    CHECK(field(frame, 35) == "A");
    CHECK(field(frame, 34) == "1");
    CHECK(driver.receive(logon(remote_comp(nf::SessionRole::initiator),
        local_comp(nf::SessionRole::initiator), 1, t1), 1, t1).ok());
    CHECK(driver.state() == nf::SessionState::active);
    return true;
}

bool start_acceptor(nf::SessionDriver& driver, Capture& capture) {
    CHECK(driver.connected(0, t0).ok());
    CHECK(driver.receive(logon(remote_comp(nf::SessionRole::acceptor),
        local_comp(nf::SessionRole::acceptor), 1, t1), 1, t1).ok());
    CHECK(driver.state() == nf::SessionState::active);
    CHECK(flush_all(driver, capture, 1));
    std::string frame;
    CHECK(take_frame(capture.bytes, frame));
    CHECK(field(frame, 35) == "A");
    CHECK(field(frame, 34) == "1");
    return true;
}

void test_handshake_callback_reply_and_receipt() {
    TempDirectory initiator_dir;
    TempDirectory acceptor_dir;
    CHECK(!initiator_dir.path.empty() && !acceptor_dir.path.empty());
    int app_deliveries = 0;
    std::uint64_t callback_receipt = 0;
    nf::SessionCallbacks initiator_callbacks;
    initiator_callbacks.on_message = [&](nf::SessionContext&, const nf::FlatMessageView&,
        const nf::DeliveryInfo&) {
        ++app_deliveries;
        return nf::Status::success();
    };
    nf::SessionCallbacks acceptor_callbacks;
    acceptor_callbacks.on_message = [&](nf::SessionContext& context, const nf::FlatMessageView& message,
        const nf::DeliveryInfo& info) {
        CHECK(!info.is_recovery);
        CHECK(field(message.frame(), 35) == "D");
        auto report = nf::message<"8">().set<11>("ORDER-1").set<17>("EXEC-1");
        const auto sent = context.send(report);
        CHECK(sent.ok());
        if (sent.ok()) callback_receipt = sent.value().id;
        return sent.status();
    };

    auto initiator_open = nf::SessionDriver::create(config_for(initiator_dir, nf::SessionRole::initiator),
        std::move(initiator_callbacks));
    auto acceptor_open = nf::SessionDriver::create(config_for(acceptor_dir, nf::SessionRole::acceptor),
        std::move(acceptor_callbacks));
    CHECK(initiator_open.ok() && acceptor_open.ok());
    if (!initiator_open.ok() || !acceptor_open.ok()) return;
    auto initiator = std::move(initiator_open).value();
    auto acceptor = std::move(acceptor_open).value();
    Capture from_initiator;
    Capture from_acceptor;

    CHECK(initiator.connected(0, t0).ok());
    CHECK(acceptor.connected(0, t0).ok());
    CHECK(flush_all(initiator, from_initiator, 0));
    std::string frame;
    CHECK(take_frame(from_initiator.bytes, frame));
    CHECK(acceptor.receive(frame, 1, t1).ok());
    CHECK(acceptor.state() == nf::SessionState::active);
    CHECK(flush_all(acceptor, from_acceptor, 1));
    CHECK(take_frame(from_acceptor.bytes, frame));
    CHECK(initiator.receive(frame, 2, t2).ok());
    CHECK(initiator.state() == nf::SessionState::active);

    auto new_order = nf::message<"D">().set<11>("ORDER-1").set<55>("AAPL");
    const auto sent = initiator.send(new_order, 3, t3);
    CHECK(sent.ok());
    if (!sent.ok()) return;
    CHECK(sent.value().msg_seq_num == 2);
    auto before_write = initiator.receipt(sent.value().id);
    CHECK(before_write.ok() && before_write.value().state == nf::ReceiptState::appended);
    CHECK(flush_all(initiator, from_initiator, 3));
    CHECK(take_frame(from_initiator.bytes, frame));
    CHECK(field(frame, 35) == "D");
    CHECK(acceptor.receive(frame, 4, t4).ok());
    CHECK(callback_receipt != 0);
    CHECK(flush_all(acceptor, from_acceptor, 4));
    CHECK(take_frame(from_acceptor.bytes, frame));
    CHECK(field(frame, 35) == "8");
    CHECK(initiator.receive(frame, 5, t5).ok());
    CHECK(app_deliveries == 1);
    auto after_write = acceptor.receipt(callback_receipt);
    CHECK(after_write.ok() && after_write.value().state == nf::ReceiptState::written);
}

void test_partial_write_transport_failure_reconnect_and_replay() {
    TempDirectory directory;
    CHECK(!directory.path.empty());
    auto opened = nf::SessionDriver::create(config_for(directory, nf::SessionRole::initiator));
    CHECK(opened.ok());
    if (!opened.ok()) return;
    auto driver = std::move(opened).value();
    Capture output;
    CHECK(start_initiator(driver, output));

    auto outgoing = nf::message<"D">().set<11>("PERSISTED-2").set<55>("MSFT");
    const auto receipt = driver.send(outgoing, 2, t2);
    CHECK(receipt.ok() && receipt.value().msg_seq_num == 2);
    if (!receipt.ok()) return;
    output.max_write = 11;
    output.block_once = true;
    CHECK(driver.flush(&capture_write, &output, 3, 11).ok());
    CHECK(output.bytes.size() == 11);
    CHECK(driver.needs_write());

    output.fail_once = true;
    const auto write_error = driver.flush(&capture_write, &output, 4, 4096);
    CHECK(write_error.code == nf::ErrorCode::io_error);
    CHECK(driver.should_close());
    CHECK(driver.disconnected(nf::Status{nf::ErrorCode::connection_closed}, 4).ok());
    output.bytes.clear();
    output.max_write = std::numeric_limits<std::size_t>::max();
    output.block_once = false;

    CHECK(driver.connected(5, t5).ok());
    CHECK(driver.state() == nf::SessionState::awaiting_logon);
    CHECK(driver.next_outbound_seq() == 4); // Seq 2 is committed; reconnect Logon uses seq 3.
    CHECK(driver.receive(resend_request("SELL", "BUY", 2, t6, 2, 2), 6, t6).ok());
    CHECK(driver.tick(7, t7).ok());
    CHECK(flush_all(driver, output, 7));

    std::string replay;
    std::string new_logon;
    CHECK(take_frame(output.bytes, replay));
    CHECK(take_frame(output.bytes, new_logon));
    CHECK(field(replay, 35) == "D");
    CHECK(field(replay, 34) == "2");
    CHECK(field(replay, 43) == "Y");
    CHECK(field(replay, 122) == t2);
    CHECK(field(replay, 52) == t7);
    CHECK(field(new_logon, 35) == "A");
    CHECK(field(new_logon, 34) == "3");
    CHECK(driver.receive(logon("SELL", "BUY", 3, t7), 8, t7).ok());
    CHECK(driver.state() == nf::SessionState::active);
}

void test_high_sequence_logon_recovery_for_both_roles() {
    for (const auto role : {nf::SessionRole::initiator, nf::SessionRole::acceptor}) {
        TempDirectory directory;
        CHECK(!directory.path.empty());
        int replay_deliveries = 0;
        bool replay_is_recovery = false;
        nf::SessionCallbacks callbacks;
        callbacks.on_message = [&](nf::SessionContext&, const nf::FlatMessageView& message,
            const nf::DeliveryInfo& info) {
            if (field(message.frame(), 35) == "D") {
                ++replay_deliveries;
                replay_is_recovery = info.is_recovery;
            }
            return nf::Status::success();
        };
        auto opened = nf::SessionDriver::create(config_for(directory, role), std::move(callbacks));
        CHECK(opened.ok());
        if (!opened.ok()) continue;
        auto driver = std::move(opened).value();
        Capture output;
        if (role == nf::SessionRole::initiator) CHECK(start_initiator(driver, output));
        else CHECK(start_acceptor(driver, output));
        CHECK(driver.disconnected(nf::Status{nf::ErrorCode::connection_closed}, 2).ok());
        output.bytes.clear();
        CHECK(driver.connected(3, t3).ok());
        if (role == nf::SessionRole::initiator) {
            CHECK(flush_all(driver, output, 3));
            std::string reconnect_logon;
            CHECK(take_frame(output.bytes, reconnect_logon));
            CHECK(field(reconnect_logon, 34) == "2");
            output.bytes.clear();
        }

        const auto high_status = driver.receive(logon(remote_comp(role), local_comp(role), 3, t4), 4, t4);
        if (!high_status.ok()) {
            std::cerr << "high Logon failed role=" << static_cast<int>(role)
                << " status=" << static_cast<int>(high_status.code)
                << " seq=" << driver.next_inbound_seq() << '\n';
        }
        CHECK(high_status.ok());
        CHECK(driver.state() == nf::SessionState::awaiting_logon);
        CHECK(replay_deliveries == 0);
        CHECK(flush_all(driver, output, 4));
        bool saw_logon_reply = false;
        bool saw_resend = false;
        std::string sent_frame;
        while (take_frame(output.bytes, sent_frame)) {
            if (field(sent_frame, 35) == "A") {
                saw_logon_reply = role == nf::SessionRole::acceptor && field(sent_frame, 34) == "2";
            }
            if (field(sent_frame, 35) == "2") {
                saw_resend = field(sent_frame, 7) == "2" && field(sent_frame, 16) == "2";
            }
        }
        CHECK(saw_resend);
        if (role == nf::SessionRole::acceptor) CHECK(saw_logon_reply);

        const auto missing_replay = order(remote_comp(role), local_comp(role),
            2, t5, "GAP-ORDER", true, t2);
        CHECK(!missing_replay.empty());
        CHECK(driver.receive(missing_replay, 5, t5).ok());
        // The buffered high-sequence Logon is processed as soon as its gap closes.
        CHECK(driver.state() == nf::SessionState::active);
        CHECK(replay_deliveries == 1);
        CHECK(replay_is_recovery);
    }
}

void test_resend_admin_gapfill_and_replayed_business_order() {
    TempDirectory directory;
    CHECK(!directory.path.empty());
    auto opened = nf::SessionDriver::create(config_for(directory, nf::SessionRole::initiator));
    CHECK(opened.ok());
    if (!opened.ok()) return;
    auto driver = std::move(opened).value();
    Capture output;
    CHECK(start_initiator(driver, output));

    auto outgoing = nf::message<"D">().set<11>("REPLAY-2").set<55>("AAPL");
    CHECK(driver.send(outgoing, 2, t2).ok());
    CHECK(driver.receive(resend_request("SELL", "BUY", 2, t3, 1, 2), 3, t3).ok());
    CHECK(flush_all(driver, output, 4));
    std::string gapfill;
    std::string replay;
    CHECK(take_frame(output.bytes, gapfill));
    CHECK(take_frame(output.bytes, replay));
    CHECK(field(gapfill, 35) == "4");
    CHECK(field(gapfill, 34) == "1");
    CHECK(field(gapfill, 123) == "Y");
    CHECK(field(gapfill, 36) == "2");
    const auto gap_flag_pos = gapfill.find(std::string{"123=Y\x01", 6});
    const auto new_seq_pos = gapfill.find(std::string{"36=2\x01", 5});
    CHECK(gap_flag_pos != std::string::npos && new_seq_pos != std::string::npos &&
        gap_flag_pos < new_seq_pos);
    CHECK(field(gapfill, 122) == t0);
    CHECK(field(replay, 35) == "D");
    CHECK(field(replay, 34) == "2");
    CHECK(field(replay, 43) == "Y");
    CHECK(field(replay, 122) == t2);
}

void test_recovered_resend_request_is_scheduled_once_and_completes() {
    TempDirectory directory;
    CHECK(!directory.path.empty());
    auto first_open = nf::SessionDriver::create(config_for(directory, nf::SessionRole::initiator));
    CHECK(first_open.ok());
    if (!first_open.ok()) return;
    {
        auto first = std::move(first_open).value();
        Capture output;
        CHECK(start_initiator(first, output));
        auto outgoing = nf::message<"D">().set<11>("RECOVER-2").set<55>("IBM");
        CHECK(first.send(outgoing, 2, t2).ok());
        CHECK(first.receive(resend_request("SELL", "BUY", 2, t3, 2, 2), 3, t3).ok());
        // The request is committed, but its replay range has not been written.
        CHECK(first.disconnected(nf::Status{nf::ErrorCode::connection_closed}, 4).ok());
    }

    auto reopened = nf::SessionDriver::create(config_for(directory, nf::SessionRole::initiator));
    CHECK(reopened.ok());
    if (!reopened.ok()) return;
    auto driver = std::move(reopened).value();
    Capture output;
    CHECK(driver.connected(5, t5).ok());
    // Recovery of the committed ResendRequest happens inside activation. It
    // must schedule the range and return so the transport can flush it.
    CHECK(driver.receive(logon("SELL", "BUY", 3, t6), 6, t6).ok());
    CHECK(driver.state() == nf::SessionState::active);
    CHECK(driver.needs_write());
    CHECK(flush_all(driver, output, 7));
    std::string replay;
    std::string new_logon;
    CHECK(take_frame(output.bytes, replay));
    CHECK(take_frame(output.bytes, new_logon));
    CHECK(field(replay, 35) == "D");
    CHECK(field(replay, 34) == "2");
    CHECK(field(replay, 43) == "Y");
    CHECK(field(new_logon, 35) == "A");
    CHECK(field(new_logon, 34) == "3");
}

void test_recovery_reuses_only_identical_callback_reply() {
    const auto run_case = [](bool change_reply) {
        TempDirectory directory;
        CHECK(!directory.path.empty());
        std::size_t callback_count = 0;
        bool recovery_callback = false;
        std::uint64_t first_receipt = 0;
        std::uint64_t recovery_receipt = 0;
        nf::SessionCallbacks callbacks;
        callbacks.on_message = [&](nf::SessionContext& context, const nf::FlatMessageView&,
            const nf::DeliveryInfo& info) {
            ++callback_count;
            recovery_callback = info.is_recovery;
            const std::string_view exec_id = change_reply && info.is_recovery ? "EXEC-CHANGED" : "EXEC-1";
            auto report = nf::message<"8">().set<11>("CALLBACK-1").set<17>(exec_id);
            const auto sent = context.send(report);
            if (!sent.ok()) return sent.status();
            if (callback_count == 1) first_receipt = sent.value().id;
            else recovery_receipt = sent.value().id;
            if (callback_count == 1) return nf::Status{nf::ErrorCode::callback_failed};
            return nf::Status::success();
        };

        auto first_open = nf::SessionDriver::create(
            config_for(directory, nf::SessionRole::acceptor), callbacks);
        CHECK(first_open.ok());
        if (!first_open.ok()) return;
        {
            auto first = std::move(first_open).value();
            Capture output;
            CHECK(start_acceptor(first, output));
            const auto incoming = order("BUY", "SELL", 2, t2, "CALLBACK-1");
            const auto failed = first.receive(incoming, 2, t2);
            CHECK(failed.code == nf::ErrorCode::callback_failed);
            CHECK(first_receipt != 0);
            CHECK(first.next_outbound_seq() == 3); // Reply seq 2 was committed before callback failure.
        }

        auto reopened = nf::SessionDriver::create(
            config_for(directory, nf::SessionRole::acceptor), callbacks);
        CHECK(reopened.ok());
        if (!reopened.ok()) return;
        auto recovered = std::move(reopened).value();
        CHECK(recovered.connected(10, t3).ok());
        const auto status = recovered.receive(logon("BUY", "SELL", 3, t4), 11, t4);
        CHECK(recovery_callback);
        if (change_reply) {
            CHECK(status.code == nf::ErrorCode::callback_failed);
            CHECK(recovery_receipt == 0);
        } else {
            CHECK(status.ok());
            CHECK(recovered.state() == nf::SessionState::active);
            CHECK(recovery_receipt == first_receipt);
            CHECK(recovered.next_outbound_seq() == 4); // Logon seq 3; no duplicate callback reply.
            const auto receipt = recovered.receipt(first_receipt);
            CHECK(receipt.ok() && receipt.value().msg_seq_num == 2);
        }
    };

    run_case(false);
    run_case(true);
}

void test_two_driver_high_logon_replays_real_admin_gapfill() {
    TempDirectory initiator_dir;
    TempDirectory acceptor_dir;
    CHECK(!initiator_dir.path.empty() && !acceptor_dir.path.empty());
    auto initiator_config = config_for(initiator_dir, nf::SessionRole::initiator);
    initiator_config.heartbeat_ms = 1'000;
    int recovered_orders = 0;
    bool recovery_flag = false;
    nf::SessionCallbacks acceptor_callbacks;
    acceptor_callbacks.on_message = [&](nf::SessionContext&, const nf::FlatMessageView& message,
        const nf::DeliveryInfo& info) {
        if (field(message.frame(), 35) == "D") {
            ++recovered_orders;
            recovery_flag = info.is_recovery;
        }
        return nf::Status::success();
    };
    auto initiator_open = nf::SessionDriver::create(std::move(initiator_config));
    auto acceptor_open = nf::SessionDriver::create(
        config_for(acceptor_dir, nf::SessionRole::acceptor), std::move(acceptor_callbacks));
    CHECK(initiator_open.ok() && acceptor_open.ok());
    if (!initiator_open.ok() || !acceptor_open.ok()) return;
    auto initiator = std::move(initiator_open).value();
    auto acceptor = std::move(acceptor_open).value();
    Capture from_initiator;
    Capture from_acceptor;

    CHECK(initiator.connected(0, t0).ok());
    CHECK(acceptor.connected(0, t0).ok());
    CHECK(flush_all(initiator, from_initiator, 0));
    std::string frame;
    CHECK(take_frame(from_initiator.bytes, frame));
    CHECK(acceptor.receive(frame, 1, t1).ok());
    CHECK(flush_all(acceptor, from_acceptor, 1));
    CHECK(take_frame(from_acceptor.bytes, frame));
    CHECK(initiator.receive(frame, 2, t2).ok());
    CHECK(initiator.state() == nf::SessionState::active);
    CHECK(acceptor.state() == nf::SessionState::active);

    auto business = nf::message<"D">().set<11>("LOST-BEFORE-WRITE").set<55>("NVDA");
    CHECK(initiator.send(business, 3, t3).ok()); // Sequence 2 is committed but not written.
    CHECK(acceptor.tick(1002, t3).ok());
    CHECK(flush_all(acceptor, from_acceptor, 1002));
    CHECK(take_frame(from_acceptor.bytes, frame));
    CHECK(field(frame, 35) == "1" && field(frame, 34) == "2");
    CHECK(initiator.receive(frame, 1003, t4).ok()); // Its response is Heartbeat sequence 3.
    CHECK(initiator.next_outbound_seq() == 4);
    CHECK(initiator.disconnected(nf::Status{nf::ErrorCode::connection_closed}, 1004).ok());
    CHECK(acceptor.disconnected(nf::Status{nf::ErrorCode::connection_closed}, 1004).ok());
    from_initiator.bytes.clear();
    from_acceptor.bytes.clear();

    CHECK(initiator.connected(1005, t5).ok()); // New Logon is sequence 4.
    CHECK(acceptor.connected(1005, t5).ok());
    CHECK(flush_all(initiator, from_initiator, 1005));
    CHECK(take_frame(from_initiator.bytes, frame));
    CHECK(field(frame, 35) == "A" && field(frame, 34) == "4");
    CHECK(acceptor.receive(frame, 1006, t6).ok());
    CHECK(acceptor.state() == nf::SessionState::awaiting_logon);
    CHECK(flush_all(acceptor, from_acceptor, 1006));
    std::string acceptor_logon;
    std::string request;
    CHECK(take_frame(from_acceptor.bytes, acceptor_logon));
    CHECK(take_frame(from_acceptor.bytes, request));
    CHECK(field(acceptor_logon, 35) == "A" && field(acceptor_logon, 34) == "3");
    CHECK(field(request, 35) == "2");
    CHECK(field(request, 7) == "2" && field(request, 16) == "3");
    CHECK(initiator.receive(acceptor_logon, 1007, t6).ok());
    CHECK(initiator.state() == nf::SessionState::active);
    CHECK(initiator.receive(request, 1008, t6).ok());
    CHECK(initiator.tick(1009, t7).ok());
    CHECK(flush_all(initiator, from_initiator, 1009));
    std::string replayed_business;
    std::string gapfill;
    CHECK(take_frame(from_initiator.bytes, replayed_business));
    CHECK(take_frame(from_initiator.bytes, gapfill));
    CHECK(field(replayed_business, 35) == "D");
    CHECK(field(replayed_business, 34) == "2" && field(replayed_business, 43) == "Y");
    CHECK(field(gapfill, 35) == "4");
    CHECK(field(gapfill, 34) == "3" && field(gapfill, 36) == "4");
    CHECK(acceptor.receive(replayed_business, 1010, t7).ok());
    CHECK(acceptor.state() == nf::SessionState::awaiting_logon);
    CHECK(recovered_orders == 0);
    CHECK(acceptor.receive(gapfill, 1011, t7).ok());
    CHECK(acceptor.state() == nf::SessionState::active);
    CHECK(recovered_orders == 1 && recovery_flag);
}

void test_two_driver_high_logon_recovers_business_and_admin_gapfill() {
    TempDirectory initiator_dir;
    TempDirectory acceptor_dir;
    CHECK(!initiator_dir.path.empty() && !acceptor_dir.path.empty());
    auto initiator_config = config_for(initiator_dir, nf::SessionRole::initiator);
    initiator_config.heartbeat_ms = 1'000; // Advertise a short peer interval to trigger TestRequest.
    int recovered_orders = 0;
    bool recovery_flag = false;
    nf::SessionCallbacks acceptor_callbacks;
    acceptor_callbacks.on_message = [&](nf::SessionContext&, const nf::FlatMessageView& message,
        const nf::DeliveryInfo& info) {
        if (field(message.frame(), 35) == "D") {
            ++recovered_orders;
            recovery_flag = info.is_recovery;
        }
        return nf::Status::success();
    };
    auto initiator_open = nf::SessionDriver::create(std::move(initiator_config));
    auto acceptor_open = nf::SessionDriver::create(
        config_for(acceptor_dir, nf::SessionRole::acceptor), std::move(acceptor_callbacks));
    CHECK(initiator_open.ok() && acceptor_open.ok());
    if (!initiator_open.ok() || !acceptor_open.ok()) return;
    auto initiator = std::move(initiator_open).value();
    auto acceptor = std::move(acceptor_open).value();
    Capture from_initiator;
    Capture from_acceptor;

    CHECK(initiator.connected(0, t0).ok());
    CHECK(acceptor.connected(0, t0).ok());
    CHECK(flush_all(initiator, from_initiator, 0));
    std::string frame;
    CHECK(take_frame(from_initiator.bytes, frame));
    CHECK(acceptor.receive(frame, 1, t1).ok());
    CHECK(flush_all(acceptor, from_acceptor, 1));
    CHECK(take_frame(from_acceptor.bytes, frame));
    CHECK(initiator.receive(frame, 2, t2).ok());
    CHECK(initiator.state() == nf::SessionState::active);
    CHECK(acceptor.state() == nf::SessionState::active);

    auto business = nf::message<"D">().set<11>("LOST-BEFORE-WRITE").set<55>("NVDA");
    CHECK(initiator.send(business, 3, t3).ok()); // Outbound seq 2 remains untransmitted.
    CHECK(acceptor.tick(1002, t3).ok());
    CHECK(flush_all(acceptor, from_acceptor, 1002));
    CHECK(take_frame(from_acceptor.bytes, frame));
    CHECK(field(frame, 35) == "1");
    CHECK(initiator.receive(frame, 1003, t4).ok()); // Persists an admin Heartbeat at outbound seq 3.
    CHECK(initiator.next_outbound_seq() == 4);
    CHECK(initiator.disconnected(nf::Status{nf::ErrorCode::connection_closed}, 1004).ok());
    CHECK(acceptor.disconnected(nf::Status{nf::ErrorCode::connection_closed}, 1004).ok());
    from_initiator.bytes.clear();
    from_acceptor.bytes.clear();

    CHECK(initiator.connected(1005, t5).ok()); // New Logon is seq 4.
    CHECK(acceptor.connected(1005, t5).ok());
    CHECK(flush_all(initiator, from_initiator, 1005));
    CHECK(take_frame(from_initiator.bytes, frame));
    CHECK(field(frame, 35) == "A" && field(frame, 34) == "4");
    CHECK(acceptor.receive(frame, 1006, t6).ok()); // High Logon buffers while requesting only 2..3.
    CHECK(acceptor.state() == nf::SessionState::awaiting_logon);
    CHECK(flush_all(acceptor, from_acceptor, 1006));
    std::string acceptor_logon;
    std::string request;
    CHECK(take_frame(from_acceptor.bytes, acceptor_logon));
    CHECK(take_frame(from_acceptor.bytes, request));
    CHECK(field(acceptor_logon, 35) == "A" && field(acceptor_logon, 34) == "3");
    CHECK(field(request, 35) == "2");
    CHECK(field(request, 7) == "2" && field(request, 16) == "3");
    CHECK(initiator.receive(acceptor_logon, 1007, t6).ok());
    CHECK(initiator.state() == nf::SessionState::active);
    CHECK(initiator.receive(request, 1008, t6).ok());
    CHECK(initiator.tick(1009, t7).ok());
    CHECK(flush_all(initiator, from_initiator, 1009));
    std::string replayed_business;
    std::string gapfill;
    CHECK(take_frame(from_initiator.bytes, replayed_business));
    CHECK(take_frame(from_initiator.bytes, gapfill));
    CHECK(field(replayed_business, 35) == "D");
    CHECK(field(replayed_business, 34) == "2" && field(replayed_business, 43) == "Y");
    CHECK(field(gapfill, 35) == "4");
    CHECK(field(gapfill, 34) == "3" && field(gapfill, 36) == "4");
    CHECK(acceptor.receive(replayed_business, 1010, t7).ok());
    CHECK(acceptor.state() == nf::SessionState::awaiting_logon);
    CHECK(recovered_orders == 0);
    CHECK(acceptor.receive(gapfill, 1011, t7).ok());
    CHECK(acceptor.state() == nf::SessionState::active);
    CHECK(recovered_orders == 1 && recovery_flag);
}

void test_logout_waits_for_peer_ack_and_peer_logout_closes_after_reply(bool simultaneous = false) {
    TempDirectory initiator_dir;
    TempDirectory acceptor_dir;
    auto initiator_open = nf::SessionDriver::create(config_for(initiator_dir, nf::SessionRole::initiator));
    auto acceptor_open = nf::SessionDriver::create(config_for(acceptor_dir, nf::SessionRole::acceptor));
    CHECK(initiator_open.ok() && acceptor_open.ok());
    if (!initiator_open.ok() || !acceptor_open.ok()) return;
    auto initiator = std::move(initiator_open).value();
    auto acceptor = std::move(acceptor_open).value();
    Capture from_initiator;
    Capture from_acceptor;
    CHECK(initiator.connected(0, t0).ok());
    CHECK(acceptor.connected(0, t0).ok());
    CHECK(flush_all(initiator, from_initiator, 0));
    std::string frame;
    CHECK(take_frame(from_initiator.bytes, frame));
    CHECK(acceptor.receive(frame, 1, t1).ok());
    CHECK(flush_all(acceptor, from_acceptor, 1));
    CHECK(take_frame(from_acceptor.bytes, frame));
    CHECK(initiator.receive(frame, 2, t2).ok());
    CHECK(initiator.state() == nf::SessionState::active);
    CHECK(acceptor.state() == nf::SessionState::active);

    CHECK(initiator.logout("client stop", 3, t3).ok());
    if (simultaneous) CHECK(acceptor.logout("server stop", 3, t3).ok());
    CHECK(initiator.state() == nf::SessionState::logout_pending);
    CHECK(flush_all(initiator, from_initiator, 3));
    CHECK(take_frame(from_initiator.bytes, frame));
    CHECK(field(frame, 35) == "5");
    CHECK(!initiator.should_close()); // Wait for peer Logout or timeout.
    CHECK(acceptor.receive(frame, 4, t4).ok());
    CHECK(!acceptor.should_close()); // Its Logout reply still needs a successful write.
    CHECK(flush_all(acceptor, from_acceptor, 4));
    CHECK(take_frame(from_acceptor.bytes, frame));
    CHECK(field(frame, 35) == "5");
    CHECK(acceptor.should_close());
    CHECK(initiator.receive(frame, 5, t5).ok());
    CHECK(initiator.should_close());
}

void test_testrequest_timeout_starts_after_write_without_past_deadline_spin() {
    TempDirectory directory;
    nf::SessionConfig config = config_for(directory, nf::SessionRole::initiator);
    config.heartbeat_ms = 60'000;
    config.write_stall_ms = 10'000;
    auto opened = nf::SessionDriver::create(std::move(config));
    CHECK(opened.ok());
    if (!opened.ok()) return;
    auto driver = std::move(opened).value();
    Capture output;
    CHECK(driver.connected(0, t0).ok());
    CHECK(flush_all(driver, output, 0));
    std::string frame;
    CHECK(take_frame(output.bytes, frame));
    CHECK(driver.receive(logon("SELL", "BUY", 1, t1, 1), 1, t1).ok());
    CHECK(driver.state() == nf::SessionState::active);

    CHECK(driver.tick(1001, t2).ok());
    CHECK(driver.needs_write());
    CHECK(driver.next_deadline_ms() > 1001);
    output.max_write = 0;
    output.block_once = true;
    CHECK(driver.flush(&capture_write, &output, 1001, 1024).ok());
    CHECK(driver.tick(2001, t3).ok()); // No timeout while TestRequest is blocked before its first byte.
    CHECK(driver.state() == nf::SessionState::active);
    CHECK(driver.next_deadline_ms() > 2001);

    output.max_write = std::numeric_limits<std::size_t>::max();
    CHECK(flush_all(driver, output, 2002));
    CHECK(take_frame(output.bytes, frame));
    CHECK(field(frame, 35) == "1");
    CHECK(driver.tick(3003, t4).ok()); // Response deadline starts after TestRequest was written.
    CHECK(driver.state() == nf::SessionState::logout_pending);
}

} // namespace

int main() {
    test_handshake_callback_reply_and_receipt();
    test_partial_write_transport_failure_reconnect_and_replay();
    test_high_sequence_logon_recovery_for_both_roles();
    test_resend_admin_gapfill_and_replayed_business_order();
    test_recovered_resend_request_is_scheduled_once_and_completes();
    test_recovery_reuses_only_identical_callback_reply();
    test_two_driver_high_logon_replays_real_admin_gapfill();
    test_two_driver_high_logon_recovers_business_and_admin_gapfill();
    test_logout_waits_for_peer_ack_and_peer_logout_closes_after_reply();
    test_logout_waits_for_peer_ack_and_peer_logout_closes_after_reply(true);
    test_testrequest_timeout_starts_after_write_without_past_deadline_spin();

    if (failures != 0) {
        std::cerr << failures << " session test checks failed\n";
        return 1;
    }
    std::cout << "session tests passed\n";
    return 0;
}
