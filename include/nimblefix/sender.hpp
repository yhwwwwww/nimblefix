#pragma once

#include <cstddef>
#include <concepts>
#include <cstdint>
#include <memory>
#include <string_view>
#include <type_traits>
#include <utility>

#include <nimblefix/execution.hpp>
#include <nimblefix/message.hpp>
#include <nimblefix/session.hpp>

namespace nimblefix {

// Capacity is reserved when the lane is created. One producer thread owns a
// lane after its first accepted send; the session owner drains it directly.
struct ProducerLaneConfig {
    std::size_t capacity_messages{64};
    std::size_t capacity_bytes{256U * 1024U};
    std::size_t receipt_capacity{1024};
};

namespace detail {
class SenderSharedState;
class ProducerLaneSharedState;

[[nodiscard]] Result<std::shared_ptr<ProducerLaneSharedState>> create_producer_lane(
    const std::shared_ptr<SenderSharedState>& state, ProducerLaneConfig config,
    std::string_view schema_fingerprint) noexcept;
[[nodiscard]] Result<SendReceipt> enqueue_producer_lane_message(
    const std::shared_ptr<ProducerLaneSharedState>& state, MessageView message,
    std::string_view schema_fingerprint) noexcept;
[[nodiscard]] Result<SendReceipt> query_producer_lane_receipt(
    const std::shared_ptr<ProducerLaneSharedState>& state, std::uint64_t id) noexcept;
[[nodiscard]] bool producer_lane_can_send(
    const std::shared_ptr<ProducerLaneSharedState>& state,
    std::string_view schema_fingerprint) noexcept;
[[nodiscard]] bool producer_lane_stopped(
    const std::shared_ptr<ProducerLaneSharedState>& state) noexcept;

[[nodiscard]] Result<SendReceipt> enqueue_sender_message(
    const std::shared_ptr<SenderSharedState>& state, MessageView message,
    std::string_view schema_fingerprint) noexcept;
[[nodiscard]] Result<SendReceipt> enqueue_dependent_sender_message(
    const std::shared_ptr<SenderSharedState>& state, MessageView message,
    DeliveryInfo delivery, std::size_t ordinal) noexcept;
[[nodiscard]] Result<SendReceipt> query_sender_receipt(
    const std::shared_ptr<SenderSharedState>& state, std::uint64_t id) noexcept;
[[nodiscard]] bool sender_can_send(const std::shared_ptr<SenderSharedState>& state,
    std::string_view schema_fingerprint) noexcept;
[[nodiscard]] bool sender_stopped(const std::shared_ptr<SenderSharedState>& state) noexcept;
[[nodiscard]] Status enqueue_sender_logout(const std::shared_ptr<SenderSharedState>& state,
    std::string_view reason) noexcept;
} // namespace detail

// Sender owns a shared reference to a bounded command channel, not to the
// engine runtime. It remains safe to use after the engine has stopped.
template <class Schema = FlatSchema>
class Sender {
public:
    Sender() noexcept = default;

    template <class Message>
        requires requires(const Message& message) {
            { message.view() } -> std::same_as<detail::MessageView>;
        }
    [[nodiscard]] Result<SendReceipt> send(const Message& message) const noexcept {
        static_assert(!requires { requires std::remove_cvref_t<Message>::is_group_entry; },
            "repeating group entries cannot be sent as standalone FIX messages");
        if constexpr (!std::is_same_v<Schema, FlatSchema>) {
            if constexpr (requires { typename Message::schema_type; }) {
                static_assert(std::is_same_v<Schema, typename Message::schema_type>,
                    "message schema does not match Sender schema");
            } else {
                static_assert(std::is_same_v<Message, void>,
                    "typed Sender requires a generated message from its Schema");
            }
        }
        if (!state_) return Status{ErrorCode::engine_stopped};
        return detail::enqueue_sender_message(state_, message.view(), schema_fingerprint_);
    }

    template <class Message>
        requires requires(const Message& message) {
            { message.view() } -> std::same_as<detail::MessageView>;
        }
    [[nodiscard]] Result<SendReceipt> send(Message&& message) const noexcept {
        return send(static_cast<const std::remove_reference_t<Message>&>(message));
    }

    [[nodiscard]] Result<SendReceipt> receipt(std::uint64_t id) const noexcept {
        if (!state_) return Status{ErrorCode::engine_stopped};
        return detail::query_sender_receipt(state_, id);
    }

    [[nodiscard]] bool can_send() const noexcept {
        return state_ && detail::sender_can_send(state_, schema_fingerprint_);
    }

    [[nodiscard]] bool stopped() const noexcept {
        return !state_ || detail::sender_stopped(state_);
    }

    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(state_); }

private:
    friend class detail::EngineRuntime;
    friend class SessionHandle;
    explicit Sender(std::shared_ptr<detail::SenderSharedState> state,
        std::string_view schema_fingerprint) noexcept
        : state_(std::move(state)), schema_fingerprint_(schema_fingerprint) {}

    std::shared_ptr<detail::SenderSharedState> state_{};
    std::string_view schema_fingerprint_{};
};

// An explicit single-producer send path. The lane owns a preallocated SPSC
// command ring and never takes the ordinary Sender MPSC queue's lock.
template <class Schema = FlatSchema>
class ProducerLane {
public:
    ProducerLane() noexcept = default;

    template <class Message>
        requires requires(const Message& message) {
            { message.view() } -> std::same_as<detail::MessageView>;
        }
    [[nodiscard]] Result<SendReceipt> send(const Message& message) const noexcept {
        static_assert(!requires { requires std::remove_cvref_t<Message>::is_group_entry; },
            "repeating group entries cannot be sent as standalone FIX messages");
        if constexpr (!std::is_same_v<Schema, FlatSchema>) {
            if constexpr (requires { typename Message::schema_type; }) {
                static_assert(std::is_same_v<Schema, typename Message::schema_type>,
                    "message schema does not match ProducerLane schema");
            } else {
                static_assert(std::is_same_v<Message, void>,
                    "typed ProducerLane requires a generated message from its Schema");
            }
        }
        if (!state_) return Status{ErrorCode::engine_stopped};
        return detail::enqueue_producer_lane_message(state_, message.view(), schema_fingerprint_);
    }

    template <class Message>
        requires requires(const Message& message) {
            { message.view() } -> std::same_as<detail::MessageView>;
        }
    [[nodiscard]] Result<SendReceipt> send(Message&& message) const noexcept {
        return send(static_cast<const std::remove_reference_t<Message>&>(message));
    }

    [[nodiscard]] Result<SendReceipt> receipt(std::uint64_t id) const noexcept {
        if (!state_) return Status{ErrorCode::engine_stopped};
        return detail::query_producer_lane_receipt(state_, id);
    }

    [[nodiscard]] bool can_send() const noexcept {
        return state_ && detail::producer_lane_can_send(state_, schema_fingerprint_);
    }

    [[nodiscard]] bool stopped() const noexcept {
        return !state_ || detail::producer_lane_stopped(state_);
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(state_);
    }

private:
    friend class SessionHandle;
    explicit ProducerLane(std::shared_ptr<detail::ProducerLaneSharedState> state,
        std::string_view schema_fingerprint) noexcept
        : state_(std::move(state)), schema_fingerprint_(schema_fingerprint) {}

    std::shared_ptr<detail::ProducerLaneSharedState> state_{};
    std::string_view schema_fingerprint_{};
};

} // namespace nimblefix
