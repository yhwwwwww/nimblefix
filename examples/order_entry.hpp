#pragma once

#include "endpoint.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <poll.h>
#include <string_view>
#include <thread>

#include <unistd.h>

#include <nimblefix/decimal.hpp>
#include <nimblefix/message.hpp>

#if defined(NIMBLEFIX_BUILTIN_FIX44) && NIMBLEFIX_BUILTIN_FIX44
#include <nimblefix/fix44.hpp>
#endif

namespace nimblefix_example::order_entry {

namespace nf = ::nimblefix;
namespace demo = ::nimblefix_example;

#if defined(NIMBLEFIX_BUILTIN_FIX44) && NIMBLEFIX_BUILTIN_FIX44
using ExampleSchema = nf::fix44::Schema;
using ExampleEngine = nf::Engine<ExampleSchema>;
using ExampleSender = nf::Sender<ExampleSchema>;
#else
using ExampleSchema = nf::FlatSchema;
using ExampleEngine = nf::Engine<>;
using ExampleSender = nf::Sender<>;
#endif

template <class T, std::size_t Capacity>
class SpscQueue {
public:
    [[nodiscard]] bool push(const T& value) noexcept {
        const auto write = write_.load(std::memory_order_relaxed);
        const auto read = read_.load(std::memory_order_acquire);
        if (write - read == Capacity) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        entries_[write % Capacity] = value;
        write_.store(write + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool pop(T& value) noexcept {
        const auto read = read_.load(std::memory_order_relaxed);
        if (read == write_.load(std::memory_order_acquire)) return false;
        value = entries_[read % Capacity];
        read_.store(read + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] std::uint64_t dropped() const noexcept {
        return dropped_.load(std::memory_order_relaxed);
    }

private:
    std::array<T, Capacity> entries_{};
    std::atomic<std::size_t> write_{};
    std::atomic<std::size_t> read_{};
    std::atomic<std::uint64_t> dropped_{};
};

struct ExecutionReportEvent {
    std::array<char, 65> cl_ord_id{};
    std::array<char, 33> symbol{};
    std::array<char, 65> order_id{};
    std::array<char, 65> exec_id{};
    std::uint8_t cl_ord_id_size{};
    std::uint8_t symbol_size{};
    std::uint8_t order_id_size{};
    std::uint8_t exec_id_size{};
    char side{};
    char exec_type{};
    char ord_status{};
    nf::Decimal order_qty{};
    nf::Decimal leaves_qty{};
    nf::Decimal cum_qty{};
    nf::Decimal avg_px{};
    nf::ErrorCode error{nf::ErrorCode::none};
    bool has_order_qty{};
    bool valid{true};
};

using ReportQueue = SpscQueue<ExecutionReportEvent, 512>;
using DiagnosticQueue = SpscQueue<demo::ConsoleEvent, 128>;

template <std::size_t Size>
inline bool copy_text(std::array<char, Size>& target, std::uint8_t& target_size,
    std::string_view value) noexcept {
    if (value.empty() || value.size() > Size || value.size() > UINT8_MAX) return false;
    std::copy(value.begin(), value.end(), target.begin());
    target_size = static_cast<std::uint8_t>(value.size());
    return true;
}

inline void mark_invalid(ExecutionReportEvent& event, nf::ErrorCode code) noexcept {
    event.valid = false;
    if (event.error == nf::ErrorCode::none) event.error = code;
}

template <std::size_t Size>
inline bool copy_required_text(const nf::FlatMessageView& message, std::uint32_t tag,
    std::array<char, Size>& target, std::uint8_t& target_size,
    ExecutionReportEvent& event) noexcept {
    const auto found = message.find_unique(tag);
    if (!found.ok()) {
        mark_invalid(event, found.status().code);
        return false;
    }
    if (!copy_text(target, target_size, found.value().value)) {
        mark_invalid(event, nf::ErrorCode::invalid_value);
        return false;
    }
    return true;
}

inline bool read_character(const nf::FlatMessageView& message, std::uint32_t tag,
    char& value, ExecutionReportEvent& event) noexcept {
    const auto found = message.find_unique(tag);
    if (!found.ok()) {
        mark_invalid(event, found.status().code);
        return false;
    }
    if (found.value().value.size() != 1) {
        mark_invalid(event, nf::ErrorCode::invalid_value);
        return false;
    }
    value = found.value().value.front();
    return true;
}

inline bool read_decimal(const nf::FlatMessageView& message, std::uint32_t tag,
    nf::Decimal& value, ExecutionReportEvent& event) noexcept {
    const auto found = message.find_unique(tag);
    if (!found.ok()) {
        mark_invalid(event, found.status().code);
        return false;
    }
    const auto parsed = nf::Decimal::parse(found.value().value);
    if (!parsed.ok()) {
        mark_invalid(event, nf::ErrorCode::invalid_decimal);
        return false;
    }
    value = parsed.value();
    return true;
}

inline ExecutionReportEvent make_report_event(const nf::FlatMessageView& message) noexcept {
    ExecutionReportEvent event{};
    (void)copy_required_text(message, 11, event.cl_ord_id, event.cl_ord_id_size, event);
    (void)copy_required_text(message, 55, event.symbol, event.symbol_size, event);
    (void)copy_required_text(message, 37, event.order_id, event.order_id_size, event);
    (void)copy_required_text(message, 17, event.exec_id, event.exec_id_size, event);
    (void)read_character(message, 54, event.side, event);
    (void)read_character(message, 150, event.exec_type, event);
    (void)read_character(message, 39, event.ord_status, event);
    (void)read_decimal(message, 151, event.leaves_qty, event);
    (void)read_decimal(message, 14, event.cum_qty, event);
    (void)read_decimal(message, 6, event.avg_px, event);

    const auto order_qty = message.find_unique(38);
    if (order_qty.ok()) {
        const auto parsed = nf::Decimal::parse(order_qty.value().value);
        if (!parsed.ok()) mark_invalid(event, nf::ErrorCode::invalid_decimal);
        else {
            event.order_qty = parsed.value();
            event.has_order_qty = true;
        }
    } else if (order_qty.status().code != nf::ErrorCode::missing_field) {
        mark_invalid(event, order_qty.status().code);
    }
    return event;
}

inline void push_diagnostic(DiagnosticQueue& events, const nf::ErrorEvent& error) noexcept {
    demo::ConsoleEvent event{};
    event.kind = demo::ConsoleEvent::Kind::error;
    event.code = error.status.code;
    std::size_t used = 0;
    const auto append = [&](std::string_view text) {
        const auto count = std::min(text.size(), event.text.size() - used);
        if (count != 0) std::copy_n(text.data(), count, event.text.data() + used);
        used += count;
    };
    append(error.session_name);
    if (used != 0) append(": ");
    append(error.status.message());
    event.size = static_cast<std::uint16_t>(used);
    (void)events.push(event);
}

inline void print_diagnostics(DiagnosticQueue& events, std::uint64_t& dropped_seen) {
    demo::ConsoleEvent event{};
    while (events.pop(event)) {
        std::cerr << "ERROR [" << static_cast<int>(event.code) << "] "
            << std::string_view{event.text.data(), event.size} << '\n' << std::flush;
    }
    const auto dropped = events.dropped();
    if (dropped != dropped_seen) {
        std::cerr << "ERROR diagnostic queue dropped " << (dropped - dropped_seen)
            << " event(s)\n" << std::flush;
        dropped_seen = dropped;
    }
}

struct ParsedOrder {
    std::string_view cl_ord_id{};
    std::string_view symbol{};
    nf::Decimal quantity{};
    nf::Decimal price{};
    char side{};
    bool market{};
};

inline bool split_words(std::string_view line, std::array<std::string_view, 7>& words,
    std::size_t& count) noexcept {
    count = 0;
    for (const unsigned char byte : line)
        if (byte < 0x20 && byte != '\t') return false;
    std::size_t cursor = 0;
    while (cursor < line.size()) {
        while (cursor < line.size() && (line[cursor] == ' ' || line[cursor] == '\t')) ++cursor;
        if (cursor == line.size()) break;
        const auto first = cursor;
        while (cursor < line.size() && line[cursor] != ' ' && line[cursor] != '\t') ++cursor;
        if (count == words.size()) return false;
        words[count++] = line.substr(first, cursor - first);
    }
    return true;
}

inline bool valid_token(std::string_view text) noexcept {
    if (text.empty()) return false;
    for (const unsigned char byte : text)
        if (byte < 0x20 || byte == 0x7f) return false;
    return true;
}

inline bool parse_order(const std::array<std::string_view, 7>& words, std::size_t count,
    ParsedOrder& result, std::string_view& error) noexcept {
    if (count != 6 || words[0] != "order") {
        error = "usage: order ID SYMBOL buy|sell QTY PRICE|market";
        return false;
    }
    if (words[1].size() > 64 || !valid_token(words[1])) {
        error = "ID must be 1..64 printable bytes";
        return false;
    }
    if (words[2].size() > 32 || !valid_token(words[2])) {
        error = "SYMBOL must be 1..32 printable bytes";
        return false;
    }
    if (words[3] == "buy") result.side = '1';
    else if (words[3] == "sell") result.side = '2';
    else {
        error = "side must be buy or sell";
        return false;
    }
    if (words[4].size() > 64) {
        error = "QTY must be a positive decimal of at most 64 bytes";
        return false;
    }
    const auto quantity = nf::Decimal::parse(words[4]);
    if (!quantity.ok() || quantity.value().coefficient() <= 0) {
        error = "QTY must be a positive exact decimal";
        return false;
    }
    if (words[5] == "market") {
        result.market = true;
    } else {
        if (words[5].size() > 64) {
            error = "PRICE must be a positive decimal of at most 64 bytes or market";
            return false;
        }
        const auto price = nf::Decimal::parse(words[5]);
        if (!price.ok() || price.value().coefficient() <= 0) {
            error = "PRICE must be a positive exact decimal or market";
            return false;
        }
        result.market = false;
        result.price = price.value();
    }
    result.cl_ord_id = words[1];
    result.symbol = words[2];
    result.quantity = quantity.value();
    return true;
}

template <class Sender>
nf::Result<nf::SendReceipt> send_order(const Sender& sender, const ParsedOrder& order,
    std::string_view transact_time) noexcept {
#if defined(NIMBLEFIX_BUILTIN_FIX44) && NIMBLEFIX_BUILTIN_FIX44
    if (order.market) {
        auto message = nf::fix44::NewOrderSingle::make()
            .cl_ord_id(order.cl_ord_id)
            .handl_inst('1')
            .symbol(order.symbol)
            .side(order.side)
            .transact_time(transact_time)
            .order_qty(order.quantity)
            .ord_type('1');
        return sender.send(message);
    }
    auto message = nf::fix44::NewOrderSingle::make()
        .cl_ord_id(order.cl_ord_id)
        .handl_inst('1')
        .symbol(order.symbol)
        .side(order.side)
        .transact_time(transact_time)
        .order_qty(order.quantity)
        .ord_type('2')
        .price(order.price);
    return sender.send(message);
#else
    if (order.market) {
        auto message = nf::message<"D">()
            .set<11>(order.cl_ord_id)
            .set<21>('1')
            .set<55>(order.symbol)
            .set<54>(order.side)
            .set<60>(transact_time)
            .set<38>(order.quantity)
            .set<40>('1');
        return sender.send(message);
    }
    auto message = nf::message<"D">()
        .set<11>(order.cl_ord_id)
        .set<21>('1')
        .set<55>(order.symbol)
        .set<54>(order.side)
        .set<60>(transact_time)
        .set<38>(order.quantity)
        .set<40>('2')
        .set<44>(order.price);
    return sender.send(message);
#endif
}

struct TrackedOrder {
    std::array<char, 65> cl_ord_id{};
    std::array<char, 33> symbol{};
    std::uint8_t cl_ord_id_size{};
    std::uint8_t symbol_size{};
    nf::Decimal quantity{};
    char side{};
    std::uint64_t receipt_id{};
    std::uint64_t msg_seq_num{};
    nf::ReceiptState last_receipt_state{nf::ReceiptState::accepted};
    std::chrono::steady_clock::time_point deadline{};
    bool receipt_written{};
    bool receipt_terminal{};
    bool report_received{};
    bool report_matches{};
    bool timeout_reported{};
};

constexpr std::size_t max_tracked_orders = 256;

inline std::string_view text_of(const TrackedOrder& order) noexcept {
    return {order.cl_ord_id.data(), order.cl_ord_id_size};
}

inline std::string_view text_of(const ExecutionReportEvent& report) noexcept {
    return {report.cl_ord_id.data(), report.cl_ord_id_size};
}

inline std::string_view text_of_symbol(const ExecutionReportEvent& report) noexcept {
    return {report.symbol.data(), report.symbol_size};
}

inline std::string_view receipt_state_name(nf::ReceiptState state) noexcept {
    switch (state) {
    case nf::ReceiptState::accepted: return "accepted";
    case nf::ReceiptState::appended: return "appended";
    case nf::ReceiptState::written: return "written";
    case nf::ReceiptState::failed: return "failed";
    case nf::ReceiptState::expired: return "expired";
    }
    return "unknown";
}

inline bool decimal_equal(nf::Decimal left, nf::Decimal right) noexcept {
    auto left_coefficient = left.coefficient();
    auto left_scale = left.scale();
    auto right_coefficient = right.coefficient();
    auto right_scale = right.scale();
    while (left_scale != 0 && left_coefficient % 10 == 0) {
        left_coefficient /= 10;
        --left_scale;
    }
    while (right_scale != 0 && right_coefficient % 10 == 0) {
        right_coefficient /= 10;
        --right_scale;
    }
    return left_coefficient == right_coefficient && left_scale == right_scale;
}

inline std::string_view decimal_text(nf::Decimal value, std::array<char, 64>& storage) noexcept {
    std::size_t written = 0;
    if (!value.format(storage, written).ok()) return "<invalid>";
    return {storage.data(), written};
}

inline bool is_report_fields_match(const TrackedOrder& order,
    const ExecutionReportEvent& report) noexcept {
    if (!report.valid || text_of(report) != text_of(order) ||
        text_of_symbol(report) != std::string_view{order.symbol.data(), order.symbol_size} ||
        report.side != order.side) return false;
    if (report.has_order_qty && !decimal_equal(report.order_qty, order.quantity)) return false;
    if (report.exec_type == '0' && report.ord_status == '0')
        return decimal_equal(report.leaves_qty, order.quantity) && report.cum_qty.coefficient() == 0;
    return true;
}

inline void process_report(const ExecutionReportEvent& report,
    std::array<TrackedOrder, max_tracked_orders>& orders, std::size_t order_count,
    bool& application_error) {
    std::array<char, 64> leaves_storage{}, cum_storage{}, avg_storage{};
    const auto side_name = report.side == '1' ? "buy" : report.side == '2' ? "sell" : "unknown";
    std::cout << "EXECUTION_REPORT ClOrdID="
        << (report.cl_ord_id_size == 0 ? "<missing>" : text_of(report))
        << " Symbol=" << (report.symbol_size == 0 ? "<missing>" : text_of_symbol(report))
        << " Side=" << side_name
        << " ExecType=" << (report.exec_type == 0 ? '?' : report.exec_type)
        << " OrdStatus=" << (report.ord_status == 0 ? '?' : report.ord_status)
        << " LeavesQty=" << decimal_text(report.leaves_qty, leaves_storage)
        << " CumQty=" << decimal_text(report.cum_qty, cum_storage)
        << " AvgPx=" << decimal_text(report.avg_px, avg_storage)
        << '\n' << std::flush;

    TrackedOrder* match = nullptr;
    for (std::size_t i = 0; i < order_count; ++i) {
        if (text_of(orders[i]) == text_of(report)) {
            match = &orders[i];
            break;
        }
    }
    if (match == nullptr) {
        std::cerr << "UNMATCHED ExecutionReport ClOrdID="
            << (report.cl_ord_id_size == 0 ? "<missing>" : text_of(report)) << '\n' << std::flush;
        return;
    }

    match->report_received = true;
    match->report_matches = is_report_fields_match(*match, report);
    if (!match->report_matches) {
        std::cout << "ORDER_RESULT ClOrdID=" << text_of(*match) << " state=mismatch\n" << std::flush;
        application_error = true;
        if (!report.valid)
            std::cerr << "ERROR invalid ExecutionReport fields, code="
                << static_cast<int>(report.error) << '\n' << std::flush;
        return;
    }
    const auto result = report.exec_type == '0' && report.ord_status == '0'
        ? "accepted" : report.exec_type == '8' || report.ord_status == '8' ? "rejected" : "response";
    std::cout << "ORDER_RESULT ClOrdID=" << text_of(*match) << " state=" << result << '\n' << std::flush;
}

inline void print_report_events(ReportQueue& events,
    std::array<TrackedOrder, max_tracked_orders>& orders, std::size_t order_count,
    bool& application_error, std::uint64_t& dropped_seen) {
    ExecutionReportEvent event{};
    while (events.pop(event)) process_report(event, orders, order_count, application_error);
    const auto dropped = events.dropped();
    if (dropped != dropped_seen) {
        std::cerr << "ERROR report queue dropped " << (dropped - dropped_seen)
            << " event(s); their business result is unknown\n" << std::flush;
        dropped_seen = dropped;
        application_error = true;
    }
}

inline void update_receipts(const ExampleSender& sender,
    std::array<TrackedOrder, max_tracked_orders>& orders, std::size_t order_count) {
    for (std::size_t i = 0; i < order_count; ++i) {
        auto& order = orders[i];
        if (order.receipt_terminal) continue;
        const auto current = sender.receipt(order.receipt_id);
        if (!current.ok()) {
            if (current.status().code == nf::ErrorCode::receipt_expired) {
                order.receipt_terminal = true;
                std::cout << "RECEIPT ClOrdID=" << text_of(order)
                    << " state=unknown sequence=" << order.msg_seq_num
                    << " reason=" << current.status().message() << '\n' << std::flush;
            } else {
                order.receipt_terminal = true;
                std::cout << "RECEIPT ClOrdID=" << text_of(order)
                    << " state=failed sequence=" << order.msg_seq_num
                    << " reason=" << current.status().message() << '\n' << std::flush;
            }
            continue;
        }
        const auto& receipt = current.value();
        order.msg_seq_num = receipt.msg_seq_num;
        if (receipt.state == nf::ReceiptState::accepted || receipt.state == order.last_receipt_state)
            continue;
        order.last_receipt_state = receipt.state;
        std::cout << "RECEIPT ClOrdID=" << text_of(order)
            << " state=" << receipt_state_name(receipt.state)
            << " sequence=" << receipt.msg_seq_num;
        if (!receipt.status.ok()) std::cout << " reason=" << receipt.status.message();
        std::cout << '\n' << std::flush;
        if (receipt.state == nf::ReceiptState::written) {
            order.receipt_written = true;
            order.receipt_terminal = true;
        } else if (receipt.state == nf::ReceiptState::failed ||
            receipt.state == nf::ReceiptState::expired) {
            order.receipt_terminal = true;
        }
    }
}

inline void print_status(const ExampleEngine& engine, const ExampleSender& sender) {
    const auto snapshot = engine.snapshot("demo-initiator");
    if (!snapshot.ok()) {
        std::cout << "STATUS error=" << snapshot.status().message()
            << " can_send=" << (sender.can_send() ? "yes" : "no") << '\n' << std::flush;
        return;
    }
    const auto& value = snapshot.value();
    std::string_view state = "unknown";
    switch (value.state) {
    case nf::SessionState::disconnected: state = "disconnected"; break;
    case nf::SessionState::awaiting_logon: state = "awaiting_logon"; break;
    case nf::SessionState::active: state = "active"; break;
    case nf::SessionState::logout_pending: state = "logout_pending"; break;
    case nf::SessionState::closed: state = "closed"; break;
    case nf::SessionState::failed: state = "failed"; break;
    }
    std::cout << "STATUS state=" << state
        << " can_send=" << (sender.can_send() ? "yes" : "no")
        << " next_in=" << value.next_inbound_seq
        << " next_out=" << value.next_outbound_seq
        << " pending=" << value.pending_messages
        << " written_bytes=" << value.written_bytes << '\n' << std::flush;
}

inline bool find_duplicate_id(const std::array<TrackedOrder, max_tracked_orders>& orders,
    std::size_t order_count, std::string_view id) noexcept {
    for (std::size_t i = 0; i < order_count; ++i)
        if (text_of(orders[i]) == id) return true;
    return false;
}

inline void handle_line(std::string_view line, const ExampleSender& sender,
    const ExampleEngine& engine, std::array<TrackedOrder, max_tracked_orders>& orders,
    std::size_t& order_count, std::uint32_t timeout_ms, bool& stop_input) {
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    std::array<std::string_view, 7> words{};
    std::size_t count = 0;
    if (!split_words(line, words, count)) {
        std::cerr << "ERROR command contains an invalid control byte or too many words\n" << std::flush;
        return;
    }
    if (count == 0) return;
    if (words[0] == "status") {
        if (count != 1) std::cerr << "ERROR usage: status\n" << std::flush;
        else print_status(engine, sender);
        return;
    }
    if (words[0] == "quit") {
        if (count != 1) {
            std::cerr << "ERROR usage: quit\n" << std::flush;
            return;
        }
        stop_input = true;
        return;
    }
    if (words[0] != "order") {
        std::cerr << "ERROR commands: order ID SYMBOL buy|sell QTY PRICE|market | status | quit\n" << std::flush;
        return;
    }
    ParsedOrder parsed{};
    std::string_view parse_error{};
    if (!parse_order(words, count, parsed, parse_error)) {
        std::cerr << "ERROR " << parse_error << '\n' << std::flush;
        return;
    }
    if (find_duplicate_id(orders, order_count, parsed.cl_ord_id)) {
        std::cerr << "ERROR duplicate ClOrdID=" << parsed.cl_ord_id << '\n' << std::flush;
        return;
    }
    if (order_count == orders.size()) {
        std::cerr << "ERROR accepted-order tracking is full (limit " << orders.size() << ")\n" << std::flush;
        return;
    }
    if (!sender.can_send()) {
        std::cerr << "ERROR session is not active; order was not accepted\n" << std::flush;
        return;
    }

    std::array<char, 22> time_storage{};
    const auto transact_time = demo::utc_timestamp(time_storage);
    if (transact_time.empty()) {
        std::cerr << "ERROR could not read UTC clock; order was not accepted\n" << std::flush;
        return;
    }

    auto& tracked = orders[order_count];
    tracked = {};
    if (!copy_text(tracked.cl_ord_id, tracked.cl_ord_id_size, parsed.cl_ord_id) ||
        !copy_text(tracked.symbol, tracked.symbol_size, parsed.symbol)) {
        std::cerr << "ERROR could not store order metadata\n" << std::flush;
        return;
    }
    tracked.quantity = parsed.quantity;
    tracked.side = parsed.side;
    tracked.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    const auto sent = send_order(sender, parsed, transact_time);
    if (!sent.ok()) {
        tracked = {};
        std::cerr << "REJECTED ClOrdID=" << parsed.cl_ord_id
            << " status=" << sent.status().message() << "\n" << std::flush;
        return;
    }
    tracked.receipt_id = sent.value().id;
    tracked.msg_seq_num = sent.value().msg_seq_num;
    tracked.last_receipt_state = sent.value().state;
    ++order_count;
    std::cout << "ACCEPTED ClOrdID=" << text_of(tracked)
        << " receipt=" << tracked.receipt_id << '\n' << std::flush;
}

inline bool all_orders_settled(const std::array<TrackedOrder, max_tracked_orders>& orders,
    std::size_t order_count, std::chrono::steady_clock::time_point now) noexcept {
    for (std::size_t i = 0; i < order_count; ++i) {
        const auto& order = orders[i];
        if (order.receipt_written && order.report_received) continue;
        if (now < order.deadline) return false;
    }
    return true;
}

inline void report_expired_orders(std::array<TrackedOrder, max_tracked_orders>& orders,
    std::size_t order_count, std::chrono::steady_clock::time_point now,
    bool& timed_out) {
    for (std::size_t i = 0; i < order_count; ++i) {
        auto& order = orders[i];
        if (order.timeout_reported || (order.receipt_written && order.report_received) || now < order.deadline)
            continue;
        order.timeout_reported = true;
        timed_out = true;
        std::cout << "TIMEOUT ClOrdID=" << text_of(order)
            << " receipt=" << receipt_state_name(order.last_receipt_state)
            << " sequence=" << order.msg_seq_num
            << " business_result=unknown\n" << std::flush;
    }
}

inline int finish_engine(ExampleEngine& engine, std::thread& owner, std::atomic<bool>& run_done,
    nf::Status& run_status, ExampleSender& sender,
    std::array<TrackedOrder, max_tracked_orders>& orders, std::size_t order_count,
    ReportQueue& report_events, DiagnosticQueue& diagnostic_events,
    bool& application_error, std::uint64_t& dropped_reports, std::uint64_t& dropped_diagnostics,
    std::uint32_t timeout_ms) {
    engine.request_stop();
    const auto safety_deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(timeout_ms) + std::chrono::seconds(2);
    bool shutdown_timeout_reported = false;
    while (!run_done.load(std::memory_order_acquire)) {
        print_diagnostics(diagnostic_events, dropped_diagnostics);
        print_report_events(report_events, orders, order_count, application_error, dropped_reports);
        update_receipts(sender, orders, order_count);
        if (!shutdown_timeout_reported && std::chrono::steady_clock::now() >= safety_deadline) {
            std::cerr << "ERROR engine did not finish bounded Logout before the endpoint deadline\n" << std::flush;
            shutdown_timeout_reported = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    owner.join();
    print_diagnostics(diagnostic_events, dropped_diagnostics);
    print_report_events(report_events, orders, order_count, application_error, dropped_reports);
    update_receipts(sender, orders, order_count);
    if (!run_status.ok()) {
        std::cerr << "Engine stopped: " << run_status.message() << '\n' << std::flush;
        return 1;
    }
    if (shutdown_timeout_reported || application_error) return 1;
    return 0;
}

} // namespace nimblefix_example::order_entry
