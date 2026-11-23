#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

#include <nimblefix/codec.hpp>
#include <nimblefix/status.hpp>

namespace nimblefix {

// Lock-free, zero-allocation (post-construction) SPSC outbound queue for Layer 0 DirectInitiator.
// Producer (strategy thread) calls try_push(order) to encode application body fields into a
// pre-allocated ring slot; consumer (I/O thread) calls initiator.drain(queue) to stamp FIX
// MsgSeqNum/SendingTime, commit to mmap Journal, and coalesce frames into a single writev/sendmsg.
class DirectSendQueue {
public:
    struct SlotView {
        std::string_view msg_type{};
        std::string_view body{};
    };

    explicit DirectSendQueue(std::size_t requested_capacity = 256,
                             std::size_t max_body_bytes = 512)
        : capacity_(next_power_of_two(requested_capacity < 2 ? 2 : requested_capacity)),
          mask_(capacity_ - 1),
          slot_stride_(align_up_64(sizeof(SlotHeader) + (max_body_bytes < 64 ? 64 : max_body_bytes))),
          max_body_bytes_(max_body_bytes < 64 ? 64 : max_body_bytes),
          storage_(capacity_ * slot_stride_) {}

    DirectSendQueue(const DirectSendQueue&) = delete;
    DirectSendQueue& operator=(const DirectSendQueue&) = delete;

    // Single-producer thread: validates and encodes application body fields into the next free slot.
    // Performs 0 heap allocations and 0 locks. Returns ErrorCode::queue_full when ring is full.
    template <class Message>
        requires requires(const Message& m) { m.view(); }
    [[nodiscard]] Status try_push(const Message& message) noexcept {
        static_assert(!requires { requires std::remove_cvref_t<Message>::is_group_entry; },
            "repeating group entries cannot be pushed as standalone FIX messages");
        return try_push_view(message.view());
    }

    [[nodiscard]] Status try_push_view(detail::MessageView view) noexcept {
        if (!view.status.ok()) return view.status;
        if (view.msg_type.empty() || view.msg_type.size() > max_msg_type_bytes)
            return Status{ErrorCode::invalid_value, 35};

        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail - cached_head_ >= capacity_) {
            cached_head_ = head_.load(std::memory_order_acquire);
            if (tail - cached_head_ >= capacity_) return Status{ErrorCode::queue_full};
        }

        char* raw = storage_.data() + (tail & mask_) * slot_stride_;
        auto* hdr = reinterpret_cast<SlotHeader*>(raw);
        char* body_dst = raw + sizeof(SlotHeader);

        const auto encoded = encode_fix_body(view, std::span<char>{body_dst, max_body_bytes_});
        if (!encoded.ok()) return encoded.status();

        hdr->msg_type_size = static_cast<std::uint8_t>(view.msg_type.size());
        std::memcpy(hdr->msg_type.data(), view.msg_type.data(), view.msg_type.size());
        hdr->body_size = static_cast<std::uint32_t>(encoded.value());

        tail_.store(tail + 1, std::memory_order_release);
        return Status::success();
    }

    // Single-consumer (I/O thread) inspection & pop primitives used by DirectInitiator::drain().
    [[nodiscard]] bool try_peek(SlotView& out) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (head == cached_tail_) {
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (head == cached_tail_) return false;
        }
        const char* raw = storage_.data() + (head & mask_) * slot_stride_;
        const auto* hdr = reinterpret_cast<const SlotHeader*>(raw);
        out.msg_type = {hdr->msg_type.data(), hdr->msg_type_size};
        out.body = {raw + sizeof(SlotHeader), hdr->body_size};
        return true;
    }

    void consume_one() noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        head_.store(head + 1, std::memory_order_release);
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t size() const noexcept {
        const std::size_t t = tail_.load(std::memory_order_acquire);
        const std::size_t h = head_.load(std::memory_order_acquire);
        return t - h;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t max_body_bytes() const noexcept { return max_body_bytes_; }

private:
    static constexpr std::size_t max_msg_type_bytes = 32;

    struct SlotHeader {
        std::uint32_t body_size{};
        std::uint8_t msg_type_size{};
        std::array<char, max_msg_type_bytes> msg_type{};
    };

    static constexpr std::size_t next_power_of_two(std::size_t n) noexcept {
        std::size_t p = 1;
        while (p < n && p < (std::size_t{1} << 30U)) p <<= 1U;
        return p;
    }

    static constexpr std::size_t align_up_64(std::size_t n) noexcept {
        return (n + 63U) & ~std::size_t{63U};
    }

    std::size_t capacity_{};
    std::size_t mask_{};
    std::size_t slot_stride_{};
    std::size_t max_body_bytes_{};
    std::vector<char> storage_{};

    alignas(64) std::atomic<std::size_t> tail_{0};
    std::size_t cached_head_{0};

    alignas(64) std::atomic<std::size_t> head_{0};
    std::size_t cached_tail_{0};
};

} // namespace nimblefix
