#include <nimblefix/engine.hpp>
#include <nimblefix/message.hpp>

#if defined(NIMBLEFIX_SAMPLE_SCHEMA)
#include <nimblefix/fix44_sample.hpp>
#endif

#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <charconv>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include <unistd.h>

namespace nf = nimblefix;
using namespace nf::literals;

namespace {

struct PairResult {
    nf::Status status{};
    nf::Status first_error{};
    std::uint64_t order_sequence{};
    bool received_report{};
    bool valid_report{};
    std::size_t error_count{};
};

bool is_message(const nf::FlatMessageView& message, std::string_view expected) {
    const auto type = message.find_unique(35);
    return type.ok() && type.value().value == expected;
}

std::string current_utc_timestamp() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto whole_seconds = time_point_cast<seconds>(now);
    const auto milliseconds_part = duration_cast<milliseconds>(now - whole_seconds).count();
    const auto time_value = system_clock::to_time_t(now);
    std::tm utc{};
    if (::gmtime_r(&time_value, &utc) == nullptr) return {};
    std::array<char, 32> buffer{};
    const auto date_size = ::strftime(buffer.data(), buffer.size(), "%Y%m%d-%H:%M:%S", &utc);
    if (date_size != 17) return {};
    (void)std::snprintf(buffer.data() + date_size, buffer.size() - date_size,
        ".%03u", static_cast<unsigned>(milliseconds_part));
    return {buffer.data(), 21};
}

std::string_view format_business_id(std::array<char, 40>& buffer,
                                    std::string_view prefix,
                                    std::uint64_t value) noexcept {
    if (prefix.size() >= buffer.size()) return {};
    std::copy(prefix.begin(), prefix.end(), buffer.begin());
    const auto converted = std::to_chars(buffer.data() + prefix.size(),
        buffer.data() + buffer.size(), value);
    if (converted.ec != std::errc{}) return {};
    return {buffer.data(), static_cast<std::size_t>(converted.ptr - buffer.data())};
}

bool is_expected_report(const nf::FlatMessageView& message, std::string_view expected_id) noexcept {
    const auto cl_ord_id = message.find_unique(11);
    const auto side = message.find_unique(54);
    const auto leaves = message.find_unique(151);
    const auto cumulative = message.find_unique(14);
    const auto average = message.find_unique(6);
    const auto symbol = message.find_unique(55);
    const auto exec_type = message.find_unique(150);
    const auto order_status = message.find_unique(39);
    const auto order_id = message.find_unique(37);
    const auto exec_id = message.find_unique(17);
    if (!cl_ord_id.ok() || !side.ok() || !leaves.ok() || !cumulative.ok() ||
        !average.ok() || !symbol.ok() || !exec_type.ok() || !order_status.ok() ||
        !order_id.ok() || !exec_id.ok()) return false;
    const auto leaves_qty = nf::Decimal::parse(leaves.value().value);
    const auto cumulative_qty = nf::Decimal::parse(cumulative.value().value);
    const auto average_price = nf::Decimal::parse(average.value().value);
    const auto expected_qty = nf::Decimal::parse("100");
    const auto zero = nf::Decimal::parse("0");
    return cl_ord_id.value().value == expected_id && side.value().value == "1" &&
        symbol.value().value == "AAPL" && exec_type.value().value == "0" &&
        order_status.value().value == "0" &&
        leaves_qty.ok() && expected_qty.ok() && leaves_qty.value() == expected_qty.value() &&
        cumulative_qty.ok() && zero.ok() && cumulative_qty.value() == zero.value() &&
        average_price.ok() && average_price.value() == zero.value() &&
        order_id.value().value.starts_with("ORD-") && exec_id.value().value.starts_with("EXEC-");
}

