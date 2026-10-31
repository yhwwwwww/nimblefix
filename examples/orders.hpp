#pragma once
#include "common.hpp"

#include <map>
#include <mutex>
#include <sstream>

namespace example {
struct Order {
    std::string id, symbol;
    char side{};
    nf::Decimal quantity{}, price{};
    bool market{};
};
inline Order parse_order(std::string_view line) {
    Order order;
    std::string command, side, quantity, price, extra;
    std::istringstream input{std::string(line)};
    if (!(input >> command >> order.id >> order.symbol >> side >> quantity >> price) ||
        input >> extra || command != "order")
        throw std::invalid_argument("order ID SYMBOL buy|sell QTY PRICE|market");
    if (order.id.size() > 64 || order.symbol.size() > 32 || (side != "buy" && side != "sell"))
        throw std::invalid_argument("ID <=64 bytes, SYMBOL <=32 bytes, side buy|sell required");
    for (const unsigned char byte : line)
        if ((byte < 32 && byte != '\t' && byte != '\r') || byte == 127)
            throw std::invalid_argument("control byte in order command");
    order.side = side == "buy" ? '1' : '2';
    const auto parsed_quantity = nf::Decimal::parse(quantity);
    if (!parsed_quantity.ok() || parsed_quantity.value().coefficient() <= 0)
        throw std::invalid_argument("quantity must be a positive decimal");
    order.quantity = parsed_quantity.value();
    order.market = price == "market";
    if (!order.market) {
        const auto parsed_price = nf::Decimal::parse(price);
        if (!parsed_price.ok() || parsed_price.value().coefficient() <= 0)
            throw std::invalid_argument("price must be a positive decimal or market");
        order.price = parsed_price.value();
    }
    return order;
}

// Only request/response correlation. This is not an order book. Queued
// callbacks mark replies while the command thread records send receipts.
class PendingRequests {
    struct Request { std::uint64_t receipt{}; bool replied{}; };
    std::mutex mutex_;
    std::map<std::string, Request, std::less<>> requests_;
public:
    void reserve(const std::string& id) {
        std::lock_guard lock(mutex_);
        if (requests_.size() == 256 || !requests_.try_emplace(id).second)
            throw std::invalid_argument("duplicate ID or 256-request example limit reached");
    }
    void accepted(const std::string& id, std::uint64_t receipt) {
        std::lock_guard lock(mutex_);
        requests_.at(id).receipt = receipt;
    }
    void discard(const std::string& id) {
        std::lock_guard lock(mutex_);
        requests_.erase(id);
    }
    void received(std::string_view id) {
        std::lock_guard lock(mutex_);
        if (auto found = requests_.find(id); found != requests_.end()) found->second.replied = true;
    }
    template<class Sender> bool complete(const Sender& sender) {
        std::lock_guard lock(mutex_);
        for (const auto& [id, request] : requests_) {
            const auto receipt = sender.receipt(request.receipt);
            if (!request.replied || !receipt.ok() || receipt.value().state != nf::ReceiptState::written)
                return false;
        }
        return true;
    }
    template<class Sender> void print_receipts(const Sender& sender) {
        std::lock_guard lock(mutex_);
        for (const auto& [id, request] : requests_) {
            const auto receipt = sender.receipt(request.receipt);
            if (receipt.ok())
                print("RECEIPT ClOrdID=", id, " state=", receipt_name(receipt.value().state),
                    " sequence=", receipt.value().msg_seq_num);
            else error("RECEIPT ClOrdID=", id, " state=unknown reason=", receipt.status().message());
        }
    }
    template<class Sender> bool drain(const Sender& sender, unsigned timeout_ms, const std::atomic<bool>& done) {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!complete(sender)) {
            if (done.load() || Clock::now() >= deadline) {
                error("ERROR outstanding requests have an unknown business result; no resubmit");
                print_receipts(sender);
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        print_receipts(sender);
        return true;
    }
};
} // namespace example
