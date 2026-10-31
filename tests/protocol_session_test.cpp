#include <nimblefix/journal.hpp>
#include <nimblefix/message.hpp>
#include <nimblefix/session.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <unistd.h>

namespace nf = nimblefix;

namespace {
int failures = 0;
#define CHECK(expression) do { \
    if (!(expression)) { \
        std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #expression << '\n'; \
        ++failures; \
    } \
} while (false)

constexpr std::string_view t0{"20261027-00:00:00.000"};
constexpr std::string_view t1{"20261027-00:00:01.000"};
constexpr std::string_view t2{"20261027-00:00:02.000"};
constexpr std::string_view t3{"20261027-00:00:03.000"};
constexpr std::string_view t4{"20261027-00:00:04.000"};
constexpr std::string_view far_time{"20261020-00:00:00.000"};

struct TempDirectory {
    std::string path;
    TempDirectory() {
        char pattern[] = "/tmp/nimblefix-protocol-XXXXXX";
        if (char* made = ::mkdtemp(pattern); made != nullptr) path = made;
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
    Capture() { bytes.reserve(1U << 20); }
};

nf::WriteResult write_capture(void* context, std::span<const char> bytes) noexcept {
    auto& capture = *static_cast<Capture*>(context);
    const auto count = std::min(bytes.size(), capture.max_write);
    try { capture.bytes.append(bytes.data(), count); }
    catch (...) { return {0, false, nf::Status{nf::ErrorCode::capacity_exceeded}}; }
    const bool blocked = capture.block_once;
    capture.block_once = false;
    return {count, blocked, nf::Status::success()};
}

template <class Message>
std::string encode(Message&& message, nf::SessionHeader header) {
    std::array<char, 16U * 1024U> output{};
    const auto result = nf::encode_fix(message, header, output);
    if (!result.ok()) return {};
    return {output.data(), result.value()};
}

template <class Message>
std::string frame(Message&& message, std::string_view sender, std::string_view target,
    std::uint64_t sequence, std::string_view wall_time, nf::FixVersion version = nf::FixVersion::fix44,
    bool poss_dup = false, std::string_view orig_time = {},
    std::optional<std::uint64_t> processed = {}) {
    nf::SessionHeader header{sender, target, sequence, wall_time, poss_dup, orig_time};
    header.version = version;
    header.last_msg_seq_num_processed = processed;
    return encode(std::forward<Message>(message), header);
}

std::string logon(std::string_view sender, std::string_view target, std::uint64_t sequence,
    std::string_view time, nf::FixVersion version = nf::FixVersion::fix44,
    bool reset = false, std::string_view username = {}, std::string_view password = {},
    std::string_view custom = {}, std::optional<std::uint64_t> processed = {}) {
    auto message = nf::message<"A">().set<98>(0).set<108>(30)
        .set<141>(reset ? 'Y' : 'N')
        .set<553>(username.empty() ? std::string_view{"placeholder-user"} : username)
        .set<554>(password.empty() ? std::string_view{"placeholder-password"} : password)
        .set<9001>(custom.empty() ? std::string_view{"placeholder-extra"} : custom)
        .set<1137>("9");
    (void)version;
    return frame(message, sender, target, sequence, time, version, false, {}, processed);
}

std::string order(std::string_view sender, std::string_view target, std::uint64_t sequence,
    std::string_view time, std::string_view id, nf::FixVersion version = nf::FixVersion::fix44,
    std::optional<std::uint64_t> processed = {}) {
    auto message = nf::message<"D">().set<11>(id).set<55>("AAPL");
    return frame(message, sender, target, sequence, time, version, false, {}, processed);
}

std::string gapfill(std::string_view sender, std::string_view target, std::uint64_t sequence,
    std::string_view time, std::uint64_t next, bool poss_dup = true) {
    auto message = nf::message<"4">().set<123>('Y').set<36>(next);
    return frame(message, sender, target, sequence, time, nf::FixVersion::fix44,
        poss_dup, poss_dup ? t0 : std::string_view{});
}

std::string sequence_reset(std::string_view sender, std::string_view target,
    std::uint64_t sequence, std::string_view time, std::uint64_t next) {
    auto message = nf::message<"4">().set<123>('N').set<36>(next);
    return frame(message, sender, target, sequence, time);
}

std::string heartbeat(std::string_view sender, std::string_view target,
    std::uint64_t sequence, std::string_view time, std::optional<std::uint64_t> ack = {}) {
    auto message = nf::message<"0">();
    return frame(message, sender, target, sequence, time, nf::FixVersion::fix44, false, {}, ack);
}

std::string_view local(nf::SessionRole role) {
    return role == nf::SessionRole::initiator ? "BUY" : "SELL";
}
std::string_view remote(nf::SessionRole role) {
    return role == nf::SessionRole::initiator ? "SELL" : "BUY";
}

nf::SessionConfig config(const TempDirectory& directory, nf::SessionRole role,
    std::string key = "FIX.4.4/BUY/SELL") {
    nf::SessionConfig result{};
    result.role = role;
    result.storage_directory = directory.path;
    result.session_key = std::move(key);
    result.local_comp_id = local(role);
    result.remote_comp_id = remote(role);
    result.heartbeat_ms = 60'000;
    result.logon_timeout_ms = 20'000;
    result.write_stall_ms = 10'000;
    result.logout_timeout_ms = 5'000;
    result.max_frame_bytes = 2'048;
    result.max_pending_inbound_bytes = 4U * 1024U * 1024U;
    return result;
}

std::string journal_path(const nf::SessionConfig& config) {
    std::string hex;
    constexpr char digits[] = "0123456789abcdef";
    for (const unsigned char value : config.session_key) {
        hex.push_back(digits[value >> 4]);
        hex.push_back(digits[value & 15]);
    }
    std::filesystem::path path{config.storage_directory};
    path /= "sessions";
    for (std::size_t offset = 0; offset < hex.size(); offset += 64)
        path /= hex.substr(offset, std::min<std::size_t>(64, hex.size() - offset));
    path /= "session.nfxj";
    return path.string();
}

nf::Result<nf::Journal> open_journal(const nf::SessionConfig& config) {
    return nf::Journal::open(nf::JournalConfig{journal_path(config), config.session_key,
        config.journal_capacity_bytes, config.journal_max_records, config.journal_test_hook,
        config.journal_segment_bytes, config.journal_max_segments});
}

std::string get_field(std::string_view input, std::uint32_t tag) {
    std::array<nf::FieldView, 256> fields{};
    nf::FrameView frame_view{};
    if (!nf::inspect_frame(input, frame_view).ok()) return {};
    nf::FlatMessageView message{};
    if (!nf::parse_flat(frame_view, fields, message).ok()) return {};
    const auto found = message.find_unique(tag);
    return found.ok() ? std::string{found.value().value} : std::string{};
}

bool flush_all(nf::SessionDriver& driver, Capture& capture, std::uint64_t now) {
    for (std::size_t i = 0; i < 256 && driver.needs_write(); ++i) {
        if (!driver.flush(&write_capture, &capture, now, 1U << 20).ok()) return false;
    }
    return !driver.needs_write();
}

bool start_initiator(nf::SessionDriver& driver, Capture& capture, nf::FixVersion version = nf::FixVersion::fix44) {
    CHECK(driver.connected(0, t0).ok());
    CHECK(flush_all(driver, capture, 0));
    const auto peer_logon = logon("SELL", "BUY", 1, t1, version);
    CHECK(!peer_logon.empty());
    CHECK(driver.receive(peer_logon, 1, t1).ok());
    CHECK(driver.state() == nf::SessionState::active);
    return true;
}

bool test_fixt_logon_wire_and_journal_redaction() {
    TempDirectory directory;
    auto cfg = config(directory, nf::SessionRole::initiator, "FIXT.1.1/BUY/SELL");
    nf::SessionSettings settings{};
    settings.version = nf::FixVersion::fixt11;
    settings.default_appl_ver_id = "9";
    settings.check_sending_time = false;
    settings.logon.username = "wire-user";
    settings.logon.password = "wire-password";
    settings.logon.extra_fields.push_back({9001, "private-extra", true});
    cfg.settings = settings;

    {
        auto created = nf::SessionDriver::create(cfg);
        CHECK(created.ok());
        if (!created.ok()) return false;
        auto driver = std::move(created).value();
        Capture capture;
        capture.max_write = 7;
        capture.block_once = true;
        CHECK(driver.connected(0, t0).ok());
        CHECK(driver.flush(&write_capture, &capture, 0, 1U << 20).ok());
        CHECK(driver.needs_write());
        CHECK(flush_all(driver, capture, 0));
        CHECK(get_field(capture.bytes, 8) == "FIXT.1.1");
        CHECK(get_field(capture.bytes, 35) == "A");
        CHECK(get_field(capture.bytes, 1137) == "9");
        CHECK(get_field(capture.bytes, 553) == "wire-user");
        CHECK(get_field(capture.bytes, 554) == "wire-password");
        CHECK(get_field(capture.bytes, 9001) == "private-extra");
    }

    auto opened = open_journal(cfg);
    CHECK(opened.ok());
    if (!opened.ok()) return false;
    bool found_logon = false;
    for (const auto& record : opened.value().records()) {
        if (record.kind != nf::RecordKind::outbound) continue;
        CHECK(record.payload.find("wire-user") == std::string_view::npos);
        CHECK(record.payload.find("wire-password") == std::string_view::npos);
        CHECK(record.payload.find("private-extra") == std::string_view::npos);
        CHECK(get_field(record.payload, 35) == "A");
        CHECK(get_field(record.payload, 98) == "0");
        CHECK(get_field(record.payload, 108) == "60");
        CHECK(get_field(record.payload, 1137) == "9");
        found_logon = true;
    }
    CHECK(found_logon);
    return true;
}

bool test_acceptor_auth_and_inbound_logon_redaction() {
    TempDirectory directory;
    auto cfg = config(directory, nf::SessionRole::acceptor, "FIXT.1.1/SELL/BUY");
    nf::SessionSettings settings{};
    settings.version = nf::FixVersion::fixt11;
    settings.check_sending_time = false;
    settings.authentication.method = nf::AuthenticationMethod::credentials;
    settings.authentication.username = "client-user";
    settings.authentication.password = "client-password";
    settings.authentication.require_tls = true;
    cfg.settings = settings;
    cfg.tls_verified = true;
    {
        auto created = nf::SessionDriver::create(cfg);
        CHECK(created.ok());
        if (!created.ok()) return false;
        auto driver = std::move(created).value();
        Capture capture;
        CHECK(driver.connected(0, t0).ok());
        const auto incoming = logon("BUY", "SELL", 1, t1, nf::FixVersion::fixt11,
            false, "client-user", "client-password", "client-custom");
        CHECK(driver.receive(incoming, 1, t1).ok());
        CHECK(driver.state() == nf::SessionState::active);
        CHECK(flush_all(driver, capture, 1));
    }
    auto opened = open_journal(cfg);
    CHECK(opened.ok());
    if (!opened.ok()) return false;
    bool found_inbound_logon = false;
    for (const auto& record : opened.value().records()) {
        if (record.kind != nf::RecordKind::inbound) continue;
        CHECK(record.payload.find("client-user") == std::string_view::npos);
        CHECK(record.payload.find("client-password") == std::string_view::npos);
        CHECK(record.payload.find("client-custom") == std::string_view::npos);
        CHECK(get_field(record.payload, 35) == "A");
        CHECK(get_field(record.payload, 98) == "0");
        CHECK(get_field(record.payload, 108) == "30");
        CHECK(get_field(record.payload, 1137) == "9");
        found_inbound_logon = true;
    }
    CHECK(found_inbound_logon);

    TempDirectory rejected_directory;
    auto rejected_cfg = config(rejected_directory, nf::SessionRole::acceptor, "FIXT.1.1/SELL/BUY");
    rejected_cfg.settings = settings;
    rejected_cfg.tls_verified = true;
    {
        auto created = nf::SessionDriver::create(rejected_cfg);
        CHECK(created.ok());
        if (!created.ok()) return false;
        auto driver = std::move(created).value();
        CHECK(driver.connected(0, t0).ok());
        const auto wrong = logon("BUY", "SELL", 1, t1, nf::FixVersion::fixt11,
            false, "client-user", "wrong-password", "client-custom");
        CHECK(driver.receive(wrong, 1, t1).code == nf::ErrorCode::authentication_failed);
        CHECK(driver.state() == nf::SessionState::failed);
    }
    auto rejected_journal = open_journal(rejected_cfg);
    CHECK(rejected_journal.ok());
    if (rejected_journal.ok()) CHECK(rejected_journal.value().records().empty());
    return true;
}

bool test_preauthenticated_logon_skips_duplicate_auth() {
    TempDirectory directory;
    auto cfg = config(directory, nf::SessionRole::acceptor);
    int auth_calls = 0;
    nf::SessionSettings settings{};
    settings.check_sending_time = false;
    settings.authentication.method = nf::AuthenticationMethod::callback;
    settings.authentication.require_tls = false;
    settings.authentication.verify = [&auth_calls](const nf::FlatMessageView&) {
        ++auth_calls;
        return nf::Status::success();
    };
    cfg.settings = std::move(settings);
    cfg.logon_preauthenticated = true;

    auto created = nf::SessionDriver::create(cfg);
    CHECK(created.ok());
    if (!created.ok()) return false;
    auto driver = std::move(created).value();
    CHECK(driver.connected(0, t0).ok());
    const auto incoming = logon("BUY", "SELL", 1, t1);
    CHECK(driver.receive(incoming, 1, t1).ok());
    CHECK(driver.state() == nf::SessionState::active);
    CHECK(auth_calls == 0);
    return true;
}

bool test_reset_flag_requires_policy_and_is_audited() {
    TempDirectory acceptor_directory;
    auto acceptor_cfg = config(acceptor_directory, nf::SessionRole::acceptor);
    nf::SessionSettings acceptor_settings{};
    acceptor_settings.check_sending_time = false;
    acceptor_settings.allow_sequence_reset = true;
    acceptor_cfg.settings = acceptor_settings;
    acceptor_cfg.journal_capacity_bytes = 2U * 1024U * 1024U;
    acceptor_cfg.journal_max_records = 20'000;
    acceptor_cfg.journal_segment_bytes = 8U * 1024U;
    acceptor_cfg.journal_max_segments = 8;
    {
        auto created = nf::SessionDriver::create(acceptor_cfg);
        if (!created.ok()) std::cerr << "acceptor create: " << created.status().message() << " tag=" << created.status().tag << " off=" << created.status().offset << '\n';
        CHECK(created.ok());
        if (!created.ok()) return false;
        auto driver = std::move(created).value();
        Capture capture;
        CHECK(driver.connected(0, t0).ok());
        CHECK(driver.receive(logon("BUY", "SELL", 1, t1, nf::FixVersion::fix44, true), 1, t1).ok());
        CHECK(driver.state() == nf::SessionState::active);
        CHECK(driver.next_inbound_seq() == 2);
        CHECK(driver.next_outbound_seq() == 2);
        CHECK(flush_all(driver, capture, 1));
        CHECK(get_field(capture.bytes, 141) == "Y");
    }
    auto journal = open_journal(acceptor_cfg);
    CHECK(journal.ok());
    if (journal.ok()) {
        CHECK(journal.value().checkpoint().epoch == 2);
        CHECK(journal.value().epoch_audit().size() == 1);
        CHECK(journal.value().epoch_audit().front().previous_epoch == 1);
        CHECK(journal.value().epoch_audit().front().next_epoch == 2);
    }

    TempDirectory denied_directory;
    auto denied_cfg = config(denied_directory, nf::SessionRole::acceptor);
    nf::SessionSettings denied_settings{};
    denied_settings.check_sending_time = false;
    denied_cfg.settings = denied_settings;
    auto denied_created = nf::SessionDriver::create(denied_cfg);
    CHECK(denied_created.ok());
    if (!denied_created.ok()) return false;
    auto denied = std::move(denied_created).value();
    CHECK(denied.connected(0, t0).ok());
    const auto denied_status = denied.receive(logon("BUY", "SELL", 1, t1,
        nf::FixVersion::fix44, true), 1, t1);
    CHECK(denied_status.code == nf::ErrorCode::sequence_reset_denied);
    CHECK(denied.state() == nf::SessionState::failed);
    return true;
}

bool test_initiator_reset_request_and_peer_agreement() {
    TempDirectory directory;
    auto cfg = config(directory, nf::SessionRole::initiator);
    nf::SessionSettings settings{};
    settings.check_sending_time = false;
    settings.allow_sequence_reset = true;
    settings.logon.request_sequence_reset = true;
    cfg.settings = settings;
    cfg.journal_capacity_bytes = 2U * 1024U * 1024U;
    cfg.journal_max_records = 20'000;
    cfg.journal_segment_bytes = 8U * 1024U;
    cfg.journal_max_segments = 8;
    {
        auto created = nf::SessionDriver::create(cfg);
        if (!created.ok()) std::cerr << "initiator reset create: " << created.status().message() << " tag=" << created.status().tag << " off=" << created.status().offset << '\n';
        CHECK(created.ok());
        if (!created.ok()) return false;
        auto driver = std::move(created).value();
        Capture capture;
        CHECK(driver.connected(0, t0).ok());
        CHECK(flush_all(driver, capture, 0));
        CHECK(get_field(capture.bytes, 141) == "Y");
        CHECK(driver.receive(logon("SELL", "BUY", 1, t1,
            nf::FixVersion::fix44, true), 1, t1).ok());
        CHECK(driver.state() == nf::SessionState::active);
    }
    auto journal = open_journal(cfg);
    CHECK(journal.ok());
    if (journal.ok()) CHECK(journal.value().checkpoint().epoch == 2);

    // A peer's 369 value belongs to its current sequence epoch. When a new
    // process requests a reset, the first Logon response correctly reports 0.
    // The old epoch's acknowledgement must not make that value look regressed.
    TempDirectory restart_directory;
    auto established_cfg = config(restart_directory, nf::SessionRole::initiator);
    nf::SessionSettings established_settings{};
    established_settings.check_sending_time = false;
    established_cfg.settings = established_settings;
    established_cfg.journal_capacity_bytes = 2U * 1024U * 1024U;
    established_cfg.journal_max_records = 20'000;
    established_cfg.journal_segment_bytes = 8U * 1024U;
    established_cfg.journal_max_segments = 8;
    {
        auto created = nf::SessionDriver::create(established_cfg);
        CHECK(created.ok());
        if (!created.ok()) return false;
        auto driver = std::move(created).value();
        Capture capture;
        CHECK(driver.connected(0, t0).ok());
        CHECK(flush_all(driver, capture, 0));
        const auto peer_logon = logon("SELL", "BUY", 1, t1,
            nf::FixVersion::fix44, false, {}, {}, {}, 1);
        CHECK(driver.receive(peer_logon, 1, t1).ok());
        CHECK(driver.state() == nf::SessionState::active);
    }
    auto reset_cfg = established_cfg;
    auto reset_settings = established_settings;
    reset_settings.allow_sequence_reset = true;
    reset_settings.logon.request_sequence_reset = true;
    reset_cfg.settings = reset_settings;
    {
        auto created = nf::SessionDriver::create(reset_cfg);
        CHECK(created.ok());
        if (!created.ok()) return false;
        auto driver = std::move(created).value();
        Capture capture;
        CHECK(driver.connected(2, t2).ok());
        CHECK(flush_all(driver, capture, 2));
        CHECK(get_field(capture.bytes, 141) == "Y");
        const auto peer_reset_logon = logon("SELL", "BUY", 1, t3,
            nf::FixVersion::fix44, true, {}, {}, {}, 0);
        CHECK(driver.receive(peer_reset_logon, 3, t3).ok());
        CHECK(driver.state() == nf::SessionState::active);
    }

    TempDirectory denied_directory;
    auto denied_cfg = config(denied_directory, nf::SessionRole::initiator);
    denied_cfg.settings = settings;
    denied_cfg.journal_capacity_bytes = 2U * 1024U * 1024U;
    denied_cfg.journal_max_records = 20'000;
    denied_cfg.journal_segment_bytes = 8U * 1024U;
    denied_cfg.journal_max_segments = 8;
    auto denied_created = nf::SessionDriver::create(denied_cfg);
    CHECK(denied_created.ok());
    if (!denied_created.ok()) return false;
    auto denied = std::move(denied_created).value();
    Capture denied_capture;
    CHECK(denied.connected(0, t0).ok());
    CHECK(flush_all(denied, denied_capture, 0));
    CHECK(denied.receive(logon("SELL", "BUY", 1, t1), 1, t1).code ==
        nf::ErrorCode::sequence_reset_denied);
    return true;
}

bool test_sequence_reset_and_trimmed_gapfill() {
    for (const bool trimmed_gapfill : {false, true}) {
        TempDirectory directory;
        auto cfg = config(directory, nf::SessionRole::acceptor);
        nf::SessionSettings settings{};
        settings.check_sending_time = false;
        cfg.settings = settings;
        cfg.journal_capacity_bytes = 2U * 1024U * 1024U;
        cfg.journal_max_records = 20'000;
        cfg.journal_segment_bytes = 8U * 1024U;
        cfg.journal_max_segments = 8;
        nf::RecordId inbound_id{};
        {
            auto created = nf::SessionDriver::create(cfg);
            if (!created.ok()) std::cerr << "sequence reset create: " << created.status().message() << " tag=" << created.status().tag << " off=" << created.status().offset << '\n';
            CHECK(created.ok());
            if (!created.ok()) return false;
            auto driver = std::move(created).value();
            Capture capture;
            CHECK(driver.connected(0, t0).ok());
            CHECK(driver.receive(logon("BUY", "SELL", 1, t1), 1, t1).ok());
            CHECK(driver.state() == nf::SessionState::active);
            const auto reset = trimmed_gapfill
                ? gapfill("BUY", "SELL", 1, t2, 5, true)
                : sequence_reset("BUY", "SELL", 999, t2, 5);
            CHECK(driver.receive(reset, 2, t2).ok());
            CHECK(driver.next_inbound_seq() == 5);
            CHECK(flush_all(driver, capture, 2));
        }
        auto journal = open_journal(cfg);
        CHECK(journal.ok());
        if (!journal.ok()) continue;
        CHECK(journal.value().next_inbound_seq() == 5);
        CHECK(journal.value().completed_inbound_seq() == 4);
        bool saw_decision = false;
        for (const auto& record : journal.value().records()) {
            if (record.kind != nf::RecordKind::inbound || record.seq != 2 || record.next_seq != 5) continue;
            CHECK(get_field(record.payload, 34) == (trimmed_gapfill ? "1" : "999"));
            saw_decision = true;
            inbound_id = record.id;
        }
        CHECK(saw_decision);
        CHECK(inbound_id != 0);
    }
    return true;
}

bool test_schema_reject_peer_reject_and_sending_time_logout() {
    TempDirectory directory;
    auto cfg = config(directory, nf::SessionRole::initiator);
    nf::SessionSettings settings{};
    settings.check_sending_time = false;
    cfg.settings = settings;
    int app_calls = 0;
    nf::SessionCallbacks callbacks{};
    callbacks.validate_message = [](const nf::FlatMessageView& message) {
        const auto type = message.find_unique(35);
        if (type.ok() && type.value().value == "Z") return nf::Status{nf::ErrorCode::message_unhandled, 35};
        return nf::Status::success();
    };
    callbacks.on_message = [&](nf::SessionContext&, const nf::FlatMessageView&,
        const nf::DeliveryInfo&) { ++app_calls; return nf::Status::success(); };
    nf::Status observed{};
    callbacks.on_error = [&](nf::Status status) { observed = status; };
    auto created = nf::SessionDriver::create(cfg, std::move(callbacks));
    CHECK(created.ok());
    if (!created.ok()) return false;
    auto driver = std::move(created).value();
    Capture capture;
    CHECK(start_initiator(driver, capture));
    auto unknown = nf::message<"Z">().set<58>("unknown type");
    const auto invalid = frame(unknown, "SELL", "BUY", 2, t2);
    CHECK(driver.receive(invalid, 2, t2).ok());
    CHECK(driver.state() == nf::SessionState::active);
    CHECK(driver.next_inbound_seq() == 3);
    CHECK(app_calls == 0);
    CHECK(observed.code == nf::ErrorCode::message_unhandled);
    CHECK(flush_all(driver, capture, 2));
    CHECK(get_field(capture.bytes, 35) == "A"); // first outbound frame is the Logon
    nf::FrameView first{};
    CHECK(nf::inspect_frame(capture.bytes, first).ok());
    const auto remain = std::string_view{capture.bytes}.substr(first.consumed);
    CHECK(get_field(remain, 35) == "3");
    CHECK(get_field(remain, 45) == "2");
    CHECK(get_field(remain, 371) == "35");
    CHECK(get_field(remain, 372) == "Z");
    CHECK(get_field(remain, 373) == "11");

    auto peer_reject = nf::message<"3">().set<45>(1).set<371>(11).set<372>("D").set<373>(5).set<58>("bad value");
    CHECK(driver.receive(frame(peer_reject, "SELL", "BUY", 3, t3), 3, t3).ok());
    CHECK(observed.code == nf::ErrorCode::peer_rejected);
    CHECK(driver.state() == nf::SessionState::active);
    return true;
}

bool test_sending_time_rejects_then_logs_out() {
    TempDirectory directory;
    auto cfg = config(directory, nf::SessionRole::initiator);
    nf::SessionSettings settings{};
    settings.check_sending_time = true;
    settings.max_sending_time_skew_ms = 1'000;
    cfg.settings = settings;
    auto created = nf::SessionDriver::create(cfg);
    if (!created.ok()) std::cerr << "retention create: " << created.status().message() << " tag=" << created.status().tag << " off=" << created.status().offset << '\n';
    CHECK(created.ok());
    if (!created.ok()) return false;
    auto driver = std::move(created).value();
    Capture capture;
    CHECK(start_initiator(driver, capture));
    CHECK(driver.receive(order("SELL", "BUY", 2, far_time, "old-time"), 4, t4).ok());
    CHECK(driver.state() == nf::SessionState::logout_pending);
    CHECK(driver.next_inbound_seq() == 2);
    CHECK(flush_all(driver, capture, 4));
    nf::FrameView first{};
    CHECK(nf::inspect_frame(capture.bytes, first).ok());
    std::string_view remaining{capture.bytes};
    remaining.remove_prefix(first.consumed);
    nf::FrameView reject{};
    CHECK(nf::inspect_frame(remaining, reject).ok());
    CHECK(get_field(reject.bytes, 35) == "3");
    CHECK(get_field(reject.bytes, 45) == "2");
    CHECK(get_field(reject.bytes, 371) == "52");
    CHECK(get_field(reject.bytes, 373) == "10");
    remaining.remove_prefix(reject.consumed);
    nf::FrameView logout{};
    CHECK(nf::inspect_frame(remaining, logout).ok());
    CHECK(get_field(logout.bytes, 35) == "5");
    return true;
}

bool test_completed_watermark_369_and_reader_backpressure() {
    TempDirectory directory;
    auto cfg = config(directory, nf::SessionRole::initiator);
    nf::SessionSettings settings{};
    settings.check_sending_time = false;
    cfg.settings = settings;
    nf::SessionCallbacks callbacks{};
    nf::SessionDriver* owner = nullptr;
    nf::DeliveryInfo pending{};
    callbacks.deferred_delivery = true;
    callbacks.on_deferred_message = [&](const nf::DeliveryLease& lease) {
        pending = lease.info;
        return owner == nullptr ? nf::Status{nf::ErrorCode::engine_stopped} : owner->defer_delivery(lease.info);
    };
    cfg.max_pending_inbound_messages = 1;
    cfg.max_pending_inbound_bytes = cfg.max_frame_bytes;
    auto created = nf::SessionDriver::create(cfg, std::move(callbacks));
    CHECK(created.ok());
    if (!created.ok()) return false;
    auto driver = std::move(created).value();
    owner = &driver;
    Capture capture;
    CHECK(start_initiator(driver, capture));
    auto outbound = nf::message<"D">().set<11>("processed").set<55>("AAPL");
    const auto receipt = driver.send(outbound, 2, t2);
    CHECK(receipt.ok() && receipt.value().msg_seq_num == 2);
    CHECK(flush_all(driver, capture, 2));
    nf::FrameView first_outbound{};
    CHECK(nf::inspect_frame(capture.bytes, first_outbound).ok());
    const auto after_logon = std::string_view{capture.bytes}.substr(first_outbound.consumed);
    CHECK(get_field(after_logon, 35) == "D");
    CHECK(get_field(after_logon, 369) == "1");
    CHECK(driver.can_receive());
    const auto first = order("SELL", "BUY", 2, t2, "pending", nf::FixVersion::fix44, 2);
    CHECK(driver.receive(first, 2, t2).ok());
    CHECK(driver.pending_inbound_messages() == 1);
    CHECK(driver.pending_inbound_bytes() == first.size());
    CHECK(!driver.can_receive());
    const auto second = order("SELL", "BUY", 3, t3, "retry", nf::FixVersion::fix44, 2);
    const auto saturated = driver.receive(second, 3, t3);
    CHECK(saturated.code == nf::ErrorCode::queue_full);
    CHECK(driver.state() == nf::SessionState::active);
    CHECK(driver.next_inbound_seq() == 3);
    CHECK(driver.complete_delivery(pending, nf::Status::success(), 0, 4, t4).ok());
    CHECK(driver.pending_inbound_messages() == 0);
    CHECK(driver.pending_inbound_bytes() == 0);
    CHECK(driver.can_receive());
    CHECK(driver.receive(second, 4, t4).ok());
    CHECK(driver.pending_inbound_messages() == 1);
    CHECK(driver.complete_delivery(pending, nf::Status::success(), 0, 5, t4).ok());
    const auto regressed_ack = heartbeat("SELL", "BUY", 4, t4, 1);
    CHECK(driver.receive(regressed_ack, 6, t4).code == nf::ErrorCode::protocol_error);
    CHECK(driver.state() == nf::SessionState::failed);
    return true;
}

bool test_retention_reports_history_unavailable() {
    TempDirectory directory;
    auto cfg = config(directory, nf::SessionRole::initiator);
    nf::SessionSettings settings{};
    settings.check_sending_time = false;
    cfg.settings = settings;
    cfg.journal_capacity_bytes = 2U * 1024U * 1024U;
    cfg.journal_max_records = 20'000;
    cfg.journal_segment_bytes = 8U * 1024U;
    cfg.journal_max_segments = 8;
    cfg.retain_outbound_messages = 1;
    auto created = nf::SessionDriver::create(cfg);
    CHECK(created.ok());
    if (!created.ok()) return false;
    auto driver = std::move(created).value();
    Capture capture;
    CHECK(start_initiator(driver, capture));
    auto message = nf::message<"D">().set<11>("retained").set<55>("AAPL");
    for (std::uint64_t i = 0; i < 120; ++i) CHECK(driver.send(message, 2 + i, t2).ok());
    CHECK(flush_all(driver, capture, 200));
    const auto last_outbound = driver.next_outbound_seq() - 1;
    CHECK(last_outbound > 1);
    CHECK(driver.receive(heartbeat("SELL", "BUY", 2, t3, last_outbound), 300, t3).ok());
    auto replay = nf::message<"2">().set<7>(1).set<16>(1);
    const auto failed = driver.receive(frame(replay, "SELL", "BUY", 3, t4), 301, t4);
    CHECK(failed.code == nf::ErrorCode::history_unavailable);
    CHECK(driver.state() == nf::SessionState::failed);
    CHECK(!driver.needs_write());
    return true;
}

} // namespace

int main() {
    test_fixt_logon_wire_and_journal_redaction();
    test_acceptor_auth_and_inbound_logon_redaction();
    test_preauthenticated_logon_skips_duplicate_auth();
    test_reset_flag_requires_policy_and_is_audited();
    test_initiator_reset_request_and_peer_agreement();
    test_sequence_reset_and_trimmed_gapfill();
    test_schema_reject_peer_reject_and_sending_time_logout();
    test_sending_time_rejects_then_logs_out();
    test_completed_watermark_369_and_reader_backpressure();
    test_retention_reports_history_unavailable();
    if (failures != 0) return 1;
    std::cout << "protocol session tests passed\n";
    return 0;
}