PairResult run_pair(const std::filesystem::path& root, bool typed,
                    std::chrono::milliseconds timeout, std::string_view business_id) {
    PairResult result;
    const auto acceptor_dir = root / "acceptor";
    const auto initiator_dir = root / "initiator";
    nf::EngineConfig acceptor_config;
    acceptor_config.storage.directory = acceptor_dir;
    auto created_acceptor = nf::Engine<>::create(std::move(acceptor_config));
    if (!created_acceptor.ok()) {
        result.status = created_acceptor.status();
        return result;
    }
    auto acceptor = std::move(created_acceptor).value();

    std::atomic<bool> report_seen{false};
    std::atomic<bool> valid_report{false};
    std::mutex error_mutex;
    std::size_t error_count = 0;
    nf::Status first_error{};
    auto accepted_callback = acceptor.on_message(
        [typed](nf::SessionContext& context, const nf::FlatMessageView& incoming,
                const nf::DeliveryInfo& delivery) -> nf::Status {
            if (!is_message(incoming, "D")) return nf::Status::success();
            const auto cl_ord_id = incoming.find_unique(11);
            const auto symbol = incoming.find_unique(55);
            const auto side = incoming.find_unique(54);
            const auto order_qty_text = incoming.find_unique(38);
            if (!cl_ord_id.ok() || !symbol.ok() || !side.ok() || !order_qty_text.ok() ||
                side.value().value.size() != 1)
                return {nf::ErrorCode::invalid_value};
            const auto order_qty = nf::Decimal::parse(order_qty_text.value().value);
            if (!order_qty.ok()) return order_qty.status();

            std::array<char, 40> order_id_buffer{};
            std::array<char, 40> exec_id_buffer{};
            const auto order_id = format_business_id(order_id_buffer, "ORD-", delivery.record_id);
            const auto exec_id = format_business_id(exec_id_buffer, "EXEC-", delivery.record_id);
            if (order_id.empty() || exec_id.empty()) return {nf::ErrorCode::too_large};
#if defined(NIMBLEFIX_SAMPLE_SCHEMA)
            if (typed) {
                return context.send_stream<nf::fix44_sample::ExecutionReport>([&](auto& w) {
                    w.order_id(order_id)
                     .exec_id(exec_id)
                     .cl_ord_id(cl_ord_id.value().value)
                     .exec_type('0')
                     .ord_status('0')
                     .symbol(symbol.value().value)
                     .side(side.value().value.front())
                     .leaves_qty(order_qty.value())
                     .cum_qty(0)
                     .avg_px("0"_dec);
                }).status();
            }
#else
            (void)typed;
#endif
            auto report = nf::message<"8">()
                .set<37>(order_id)
                .set<17>(exec_id)
                .set<11>(cl_ord_id.value().value)
                .set<150>('0')
                .set<39>('0')
                .set<55>(symbol.value().value)
                .set<54>(side.value().value.front())
                .set<151>(order_qty.value())
                .set<14>(0)
                .set<6>("0"_dec);
            return context.send(report).status();
        });
    if (!accepted_callback.ok()) {
        result.status = accepted_callback;
        return result;
    }
    auto acceptor_errors = acceptor.on_error([&](const nf::ErrorEvent& error) {
        std::lock_guard lock(error_mutex);
        if (error_count++ == 0) first_error = error.status;
    });
    if (!acceptor_errors.ok()) {
        result.status = acceptor_errors;
        return result;
    }
    auto listener = acceptor.add_listener({
        .name = "loopback",
        .endpoint = {"127.0.0.1", 0},
    });
    if (!listener.ok()) {
        result.status = listener.status();
        return result;
    }
    auto acceptor_session = acceptor.add_acceptor({
        .name = "demo-sell",
        .listener = "loopback",
        .local_comp_id = "SELL",
        .remote_comp_id = "BUY",
    });
    if (!acceptor_session.ok()) {
        result.status = acceptor_session.status();
        return result;
    }
    const auto accepted_validation = acceptor.validate();
    if (!accepted_validation.ok()) {
        result.status = accepted_validation;
        return result;
    }

    nf::Status acceptor_run_status{};
    std::thread acceptor_thread([&] { acceptor_run_status = acceptor.run(); });
    const auto bound_deadline = std::chrono::steady_clock::now() + timeout;
    nf::Result<nf::TcpEndpoint> bound{nf::Status{nf::ErrorCode::need_more}};
    while (std::chrono::steady_clock::now() < bound_deadline) {
        bound = acceptor.listener_endpoint("loopback");
        if (bound.ok()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!bound.ok()) {
        acceptor.request_stop();
        acceptor_thread.join();
        result.status = acceptor_run_status.ok() ? bound.status() : acceptor_run_status;
        return result;
    }

    nf::EngineConfig initiator_config;
    initiator_config.storage.directory = initiator_dir;
    auto created_initiator = nf::Engine<>::create(std::move(initiator_config));
    if (!created_initiator.ok()) {
        acceptor.request_stop();
        acceptor_thread.join();
        result.status = created_initiator.status();
        return result;
    }
    auto initiator = std::move(created_initiator).value();
    auto initiator_errors = initiator.on_error([&](const nf::ErrorEvent& error) {
        std::lock_guard lock(error_mutex);
        if (error_count++ == 0) first_error = error.status;
    });
    if (!initiator_errors.ok()) {
        acceptor.request_stop();
        acceptor_thread.join();
        result.status = initiator_errors;
        return result;
    }
    auto received_callback = initiator.on_message(
        [&](nf::SessionContext&, const nf::FlatMessageView& incoming) -> nf::Status {
            if (is_message(incoming, "8")) {
                valid_report.store(is_expected_report(incoming, business_id), std::memory_order_release);
                report_seen.store(true, std::memory_order_release);
            }
            return nf::Status::success();
        });
    if (!received_callback.ok()) {
        acceptor.request_stop();
        acceptor_thread.join();
        result.status = received_callback;
        return result;
    }
    auto initiator_session = initiator.add_initiator({
        .name = "demo-buy",
        .sender_comp_id = "BUY",
        .target_comp_id = "SELL",
        .endpoint = {bound.value().host, bound.value().port},
    });
    if (!initiator_session.ok()) {
        acceptor.request_stop();
        acceptor_thread.join();
        result.status = initiator_session.status();
        return result;
    }
    const auto initiator_validation = initiator.validate();
    if (!initiator_validation.ok()) {
        acceptor.request_stop();
        acceptor_thread.join();
        result.status = initiator_validation;
        return result;
    }

    auto sender = initiator_session.value().sender<>();
    nf::Status initiator_run_status{};
    std::atomic<bool> initiator_done{false};
    std::thread initiator_thread([&] {
        initiator_run_status = initiator.run();
        initiator_done.store(true, std::memory_order_release);
    });
    const auto report_deadline = std::chrono::steady_clock::now() + timeout;
    while (!sender.can_send() && !initiator_done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < report_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));

    // The self-test coordinator explicitly submits one business request.
    // Connection lifecycle callbacks never create or resubmit an order.
    const auto transact_time = current_utc_timestamp();
    const auto submit_order = [&]() -> nf::Result<nf::SendReceipt> {
        if (transact_time.empty()) return nf::Status{nf::ErrorCode::invalid_timestamp};
#if defined(NIMBLEFIX_SAMPLE_SCHEMA)
        if (typed) {
            return sender.send_stream<nf::fix44_sample::NewOrderSingle>([&](auto& w) {
                w.cl_ord_id(business_id).symbol("AAPL").side('1')
                 .transact_time(transact_time).order_qty(100).ord_type('2').price("150.25"_dec);
            });
        }
#else
        (void)typed;
#endif
        auto order = nf::message<"D">()
            .set<11>(business_id).set<21>('1').set<55>("AAPL").set<54>('1')
            .set<60>(transact_time).set<38>(100).set<40>('2').set<44>("150.25"_dec);
        return sender.send(order);
    };
    const auto submitted = sender.can_send() ? submit_order()
        : nf::Result<nf::SendReceipt>{nf::Status{nf::ErrorCode::logon_timeout}};
    nf::Status submission_status = submitted.status();
    bool written = false;
    while (submission_status.ok() && !initiator_done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < report_deadline) {
        const auto receipt = sender.receipt(submitted.value().id);
        if (!receipt.ok()) { submission_status = receipt.status(); break; }
        result.order_sequence = receipt.value().msg_seq_num;
        if (receipt.value().state == nf::ReceiptState::failed ||
            receipt.value().state == nf::ReceiptState::expired) {
            submission_status = receipt.value().status;
            break;
        }
        written = receipt.value().state == nf::ReceiptState::written;
        if (written && report_seen.load(std::memory_order_acquire)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (submission_status.ok() && (!written || !report_seen.load(std::memory_order_acquire)))
        submission_status = {nf::ErrorCode::backlog_timeout};
    initiator.request_stop();
    acceptor.request_stop();
    initiator_thread.join();
    acceptor_thread.join();
    result.status = submission_status.ok() ? initiator_run_status : submission_status;
    result.received_report = report_seen.load(std::memory_order_acquire);
    result.valid_report = valid_report.load(std::memory_order_acquire);
    {
        std::lock_guard lock(error_mutex);
        result.error_count = error_count;
        result.first_error = first_error;
    }
    if (result.status.ok() && !acceptor_run_status.ok()) result.status = acceptor_run_status;
    if (result.status.ok() && !result.received_report) result.status = {nf::ErrorCode::logon_timeout};
    if (result.status.ok() && !result.valid_report) result.status = {nf::ErrorCode::invalid_value};
    if (result.status.ok() && result.error_count != 0)
        result.status = {nf::ErrorCode::callback_failed};
    return result;
}

} // namespace

int main(int argc, char** argv) {
    bool typed = false;
    bool restart = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view option{argv[i]};
        if (option == "--self-test") continue;
        if (option == "--typed") typed = true;
        else if (option == "--restart") restart = true;
        else {
            std::cerr << "usage: hello-fix [--self-test] [--typed] [--restart]\n";
            return 2;
        }
    }
#if !defined(NIMBLEFIX_SAMPLE_SCHEMA)
    if (typed) {
        std::cerr << "typed example requires the optional sample schema target\n";
        return 2;
    }
#endif
    char temp_template[] = "/tmp/nimblefix-hello-XXXXXX";
    char* temp_path = ::mkdtemp(temp_template);
    if (temp_path == nullptr) {
        std::cerr << "could not create temporary session directory\n";
        return 1;
    }
    const std::filesystem::path root{temp_path};
    auto first = run_pair(root, typed, std::chrono::seconds(10), "DEMO-001");
    bool restart_ok = true;
    std::uint64_t second_sequence = 0;
    if (first.status.ok() && first.received_report && restart) {
        const auto second = run_pair(root, typed, std::chrono::seconds(10), "DEMO-002");
        restart_ok = second.status.ok() && second.received_report && second.valid_report &&
            second.order_sequence > first.order_sequence;
        second_sequence = second.order_sequence;
        if (!restart_ok && first.status.ok()) first.status = second.status.ok()
            ? nf::Status{nf::ErrorCode::sequence_gap} : second.status;
        if (!restart_ok && first.error_count == 0) {
            first.error_count = second.error_count;
            first.first_error = second.first_error;
        }
    }
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    if (!first.status.ok() || !first.received_report || !first.valid_report || !restart_ok) {
        const auto failure = first.error_count != 0 ? first.first_error : first.status;
        std::cerr << "hello-fix self-test failed: "
                  << nf::ErrorEvent{"self-test", failure}.format() << '\n';
        return 1;
    }
    std::cout << "FIX 4.4 TCP self-test passed: Logon, NewOrderSingle, ExecutionReport, Logout";
    if (typed) std::cout << " (generated typed setter path)";
    if (restart) std::cout << "; outbound sequence advanced across restart to " << second_sequence;
    std::cout << '\n';
    return 0;
}
