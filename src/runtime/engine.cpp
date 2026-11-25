#include <nimblefix/engine.hpp>
#include <nimblefix/sender.hpp>

#include "io_backend.hpp"
#include "../transport/tcp.hpp"
#include "../transport/tls.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <sys/eventfd.h>
#include <unistd.h>

namespace nimblefix {
namespace detail {

namespace {

enum class EngineLife : std::uint8_t { configured, running, stopping, stopped };

constexpr std::size_t align_up(std::size_t value, std::size_t alignment) noexcept {
    return (value + alignment - 1) / alignment * alignment;
}

std::uint64_t monotonic_ms() noexcept;

[[nodiscard]] constexpr std::uint64_t pending_deadline(std::uint64_t accepted_ms,
    std::uint32_t age_limit_ms) noexcept {
    if (age_limit_ms == 0) return 0;
    return accepted_ms > std::numeric_limits<std::uint64_t>::max() - age_limit_ms
        ? std::numeric_limits<std::uint64_t>::max()
        : accepted_ms + age_limit_ms;
}

[[nodiscard]] constexpr bool pending_expired(std::uint64_t accepted_ms,
    std::uint64_t now_ms, std::uint32_t age_limit_ms) noexcept {
    return age_limit_ms != 0 && now_ms >= accepted_ms &&
        now_ms - accepted_ms >= age_limit_ms;
}

constexpr std::size_t maximum_msg_type_size = 32;

class WakeControl {
public:
    explicit WakeControl(int fd) noexcept : fd_(fd) {}

    void wake() noexcept {
        constexpr std::uint64_t closed = std::uint64_t{1} << 63U;
        gate_.fetch_add(1, std::memory_order_acq_rel);
        if ((gate_.load(std::memory_order_acquire) & closed) != 0) {
            gate_.fetch_sub(1, std::memory_order_release);
            return;
        }
        const auto fd = fd_.load(std::memory_order_acquire);
        if (fd < 0) {
            gate_.fetch_sub(1, std::memory_order_release);
            return;
        }
        const std::uint64_t one = 1;
        ssize_t written{};
        do { written = ::write(fd, &one, sizeof(one)); } while (written < 0 && errno == EINTR);
        (void)written;
        gate_.fetch_sub(1, std::memory_order_release);
    }

    void close() noexcept {
        constexpr std::uint64_t closed = std::uint64_t{1} << 63U;
        constexpr std::uint64_t active_mask = ~closed;
        gate_.fetch_or(closed, std::memory_order_acq_rel);
        while ((gate_.load(std::memory_order_acquire) & active_mask) != 0)
            std::this_thread::yield();
        const auto fd = fd_.exchange(-1, std::memory_order_acq_rel);
        if (fd >= 0) ::close(fd);
    }

private:
    std::atomic<std::uint64_t> gate_{0};
    std::atomic<int> fd_{-1};
};

struct OwnedMessageHeader {
    std::size_t field_count{};
    std::size_t value_byte_count{};
    std::size_t required_count{};
    std::size_t msg_type_size{};
    std::size_t slots_offset{};
    std::size_t values_offset{};
    std::size_t plan_offset{};
    std::size_t map_offset{};
    std::size_t presence_offset{};
    std::size_t required_offset{};
    std::size_t msg_type_offset{};
    std::size_t groups_offset{};
    std::size_t group_count{};
    std::size_t total_size{};
    Status status{};
    EnumValueValidator enum_value_allowed{};
    bool has_slot_map{};
    bool has_presence{};
    bool prebuilt_body{};
};

struct OwnedMessageLayout {
    OwnedMessageHeader header{};
};

struct SenderReceiptSlot {
    SendReceipt value{};
    std::uint64_t driver_receipt_id{};
    bool occupied{};
    bool tracked{};
};

struct SenderCommandSlot {
    std::uint64_t id{};
    std::uint64_t reservation_begin{};
    std::size_t reservation_size{};
    std::size_t payload_offset{};
    std::size_t payload_size{};
    RecordId dependency{};
    std::size_t dependency_ordinal{};
    std::uint64_t generation{};
    bool recovery{};
    std::uint64_t accepted_ms{};
};

struct ProducerLaneCommandSlot {
    std::uint64_t id{};
    std::uint64_t reservation_begin{};
    std::size_t reservation_size{};
    std::size_t payload_offset{};
    std::size_t payload_size{};
    std::uint64_t accepted_ms{};
};

struct ProducerLaneReceiptSlot {
    std::atomic<std::uint64_t> version{0};
    std::atomic<std::uint64_t> id{0};
    std::atomic<std::uint64_t> driver_receipt_id{0};
    std::atomic<std::uint64_t> msg_seq_num{0};
    std::atomic<int> state{static_cast<int>(ReceiptState::expired)};
    std::atomic<int> code{static_cast<int>(ErrorCode::receipt_expired)};
    std::atomic<int> tag{0};
    std::atomic<std::size_t> offset{0};
};

struct SenderCommandView {
    std::uint64_t id{};
    RecordId dependency{};
    std::size_t dependency_ordinal{};
    bool recovery{};
    MessageView message{};
};

[[nodiscard]] bool session_admin_type(std::string_view type) noexcept {
    return type == "A" || type == "0" || type == "1" || type == "2" ||
        type == "3" || type == "4" || type == "5";
}

[[nodiscard]] bool add_size(std::size_t left, std::size_t right,
    std::size_t& result) noexcept {
    if (right > std::numeric_limits<std::size_t>::max() - left) return false;
    result = left + right;
    return true;
}

[[nodiscard]] bool add_array_size(std::size_t& cursor, std::size_t count,
    std::size_t element_size) noexcept {
    if (count != 0 && element_size > std::numeric_limits<std::size_t>::max() / count)
        return false;
    return add_size(cursor, count * element_size, cursor);
}

[[nodiscard]] bool aligned_cursor(std::size_t& cursor, std::size_t alignment) noexcept {
    if (cursor > std::numeric_limits<std::size_t>::max() - (alignment - 1)) return false;
    cursor = align_up(cursor, alignment);
    return true;
}

[[nodiscard]] bool message_node_size(const MessageView& message, std::size_t& size,
    std::size_t depth = 0) noexcept {
    if (depth > 64 || (message.group_count != 0 && message.groups == nullptr)) return false;
    std::size_t cursor = sizeof(OwnedMessageHeader);
    if (!aligned_cursor(cursor, alignof(SlotValue)) ||
        !add_array_size(cursor, message.field_count, sizeof(SlotValue)) ||
        !add_size(cursor, message.value_byte_count, cursor) ||
        !aligned_cursor(cursor, alignof(PlanField)) ||
        !add_array_size(cursor, message.field_count, sizeof(PlanField)) ||
        !aligned_cursor(cursor, alignof(std::uint32_t)) ||
        (message.slot_map != nullptr && !add_array_size(cursor, message.field_count,
            sizeof(std::uint32_t))) ||
        (message.presence_bits != nullptr &&
            !add_array_size(cursor, (message.field_count + 7) / 8, sizeof(std::uint8_t))) ||
        !aligned_cursor(cursor, alignof(std::uint32_t)) ||
        !add_array_size(cursor, message.required_tag_count, sizeof(std::uint32_t)) ||
        !add_size(cursor, message.msg_type.size(), cursor) ||
        !aligned_cursor(cursor, alignof(RepeatingGroupView)) ||
        !add_array_size(cursor, message.group_count, sizeof(RepeatingGroupView)))
        return false;

    for (std::size_t group_index = 0; group_index < message.group_count; ++group_index) {
        const auto& group = message.groups[group_index];
        if (group.entry_count != 0 && group.entries == nullptr) return false;
        if (!aligned_cursor(cursor, alignof(MessageView)) ||
            !add_array_size(cursor, group.entry_count, sizeof(MessageView))) return false;
        for (std::size_t entry_index = 0; entry_index < group.entry_count; ++entry_index) {
            if (!aligned_cursor(cursor, alignof(std::max_align_t))) return false;
            std::size_t child_size{};
            if (!message_node_size(group.entries[entry_index], child_size, depth + 1) ||
                !add_size(cursor, child_size, cursor)) return false;
        }
    }
    if (!aligned_cursor(cursor, alignof(std::max_align_t))) return false;
    size = cursor;
    return true;
}

[[nodiscard]] OwnedMessageHeader make_owned_header(const MessageView& message,
    std::size_t total_size) noexcept {
    OwnedMessageHeader header{};
    header.field_count = message.field_count;
    header.value_byte_count = message.value_byte_count;
    header.required_count = message.required_tag_count;
    header.msg_type_size = message.msg_type.size();
    header.group_count = message.group_count;
    header.status = message.status;
    header.enum_value_allowed = message.enum_value_allowed;
    header.has_slot_map = message.slot_map != nullptr;
    header.has_presence = message.presence_bits != nullptr;
    header.prebuilt_body = message.prebuilt_body;
    header.slots_offset = align_up(sizeof(OwnedMessageHeader), alignof(SlotValue));
    header.values_offset = header.slots_offset + message.field_count * sizeof(SlotValue);
    header.plan_offset = align_up(header.values_offset + message.value_byte_count, alignof(PlanField));
    header.map_offset = align_up(header.plan_offset + message.field_count * sizeof(PlanField),
        alignof(std::uint32_t));
    header.presence_offset = header.map_offset +
        (header.has_slot_map ? message.field_count * sizeof(std::uint32_t) : 0);
    header.required_offset = align_up(header.presence_offset +
        (header.has_presence ? (message.field_count + 7) / 8 : 0), alignof(std::uint32_t));
    header.msg_type_offset = header.required_offset + message.required_tag_count * sizeof(std::uint32_t);
    header.groups_offset = align_up(header.msg_type_offset + message.msg_type.size(),
        alignof(RepeatingGroupView));
    header.total_size = total_size;
    return header;
}

[[nodiscard]] Result<OwnedMessageLayout> measure_owned_message(const MessageView& message,
    std::size_t max_frame_bytes, std::string_view local_comp_id,
    std::string_view remote_comp_id, FixVersion version, bool emit_last_processed) noexcept {
    if (!message.status.ok()) return message.status;
    if (message.msg_type.size() > maximum_msg_type_size || session_admin_type(message.msg_type))
        return Status{ErrorCode::managed_field, 35};
    const auto timestamp = std::string_view{"99991231-23:59:59.999"};
    SessionHeader header{local_comp_id, remote_comp_id,
        std::numeric_limits<std::uint64_t>::max(), timestamp, false, {}, version};
    if (emit_last_processed) header.last_msg_seq_num_processed = std::numeric_limits<std::uint64_t>::max();
    const auto wire_size = measure_fix(message, header);
    if (!wire_size.ok()) return wire_size.status();
    if (wire_size.value() > max_frame_bytes) return Status{ErrorCode::too_large};
    std::size_t payload_size{};
    if (!message_node_size(message, payload_size)) return Status{ErrorCode::too_large};
    OwnedMessageLayout result{};
    result.header = make_owned_header(message, payload_size);
    return result;
}

[[nodiscard]] MessageView view_owned_message(std::span<char> bytes) noexcept {
    OwnedMessageHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    auto* base = bytes.data();
    RepeatingGroupView* groups = header.group_count == 0
        ? nullptr : reinterpret_cast<RepeatingGroupView*>(base + header.groups_offset);
    if (groups != nullptr) {
        // Relocate repeating group entry pointers relative to `bytes.data()` so
        // messages copied from ring arena to scratch remain 100% position-independent.
        std::size_t cursor = header.groups_offset + header.group_count * sizeof(RepeatingGroupView);
        for (std::size_t g = 0; g < header.group_count; ++g) {
            cursor = align_up(cursor, alignof(MessageView));
            auto* entries = reinterpret_cast<MessageView*>(base + cursor);
            groups[g].entries = entries;
            cursor += groups[g].entry_count * sizeof(MessageView);
            for (std::size_t e = 0; e < groups[g].entry_count; ++e) {
                cursor = align_up(cursor, alignof(std::max_align_t));
                OwnedMessageHeader child_header{};
                std::memcpy(&child_header, base + cursor, sizeof(child_header));
                auto child = std::span<char>{base + cursor, child_header.total_size};
                entries[e] = view_owned_message(child);
                cursor += child_header.total_size;
            }
        }
    }
    return MessageView{
        header.field_count == 0 ? nullptr : reinterpret_cast<const SlotValue*>(base + header.slots_offset),
        header.value_byte_count == 0 ? nullptr : base + header.values_offset,
        header.value_byte_count,
        header.field_count == 0 ? nullptr : reinterpret_cast<const PlanField*>(base + header.plan_offset),
        header.has_slot_map && header.field_count != 0
            ? reinterpret_cast<const std::uint32_t*>(base + header.map_offset) : nullptr,
        header.field_count,
        header.has_presence && header.field_count != 0
            ? reinterpret_cast<const std::uint8_t*>(base + header.presence_offset) : nullptr,
        {base + header.msg_type_offset, header.msg_type_size}, header.status,
        header.required_count == 0
            ? nullptr : reinterpret_cast<const std::uint32_t*>(base + header.required_offset),
        header.required_count, header.enum_value_allowed,
        groups,
        header.group_count,
        header.prebuilt_body,
        header.prebuilt_body,
        false,
        header.prebuilt_body};
}

void copy_owned_message(std::span<char> destination, const MessageView& source,
    std::size_t depth = 0) noexcept {
    std::size_t total_size{};
    (void)message_node_size(source, total_size, depth);
    const auto header = make_owned_header(source, total_size);
    std::memcpy(destination.data(), &header, sizeof(header));
    auto* base = destination.data();
    if (header.field_count != 0) {
        std::memcpy(base + header.slots_offset, source.slots,
            header.field_count * sizeof(SlotValue));
        std::memcpy(base + header.plan_offset, source.plan,
            header.field_count * sizeof(PlanField));
    }
    if (header.value_byte_count != 0)
        std::memcpy(base + header.values_offset, source.value_bytes, header.value_byte_count);
    if (header.has_slot_map && header.field_count != 0)
        std::memcpy(base + header.map_offset, source.slot_map,
            header.field_count * sizeof(std::uint32_t));
    if (header.has_presence && header.field_count != 0)
        std::memcpy(base + header.presence_offset, source.presence_bits,
            (header.field_count + 7) / 8);
    if (header.required_count != 0)
        std::memcpy(base + header.required_offset, source.required_tags,
            header.required_count * sizeof(std::uint32_t));
    if (header.msg_type_size != 0)
        std::memcpy(base + header.msg_type_offset, source.msg_type.data(), header.msg_type_size);

    auto* groups = header.group_count == 0 ? nullptr
        : reinterpret_cast<RepeatingGroupView*>(base + header.groups_offset);
    std::size_t cursor = header.groups_offset + header.group_count * sizeof(RepeatingGroupView);
    for (std::size_t group_index = 0; group_index < header.group_count; ++group_index) {
        const auto& source_group = source.groups[group_index];
        cursor = align_up(cursor, alignof(MessageView));
        auto* entries = reinterpret_cast<MessageView*>(base + cursor);
        std::construct_at(groups + group_index, RepeatingGroupView{source_group.count_tag,
            source_group.delimiter_tag, entries, source_group.entry_count});
        cursor += source_group.entry_count * sizeof(MessageView);
        for (std::size_t entry_index = 0; entry_index < source_group.entry_count; ++entry_index) {
            cursor = align_up(cursor, alignof(std::max_align_t));
            std::size_t child_size{};
            (void)message_node_size(source_group.entries[entry_index], child_size, depth + 1);
            auto child = std::span<char>{base + cursor, child_size};
            copy_owned_message(child, source_group.entries[entry_index], depth + 1);
            std::construct_at(entries + entry_index, view_owned_message(child));
            cursor += child_size;
        }
    }
}

} // namespace

class ProducerLaneSharedState {
public:
    ProducerLaneSharedState(std::string requested_local, std::string requested_remote,
        std::string requested_schema_fingerprint, ProducerLaneConfig requested_config,
        std::size_t requested_max_frame,
        std::shared_ptr<std::atomic<EngineLife>> requested_life,
        std::shared_ptr<WakeControl> requested_wake, FixVersion requested_version,
        bool requested_last_processed, std::uint32_t requested_max_pending_age_ms)
        : local_comp_id(std::move(requested_local)),
          remote_comp_id(std::move(requested_remote)),
          schema_fingerprint(std::move(requested_schema_fingerprint)),
          slots(requested_config.capacity_messages),
          arena(align_up(requested_config.capacity_bytes, alignof(std::max_align_t))),
          receipts(requested_config.receipt_capacity), pending_indices(requested_config.receipt_capacity), max_frame_bytes(requested_max_frame),
          life(std::move(requested_life)), wake_control(std::move(requested_wake)),
          version(requested_version), emit_last_processed(requested_last_processed),
          max_pending_age_ms(requested_max_pending_age_ms) {}

    std::string local_comp_id;
    std::string remote_comp_id;
    std::string schema_fingerprint;
    std::vector<ProducerLaneCommandSlot> slots;
    std::vector<char> arena;
    std::vector<ProducerLaneReceiptSlot> receipts;
    std::vector<std::size_t> pending_indices;
    std::size_t max_frame_bytes{};
    std::shared_ptr<std::atomic<EngineLife>> life;
    std::shared_ptr<WakeControl> wake_control;
    FixVersion version{FixVersion::fix44};
    bool emit_last_processed{};
    std::uint32_t max_pending_age_ms{};
    std::atomic<bool> active{false};
    std::atomic<bool> stopped{false};
    std::atomic<bool> blocked{false};
    std::atomic<std::uint64_t> producer_token{0};
    std::atomic<std::uint64_t> admission_gate{0};
    std::atomic<std::uint64_t> head{0};
    std::atomic<std::uint64_t> tail{0};
    std::atomic<std::uint64_t> byte_read{0};
    std::uint64_t byte_write{};
    std::atomic<std::uint64_t> next_id{1};
    std::size_t pending_driver_receipts{};

    [[nodiscard]] bool enter_send() noexcept {
        constexpr auto stop_bit = std::uint64_t{1} << 63U;
        auto value = admission_gate.load(std::memory_order_acquire);
        while ((value & stop_bit) == 0) {
            if (admission_gate.compare_exchange_weak(value, value + 1,
                    std::memory_order_acq_rel, std::memory_order_acquire)) return true;
        }
        return false;
    }

    void leave_send() noexcept {
        admission_gate.fetch_sub(1, std::memory_order_release);
    }

    [[nodiscard]] bool life_stopped() const noexcept {
        const auto current = life ? life->load(std::memory_order_acquire) : EngineLife::stopped;
        return stopped.load(std::memory_order_acquire) || current == EngineLife::stopping ||
            current == EngineLife::stopped;
    }

    [[nodiscard]] bool producer_thread_ok() const noexcept {
        const auto current = current_producer_token();
        const auto owner = producer_token.load(std::memory_order_acquire);
        return owner == 0 || owner == current;
    }

    [[nodiscard]] Result<SendReceipt> enqueue(MessageView message,
        std::string_view expected_schema_fingerprint) noexcept {
        if (life_stopped()) return Status{ErrorCode::engine_stopped};
        if (expected_schema_fingerprint != schema_fingerprint)
            return Status{ErrorCode::schema_mismatch};
        if (!producer_thread_ok()) return Status{ErrorCode::wrong_thread};
        if (!active.load(std::memory_order_acquire)) return Status{ErrorCode::not_ready};

        const auto measured = measure_owned_message(message, max_frame_bytes,
            local_comp_id, remote_comp_id, version, emit_last_processed);
        if (!measured.ok()) return measured.status();
        const auto payload_size = measured.value().header.total_size;
        if (slots.empty() || arena.empty() || receipts.empty() || payload_size > arena.size())
            return Status{ErrorCode::queue_full};

        if (!enter_send()) return Status{ErrorCode::engine_stopped};
        struct SendGuard {
            ProducerLaneSharedState& state;
            ~SendGuard() { state.leave_send(); }
        } guard{*this};
        if (life_stopped()) return Status{ErrorCode::engine_stopped};
        if (!producer_thread_ok()) return Status{ErrorCode::wrong_thread};
        if (!active.load(std::memory_order_acquire)) return Status{ErrorCode::not_ready};

        std::uint64_t expected_owner = 0;
        const auto current_owner = current_producer_token();
        if (!producer_token.compare_exchange_strong(expected_owner, current_owner,
                std::memory_order_acq_rel, std::memory_order_acquire) &&
            expected_owner != current_owner)
            return Status{ErrorCode::wrong_thread};

        const auto producer_head = head.load(std::memory_order_acquire);
        const auto producer_tail = tail.load(std::memory_order_relaxed);
        if (producer_tail < producer_head || producer_tail - producer_head >= slots.size() ||
            producer_tail == std::numeric_limits<std::uint64_t>::max())
            return Status{ErrorCode::queue_full};

        if (byte_write > std::numeric_limits<std::uint64_t>::max() / 2 &&
            producer_head == producer_tail) {
            byte_write = 0;
            byte_read.store(0, std::memory_order_relaxed);
        }
        const auto capacity = arena.size();
        std::size_t offset = static_cast<std::size_t>(byte_write % capacity);
        std::size_t padding = 0;
        if (payload_size > capacity - offset) {
            padding = capacity - offset;
            offset = 0;
        }
        const auto consumed = byte_read.load(std::memory_order_acquire);
        if (byte_write < consumed || byte_write - consumed > capacity)
            return Status{ErrorCode::queue_full};
        const auto used = static_cast<std::size_t>(byte_write - consumed);
        if (padding > capacity - used || payload_size > capacity - used - padding)
            return Status{ErrorCode::queue_full};
        const auto reserved = padding + payload_size;
        if (reserved > std::numeric_limits<std::uint64_t>::max() - byte_write)
            return Status{ErrorCode::queue_full};

        const auto id = next_id.load(std::memory_order_relaxed);
        if (id == 0 || id == std::numeric_limits<std::uint64_t>::max())
            return Status{ErrorCode::receipt_expired};
        auto& receipt = receipts[static_cast<std::size_t>(id % receipts.size())];
        const auto old_receipt = read_receipt(receipt);
        if (old_receipt.id != 0 && old_receipt.value.state != ReceiptState::written &&
            old_receipt.value.state != ReceiptState::failed &&
            old_receipt.value.state != ReceiptState::expired)
            return Status{ErrorCode::queue_full};

        next_id.store(id + 1, std::memory_order_relaxed);
        (void)write_receipt(receipt,
            SendReceipt{id, 0, ReceiptState::accepted, Status::success()}, 0);
        const auto reservation_begin = byte_write;
        byte_write += reserved;
        const auto payload_position = static_cast<std::size_t>((reservation_begin + padding) % capacity);
        copy_owned_message(std::span<char>{arena.data() + payload_position, payload_size}, message);
        const auto accepted_ms = monotonic_ms();
        slots[static_cast<std::size_t>(producer_tail % slots.size())] = ProducerLaneCommandSlot{
            id, reservation_begin, reserved, payload_position, payload_size, accepted_ms};
        tail.store(producer_tail + 1, std::memory_order_release);
        if (wake_control) wake_control->wake();
        return SendReceipt{id, 0, ReceiptState::accepted, Status::success()};
    }

    [[nodiscard]] bool peek(ProducerLaneCommandSlot& command,
        std::span<char> scratch, MessageView& message) noexcept {
        if (blocked.load(std::memory_order_acquire) ||
            !active.load(std::memory_order_acquire) || stopped.load(std::memory_order_acquire))
            return false;
        const auto consumer_head = head.load(std::memory_order_relaxed);
        const auto consumer_tail = tail.load(std::memory_order_acquire);
        if (consumer_head == consumer_tail || consumer_head > consumer_tail || slots.empty())
            return false;
        command = slots[static_cast<std::size_t>(consumer_head % slots.size())];
        if (command.payload_size > scratch.size()) return false;
        std::memcpy(scratch.data(), arena.data() + command.payload_offset, command.payload_size);
        message = view_owned_message(scratch.first(command.payload_size));
        return true;
    }

    [[nodiscard]] std::optional<ProducerLaneCommandSlot> expire_head(
        std::uint64_t now_ms) noexcept {
        if (max_pending_age_ms == 0) return {};
        const auto consumer_head = head.load(std::memory_order_relaxed);
        const auto consumer_tail = tail.load(std::memory_order_acquire);
        if (consumer_head == consumer_tail || consumer_head > consumer_tail || slots.empty())
            return {};
        const auto command = slots[static_cast<std::size_t>(consumer_head % slots.size())];
        if (!pending_expired(command.accepted_ms, now_ms, max_pending_age_ms)) return {};
        if (receipts.empty()) return {};
        auto& receipt = receipts[static_cast<std::size_t>(command.id % receipts.size())];
        (void)write_receipt(receipt, SendReceipt{command.id, 0, ReceiptState::expired,
            Status{ErrorCode::backlog_timeout}}, 0, command.id, ReceiptState::accepted);
        byte_read.store(command.reservation_begin + command.reservation_size,
            std::memory_order_release);
        head.store(consumer_head + 1, std::memory_order_release);
        return command;
    }

    [[nodiscard]] std::uint64_t next_expiry_deadline() const noexcept {
        if (max_pending_age_ms == 0) return 0;
        const auto consumer_head = head.load(std::memory_order_relaxed);
        const auto consumer_tail = tail.load(std::memory_order_acquire);
        if (consumer_head == consumer_tail || consumer_head > consumer_tail || slots.empty())
            return 0;
        return pending_deadline(
            slots[static_cast<std::size_t>(consumer_head % slots.size())].accepted_ms,
            max_pending_age_ms);
    }

    void finish(ProducerLaneCommandSlot command, Result<SendReceipt> result) noexcept {
        if (receipts.empty()) return;
        auto& receipt = receipts[static_cast<std::size_t>(command.id % receipts.size())];
        if (receipt.id.load(std::memory_order_acquire) == command.id) {
            if (result.ok()) {
                auto lane_receipt = result.value();
                lane_receipt.id = command.id;
                const bool updated = write_receipt(receipt, lane_receipt,
                    result.value().id, command.id);
                if (!updated) {
                    const auto consumer_head = head.load(std::memory_order_relaxed);
                    byte_read.store(command.reservation_begin + command.reservation_size,
                        std::memory_order_release);
                    head.store(consumer_head + 1, std::memory_order_release);
                    return;
                }
                if (lane_receipt.state != ReceiptState::written &&
                    lane_receipt.state != ReceiptState::failed &&
                    lane_receipt.state != ReceiptState::expired)
                    pending_indices[pending_driver_receipts++] = static_cast<std::size_t>(command.id % receipts.size());
            } else {
                const auto state = result.status().code == ErrorCode::receipt_expired ||
                    result.status().code == ErrorCode::backlog_timeout
                    ? ReceiptState::expired : ReceiptState::failed;
                (void)write_receipt(receipt,
                    SendReceipt{command.id, 0, state, result.status()}, 0, command.id);
            }
        }
        const auto consumer_head = head.load(std::memory_order_relaxed);
        byte_read.store(command.reservation_begin + command.reservation_size,
            std::memory_order_release);
        head.store(consumer_head + 1, std::memory_order_release);
    }

    void sync_receipts(const SessionDriver& driver) noexcept {
        std::size_t index = 0;
        while (index < pending_driver_receipts) {
            auto& receipt = receipts[pending_indices[index]];
            const auto snapshot = read_receipt(receipt);
            bool terminal = snapshot.id == 0 || snapshot.driver_receipt_id == 0 ||
                snapshot.value.state == ReceiptState::failed ||
                snapshot.value.state == ReceiptState::expired || snapshot.value.state == ReceiptState::written;
            if (!terminal) {
                const auto current = driver.receipt(snapshot.driver_receipt_id);
                if (current.ok()) {
                    auto value = current.value();
                    value.id = snapshot.id;
                    (void)write_receipt(receipt, value, snapshot.driver_receipt_id, snapshot.id);
                    terminal = value.state == ReceiptState::written || value.state == ReceiptState::failed ||
                        value.state == ReceiptState::expired;
                } else {
                    const auto state = current.status().code == ErrorCode::receipt_expired
                        ? ReceiptState::expired : ReceiptState::failed;
                    (void)write_receipt(receipt, SendReceipt{snapshot.id, snapshot.value.msg_seq_num,
                        state, current.status()}, 0, snapshot.id);
                    terminal = true;
                }
            }
            if (terminal) pending_indices[index] = pending_indices[--pending_driver_receipts];
            else ++index;
        }
    }

    [[nodiscard]] Result<SendReceipt> query(std::uint64_t id) const noexcept {
        if (id == 0 || receipts.empty()) return Status{ErrorCode::receipt_expired};
        const auto snapshot = read_receipt(receipts[static_cast<std::size_t>(id % receipts.size())]);
        if (snapshot.id != id) return Status{ErrorCode::receipt_expired};
        return snapshot.value;
    }

    void set_active(bool value) noexcept {
        active.store(value, std::memory_order_release);
        if (value) blocked.store(false, std::memory_order_release);
    }

    void block_commands() noexcept { blocked.store(true, std::memory_order_release); }
    void unblock_commands() noexcept { blocked.store(false, std::memory_order_release); }

    [[nodiscard]] bool has_commands() const noexcept {
        return tail.load(std::memory_order_acquire) != head.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool has_ready_commands() const noexcept {
        return !blocked.load(std::memory_order_acquire) && active.load(std::memory_order_acquire) &&
            !stopped.load(std::memory_order_acquire) && has_commands();
    }

    [[nodiscard]] bool can_enqueue() const noexcept {
        if (life_stopped() || !producer_thread_ok() || !active.load(std::memory_order_acquire) ||
            blocked.load(std::memory_order_acquire) || slots.empty() || arena.empty() || receipts.empty())
            return false;
        const auto producer_head = head.load(std::memory_order_acquire);
        const auto producer_tail = tail.load(std::memory_order_acquire);
        if (producer_tail < producer_head || producer_tail - producer_head >= slots.size()) return false;
        const auto id = next_id.load(std::memory_order_relaxed);
        if (id == 0 || id == std::numeric_limits<std::uint64_t>::max()) return false;
        const auto& receipt = receipts[static_cast<std::size_t>(id % receipts.size())];
        const auto old_receipt = read_receipt(receipt);
        return old_receipt.id == 0 || old_receipt.value.state == ReceiptState::written ||
            old_receipt.value.state == ReceiptState::failed ||
            old_receipt.value.state == ReceiptState::expired;
    }

    void stop() noexcept {
        constexpr auto stop_bit = std::uint64_t{1} << 63U;
        constexpr auto active_mask = ~stop_bit;
        admission_gate.fetch_or(stop_bit, std::memory_order_acq_rel);
        while ((admission_gate.load(std::memory_order_acquire) & active_mask) != 0)
            std::this_thread::yield();
        stopped.store(true, std::memory_order_release);
        active.store(false, std::memory_order_release);
        blocked.store(true, std::memory_order_release);
        for (auto& receipt : receipts) {
            const auto snapshot = read_receipt(receipt);
            if (snapshot.id != 0 && (snapshot.value.state == ReceiptState::accepted ||
                snapshot.value.state == ReceiptState::appended))
                (void)write_receipt(receipt, SendReceipt{snapshot.id,
                    snapshot.value.msg_seq_num,
                    ReceiptState::failed, Status{ErrorCode::engine_stopped}}, 0,
                    snapshot.id, snapshot.value.state);
        }
        const auto consumer_tail = tail.load(std::memory_order_acquire);
        const auto consumer_head = head.load(std::memory_order_relaxed);
        if (consumer_tail >= consumer_head) {
            if (consumer_tail != consumer_head) {
                const auto& last = slots[static_cast<std::size_t>((consumer_tail - 1) % slots.size())];
                byte_read.store(last.reservation_begin + last.reservation_size,
                    std::memory_order_release);
            }
            head.store(consumer_tail, std::memory_order_release);
        }
    }

private:
    struct ReceiptSnapshot {
        std::uint64_t id{};
        std::uint64_t driver_receipt_id{};
        SendReceipt value{};
    };

    [[nodiscard]] static std::uint64_t current_producer_token() noexcept {
        static std::atomic<std::uint64_t> next_token{1};
        static thread_local const std::uint64_t token =
            next_token.fetch_add(1, std::memory_order_relaxed);
        return token;
    }

    [[nodiscard]] static bool write_receipt(ProducerLaneReceiptSlot& slot,
        SendReceipt value, std::uint64_t driver_receipt_id,
        std::uint64_t expected_id = 0,
        ReceiptState expected_state = static_cast<ReceiptState>(-1)) noexcept {
        auto version = slot.version.load(std::memory_order_acquire);
        for (;;) {
            if ((version & 1U) != 0) {
                std::this_thread::yield();
                version = slot.version.load(std::memory_order_acquire);
                continue;
            }
            if (slot.version.compare_exchange_weak(version, version + 1,
                    std::memory_order_acq_rel, std::memory_order_acquire)) break;
        }
        const auto old_id = slot.id.load(std::memory_order_relaxed);
        const auto old_state = static_cast<ReceiptState>(
            slot.state.load(std::memory_order_relaxed));
        if ((expected_id != 0 && old_id != expected_id) ||
            (expected_state != static_cast<ReceiptState>(-1) &&
                old_state != expected_state)) {
            slot.version.store(version + 2, std::memory_order_release);
            return false;
        }
        slot.id.store(value.id, std::memory_order_relaxed);
        slot.driver_receipt_id.store(driver_receipt_id, std::memory_order_relaxed);
        slot.msg_seq_num.store(value.msg_seq_num, std::memory_order_relaxed);
        slot.code.store(static_cast<int>(value.status.code), std::memory_order_relaxed);
        slot.tag.store(value.status.tag, std::memory_order_relaxed);
        slot.offset.store(value.status.offset, std::memory_order_relaxed);
        slot.state.store(static_cast<int>(value.state), std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        slot.version.store(version + 2, std::memory_order_release);
        return true;
    }

    [[nodiscard]] static ReceiptSnapshot read_receipt(
        const ProducerLaneReceiptSlot& slot) noexcept {
        for (;;) {
            const auto before = slot.version.load(std::memory_order_acquire);
            if ((before & 1U) != 0) continue;
            ReceiptSnapshot result;
            result.id = slot.id.load(std::memory_order_relaxed);
            result.driver_receipt_id = slot.driver_receipt_id.load(std::memory_order_relaxed);
            result.value.id = result.id;
            result.value.msg_seq_num = slot.msg_seq_num.load(std::memory_order_relaxed);
            result.value.state = static_cast<ReceiptState>(slot.state.load(std::memory_order_relaxed));
            result.value.status = Status{
                static_cast<ErrorCode>(slot.code.load(std::memory_order_relaxed)),
                slot.tag.load(std::memory_order_relaxed),
                slot.offset.load(std::memory_order_relaxed)};
            std::atomic_thread_fence(std::memory_order_acquire);
            const auto after = slot.version.load(std::memory_order_relaxed);
            if (before == after) return result;
        }
    }
};

class SenderSharedState {
public:
    SenderSharedState(std::string requested_name, std::string requested_local,
        std::string requested_remote, std::string requested_schema_fingerprint,
        std::size_t message_capacity,
        std::size_t byte_capacity, std::size_t receipt_capacity,
        std::size_t requested_max_frame,
        std::size_t requested_max_producer_lanes,
        std::shared_ptr<std::atomic<EngineLife>> requested_life,
        std::shared_ptr<WakeControl> requested_wake, FixVersion requested_version,
        bool requested_last_processed, std::uint32_t requested_max_pending_age_ms)
        : name(std::move(requested_name)), local_comp_id(std::move(requested_local)),
          remote_comp_id(std::move(requested_remote)),
          schema_fingerprint(std::move(requested_schema_fingerprint)), slots(message_capacity),
          arena(align_up(byte_capacity, alignof(std::max_align_t))),
          receipts(receipt_capacity), pending_indices(receipt_capacity), max_frame_bytes(requested_max_frame),
          max_producer_lanes(requested_max_producer_lanes), max_producer_lane_bytes(byte_capacity),
          life(std::move(requested_life)), wake_control(std::move(requested_wake)),
          version(requested_version), emit_last_processed(requested_last_processed),
          max_pending_age_ms(requested_max_pending_age_ms) {
        producer_lanes.reserve(std::min<std::size_t>(max_producer_lanes, 64));
    }

    std::string name;
    std::string local_comp_id;
    std::string remote_comp_id;
    std::string schema_fingerprint;
    std::vector<SenderCommandSlot> slots;
    std::vector<char> arena;
    std::vector<SenderReceiptSlot> receipts;
    std::vector<std::size_t> pending_indices;
    std::size_t pending_count{};
    std::size_t max_frame_bytes{};
    std::size_t max_producer_lanes{};
    std::size_t max_producer_lane_bytes{};
    std::vector<std::shared_ptr<ProducerLaneSharedState>> producer_lanes;
    std::size_t producer_lane_cursor{};
    bool producer_lanes_frozen{};
    std::shared_ptr<std::atomic<EngineLife>> life;
    std::shared_ptr<WakeControl> wake_control;
    FixVersion version{FixVersion::fix44};
    bool emit_last_processed{};
    std::uint32_t max_pending_age_ms{};
    mutable std::mutex mutex;
    std::atomic<bool> active{false};
    std::atomic<bool> stopped{false};
    std::atomic<std::size_t> pending_command_hint{0};
    std::atomic<std::uint64_t> oldest_deadline_hint{0};
    std::atomic<bool> pending_receipt_hint{false};
    std::atomic<bool> command_blocked_hint{false};
    std::atomic<bool> logout_requested_hint{false};
    std::uint64_t byte_write{};
    std::uint64_t byte_read{};
    std::uint64_t next_id{1};
    std::size_t head{};
    std::size_t count{};
    bool command_blocked{};
    std::array<char, 256> logout_reason{};
    std::size_t logout_reason_size{};
    bool logout_requested{};

    [[nodiscard]] Result<std::shared_ptr<ProducerLaneSharedState>> create_producer_lane(
        ProducerLaneConfig requested, std::string_view expected_schema_fingerprint) noexcept;

    void freeze_producer_lanes() noexcept {
        std::lock_guard lock(mutex);
        producer_lanes_frozen = true;
    }

    void set_active(bool value) noexcept {
        active.store(value, std::memory_order_release);
        std::lock_guard lock(mutex);
        command_blocked = false;
        command_blocked_hint.store(false, std::memory_order_release);
        if (!value) {
            logout_requested = false;
            logout_requested_hint.store(false, std::memory_order_release);
        }
        if (!producer_lanes.empty())
            for (const auto& lane : producer_lanes) if (lane) lane->set_active(value);
    }
    void mark_stopped() noexcept {
        active.store(false, std::memory_order_release);
        stopped.store(true, std::memory_order_release);
        {
            std::lock_guard lock(mutex);
            for (auto& slot : receipts) {
                if (!slot.occupied || (slot.value.state != ReceiptState::accepted &&
                    slot.value.state != ReceiptState::appended)) continue;
                slot.value.state = ReceiptState::failed;
                slot.value.status = Status{ErrorCode::engine_stopped};
            }
            for (auto& slot : receipts) slot.tracked = false;
            pending_count = 0;
            count = 0;
            pending_command_hint.store(0, std::memory_order_release);
            oldest_deadline_hint.store(0, std::memory_order_release);
            byte_read = byte_write;
            command_blocked = false;
            logout_requested = false;
            logout_requested_hint.store(false, std::memory_order_release);
        }
        pending_receipt_hint.store(false, std::memory_order_release);
        for (const auto& lane : producer_lanes) if (lane) lane->stop();
    }

    [[nodiscard]] Result<SendReceipt> enqueue(MessageView message,
        RecordId dependency = 0, std::size_t ordinal = 0, bool recovery = false,
        std::uint64_t generation = 0,
        std::string_view expected_schema_fingerprint = {}) noexcept {
        const auto current_life = life ? life->load(std::memory_order_acquire) : EngineLife::stopped;
        if (current_life == EngineLife::stopping || current_life == EngineLife::stopped ||
            stopped.load(std::memory_order_acquire)) return Status{ErrorCode::engine_stopped};
        if (expected_schema_fingerprint != schema_fingerprint)
            return Status{ErrorCode::schema_mismatch};
        if (!active.load(std::memory_order_acquire)) return Status{ErrorCode::not_ready};
        auto measured = measure_owned_message(message, max_frame_bytes, local_comp_id, remote_comp_id, version, emit_last_processed);
        if (!measured.ok()) return measured.status();
        const auto layout = measured.value().header;
        if (layout.total_size > arena.size()) return Status{ErrorCode::queue_full};
        std::unique_lock lock(mutex);
        const auto locked_life = life ? life->load(std::memory_order_acquire) : EngineLife::stopped;
        if (locked_life == EngineLife::stopping || locked_life == EngineLife::stopped ||
            stopped.load(std::memory_order_acquire)) return Status{ErrorCode::engine_stopped};
        if (expected_schema_fingerprint != schema_fingerprint)
            return Status{ErrorCode::schema_mismatch};
        if (!active.load(std::memory_order_acquire)) return Status{ErrorCode::not_ready};
        if (slots.empty() || receipts.empty() || arena.empty()) return Status{ErrorCode::invalid_config};
        if (count == slots.size()) return Status{ErrorCode::queue_full};
        if (count == 0) byte_read = byte_write = 0;
        if (byte_read == byte_write && byte_write > (std::numeric_limits<std::uint64_t>::max() / 2))
            byte_read = byte_write = 0;
        const auto capacity = arena.size();
        std::size_t offset = static_cast<std::size_t>(byte_write % capacity);
        std::size_t padding = 0;
        if (layout.total_size > capacity - offset) {
            padding = capacity - offset;
            offset = 0;
        }
        const auto used = static_cast<std::size_t>(byte_write - byte_read);
        if (padding > capacity - used || layout.total_size > capacity - used - padding)
            return Status{ErrorCode::queue_full};
        const auto reserved = padding + layout.total_size;
        if (reserved > std::numeric_limits<std::uint64_t>::max() - byte_write)
            return Status{ErrorCode::queue_full};
        const auto id = next_id;
        if (id == 0 || id == std::numeric_limits<std::uint64_t>::max())
            return Status{ErrorCode::receipt_expired};
        auto& receipt = receipts[static_cast<std::size_t>(id % receipts.size())];
        if (receipt.occupied && receipt.value.state == ReceiptState::accepted)
            return Status{ErrorCode::queue_full};
        ++next_id;
        receipt.value = SendReceipt{id, 0, ReceiptState::accepted, Status::success()};
        receipt.driver_receipt_id = 0;
        receipt.occupied = true;
        const auto reservation_begin = byte_write;
        byte_write += reserved;
        const auto payload_position = static_cast<std::size_t>((reservation_begin + padding) % capacity);
        auto payload = std::span<char>{arena.data() + payload_position, layout.total_size};
        copy_owned_message(payload, message);
        const auto tail = (head + count) % slots.size();
        const auto accepted_ms = monotonic_ms();
        slots[tail] = SenderCommandSlot{id, reservation_begin, reserved,
            payload_position, layout.total_size, dependency, ordinal, generation, recovery,
            accepted_ms};
        const bool was_empty = count == 0;
        ++count;
        pending_command_hint.store(count, std::memory_order_release);
        if (was_empty) oldest_deadline_hint.store(
            pending_deadline(accepted_ms, max_pending_age_ms), std::memory_order_release);
        const auto accepted = receipt.value;
        lock.unlock();
        if (wake_control) wake_control->wake();
        return accepted;
    }

    [[nodiscard]] Status request_logout(std::string_view reason) noexcept {
        if (stopped.load(std::memory_order_acquire)) return {ErrorCode::engine_stopped};
        if (reason.size() > logout_reason.size() || !detail::is_valid_text(reason))
            return {ErrorCode::invalid_value, 58};
        {
            std::lock_guard lock(mutex);
            if (!active.load(std::memory_order_acquire)) return {ErrorCode::not_ready};
            if (logout_requested) return {ErrorCode::not_ready};
            std::copy(reason.begin(), reason.end(), logout_reason.begin());
            logout_reason_size = reason.size();
            logout_requested = true;
            logout_requested_hint.store(true, std::memory_order_release);
        }
        if (wake_control) wake_control->wake();
        return Status::success();
    }

    [[nodiscard]] std::optional<std::size_t> take_logout(std::span<char> output) noexcept {
        if (!logout_requested_hint.load(std::memory_order_acquire) ||
            pending_command_hint.load(std::memory_order_acquire) != 0) return {};
        if (!producer_lanes.empty())
            for (const auto& lane : producer_lanes)
                if (lane && lane->has_commands()) return {};
        std::lock_guard lock(mutex);
        if (!logout_requested || count != 0 || output.size() < logout_reason_size) return {};
        if (!producer_lanes.empty())
            for (const auto& lane : producer_lanes)
                if (lane && lane->has_commands()) return {};
        std::copy_n(logout_reason.data(), logout_reason_size, output.data());
        logout_requested = false;
        logout_requested_hint.store(false, std::memory_order_release);
        return logout_reason_size;
    }

    [[nodiscard]] Result<SendReceipt> query(std::uint64_t id) const noexcept {
        if (id == 0 || receipts.empty()) return Status{ErrorCode::receipt_expired};
        std::lock_guard lock(mutex);
        const auto& slot = receipts[static_cast<std::size_t>(id % receipts.size())];
        if (!slot.occupied || slot.value.id != id) return Status{ErrorCode::receipt_expired};
        return slot.value;
    }

    void complete(std::uint64_t id, Result<SendReceipt> result) noexcept {
        std::lock_guard lock(mutex);
        auto& slot = receipts[static_cast<std::size_t>(id % receipts.size())];
        if (!slot.occupied || slot.value.id != id) return;
        if (!result.ok()) {
            slot.value.state = result.status().code == ErrorCode::receipt_expired ||
                result.status().code == ErrorCode::backlog_timeout
                ? ReceiptState::expired : ReceiptState::failed;
            slot.value.status = result.status();
            slot.driver_receipt_id = 0;
            return;
        }
        slot.driver_receipt_id = result.value().id;
        slot.value.msg_seq_num = result.value().msg_seq_num;
        slot.value.state = result.value().state;
        slot.value.status = result.value().status;
        if (slot.value.state == ReceiptState::failed || slot.value.state == ReceiptState::expired)
            slot.driver_receipt_id = 0;
        if (slot.value.state != ReceiptState::failed && slot.value.state != ReceiptState::expired &&
            slot.value.state != ReceiptState::written) {
            if (!slot.tracked) {
                pending_indices[pending_count++] = static_cast<std::size_t>(id % receipts.size());
                slot.tracked = true;
            }
            pending_receipt_hint.store(true, std::memory_order_release);
        }
    }

    void sync_driver_receipt(std::uint64_t id, Result<SendReceipt> result) noexcept {
        std::lock_guard lock(mutex);
        auto& slot = receipts[static_cast<std::size_t>(id % receipts.size())];
        if (!slot.occupied || slot.value.id != id || slot.driver_receipt_id == 0) return;
        if (!result.ok()) {
            if (result.status().code == ErrorCode::receipt_expired) {
                slot.value.state = ReceiptState::expired;
            } else {
                slot.value.state = ReceiptState::failed;
            }
            slot.value.status = result.status();
            return;
        }
        slot.value.msg_seq_num = result.value().msg_seq_num;
        slot.value.state = result.value().state;
        slot.value.status = result.value().status;
    }

    void sync_receipts(const SessionDriver& driver) noexcept {
        if (!pending_receipt_hint.load(std::memory_order_acquire)) return;
        std::lock_guard lock(mutex);
        std::size_t index = 0;
        while (index < pending_count) {
            auto& slot = receipts[pending_indices[index]];
            bool terminal = !slot.occupied || slot.driver_receipt_id == 0 ||
                slot.value.state == ReceiptState::failed || slot.value.state == ReceiptState::expired ||
                slot.value.state == ReceiptState::written;
            if (!terminal) {
                const auto current = driver.receipt(slot.driver_receipt_id);
                if (!current.ok()) {
                    slot.value.state = ReceiptState::expired;
                    slot.value.status = current.status();
                } else {
                    slot.value.msg_seq_num = current.value().msg_seq_num;
                    slot.value.state = current.value().state;
                    slot.value.status = current.value().status;
                }
                terminal = slot.value.state == ReceiptState::failed ||
                    slot.value.state == ReceiptState::expired || slot.value.state == ReceiptState::written;
            }
            if (terminal) {
                slot.tracked = false;
                pending_indices[index] = pending_indices[--pending_count];
            } else ++index;
        }
        pending_receipt_hint.store(pending_count != 0, std::memory_order_release);
    }

    [[nodiscard]] bool has_commands() const noexcept {
        if (pending_command_hint.load(std::memory_order_acquire) != 0) {
            std::lock_guard lock(mutex);
            if (count != 0) return true;
        }
        if (!producer_lanes.empty())
            for (const auto& lane : producer_lanes)
                if (lane && lane->has_commands()) return true;
        return false;
    }

    [[nodiscard]] bool has_ready_commands() const noexcept {
        if (pending_command_hint.load(std::memory_order_acquire) != 0) {
            std::lock_guard lock(mutex);
            if (count != 0 && !command_blocked && active.load(std::memory_order_acquire)) return true;
        }
        if (producer_lanes.empty()) return false;
        if (active.load(std::memory_order_acquire))
            for (const auto& lane : producer_lanes)
                if (lane && lane->has_ready_commands()) return true;
        return false;
    }

    [[nodiscard]] bool is_active() const noexcept {
        return active.load(std::memory_order_acquire);
    }

    void block_commands() noexcept {
        std::lock_guard lock(mutex);
        command_blocked = true;
        command_blocked_hint.store(true, std::memory_order_release);
        if (!producer_lanes.empty())
            for (const auto& lane : producer_lanes) if (lane) lane->block_commands();
    }

    void unblock_commands() noexcept {
        if (!command_blocked_hint.load(std::memory_order_acquire)) return;
        std::lock_guard lock(mutex);
        command_blocked = false;
        command_blocked_hint.store(false, std::memory_order_release);
        if (!producer_lanes.empty())
            for (const auto& lane : producer_lanes) if (lane) lane->unblock_commands();
    }

    [[nodiscard]] bool pop(SenderCommandSlot& command, std::span<char> scratch,
        MessageView& message) noexcept {
        if (pending_command_hint.load(std::memory_order_acquire) == 0) return false;
        std::lock_guard lock(mutex);
        if (count == 0 || command_blocked) return false;
        command = slots[head];
        if (command.payload_size > scratch.size()) {
            // Configuration guarantees the scratch covers every valid slot.
            auto& receipt = receipts[static_cast<std::size_t>(command.id % receipts.size())];
            if (receipt.occupied && receipt.value.id == command.id) {
                receipt.value.state = ReceiptState::failed;
                receipt.value.status = Status{ErrorCode::capacity_exceeded};
            }
        } else {
            std::memcpy(scratch.data(), arena.data() + command.payload_offset, command.payload_size);
            message = view_owned_message(scratch.first(command.payload_size));
        }
        return command.payload_size <= scratch.size();
    }

    void finish_command(SenderCommandSlot command) noexcept {
        std::lock_guard lock(mutex);
        if (count == 0 || slots[head].id != command.id) return;
        byte_read = command.reservation_begin + command.reservation_size;
        head = (head + 1) % slots.size();
        --count;
        pending_command_hint.store(count, std::memory_order_release);
        update_oldest_deadline_hint_locked();
        command_blocked = false;
    }

    [[nodiscard]] std::optional<SenderCommandSlot> expire_head(
        std::uint64_t now_ms) noexcept {
        if (max_pending_age_ms == 0) return {};
        const auto deadline = oldest_deadline_hint.load(std::memory_order_acquire);
        if (deadline == 0 || now_ms < deadline) return {};
        std::lock_guard lock(mutex);
        if (count == 0) {
            oldest_deadline_hint.store(0, std::memory_order_release);
            return {};
        }
        const auto command = slots[head];
        if (!pending_expired(command.accepted_ms, now_ms, max_pending_age_ms)) {
            oldest_deadline_hint.store(
                pending_deadline(command.accepted_ms, max_pending_age_ms),
                std::memory_order_release);
            return {};
        }
        auto& receipt = receipts[static_cast<std::size_t>(command.id % receipts.size())];
        if (receipt.occupied && receipt.value.id == command.id) {
            receipt.value.state = ReceiptState::expired;
            receipt.value.status = Status{ErrorCode::backlog_timeout};
            receipt.driver_receipt_id = 0;
        }
        byte_read = command.reservation_begin + command.reservation_size;
        head = (head + 1) % slots.size();
        --count;
        pending_command_hint.store(count, std::memory_order_release);
        update_oldest_deadline_hint_locked();
        return command;
    }

    [[nodiscard]] std::uint64_t next_expiry_deadline() const noexcept {
        return oldest_deadline_hint.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool can_enqueue() const noexcept {
        const auto current_life = life ? life->load(std::memory_order_acquire) : EngineLife::stopped;
        if (stopped.load(std::memory_order_acquire) || current_life != EngineLife::running ||
            !active.load(std::memory_order_acquire)) return false;
        std::lock_guard lock(mutex);
        if (count >= slots.size() || arena.empty() || slots.empty()) return false;
        return static_cast<std::size_t>(byte_write - byte_read) < arena.size();
    }

private:
    void update_oldest_deadline_hint_locked() noexcept {
        const auto deadline = count == 0 || max_pending_age_ms == 0
            ? 0 : pending_deadline(slots[head].accepted_ms, max_pending_age_ms);
        oldest_deadline_hint.store(deadline, std::memory_order_release);
    }
};

Result<std::shared_ptr<ProducerLaneSharedState>> SenderSharedState::create_producer_lane(
    ProducerLaneConfig requested, std::string_view expected_schema_fingerprint) noexcept {
    try {
        std::lock_guard lock(mutex);
        const auto current_life = life ? life->load(std::memory_order_acquire) : EngineLife::stopped;
        if (current_life == EngineLife::stopping || current_life == EngineLife::stopped || stopped.load())
            return Status{ErrorCode::engine_stopped};
        if (current_life != EngineLife::configured || producer_lanes_frozen)
            return Status{ErrorCode::invalid_config};
        if (expected_schema_fingerprint != schema_fingerprint)
            return Status{ErrorCode::schema_mismatch};
        if (requested.capacity_messages == 0 || requested.capacity_bytes == 0 ||
            requested.receipt_capacity == 0 || requested.capacity_bytes > max_producer_lane_bytes ||
            requested.capacity_bytes > std::numeric_limits<std::size_t>::max() -
                (alignof(std::max_align_t) - 1))
            return Status{ErrorCode::invalid_config};
        if (producer_lanes.size() >= max_producer_lanes) return Status{ErrorCode::capacity_exceeded};
        auto lane = std::make_shared<ProducerLaneSharedState>(local_comp_id, remote_comp_id,
            schema_fingerprint, requested, max_frame_bytes, life, wake_control, version,
            emit_last_processed, max_pending_age_ms);
        producer_lanes.push_back(lane);
        return lane;
    } catch (...) {
        return Status{ErrorCode::capacity_exceeded};
    }
}

Result<std::shared_ptr<ProducerLaneSharedState>> create_producer_lane(
    const std::shared_ptr<SenderSharedState>& state, ProducerLaneConfig config,
    std::string_view schema_fingerprint) noexcept {
    return state ? state->create_producer_lane(config, schema_fingerprint)
        : Result<std::shared_ptr<ProducerLaneSharedState>>{Status{ErrorCode::engine_stopped}};
}

Result<SendReceipt> enqueue_producer_lane_message(
    const std::shared_ptr<ProducerLaneSharedState>& state, MessageView message,
    std::string_view schema_fingerprint) noexcept {
    return state ? state->enqueue(message, schema_fingerprint)
        : Result<SendReceipt>{Status{ErrorCode::engine_stopped}};
}

Result<SendReceipt> query_producer_lane_receipt(
    const std::shared_ptr<ProducerLaneSharedState>& state, std::uint64_t id) noexcept {
    return state ? state->query(id) : Result<SendReceipt>{Status{ErrorCode::engine_stopped}};
}

bool producer_lane_can_send(const std::shared_ptr<ProducerLaneSharedState>& state,
    std::string_view schema_fingerprint) noexcept {
    return state && state->schema_fingerprint == schema_fingerprint && state->can_enqueue();
}

bool producer_lane_stopped(const std::shared_ptr<ProducerLaneSharedState>& state) noexcept {
    return !state || state->life_stopped();
}

Result<SendReceipt> enqueue_sender_message(const std::shared_ptr<SenderSharedState>& state,
    MessageView message, std::string_view schema_fingerprint) noexcept {
    return state ? state->enqueue(message, 0, 0, false, 0, schema_fingerprint)
        : Result<SendReceipt>{Status{ErrorCode::engine_stopped}};
}

Result<SendReceipt> enqueue_dependent_sender_message(
    const std::shared_ptr<SenderSharedState>& state, MessageView message,
    DeliveryInfo delivery, std::size_t ordinal) noexcept {
    return state ? state->enqueue(message, delivery.record_id, ordinal,
        delivery.is_recovery, 0, state->schema_fingerprint)
        : Result<SendReceipt>{Status{ErrorCode::engine_stopped}};
}

Result<SendReceipt> query_sender_receipt(const std::shared_ptr<SenderSharedState>& state,
    std::uint64_t id) noexcept {
    return state ? state->query(id) : Result<SendReceipt>{Status{ErrorCode::engine_stopped}};
}

bool sender_can_send(const std::shared_ptr<SenderSharedState>& state,
    std::string_view schema_fingerprint) noexcept {
    return state && state->schema_fingerprint == schema_fingerprint && state->can_enqueue();
}

bool sender_stopped(const std::shared_ptr<SenderSharedState>& state) noexcept {
    if (!state || state->stopped.load(std::memory_order_acquire) || !state->life) return true;
    const auto current_life = state->life->load(std::memory_order_acquire);
    return current_life == EngineLife::stopping || current_life == EngineLife::stopped;
}

Status enqueue_sender_logout(const std::shared_ptr<SenderSharedState>& state,
    std::string_view reason) noexcept {
    return state ? state->request_logout(reason) : Status{ErrorCode::engine_stopped};
}


namespace {

constexpr std::size_t no_index = std::numeric_limits<std::size_t>::max();
constexpr std::size_t io_byte_budget = 64U * 1024U;
constexpr std::size_t frame_budget = 16;
constexpr std::size_t read_chunk_size = 8192;

std::uint64_t monotonic_ms() noexcept {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(duration_cast<milliseconds>(
        steady_clock::now().time_since_epoch()).count());
}

std::array<char, 32> wall_timestamp() noexcept {
    // Per-thread cached second prefix avoids strftime/snprintf/gmtime_r per frame.
    thread_local std::time_t cached_sec = -1;
    thread_local std::array<char, 32> cached_buf{};
    timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    if (ts.tv_sec != cached_sec) {
        cached_sec = ts.tv_sec;
        std::tm utc{};
        if (::gmtime_r(&ts.tv_sec, &utc) == nullptr) {
            std::memcpy(cached_buf.data(), "19700101-00:00:00.000000000", 28);
        } else {
            auto w2 = [](char* o, unsigned v) noexcept {
                o[0] = static_cast<char>('0' + (v / 10U) % 10U);
                o[1] = static_cast<char>('0' + v % 10U);
            };
            auto w4 = [](char* o, unsigned v) noexcept {
                o[0] = static_cast<char>('0' + (v / 1000U) % 10U);
                o[1] = static_cast<char>('0' + (v / 100U) % 10U);
                o[2] = static_cast<char>('0' + (v / 10U) % 10U);
                o[3] = static_cast<char>('0' + v % 10U);
            };
            w4(cached_buf.data() + 0, static_cast<unsigned>(utc.tm_year + 1900));
            w2(cached_buf.data() + 4, static_cast<unsigned>(utc.tm_mon + 1));
            w2(cached_buf.data() + 6, static_cast<unsigned>(utc.tm_mday));
            cached_buf[8] = '-';
            w2(cached_buf.data() + 9, static_cast<unsigned>(utc.tm_hour));
            cached_buf[11] = ':';
            w2(cached_buf.data() + 12, static_cast<unsigned>(utc.tm_min));
            cached_buf[14] = ':';
            w2(cached_buf.data() + 15, static_cast<unsigned>(utc.tm_sec));
            cached_buf[17] = '.';
            cached_buf[27] = '\0';
        }
    }
    const auto nsec = static_cast<unsigned>(ts.tv_nsec);
    const unsigned ms = nsec / 1'000'000U;
    const unsigned rem_us = nsec - ms * 1'000'000U;
    const unsigned us = rem_us / 1'000U;
    const unsigned ns = rem_us - us * 1'000U;
    auto w3 = [](char* o, unsigned v) noexcept {
        o[0] = static_cast<char>('0' + (v / 100U) % 10U);
        o[1] = static_cast<char>('0' + (v / 10U) % 10U);
        o[2] = static_cast<char>('0' + v % 10U);
    };
    w3(cached_buf.data() + 18, ms);
    w3(cached_buf.data() + 21, us);
    w3(cached_buf.data() + 24, ns);
    return cached_buf;
}

constexpr std::string_view timestamp_view(const std::array<char, 32>& value,
    TimestampPrecision precision = TimestampPrecision::milliseconds) noexcept {
    return {value.data(), timestamp_length(precision)};
}

bool plain_identifier(std::string_view value, std::size_t max_size) noexcept {
    if (value.empty() || value.size() > max_size) return false;
    for (const unsigned char c : value) {
        if (c < 0x21 || c > 0x7e || c == '=' || c == soh) return false;
    }
    return true;
}

bool same_identity_options(const SessionSettings& first, const SessionSettings& second) noexcept {
    return first.version == second.version && first.qualifier == second.qualifier;
}

std::string stable_session_key(std::string_view first, std::string_view second,
    const SessionSettings& settings) {
    std::string key{begin_string(settings.version)};
    key.push_back('|');
    key.append(std::to_string(first.size()));
    key.push_back(':');
    key.append(first);
    key.push_back('|');
    key.append(std::to_string(second.size()));
    key.push_back(':');
    key.append(second);
    if (!settings.qualifier.empty()) {
        key.push_back('|');
        key.append(std::to_string(settings.qualifier.size()));
        key.push_back(':');
        key.append(settings.qualifier);
    }
    return key;
}

Status field_value(const FlatMessageView& message, std::uint32_t tag,
                   std::string_view& value) noexcept {
    auto found = message.find_unique(tag);
    if (!found.ok()) return found.status();
    value = found.value().value;
    return Status::success();
}

} // namespace

namespace {

struct SessionControl;

struct RegisteredMessageHandler {
    std::string msg_type;
    EngineMessageCallback callback;
};

struct IgnoredMessageType {
    std::string msg_type;
    std::string reason;
};

struct DeliveryCompletion {
    std::shared_ptr<SessionControl> session;
    std::uint64_t generation{};
    DeliveryInfo info{};
    Status status{};
    std::size_t accepted_sends{};
    JournalLease journal_lease{};
    bool active_callback{};
};

struct SessionControl {
    std::string name;
    std::shared_ptr<SenderSharedState> sender;
    std::atomic<SessionState> state{SessionState::disconnected};
    std::atomic<bool> callback_in_flight{false};
    std::atomic<std::uint64_t> generation{0};
    std::atomic<ErrorCode> last_error{ErrorCode::none};
    std::atomic<int> last_error_tag{0};
    std::atomic<std::size_t> last_error_offset{0};
    std::atomic<std::uint64_t> next_inbound_seq{1}, next_outbound_seq{1};
    std::atomic<std::uint64_t> received_frames{0}, received_bytes{0}, written_bytes{0};
    std::atomic<std::uint64_t> business_deliveries{0};
    std::atomic<std::size_t> pending_messages{0}, pending_bytes{0};
    std::atomic<std::size_t> pending_inbound_messages{0}, pending_inbound_bytes{0};
    std::atomic<bool> read_paused{false};
    std::uint64_t callback_started_ms{};
    bool active_callback_pending{};
    std::uint64_t active_pending_generation{};
    RecordId callback_record_id{};
    std::uint64_t callback_generation{};
    std::size_t callback_commands_processed{};
    std::size_t callback_replies_appended{};
    std::size_t expiry_source_cursor{};
    Status callback_send_failure{};
    std::optional<DeliveryCompletion> pending_completion;
};

Result<SendReceipt> queued_context_send(void* raw_state, MessageView message,
    std::uint64_t, std::string_view, DeliveryInfo info, std::size_t ordinal,
    std::uint64_t generation) noexcept {
    auto* state = static_cast<SenderSharedState*>(raw_state);
    return state == nullptr ? Result<SendReceipt>{Status{ErrorCode::engine_stopped}}
        : state->enqueue(message, info.record_id, ordinal, info.is_recovery,
            generation, state->schema_fingerprint);
}

} // namespace

struct EngineRuntime::Impl {
    enum class TargetKind : std::uint8_t { wake, listener, pending, peer };

    static IoTag make_tag(TargetKind k) noexcept {
        IoTag t{};
        t.kind = static_cast<std::uint8_t>(k);
        return t;
    }

    struct Listener {
        ListenerConfig config;
        std::optional<TcpListener> socket;
        TcpEndpoint published;
        TlsContext tls_context;
        std::uint64_t accept_backoff_until{};
        IoTag io_tag{make_tag(TargetKind::listener)};
    };

    struct Peer;

    struct Acceptor {
        AcceptorConfig config;
        std::size_t listener_index{no_index};
        Peer* active{};
        std::shared_ptr<SessionControl> session;
    };

    enum class Connection : std::uint8_t { disconnected, connecting, handshaking, connected };

    struct Peer {
        bool initiator{};
        bool closed{};
        bool disabled{};
        bool schedule_logout_sent{};
        SessionSettings settings;
        TlsConfig tls_config;
        TlsContext tls_context;
        std::optional<TlsStream> tls;
        std::uint64_t handshake_deadline{};
        std::uint64_t read_pause_since{};
        std::string name;
        std::string local_comp_id;
        std::string remote_comp_id;
        std::shared_ptr<SessionControl> session;
        std::uint64_t generation{};
        TcpEndpoint endpoint;
        std::size_t acceptor_index{no_index};
        Connection connection{Connection::disconnected};
        TcpStream stream;
        std::optional<SessionDriver> driver;
        std::string receive_buffer;
        std::size_t receive_head{};
        std::uint64_t reconnect_at{};
        std::uint64_t connect_deadline{};
        bool driver_connected{};
        bool shutdown_logout_sent{};
        RecordId deferred_delivery_id{};
        std::size_t dependent_reply_count{};
        std::size_t dependent_commands_processed{};
        Status dependent_send_failure{};
        std::optional<DeliveryCompletion> pending_completion;
        IoTag io_tag{make_tag(TargetKind::peer)};
        // Proactor (io_uring) owned async I/O buffers & in-flight guards.
        std::vector<char> uring_rx_buf;
        std::vector<char> uring_tx_buf;
        std::size_t uring_tx_size{};
        std::size_t uring_tx_offset{};
        bool uring_rx_in_flight{};
        bool uring_tx_in_flight{};

        [[nodiscard]] std::string_view unread_view() const noexcept {
            return {receive_buffer.data() + receive_head, receive_buffer.size() - receive_head};
        }
        [[nodiscard]] std::size_t unread_size() const noexcept {
            return receive_buffer.size() - receive_head;
        }
        void consume_receive(std::size_t n) noexcept {
            receive_head += n;
            if (receive_head >= receive_buffer.size()) {
                receive_buffer.clear();
                receive_head = 0;
            }
        }
        void compact_receive() noexcept {
            if (receive_head == 0) return;
            const std::size_t rem = receive_buffer.size() - receive_head;
            if (rem != 0) std::memmove(receive_buffer.data(), receive_buffer.data() + receive_head, rem);
            receive_buffer.resize(rem);
            receive_head = 0;
        }
        void clear_receive() noexcept {
            receive_buffer.clear();
            receive_head = 0;
        }
    };

    struct Pending {
        bool closed{};
        std::size_t listener_index{no_index};
        TcpStream stream;
        std::optional<TlsStream> tls;
        bool handshaking{};
        std::string receive_buffer;
        std::uint64_t deadline{};
        IoTag io_tag{make_tag(TargetKind::pending)};
    };

    struct DeliveryTask {
        std::shared_ptr<SessionControl> session;
        std::uint64_t generation{};
        DeliveryInfo info{};
        DeliveryLease lease{};
        std::size_t ring_offset{};
        std::size_t frame_size{};
        bool active_callback{};
    };

    explicit Impl(EngineConfig value, bool use_queued,
        const SchemaDescriptor* message_schema, WireHints schema_hints)
        : config(std::move(value)), field_scratch((config.max_frame_bytes / 4) + 16),
          queued(use_queued), schema(message_schema),
          life(std::make_shared<std::atomic<EngineLife>>(EngineLife::configured)) {
        const auto custom_hints = config.wire_hints.length_data_pairs;
        wire_hint_pairs.reserve(custom_hints.size() + schema_hints.length_data_pairs.size());
        wire_hint_pairs.insert(wire_hint_pairs.end(), custom_hints.begin(), custom_hints.end());
        wire_hint_pairs.insert(wire_hint_pairs.end(), schema_hints.length_data_pairs.begin(),
            schema_hints.length_data_pairs.end());
        wire_hints = WireHints{wire_hint_pairs};
        wake_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        auto created_backend = create_io_backend(config.io_backend);
        if (!created_backend.ok()) {
            io_backend_status = created_backend.status();
            if (wake_fd >= 0) { ::close(wake_fd); wake_fd = -1; }
        } else if (wake_fd >= 0) {
            io_backend = std::move(created_backend).value();
            try {
                wake_control = std::make_shared<WakeControl>(wake_fd);
            } catch (...) {
                ::close(wake_fd);
                wake_fd = -1;
                io_backend.reset();
                throw;
            }
            wake_tag = make_tag(TargetKind::wake);
            io_sync(wake_tag, wake_fd, io_event_read);
        } else {
            io_backend.reset();
        }
    }

    ~Impl() {
        stop_workers();
        discard_stopped_worker_state();
        stop_diagnostics();
        close_all();
        for (auto& session : sessions) if (session && session->sender) session->sender->mark_stopped();
        // Tear down io_uring / epoll before peer buffers or wake_fd are destroyed.
        io_backend.reset();
        if (wake_control) wake_control->close();
        wake_fd = -1;
    }

    void io_remove(IoTag& tag) noexcept {
        if (io_backend) io_backend->remove(tag);
    }

    void io_sync(IoTag& tag, int fd, std::uint32_t desired) noexcept {
        if (io_backend) io_backend->sync_readiness(tag, fd, desired);
    }

    void cancel_peer_proactor(Peer& peer) noexcept {
        if (io_backend && io_backend->supports_proactor() &&
            (peer.uring_rx_in_flight || peer.uring_tx_in_flight) && peer.stream.valid()) {
            io_backend->cancel_fd(peer.stream.native_handle());
            io_backend->flush_submissions();
        }
        peer.uring_tx_size = 0;
        peer.uring_tx_offset = 0;
    }

    void close_pending(Pending& item) noexcept {
        const bool had_poll = item.io_tag.poll_armed;
        io_remove(item.io_tag);
        if (had_poll && io_backend && io_backend->supports_proactor()) {
            io_backend->flush_submissions();
        }
        item.closed = true;
        if (item.tls) {
            item.tls->close();
            item.tls.reset();
        }
        item.stream.close();
    }

    void close_all() noexcept {
        for (auto& session : sessions) if (session && session->sender) session->sender->set_active(false);
        for (auto& peer : peers) {
            if (peer->session && peer->driver) {
                peer->session->sender->sync_receipts(*peer->driver);
                if (!peer->session->sender->producer_lanes.empty())
                    for (const auto& lane : peer->session->sender->producer_lanes)
                        if (lane) lane->sync_receipts(*peer->driver);
            }
            io_remove(peer->io_tag);
            cancel_peer_proactor(*peer);
            if (peer->tls) {
                peer->tls->close();
                peer->tls.reset();
            }
            peer->stream.close();
            peer->driver.reset();
        }
        for (auto& pending_peer : pending) {
            close_pending(*pending_peer);
        }
        for (auto& listener : listeners) {
            io_remove(listener.io_tag);
            if (listener.socket) listener.socket->close();
        }
        io_remove(wake_tag);
    }

    [[nodiscard]] Status validate_config() const noexcept {
        if (wake_fd < 0 || config.storage.directory.empty() ||
            config.heartbeat_ms == 0 || config.connect_timeout_ms == 0 ||
            config.reconnect_delay_ms == 0 || config.logon_timeout_ms == 0 ||
            config.write_stall_ms == 0 || config.logout_timeout_ms == 0 ||
            config.max_frame_bytes < 128 || config.max_frame_bytes > default_max_frame_bytes ||
            config.max_sessions == 0 || config.max_pending_connections == 0 ||
            config.max_pending_messages == 0 || config.max_pending_bytes == 0 ||
            config.max_pending_inbound_messages == 0 ||
            config.max_pending_sender_messages == 0 || config.max_pending_sender_bytes == 0 ||
            config.sender_receipt_capacity == 0 || config.diagnostic_capacity == 0 ||
            config.max_producer_lanes_per_session > 64 ||
            config.max_pending_sender_bytes > std::numeric_limits<std::size_t>::max() -
                (alignof(std::max_align_t) - 1))
            return {ErrorCode::invalid_config};
        if (config.max_pending_messages <= 8 ||
            config.max_frame_bytes > std::numeric_limits<std::size_t>::max() / 3 ||
            config.max_pending_bytes < config.max_frame_bytes * 3 ||
            config.max_pending_inbound_bytes < config.max_frame_bytes)
            return {ErrorCode::invalid_config};
        if (queued && (config.queued.worker_threads == 0 ||
            config.queued.max_pending_callbacks == 0 ||
            config.queued.max_pending_callback_bytes < config.max_frame_bytes))
            return {ErrorCode::invalid_config};
        for (std::size_t i = 0; i < wire_hint_pairs.size(); ++i) {
            const auto left = wire_hint_pairs[i];
            if (left.length_tag == 0 || left.data_tag == 0 || left.length_tag == left.data_tag)
                return {ErrorCode::invalid_config};
            for (std::size_t j = i + 1; j < wire_hint_pairs.size(); ++j) {
                const auto right = wire_hint_pairs[j];
                const bool conflict =
                    (left.length_tag == right.length_tag && left.data_tag != right.data_tag) ||
                    (left.data_tag == right.data_tag && left.length_tag != right.length_tag) ||
                    (left.length_tag == right.data_tag && left.data_tag != right.length_tag) ||
                    (left.data_tag == right.length_tag && left.length_tag != right.data_tag);
                if (conflict) return {ErrorCode::invalid_config};
            }
        }
        constexpr std::size_t journal_superblock_bytes = 4096;
        constexpr std::size_t journal_record_header_bytes = 64;
        if (config.storage.journal_capacity_bytes <
                journal_superblock_bytes + journal_record_header_bytes ||
            config.storage.journal_max_records == 0 ||
            config.storage.journal_max_records >
                (config.storage.journal_capacity_bytes - journal_superblock_bytes) /
                    journal_record_header_bytes)
            return {ErrorCode::invalid_config};
        if (config.storage.segment_bytes != 0 &&
            (config.storage.segment_bytes < 8192 ||
             config.storage.segment_bytes > config.storage.journal_capacity_bytes))
            return {ErrorCode::invalid_config};
        for (const auto& item : initiators) {
            if (schema && !schema->matches_session(item.settings.version, item.settings.default_appl_ver_id))
                return {ErrorCode::unsupported_protocol, 8};
            const auto status = validate_session_settings(item.settings, false, item.tls.enabled);
            if (!status.ok()) return status;
            if (item.settings.flow_control &&
                (item.settings.flow_control->max_pending_bytes < config.max_frame_bytes * 3 ||
                 item.settings.flow_control->max_pending_inbound_bytes < config.max_frame_bytes))
                return {ErrorCode::invalid_config};
            if (item.tls.enabled) {
                const auto context = TlsContext::create(item.tls, false);
                if (!context.ok()) return context.status();
            }
        }
        for (const auto& item : listeners) if (item.config.tls.enabled) {
            const auto context = TlsContext::create(item.config.tls, true);
            if (!context.ok()) return context.status();
        }
        for (const auto& item : acceptors) {
            if (schema && !schema->matches_session(item.config.settings.version, item.config.settings.default_appl_ver_id))
                return {ErrorCode::unsupported_protocol, 8};
            if (item.listener_index >= listeners.size()) return {ErrorCode::invalid_config};
            const auto status = validate_session_settings(item.config.settings, true,
                listeners[item.listener_index].config.tls.enabled);
            if (!status.ok()) return status;
            if (item.config.settings.flow_control &&
                (item.config.settings.flow_control->max_pending_bytes < config.max_frame_bytes * 3 ||
                 item.config.settings.flow_control->max_pending_inbound_bytes < config.max_frame_bytes))
                return {ErrorCode::invalid_config};
        }
        if (initiators.size() + acceptors.size() > config.max_sessions)
            return {ErrorCode::invalid_config};
        return Status::success();
    }

    void wake() noexcept {
        if (wake_control) wake_control->wake();
    }

    [[nodiscard]] std::shared_ptr<SessionControl> make_session_control(
        std::string name, std::string local_comp, std::string remote_comp, const SessionSettings& settings) {
        auto control = std::make_shared<SessionControl>();
        control->name = name;
        const auto max_pending_age_ms = settings.flow_control
            ? settings.flow_control->max_pending_age_ms : config.max_pending_age_ms;
        control->sender = std::make_shared<SenderSharedState>(name,
            std::move(local_comp), std::move(remote_comp),
            schema == nullptr ? std::string{} : std::string{schema->fingerprint},
            config.max_pending_sender_messages, config.max_pending_sender_bytes,
            config.sender_receipt_capacity, config.max_frame_bytes,
            config.max_producer_lanes_per_session, life, wake_control,
            settings.version, settings.emit_last_processed_seq, max_pending_age_ms);
        sessions.push_back(control);
        return control;
    }

    [[nodiscard]] Status dispatch_business(SessionContext& context,
        const FlatMessageView& message, const DeliveryInfo& info) {
        std::string_view msg_type{};
        std::size_t msg_type_offset = 0;
        const auto fields = message.fields();
        if (fields.size() >= 3 && fields[2].tag == 35) {
            msg_type = fields[2].value;
            msg_type_offset = fields[2].offset;
        } else {
            auto type_field = message.find_unique(35);
            if (!type_field.ok()) return type_field.status();
            msg_type = type_field.value().value;
            msg_type_offset = type_field.value().offset;
        }
        if (schema == nullptr) {
            const auto ignored_type = std::lower_bound(ignored_messages.begin(), ignored_messages.end(),
                msg_type, [](const IgnoredMessageType& item, std::string_view value) {
                    return item.msg_type < value;
                });
            if (ignored_type != ignored_messages.end() &&
                ignored_type->msg_type == msg_type) return Status::success();
            if (!message_callback) return Status{ErrorCode::message_unhandled, 35};
            return message_callback(context, message, info);
        }
        const auto* descriptor = schema->find_message(msg_type);
        if (descriptor == nullptr)
            return Status{ErrorCode::message_unhandled, 35, msg_type_offset};
        const auto handler = std::lower_bound(typed_handlers.begin(), typed_handlers.end(),
            msg_type, [](const RegisteredMessageHandler& item, std::string_view value) {
                return item.msg_type < value;
            });
        if (handler != typed_handlers.end() && handler->msg_type == msg_type)
            return handler->callback(context, message, info);
        const auto ignored_type = std::lower_bound(ignored_messages.begin(), ignored_messages.end(),
            msg_type, [](const IgnoredMessageType& item, std::string_view value) {
                return item.msg_type < value;
            });
        if (ignored_type != ignored_messages.end() && ignored_type->msg_type == msg_type)
            return Status::success();
        return Status{ErrorCode::message_unhandled, 35, msg_type_offset};
    }

    [[nodiscard]] Status enqueue_delivery(const DeliveryLease& lease,
        const std::shared_ptr<SessionControl>& session, std::uint64_t generation) noexcept {
        if (!queued || !session || lease.frame.empty()) return Status{ErrorCode::invalid_config};
        bool expected = false;
        if (!session->callback_in_flight.compare_exchange_strong(expected, true,
                std::memory_order_acq_rel)) return Status{ErrorCode::queue_full};
        std::lock_guard lock(worker_mutex);
        if (worker_stopping || !workers_started) {
            session->callback_in_flight.store(false, std::memory_order_release);
            return Status{ErrorCode::engine_stopped};
        }
        if (worker_free_count == 0 || lease.frame.size() > config.queued.max_pending_callback_bytes -
                std::min(worker_payload_bytes, config.queued.max_pending_callback_bytes)) {
            session->callback_in_flight.store(false, std::memory_order_release);
            return Status{ErrorCode::queue_full};
        }
        const auto index = worker_free_indices[--worker_free_count];
        auto& task = worker_tasks[index];
        task.session = session;
        task.generation = generation;
        task.info = lease.info;
        task.frame_size = lease.frame.size();
        task.active_callback = false;
        if (config.queued.delivery_strategy == QueuedDeliveryStrategy::descriptor_ring) {
            const auto ring_capacity = worker_ring_arena.size() / config.max_frame_bytes;
            if (index >= ring_capacity || lease.frame.size() > config.max_frame_bytes) {
                worker_free_indices[worker_free_count++] = index;
                session->callback_in_flight.store(false, std::memory_order_release);
                return Status{ErrorCode::queue_full};
            }
            task.ring_offset = index * config.max_frame_bytes;
            std::memcpy(worker_ring_arena.data() + task.ring_offset,
                lease.frame.data(), lease.frame.size());
            task.lease = DeliveryLease{lease.info, {}, lease.journal_lease};
        } else {
            task.lease = lease;
        }
        worker_payload_bytes += lease.frame.size();
        worker_ready[worker_ready_tail] = index;
        worker_ready_tail = (worker_ready_tail + 1) % worker_ready.size();
        ++worker_ready_count;
        worker_cv.notify_one();
        return Status::success();
    }

    [[nodiscard]] Status enqueue_active_callback(
        const std::shared_ptr<SessionControl>& session, std::uint64_t generation) noexcept {
        if (!queued || !session) return Status{ErrorCode::invalid_config};
        bool expected = false;
        if (!session->callback_in_flight.compare_exchange_strong(expected, true,
                std::memory_order_acq_rel)) return Status{ErrorCode::queue_full};
        std::lock_guard lock(worker_mutex);
        if (worker_stopping || !workers_started || worker_free_count == 0) {
            session->callback_in_flight.store(false, std::memory_order_release);
            return Status{worker_stopping ? ErrorCode::engine_stopped : ErrorCode::queue_full};
        }
        const auto index = worker_free_indices[--worker_free_count];
        auto& task = worker_tasks[index];
        task.session = session;
        task.generation = generation;
        task.info = {};
        task.lease = {};
        task.ring_offset = 0;
        task.frame_size = 0;
        task.active_callback = true;
        session->callback_started_ms = monotonic_ms();
        worker_ready[worker_ready_tail] = index;
        worker_ready_tail = (worker_ready_tail + 1) % worker_ready.size();
        ++worker_ready_count;
        worker_cv.notify_one();
        return Status::success();
    }

    [[nodiscard]] Status schedule_active_callback(
        const std::shared_ptr<SessionControl>& session, std::uint64_t generation) noexcept {
        if (!active_callback) return Status::success();
        session->callback_record_id = 0;
        session->callback_generation = generation;
        session->callback_commands_processed = 0;
        session->callback_replies_appended = 0;
        session->callback_send_failure = Status::success();
        const auto status = enqueue_active_callback(session, generation);
        if (status.ok()) session->active_callback_pending = false;
        else {
            session->callback_record_id = 0;
            session->callback_generation = 0;
            session->callback_commands_processed = 0;
            session->callback_replies_appended = 0;
            session->callback_send_failure = Status::success();
        }
        return status;
    }

    void schedule_pending_active_callbacks() noexcept {
        if (!queued || !active_callback) return;
        for (const auto& session : sessions) {
            if (!session || !session->active_callback_pending ||
                session->callback_in_flight.load(std::memory_order_acquire)) continue;
            auto* peer = active_peer_for(session);
            if (peer == nullptr || peer->generation != session->active_pending_generation ||
                peer->connection != Connection::connected || !peer->driver ||
                session->state.load(std::memory_order_acquire) != SessionState::active) continue;
            const auto status = schedule_active_callback(session, peer->generation);
            if (!status.ok() && status.code != ErrorCode::queue_full) {
                report_error(session->name, status);
                stopping.store(true, std::memory_order_release);
            }
        }
    }

    [[nodiscard]] Status start_workers() noexcept {
        if (!queued) return Status::success();
        try {
            std::size_t capacity = config.queued.max_pending_callbacks;
            if (config.queued.delivery_strategy == QueuedDeliveryStrategy::descriptor_ring) {
                capacity = std::min(capacity,
                    config.queued.max_pending_callback_bytes / config.max_frame_bytes);
                if (capacity == 0 || capacity > std::numeric_limits<std::size_t>::max() /
                        config.max_frame_bytes) return {ErrorCode::invalid_config};
                worker_ring_arena.resize(capacity * config.max_frame_bytes);
            }
            worker_tasks.resize(capacity);
            worker_ready.resize(capacity);
            worker_free_indices.resize(capacity);
            for (std::size_t i = 0; i < worker_tasks.size(); ++i)
                worker_free_indices[i] = worker_tasks.size() - i - 1;
            worker_free_count = worker_tasks.size();
            worker_completions.resize(config.max_sessions);
            workers.reserve(config.queued.worker_threads);
            for (std::size_t i = 0; i < config.queued.worker_threads; ++i)
                workers.emplace_back([this] { worker_loop(); });
            workers_started = true;
            return Status::success();
        } catch (...) {
            stop_workers();
            return Status{ErrorCode::capacity_exceeded};
        }
    }

    void stop_workers() noexcept {
        {
            std::lock_guard lock(worker_mutex);
            worker_stopping = true;
        }
        worker_cv.notify_all();
        for (auto& worker : workers) if (worker.joinable()) worker.join();
        workers.clear();
        workers_started = false;
    }

    void discard_stopped_worker_state() noexcept {
        for (const auto& session : sessions) {
            if (!session) continue;
            // A completed worker result may still own this session through
            // pending_completion. Shutdown discards it without acknowledging
            // the journal delivery so recovery can redeliver it.
            session->pending_completion.reset();
            session->callback_record_id = 0;
            session->callback_generation = 0;
            session->callback_commands_processed = 0;
            session->callback_replies_appended = 0;
            session->callback_send_failure = Status::success();
            session->active_callback_pending = false;
            session->active_pending_generation = 0;
            session->callback_in_flight.store(false, std::memory_order_release);
        }
        std::lock_guard lock(worker_mutex);
        for (auto& task : worker_tasks) task = {};
        for (auto& completion : worker_completions) completion = {};
        worker_ready_count = 0;
        worker_completion_count = 0;
        worker_payload_bytes = 0;
        worker_free_count = worker_free_indices.size();
    }

    void worker_loop() noexcept {
        std::vector<FieldView> fields;
        try { fields.resize(config.max_frame_bytes / 4 + 16); }
        catch (...) {
            worker_failure.store(true, std::memory_order_release);
            stopping.store(true, std::memory_order_release);
            wake();
            return;
        }
        for (;;) {
            std::size_t index{};
            {
                std::unique_lock lock(worker_mutex);
                worker_cv.wait(lock, [&] { return worker_stopping || worker_ready_count != 0; });
                if (worker_ready_count == 0 && worker_stopping) return;
                index = worker_ready[worker_ready_head];
                worker_ready_head = (worker_ready_head + 1) % worker_ready.size();
                --worker_ready_count;
            }
            auto& task = worker_tasks[index];
            const std::string_view wire = config.queued.delivery_strategy ==
                QueuedDeliveryStrategy::descriptor_ring
                ? std::string_view{worker_ring_arena.data() + task.ring_offset, task.frame_size}
                : task.lease.frame;
            Status callback_status{};
            std::size_t accepted_sends = 0;
            try {
                if (task.active_callback) {
                    auto wall = wall_timestamp();
                    SessionContext context{task.session->sender, &queued_context_send,
                        task.session->state.load(std::memory_order_acquire),
                        monotonic_ms(), timestamp_view(wall),
                        {}, task.session->sender->name,
                        task.session->sender->local_comp_id,
                        task.session->sender->remote_comp_id, task.generation};
                    callback_status = active_callback
                        ? active_callback(context) : Status::success();
                    accepted_sends = context.accepted_sends();
                } else {
                    FrameView frame{};
                    callback_status = inspect_frame(wire, frame, wire_hints,
                        CodecLimits{config.max_frame_bytes});
                    FlatMessageView message{};
                    if (callback_status.ok()) callback_status = parse_flat(frame,
                        std::span<FieldView>{fields.data(), fields.size()}, message, wire_hints,
                        CodecLimits{config.max_frame_bytes});
                    if (callback_status.ok()) {
                        auto wall = wall_timestamp();
                        SessionContext context{task.session->sender, &queued_context_send,
                            task.session->state.load(std::memory_order_acquire),
                            monotonic_ms(), timestamp_view(wall),
                            task.info, task.session->sender->name,
                            task.session->sender->local_comp_id,
                            task.session->sender->remote_comp_id, task.generation};
                        callback_status = dispatch_business(context, message, task.info);
                        if (callback_status.ok()) task.session->business_deliveries.fetch_add(1, std::memory_order_relaxed);
                        accepted_sends = context.accepted_sends();
                    }
                }
            } catch (...) {
                callback_status = Status{ErrorCode::callback_failed};
            }

            {
                std::lock_guard lock(worker_mutex);
                if (worker_completion_count == worker_completions.size()) {
                    worker_failure.store(true, std::memory_order_release);
                    stopping.store(true, std::memory_order_release);
                } else {
                    worker_completions[worker_completion_tail] = DeliveryCompletion{
                        task.session, task.generation, task.info, callback_status,
                        accepted_sends, std::move(task.lease.journal_lease),
                        task.active_callback};
                    worker_completion_tail = (worker_completion_tail + 1) % worker_completions.size();
                    ++worker_completion_count;
                }
                worker_payload_bytes -= task.frame_size;
                worker_free_indices[worker_free_count++] = index;
                task.session.reset();
                task.lease = {};
                task.frame_size = 0;
                task.ring_offset = 0;
                task.active_callback = false;
            }
            wake();
        }
    }

    [[nodiscard]] bool pop_worker_completion(DeliveryCompletion& result) noexcept {
        std::lock_guard lock(worker_mutex);
        if (worker_completion_count == 0) return false;
        result = std::move(worker_completions[worker_completion_head]);
        worker_completions[worker_completion_head] = {};
        worker_completion_head = (worker_completion_head + 1) % worker_completions.size();
        --worker_completion_count;
        return true;
    }

    [[nodiscard]] std::span<char> sender_scratch() noexcept {
        if (sender_scratch_words.empty()) return {};
        return {reinterpret_cast<char*>(sender_scratch_words.data()),
            sender_scratch_words.size() * sizeof(std::max_align_t)};
    }

    void note_sender_command_result(SessionControl& session,
        const SenderCommandSlot& command, const Result<SendReceipt>& result) noexcept {
        if (command.dependency != 0) {
            if (command.dependency == session.callback_record_id &&
                command.generation == session.callback_generation) {
                ++session.callback_commands_processed;
                if (result.ok()) {
                    ++session.callback_replies_appended;
                } else if (session.callback_send_failure.ok()) {
                    session.callback_send_failure = result.status();
                }
            }
        } else if (command.generation != 0 && session.callback_record_id == 0 &&
            command.generation == session.callback_generation) {
            ++session.callback_commands_processed;
            if (result.ok()) {
                ++session.callback_replies_appended;
            } else if (session.callback_send_failure.ok()) {
                session.callback_send_failure = result.status();
            }
        }
    }

    void expire_pending_commands(std::uint64_t now,
        std::size_t command_budget = 32) noexcept {
        if (sessions.empty() || command_budget == 0) return;
        std::size_t idle_sessions = 0;
        while (command_budget != 0 && idle_sessions < sessions.size()) {
            const auto session_index = pending_expiry_session_cursor % sessions.size();
            pending_expiry_session_cursor = (session_index + 1) % sessions.size();
            const auto& session = sessions[session_index];
            if (!session || !session->sender) {
                ++idle_sessions;
                continue;
            }

            auto& sender = *session->sender;
            const auto source_count = sender.producer_lanes.size() + 1;
            auto source = session->expiry_source_cursor % source_count;
            bool expired = false;
            for (std::size_t checked = 0; checked < source_count; ++checked) {
                const auto source_index = source;
                source = (source + 1) % source_count;
                session->expiry_source_cursor = source;
                if (source_index == 0) {
                    const auto command = sender.expire_head(now);
                    if (!command) continue;
                    const Result<SendReceipt> result{Status{ErrorCode::backlog_timeout}};
                    note_sender_command_result(*session, *command, result);
                    report_error(session->name, result.status());
                    expired = true;
                    break;
                }
                const auto& lane = sender.producer_lanes[source_index - 1];
                if (!lane) continue;
                const auto command = lane->expire_head(now);
                if (!command) continue;
                report_error(session->name, Status{ErrorCode::backlog_timeout});
                expired = true;
                break;
            }
            if (expired) {
                --command_budget;
                idle_sessions = 0;
            } else {
                ++idle_sessions;
            }
        }
    }

    void process_sender_commands(Peer& peer, std::uint64_t,
        std::size_t command_budget = 32) noexcept {
        if (!peer.session || !peer.driver || peer.closed) return;
        auto scratch = sender_scratch();
        if (scratch.empty()) return;
        const auto wall = wall_timestamp();
        auto& sender = *peer.session->sender;
        const bool drain_in_flight = peer.session->callback_in_flight.load(std::memory_order_acquire);
        if (!sender.is_active() && !drain_in_flight) return;
        for (std::size_t processed = 0; processed < command_budget; ++processed) {
            auto admission_now = monotonic_ms();
            const auto expired = sender.expire_head(admission_now);
            if (expired) {
                const Result<SendReceipt> result{Status{ErrorCode::backlog_timeout}};
                note_sender_command_result(*peer.session, *expired, result);
                report_error(peer.name, result.status());
                continue;
            }
            SenderCommandSlot command{};
            MessageView message{};
            if (!sender.pop(command, scratch, message)) break;
            admission_now = monotonic_ms();
            if (pending_expired(command.accepted_ms, admission_now, sender.max_pending_age_ms)) {
                const auto overdue = sender.expire_head(admission_now);
                if (overdue) {
                    const Result<SendReceipt> expired_result{Status{ErrorCode::backlog_timeout}};
                    note_sender_command_result(*peer.session, *overdue, expired_result);
                    report_error(peer.name, expired_result.status());
                }
                continue;
            }
            Result<SendReceipt> accepted{Status{ErrorCode::engine_stopped}};
            const bool active_callback_command = command.dependency == 0 && command.generation != 0;
            if (!sender.is_active() && command.dependency == 0 && !active_callback_command) {
                accepted = Status{ErrorCode::not_ready};
            } else if (active_callback_command && command.generation != peer.generation) {
                accepted = Status{ErrorCode::connection_closed};
            } else if (command.dependency == 0) {
                accepted = peer.driver->send(message, admission_now,
                    timestamp_view(wall, peer.settings.timestamp_precision));
            } else {
                const bool recovery = command.recovery || command.generation != peer.generation;
                accepted = peer.driver->send_dependent(message, admission_now,
                    timestamp_view(wall, peer.settings.timestamp_precision),
                    command.dependency, command.dependency_ordinal, recovery);
            }
            if (!accepted.ok() && accepted.status().code == ErrorCode::queue_full) {
                sender.block_commands();
                break;
            }
            note_sender_command_result(*peer.session, command, accepted);
            sender.complete(command.id, accepted);
            sender.finish_command(command);
            if (!accepted.ok()) report_error(peer.name, accepted.status());
        }
        sender.sync_receipts(*peer.driver);
    }

    void process_producer_lanes(Peer& peer, std::uint64_t,
        std::size_t command_budget = 32) noexcept {
        if (command_budget == 0 || !peer.session || !peer.driver || peer.closed) return;
        auto& sender = *peer.session->sender;
        if (sender.producer_lanes.empty() || !sender.is_active()) return;
        auto scratch = sender_scratch();
        if (scratch.empty()) return;

        const auto lane_count = sender.producer_lanes.size();
        auto& cursor = sender.producer_lane_cursor;
        cursor %= lane_count;
        std::size_t processed = 0;
        std::size_t empty_lanes = 0;
        const auto wall = wall_timestamp();
        while (processed < command_budget && empty_lanes < lane_count) {
            const auto lane_index = cursor;
            cursor = (cursor + 1) % lane_count;
            const auto& lane = sender.producer_lanes[lane_index];
            if (!lane) {
                ++empty_lanes;
                continue;
            }
            auto admission_now = monotonic_ms();
            const auto expired = lane->expire_head(admission_now);
            if (expired) {
                empty_lanes = 0;
                ++processed;
                report_error(peer.name, Status{ErrorCode::backlog_timeout});
                continue;
            }
            ProducerLaneCommandSlot command{};
            MessageView message{};
            if (!lane->peek(command, scratch, message)) {
                ++empty_lanes;
                continue;
            }
            empty_lanes = 0;
            admission_now = monotonic_ms();
            if (pending_expired(command.accepted_ms, admission_now, lane->max_pending_age_ms)) {
                if (lane->expire_head(admission_now)) report_error(peer.name, Status{ErrorCode::backlog_timeout});
                ++processed;
                continue;
            }
            auto accepted = peer.driver->send(message, admission_now,
                timestamp_view(wall, peer.settings.timestamp_precision));
            if (!accepted.ok() && accepted.status().code == ErrorCode::queue_full) {
                sender.block_commands();
                break;
            }
            const auto send_status = accepted.status();
            const bool send_ok = accepted.ok();
            lane->finish(command, std::move(accepted));
            ++processed;
            if (!send_ok) report_error(peer.name, send_status);
        }
        for (const auto& lane : sender.producer_lanes)
            if (lane) lane->sync_receipts(*peer.driver);
    }

    void collect_worker_completions() noexcept {
        if (!queued) return;
        DeliveryCompletion completion{};
        while (pop_worker_completion(completion)) {
            if (!completion.session) continue;
            if (completion.session->pending_completion) {
                worker_failure.store(true, std::memory_order_release);
                stopping.store(true, std::memory_order_release);
                continue;
            }
            completion.session->pending_completion = std::move(completion);
        }
        if (worker_failure.load(std::memory_order_acquire)) {
            fatal_status = Status{ErrorCode::callback_failed};
            observer_failed = true;
        }
    }

    [[nodiscard]] Peer* active_peer_for(const std::shared_ptr<SessionControl>& session) noexcept {
        for (auto& peer : peers) {
            if (!peer->closed && peer->session == session) return peer.get();
        }
        return nullptr;
    }

    [[nodiscard]] bool finish_worker_completions(std::uint64_t now) noexcept {
        if (!queued) return false;
        bool completed_any = false;
        const auto wall = wall_timestamp();
        for (const auto& session : sessions) {
            if (!session || !session->pending_completion) continue;
            const auto& completion = *session->pending_completion;
            auto* peer = active_peer_for(session);
            const bool same_delivery = peer != nullptr && peer->generation == completion.generation &&
                peer->driver && (completion.active_callback
                    ? peer->deferred_delivery_id == 0 &&
                        session->state.load(std::memory_order_acquire) == SessionState::active
                    : peer->deferred_delivery_id == completion.info.record_id);
            if (same_delivery) {
                if (session->callback_commands_processed < completion.accepted_sends) continue;
                Status completion_status = completion.status;
                if (session->callback_commands_processed != completion.accepted_sends && completion_status.ok())
                    completion_status = Status{ErrorCode::protocol_error};
                if (!session->callback_send_failure.ok()) completion_status = session->callback_send_failure;
                if (session->callback_replies_appended != completion.accepted_sends && completion_status.ok())
                    completion_status = Status{ErrorCode::callback_failed};
                Status status{};
                if (completion.active_callback) {
                    status = peer->driver->complete_active(completion_status,
                        now, timestamp_view(wall));
                } else {
                    peer->deferred_delivery_id = 0;
                    status = peer->driver->complete_delivery(completion.info,
                        completion_status, completion.accepted_sends, now, timestamp_view(wall));
                }
                if (!status.ok()) {
                    const auto driver_status = peer->driver->status();
                    if (driver_status.code != status.code || driver_status.tag != status.tag ||
                        driver_status.offset != status.offset) report_error(peer->name, status);
                    drop_peer(*peer, status, now, false);
                }
            } else {
                // The callback finished after its connection generation was
                // discarded. Its journal record stays incomplete and will be
                // redelivered after reconnect; never commit its old marker.
                report_error(session->name, Status{ErrorCode::connection_closed});
            }
            session->pending_completion.reset();
            session->callback_record_id = 0;
            session->callback_generation = 0;
            session->callback_commands_processed = 0;
            session->callback_replies_appended = 0;
            session->callback_send_failure = Status::success();
            session->callback_in_flight.store(false, std::memory_order_release);
            completed_any = true;
        }
        return completed_any;
    }

    [[nodiscard]] bool workers_idle() const noexcept {
        if (!queued) return true;
        std::lock_guard lock(worker_mutex);
        return worker_ready_count == 0 && worker_payload_bytes == 0 &&
            worker_completion_count == 0;
    }

    static constexpr std::string_view error_operation(Status status) noexcept {
        switch (status.code) {
        case ErrorCode::storage_full: case ErrorCode::corrupt_store: case ErrorCode::store_locked:
        case ErrorCode::identity_mismatch: case ErrorCode::unsupported_version: return "journal";
        case ErrorCode::tls_error: case ErrorCode::certificate_error: case ErrorCode::tls_handshake_timeout: return "tls";
        case ErrorCode::authentication_failed: return "authenticate";
        case ErrorCode::callback_failed: case ErrorCode::message_unhandled: return "deliver";
        case ErrorCode::connection_closed: case ErrorCode::write_stalled: case ErrorCode::io_error: return "transport";
        case ErrorCode::history_unavailable: return "replay";
        case ErrorCode::schema_mismatch: return "schema";
        case ErrorCode::invalid_config: return "configure";
        default: return "session";
        }
    }

    struct DiagnosticRecord {
        std::array<char, 129> name{};
        std::size_t name_size{};
        Status status{};
    };

    [[nodiscard]] Status start_diagnostics() noexcept {
        if (!error_callback) return Status::success();
        try {
            diagnostic_records.resize(config.diagnostic_capacity);
            diagnostic_thread = std::thread([this] {
                while (true) {
                    DiagnosticRecord record;
                    {
                        std::unique_lock lock(diagnostic_mutex);
                        diagnostic_cv.wait(lock, [this] { return diagnostic_stopping || diagnostic_count != 0; });
                        if (diagnostic_count == 0 && diagnostic_stopping) break;
                        record = diagnostic_records[diagnostic_head];
                        diagnostic_head = (diagnostic_head + 1) % diagnostic_records.size();
                        --diagnostic_count;
                    }
                    try {
                        error_callback(ErrorEvent{std::string{record.name.data(), record.name_size}, record.status, error_operation(record.status)});
                    } catch (...) {
                        diagnostic_failure.store(true, std::memory_order_release);
                        stopping.store(true, std::memory_order_release);
                        wake();
                        break;
                    }
                }
            });
            return Status::success();
        } catch (...) { return {ErrorCode::capacity_exceeded}; }
    }

    void stop_diagnostics() noexcept {
        {
            std::lock_guard lock(diagnostic_mutex);
            diagnostic_stopping = true;
        }
        diagnostic_cv.notify_one();
        if (diagnostic_thread.joinable()) diagnostic_thread.join();
    }

    void report_error(std::string_view name, Status status) noexcept {
        if (status.ok()) return;
        for (const auto& session : sessions) if (session && session->name == name) {
            session->last_error_tag.store(status.tag, std::memory_order_relaxed);
            session->last_error_offset.store(status.offset, std::memory_order_relaxed);
            session->last_error.store(status.code, std::memory_order_release);
            break;
        }
        if (!error_callback) return;
        {
            std::lock_guard lock(diagnostic_mutex);
            if (diagnostic_records.empty() || diagnostic_count == diagnostic_records.size()) {
                diagnostics_dropped.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            auto& record = diagnostic_records[diagnostic_tail];
            record.name_size = std::min(name.size(), record.name.size() - 1);
            std::copy_n(name.data(), record.name_size, record.name.data());
            record.status = status;
            diagnostic_tail = (diagnostic_tail + 1) % diagnostic_records.size();
            ++diagnostic_count;
        }
        diagnostic_cv.notify_one();
    }

    void publish_snapshots() noexcept {
        for (const auto& peer : peers) if (peer->session && peer->driver) {
            auto& session = *peer->session;
            const auto& driver = *peer->driver;
            session.next_inbound_seq.store(driver.next_inbound_seq(), std::memory_order_relaxed);
            session.next_outbound_seq.store(driver.next_outbound_seq(), std::memory_order_relaxed);
            session.pending_messages.store(driver.pending_messages(), std::memory_order_relaxed);
            session.pending_bytes.store(driver.pending_bytes(), std::memory_order_relaxed);
            session.pending_inbound_messages.store(driver.pending_inbound_messages(), std::memory_order_relaxed);
            session.pending_inbound_bytes.store(driver.pending_inbound_bytes(), std::memory_order_relaxed);
            session.read_paused.store(!driver.can_receive(), std::memory_order_relaxed);
        }
    }

    SessionConfig make_session_config(const Peer& peer, SessionRole role) const {
        const auto key = stable_session_key(peer.local_comp_id, peer.remote_comp_id, peer.settings);
        SessionConfig session;
        session.role = role;
        session.name = peer.name;
        session.storage_directory = config.storage.directory.string();
        session.session_key = key;
        session.local_comp_id = peer.local_comp_id;
        session.remote_comp_id = peer.remote_comp_id;
        session.heartbeat_ms = config.heartbeat_ms;
        session.logon_timeout_ms = config.logon_timeout_ms;
        session.write_stall_ms = config.write_stall_ms;
        session.logout_timeout_ms = config.logout_timeout_ms;
        session.max_pending_age_ms = config.max_pending_age_ms;
        session.max_frame_bytes = config.max_frame_bytes;
        session.max_pending_messages = config.max_pending_messages;
        session.max_pending_bytes = config.max_pending_bytes;
        session.admin_reserve_bytes = config.max_frame_bytes * 2;
        session.max_pending_inbound_messages = config.max_pending_inbound_messages;
        session.max_pending_inbound_bytes = config.max_pending_inbound_bytes;
        session.receipt_capacity = config.sender_receipt_capacity;
        session.journal_capacity_bytes = config.storage.journal_capacity_bytes;
        session.journal_max_records = config.storage.journal_max_records;
        session.journal_segment_bytes = config.storage.segment_bytes;
        session.journal_max_segments = config.storage.max_segments;
        session.retain_outbound_messages = config.storage.retain_outbound_messages;
        session.settings = peer.settings;
        session.tls_verified = peer.tls && peer.tls->handshake_complete();
        session.logon_preauthenticated = role == SessionRole::acceptor;
        if (peer.settings.flow_control) {
            const auto& flow = *peer.settings.flow_control;
            session.max_pending_messages = flow.max_pending_messages;
            session.max_pending_bytes = flow.max_pending_bytes;
            session.max_pending_inbound_messages = flow.max_pending_inbound_messages;
            session.max_pending_inbound_bytes = flow.max_pending_inbound_bytes;
            session.max_pending_age_ms = flow.max_pending_age_ms;
            session.write_stall_ms = flow.write_stall_ms;
        }
        return session;
    }

    [[nodiscard]] Status open_driver(Peer& peer, SessionRole role) noexcept {
        try {
            SessionCallbacks callbacks;
            callbacks.deferred_delivery = queued;
            {
                callbacks.validate_message = [this](const FlatMessageView& message) {
                    std::string_view msg_type{};
                    std::size_t msg_type_offset = 0;
                    if (schema != nullptr) {
                        const auto fields = message.fields();
                        if (fields.size() >= 3 && fields[2].tag == 35) {
                            msg_type = fields[2].value;
                            msg_type_offset = fields[2].offset;
                        } else {
                            const auto type = message.find_unique(35);
                            if (!type.ok()) return type.status();
                            msg_type = type.value().value;
                            msg_type_offset = type.value().offset;
                        }
                        const auto schema_status = validate_schema_message_for_type(
                            *schema, msg_type, msg_type_offset, fields, ValidationLevel::strict,
                            message.parsed_index());
                        if (!schema_status.ok()) return schema_status;
                    }
                    if (config.message_validator) {
                        const auto checked = config.message_validator(message);
                        if (!checked.ok()) return checked;
                    }
                    if (schema != nullptr) {
                        const auto h = std::lower_bound(typed_handlers.begin(), typed_handlers.end(),
                            msg_type, [](const RegisteredMessageHandler& item, std::string_view value) {
                                return item.msg_type < value;
                            });
                        const bool handled = h != typed_handlers.end() && h->msg_type == msg_type;
                        if (!handled) {
                            const auto ig = std::lower_bound(ignored_messages.begin(), ignored_messages.end(),
                                msg_type, [](const IgnoredMessageType& item, std::string_view value) {
                                    return item.msg_type < value;
                                });
                            const bool ignored = ig != ignored_messages.end() && ig->msg_type == msg_type;
                            if (!ignored) return Status{ErrorCode::message_unhandled, 35};
                        }
                    }
                    return Status::success();
                };
            }
            callbacks.wire_hints = wire_hints;
            callbacks.deferred_active = queued && static_cast<bool>(active_callback);
            if (callbacks.deferred_active) {
                callbacks.on_deferred_active = [this, peer_ptr = &peer]() -> Status {
                    if (!peer_ptr->session || !peer_ptr->driver)
                        return Status{ErrorCode::engine_stopped};
                    auto& session = *peer_ptr->session;
                    const auto generation = peer_ptr->generation;
                    const auto deferred = peer_ptr->driver->defer_active();
                    if (!deferred.ok()) return deferred;
                    session.state.store(SessionState::active, std::memory_order_release);
                    session.sender->set_active(true);
                    session.active_callback_pending = true;
                    session.active_pending_generation = generation;
                    const auto scheduled = schedule_active_callback(peer_ptr->session, generation);
                    if (scheduled.ok() || scheduled.code == ErrorCode::queue_full)
                        return Status::success();
                    session.active_callback_pending = false;
                    session.sender->set_active(false);
                    return scheduled;
                };
            }
            if (queued) {
                callbacks.on_deferred_message = [this, peer_ptr = &peer](const DeliveryLease& lease) {
                    if (!peer_ptr->session) return Status{ErrorCode::engine_stopped};
                    const auto generation = peer_ptr->generation;
                    peer_ptr->session->callback_record_id = lease.info.record_id;
                    peer_ptr->session->callback_generation = generation;
                    peer_ptr->session->callback_commands_processed = 0;
                    peer_ptr->session->callback_replies_appended = 0;
                    peer_ptr->session->callback_send_failure = Status::success();
                    const auto queued_status = enqueue_delivery(lease, peer_ptr->session, generation);
                    if (!queued_status.ok()) {
                        peer_ptr->session->callback_record_id = 0;
                        return queued_status;
                    }
                    peer_ptr->session->callback_started_ms = monotonic_ms();
                    peer_ptr->deferred_delivery_id = lease.info.record_id;
                    peer_ptr->dependent_reply_count = 0;
                    peer_ptr->dependent_commands_processed = 0;
                    peer_ptr->dependent_send_failure = Status::success();
                    const auto deferred = peer_ptr->driver
                        ? peer_ptr->driver->defer_delivery(lease.info)
                        : Status{ErrorCode::engine_stopped};
                    return deferred;
                };
            } else {
                callbacks.on_message = [this, peer_ptr = &peer](SessionContext& context,
                    const FlatMessageView& message, const DeliveryInfo& info) -> Status {
                    const auto delivered = dispatch_business(context, message, info);
                    if (delivered.ok() && peer_ptr->session)
                        peer_ptr->session->business_deliveries.fetch_add(1, std::memory_order_relaxed);
                    return delivered;
                };
            }
            callbacks.on_active = [this, peer_ptr = &peer](SessionContext& context) -> Status {
                if (peer_ptr->session) {
                    peer_ptr->session->sender->set_active(true);
                    peer_ptr->session->state.store(SessionState::active, std::memory_order_release);
                }
                if (!active_callback || queued) return Status::success();
                return active_callback(context);
            };
            callbacks.on_error = [this, peer_ptr = &peer](Status status) {
                if (peer_ptr->session && peer_ptr->driver) {
                    peer_ptr->session->state.store(peer_ptr->driver->state(), std::memory_order_release);
                    if (status.code == ErrorCode::io_error && peer_ptr->driver->state() == SessionState::failed)
                        peer_ptr->disabled = true;
                }
                report_error(peer_ptr->name, status);
            };
            auto opened = SessionDriver::create(make_session_config(peer, role), std::move(callbacks));
            if (!opened.ok()) return opened.status();
            peer.driver.emplace(std::move(opened).value());
            if (peer.session) {
                peer.generation = peer.session->generation.fetch_add(1, std::memory_order_acq_rel) + 1;
                peer.session->state.store(peer.driver->state(), std::memory_order_release);
                peer.session->sender->set_active(false);
            }
            return Status::success();
        } catch (...) {
            return {ErrorCode::capacity_exceeded};
        }
    }

    [[nodiscard]] Status start_initiators() noexcept {
        try {
            peers.reserve(peers.size() + initiators.size());
            for (std::size_t i = 0; i < initiators.size(); ++i) {
                const auto& declaration = initiators[i];
                auto peer = std::make_unique<Peer>();
                peer->initiator = true;
                peer->name = declaration.name;
                peer->local_comp_id = declaration.sender_comp_id;
                peer->remote_comp_id = declaration.target_comp_id;
                peer->session = initiator_sessions[i];
                peer->endpoint = declaration.endpoint;
                peer->settings = declaration.settings;
                peer->tls_config = declaration.tls;
                if (declaration.tls.enabled) {
                    auto context = TlsContext::create(declaration.tls, false);
                    if (!context.ok()) return context.status();
                    peer->tls_context = std::move(context).value();
                }
                peer->receive_buffer.reserve(config.max_frame_bytes + read_chunk_size);
                auto opened = open_driver(*peer, SessionRole::initiator);
                if (!opened.ok()) {
                    report_error(peer->name, opened);
                    peer->disabled = true;
                    peer->session->state.store(SessionState::failed, std::memory_order_release);
                }
                peers.push_back(std::move(peer));
            }
            return Status::success();
        } catch (...) {
            return {ErrorCode::capacity_exceeded};
        }
    }

    [[nodiscard]] Status bind_listeners() noexcept {
        try {
        for (auto& listener : listeners) {
            if (listener.config.tls.enabled) {
                auto context = TlsContext::create(listener.config.tls, true);
                if (!context.ok()) return context.status();
                listener.tls_context = std::move(context).value();
            }
            auto bound = TcpListener::bind(listener.config.endpoint.host,
                listener.config.endpoint.port, static_cast<int>(listener.config.backlog));
            if (!bound.ok()) return bound.status();
            listener.socket.emplace(std::move(bound).value());
            auto endpoint = listener.socket->local_endpoint();
            if (!endpoint.ok()) return endpoint.status();
            {
                std::lock_guard lock(endpoint_mutex);
                listener.published = TcpEndpoint{endpoint.value().host, endpoint.value().port};
            }
        }
        return Status::success();
        } catch (...) {
            return {ErrorCode::capacity_exceeded};
        }
    }

    [[nodiscard]] Status create_storage_directory() noexcept {
        try {
            std::error_code error;
            std::filesystem::create_directories(config.storage.directory, error);
            if (error) return {ErrorCode::io_error, 0, static_cast<std::size_t>(error.value())};
            return Status::success();
        } catch (...) {
            return {ErrorCode::io_error};
        }
    }

    [[nodiscard]] Status begin_connect(Peer& peer, std::uint64_t now) noexcept {
        if (!peer.initiator || peer.connection != Connection::disconnected ||
            peer.closed || peer.disabled || now < peer.reconnect_at) return Status::success();
        if (!peer.settings.schedule.contains(std::chrono::system_clock::now())) {
            peer.reconnect_at = now + 1000;
            return Status::success();
        }
        if (!peer.driver) {
            const auto opened = open_driver(peer, SessionRole::initiator);
            if (!opened.ok()) {
                report_error(peer.name, opened);
                peer.disabled = true;
                if (peer.session) peer.session->state.store(SessionState::failed, std::memory_order_release);
                return opened;
            }
            now = monotonic_ms();
        }
        bool in_progress = false;
        auto connected = TcpStream::connect(peer.endpoint.host, peer.endpoint.port, in_progress);
        if (!connected.ok()) {
            report_error(peer.name, connected.status());
            peer.reconnect_at = now + config.reconnect_delay_ms;
            return connected.status();
        }
        peer.clear_receive();
        peer.stream = std::move(connected).value();
        peer.connection = in_progress ? Connection::connecting : Connection::connected;
        peer.connect_deadline = now + config.connect_timeout_ms;
        if (!in_progress) return connected_peer(peer, now);
        return Status::success();
    }

    [[nodiscard]] Status connected_peer(Peer& peer, std::uint64_t now) noexcept {
        if (peer.tls_config.enabled && !peer.tls) {
            auto stream = TlsStream::create(peer.tls_context, peer.stream.native_handle(),
                peer.tls_config.server_name.empty() ? peer.endpoint.host : peer.tls_config.server_name);
            if (!stream.ok()) {
                drop_peer(peer, stream.status(), now, true);
                return stream.status();
            }
            peer.tls.emplace(std::move(stream).value());
            peer.handshake_deadline = now + peer.tls_config.handshake_timeout_ms;
        }
        if (peer.tls && !peer.tls->handshake_complete()) {
            peer.connection = Connection::handshaking;
            const auto progress = peer.tls->handshake();
            if (!progress.status.ok() || progress.peer_closed) {
                const auto failure = progress.status.ok() ? Status{ErrorCode::tls_error} : progress.status;
                drop_peer(peer, failure, now, true);
                return failure;
            }
            if (!peer.tls->handshake_complete()) return Status::success();
        }
        peer.connection = Connection::connected;
        peer.schedule_logout_sent = false;
        peer.read_pause_since = 0;
        const auto wall = wall_timestamp();
        const auto status = peer.driver
            ? peer.driver->connected(now, timestamp_view(wall, peer.settings.timestamp_precision))
            : Status{ErrorCode::invalid_config};
        if (!status.ok()) {
            drop_peer(peer, status, now, false);
            return status;
        }
        peer.driver_connected = true;
        if (peer.session) {
            const auto state = peer.driver->state();
            peer.session->state.store(state, std::memory_order_release);
            if (state != SessionState::active) peer.session->sender->set_active(false);
        }
        return Status::success();
    }

    void drop_peer(Peer& peer, Status reason, std::uint64_t now,
                   bool report) noexcept {
        const bool graceful_eof = reason.code == ErrorCode::connection_closed &&
            peer.driver && peer.driver->state() == SessionState::closed;
        if (shutdown_started && !reason.ok() && reason.code != ErrorCode::engine_stopped &&
            !graceful_eof) {
            if (shutdown_status.ok()) shutdown_status = reason;
            // A raw EOF before the Logout acknowledgement has no driver
            // error notification. Transport write errors already notified.
            if (reason.code == ErrorCode::connection_closed && peer.driver &&
                peer.driver->status().ok()) report = true;
        }
        io_remove(peer.io_tag);
        cancel_peer_proactor(peer);
        if (peer.tls) {
            if (graceful_eof || shutdown_started ||
                (peer.driver && peer.driver->state() == SessionState::closed)) {
                peer.tls->close();
            }
            peer.tls.reset();
        }
        peer.stream.close();
        if (peer.session) peer.session->sender->set_active(false);
        const bool was_connected = peer.connection == Connection::connected;
        peer.connection = Connection::disconnected;
        peer.clear_receive();
        if (was_connected && peer.driver && peer.driver_connected) {
            const auto disconnected = peer.driver->disconnected(
                Status{ErrorCode::connection_closed}, now);
            if (!disconnected.ok() && report) report_error(peer.name, disconnected);
        }
        if (peer.session) peer.session->state.store(peer.driver
            ? peer.driver->state() : SessionState::disconnected, std::memory_order_release);
        peer.driver_connected = false;
        peer.deferred_delivery_id = 0;
        peer.dependent_reply_count = 0;
        peer.dependent_commands_processed = 0;
        peer.dependent_send_failure = Status::success();
        if (peer.session && peer.driver) {
            peer.session->sender->sync_receipts(*peer.driver);
            if (!peer.session->sender->producer_lanes.empty())
                for (const auto& lane : peer.session->sender->producer_lanes)
                    if (lane) lane->sync_receipts(*peer.driver);
        }
        // Reopen the journal from its committed sequence state on reconnect.
        // This also discards a failed driver after a protocol/callback error.
        peer.driver.reset();
        if (report) report_error(peer.name, reason);
        const bool session_fatal = reason.code == ErrorCode::corrupt_store || reason.code == ErrorCode::storage_full ||
            reason.code == ErrorCode::callback_failed || reason.code == ErrorCode::certificate_error ||
            reason.code == ErrorCode::authentication_failed || reason.code == ErrorCode::schema_mismatch ||
            reason.code == ErrorCode::history_unavailable || reason.code == ErrorCode::sequence_reset_denied;
        if ((session_fatal || peer.disabled) && peer.session) {
            peer.disabled = true;
            peer.session->state.store(SessionState::failed, std::memory_order_release);
        }
        if (peer.initiator) {
            peer.reconnect_at = now + config.reconnect_delay_ms;
        } else {
            peer.closed = true;
            if (peer.acceptor_index < acceptors.size() &&
                acceptors[peer.acceptor_index].active == &peer)
                acceptors[peer.acceptor_index].active = nullptr;
        }
    }

    [[nodiscard]] Status start_accepts(Listener& listener, std::size_t listener_index,
                                      std::uint64_t now) noexcept {
        if (listener.accept_backoff_until != 0 && now < listener.accept_backoff_until)
            return Status::success();
        listener.accept_backoff_until = 0;
        for (std::size_t count = 0; count < 16; ++count) {
            auto accepted = listener.socket->accept();
            if (!accepted.ok()) {
                if (accepted.status().code == ErrorCode::need_more) return Status::success();
                if (accepted.status().code == ErrorCode::io_error &&
                    (accepted.status().offset == EMFILE || accepted.status().offset == ENFILE)) {
                    // Back off briefly under fd exhaustion instead of spinning 100% CPU.
                    listener.accept_backoff_until = now + 50;
                }
                report_error(listener.config.name, accepted.status());
                return accepted.status();
            }
            if (pending.size() >= config.max_pending_connections)
                continue; // The RAII accepted stream closes here.
            try {
                auto connection = std::make_unique<Pending>();
                connection->listener_index = listener_index;
                connection->stream = std::move(accepted).value();
                connection->deadline = now + config.logon_timeout_ms;
                if (listener.config.tls.enabled) {
                    auto stream = TlsStream::create(listener.tls_context, connection->stream.native_handle());
                    if (!stream.ok()) { report_error({}, stream.status()); continue; }
                    connection->tls.emplace(std::move(stream).value());
                    connection->handshaking = true;
                    connection->deadline = now + listener.config.tls.handshake_timeout_ms;
                }
                connection->receive_buffer.reserve(config.max_frame_bytes + read_chunk_size);
                pending.push_back(std::move(connection));
            } catch (...) {
                return {ErrorCode::capacity_exceeded};
            }
        }
        return Status::success();
    }

    [[nodiscard]] std::size_t match_acceptor(std::size_t listener_index,
        std::string_view sender, std::string_view target, FixVersion version) const noexcept {
        for (std::size_t i = 0; i < acceptors.size(); ++i) {
            const auto& acceptor = acceptors[i];
            if (acceptor.listener_index == listener_index &&
                acceptor.config.settings.version == version &&
                acceptor.config.remote_comp_id == sender &&
                acceptor.config.local_comp_id == target) return i;
        }
        return no_index;
    }

    [[nodiscard]] Status adopt_pending(Pending& source, FrameView first_frame,
                                       std::uint64_t now) noexcept {
        Status frame_status{};
        std::string error_name;
        bool identity_rejected = false;
        bool driver_registered = false;
        try {
        const std::string first_wire{first_frame.bytes};
        frame_status = [&]() -> Status {
            const auto scratch_size = field_scratch.size();
            FlatMessageView message;
            auto status = parse_flat(first_frame,
                std::span<FieldView>{field_scratch.data(), scratch_size}, message,
                wire_hints, CodecLimits{config.max_frame_bytes});
            if (!status.ok()) return status;
            std::string_view type, sender, target;
            if (!(status = field_value(message, 35, type)).ok() ||
                !(status = field_value(message, 49, sender)).ok() ||
                !(status = field_value(message, 56, target)).ok()) return status;
            if (type != "A") return {ErrorCode::protocol_error, 35};
            const auto acceptor_index = match_acceptor(source.listener_index, sender, target, first_frame.version);
            if (acceptor_index == no_index) {
                identity_rejected = true;
                return {ErrorCode::protocol_error, 49};
            }
            auto& acceptor = acceptors[acceptor_index];
            error_name = acceptor.config.name;
            if (!acceptor.config.settings.schedule.contains(std::chrono::system_clock::now()))
                return {ErrorCode::schedule_closed};
            const auto authenticated = authenticate_logon(acceptor.config.settings.authentication,
                message, source.tls && source.tls->handshake_complete());
            if (!authenticated.ok()) return authenticated;
            if (acceptor.session->state.load(std::memory_order_acquire) == SessionState::failed)
                return {ErrorCode::not_ready};
            if (acceptor.active != nullptr && !acceptor.active->closed) {
                identity_rejected = true;
                return {ErrorCode::protocol_error, 49};
            }

            auto peer = std::make_unique<Peer>();
            peer->name = acceptor.config.name;
            peer->local_comp_id = acceptor.config.local_comp_id;
            peer->remote_comp_id = acceptor.config.remote_comp_id;
            peer->session = acceptor.session;
            peer->acceptor_index = acceptor_index;
            peer->settings = acceptor.config.settings;
            peer->tls = std::move(source.tls);
            io_remove(source.io_tag);
            peer->stream = std::move(source.stream);
            peer->receive_buffer = std::move(source.receive_buffer);
            peer->receive_head = first_frame.consumed;
            if (peer->receive_head >= peer->receive_buffer.size()) {
                peer->receive_buffer.clear();
                peer->receive_head = 0;
            }
            peer->receive_buffer.reserve(config.max_frame_bytes + read_chunk_size);
            auto opened = open_driver(*peer, SessionRole::acceptor);
            if (!opened.ok()) return opened;
            now = monotonic_ms();
            peers.push_back(std::move(peer));
            auto& stored = *peers.back();
            driver_registered = true;
            acceptor.active = &stored;
            const auto connected_status = connected_peer(stored, now);
            if (!connected_status.ok()) return connected_status;
            const auto wall = wall_timestamp();
            const auto received = stored.driver->receive(first_wire, now,
                timestamp_view(wall, stored.settings.timestamp_precision));
            if (!received.ok()) {
                drop_peer(stored, received, now, false);
                return received;
            }
            const auto processed = process_peer_buffer(stored, now);
            if (!processed.ok() || stored.closed) return processed;
            if (stored.driver && stored.driver->needs_write()) {
                (void)write_peer(stored, now);
            }
            return Status::success();
        }();
        } catch (...) {
            frame_status = {ErrorCode::capacity_exceeded};
        }
        close_pending(source);
        if (!frame_status.ok()) {
            // A rejected connection is isolated. Unknown identities do not
            // acquire a driver, journal, or sequence state.
            if (!identity_rejected && !driver_registered)
                report_error(error_name, frame_status);
        }
        return frame_status;
    }

    [[nodiscard]] Status process_peer_buffer(Peer& peer, std::uint64_t now) noexcept {
        std::size_t processed = 0;
        while (!peer.closed && peer.connection == Connection::connected &&
               peer.unread_size() != 0 && processed < frame_budget && peer.driver->can_receive()) {
            FrameView frame{};
            const auto status = inspect_frame(peer.unread_view(), frame, {},
                CodecLimits{config.max_frame_bytes, peer.settings.verify_checksum});
            if (status.code == ErrorCode::need_more) return Status::success();
            if (!status.ok()) {
                drop_peer(peer, status, now, true);
                return status;
            }
            const auto wall = wall_timestamp();
            const auto received = peer.driver->receive_verified(frame, now,
                timestamp_view(wall, peer.settings.timestamp_precision));
            if (received.code == ErrorCode::queue_full) return Status::success();
            if (peer.session) {
                peer.session->received_frames.fetch_add(1, std::memory_order_relaxed);
                peer.session->received_bytes.fetch_add(frame.consumed, std::memory_order_relaxed);
            }
            peer.consume_receive(frame.consumed);
            ++processed;
            if (!received.ok()) {
                drop_peer(peer, received, now, false);
                return received;
            }
            if (peer.session) {
                const auto state = peer.driver->state();
                peer.session->state.store(state, std::memory_order_release);
                if (state != SessionState::active) peer.session->sender->set_active(false);
            }
        }
        return Status::success();
    }

    [[nodiscard]] bool has_complete_buffered_frame(Peer& peer) noexcept {
        if (peer.closed || peer.connection != Connection::connected ||
            peer.unread_size() == 0 || !peer.driver || !peer.driver->can_receive()) return false;
        FrameView frame{};
        return inspect_frame(peer.unread_view(), frame, wire_hints,
            CodecLimits{config.max_frame_bytes, peer.settings.verify_checksum}).ok();
    }

    [[nodiscard]] Status process_pending_buffer(Pending& connection,
                                               std::uint64_t now) noexcept {
        FrameView frame{};
        const auto status = inspect_frame(connection.receive_buffer, frame, wire_hints,
            CodecLimits{config.max_frame_bytes});
        if (status.code == ErrorCode::need_more) return Status::success();
        if (!status.ok()) {
            close_pending(connection);
            report_error({}, status);
            return status;
        }
        return adopt_pending(connection, frame, now);
    }

    [[nodiscard]] Status read_peer(Peer& peer, std::uint64_t now) noexcept {
        char scratch[read_chunk_size];
        std::size_t total = 0;
        while (!peer.closed && peer.driver && peer.driver->can_receive() && total < io_byte_budget) {
            const auto already_buffered = process_peer_buffer(peer, now);
            if (!already_buffered.ok() || peer.closed) return already_buffered;
            if (!peer.driver->can_receive()) break;
            if (has_complete_buffered_frame(peer)) break;
            if (peer.unread_size() >= config.max_frame_bytes) {
                drop_peer(peer, Status{ErrorCode::too_large}, now, true);
                return {ErrorCode::too_large};
            }
            const auto high_water = config.max_frame_bytes + read_chunk_size;
            if (peer.receive_buffer.size() >= high_water) peer.compact_receive();
            const auto available = high_water - peer.unread_size();
            const auto budget = io_byte_budget - total;
            const auto read_size = std::min({read_chunk_size, available, budget});
            if (read_size == 0) break;
            const auto io = peer.tls ? peer.tls->read(std::span<char>{scratch, read_size})
                : peer.stream.read(std::span<char>{scratch, read_size});
            if (!io.status.ok()) {
                drop_peer(peer, io.status, now, true);
                return io.status;
            }
            if (io.peer_closed) {
                drop_peer(peer, Status{ErrorCode::connection_closed}, now, false);
                return Status::success();
            }
            if (io.would_block) break;
            if (io.bytes == 0) break;
            total += io.bytes;
            try {
                peer.receive_buffer.append(scratch, io.bytes);
            } catch (...) {
                drop_peer(peer, Status{ErrorCode::capacity_exceeded}, now, true);
                return {ErrorCode::capacity_exceeded};
            }
            const auto processed = process_peer_buffer(peer, now);
            if (!processed.ok() || peer.closed) return processed;
            // Must check can_receive() before too_large: backpressure read-pause can leave
            // multiple complete frames buffered past max_frame_bytes without any single frame
            // exceeding max_frame_bytes.
            if (!peer.driver->can_receive()) break;
            if (has_complete_buffered_frame(peer)) break;
            if (peer.unread_size() >= config.max_frame_bytes) {
                drop_peer(peer, Status{ErrorCode::too_large}, now, true);
                return {ErrorCode::too_large};
            }
            // Short read on plaintext nonblocking TCP means the kernel receive buffer is drained;
            // exit immediately so replies flush before any extra EAGAIN recv() syscall.
            if (!peer.tls && io.bytes < read_size) break;
        }
        return Status::success();
    }

    [[nodiscard]] Status read_pending(Pending& connection,
                                      std::uint64_t now) noexcept {
        if (connection.handshaking) {
            const auto io = connection.tls->handshake();
            if (!io.status.ok() || io.peer_closed) {
                close_pending(connection);
                const auto status = io.status.ok() ? Status{ErrorCode::tls_error} : io.status;
                report_error({}, status);
                return status;
            }
            if (!connection.tls->handshake_complete()) return Status::success();
            connection.handshaking = false;
            connection.deadline = now + config.logon_timeout_ms;
        }
        std::array<char, read_chunk_size> scratch{};
        std::size_t total = 0;
        while (!connection.closed && total < io_byte_budget) {
            const auto high_water = config.max_frame_bytes + read_chunk_size;
            if (connection.receive_buffer.size() >= high_water) {
                close_pending(connection);
                report_error({}, Status{ErrorCode::too_large});
                return {ErrorCode::too_large};
            }
            const auto available = high_water - connection.receive_buffer.size();
            const auto budget = io_byte_budget - total;
            const auto read_size = std::min({scratch.size(), available, budget});
            if (read_size == 0) break;
            const auto io = connection.tls ? connection.tls->read(std::span<char>{scratch.data(), read_size})
                : connection.stream.read(std::span<char>{scratch.data(), read_size});
            if (!io.status.ok()) {
                close_pending(connection);
                report_error({}, io.status);
                return io.status;
            }
            if (io.peer_closed) {
                close_pending(connection);
                return Status::success();
            }
            if (io.would_block || io.bytes == 0) break;
            total += io.bytes;
            try {
                connection.receive_buffer.append(scratch.data(), io.bytes);
            } catch (...) {
                close_pending(connection);
                report_error({}, Status{ErrorCode::capacity_exceeded});
                return {ErrorCode::capacity_exceeded};
            }
            const auto processed = process_pending_buffer(connection, now);
            if (!processed.ok() || connection.closed) return processed;
            if (connection.receive_buffer.size() >= config.max_frame_bytes) {
                close_pending(connection);
                report_error({}, Status{ErrorCode::too_large});
                return {ErrorCode::too_large};
            }
        }
        return Status::success();
    }

    static WriteResult socket_writer(void* context, std::span<const char> bytes) noexcept {
        auto* peer = static_cast<Peer*>(context);
        const auto result = peer->tls ? peer->tls->write(bytes) : peer->stream.write(bytes);
        const auto status = result.peer_closed
            ? Status{ErrorCode::connection_closed} : result.status;
        if (result.bytes != 0 && peer->session)
            peer->session->written_bytes.fetch_add(result.bytes, std::memory_order_relaxed);
        return {result.bytes, result.would_block, status};
    }

    [[nodiscard]] Status write_peer(Peer& peer, std::uint64_t now) noexcept {
        if (peer.closed || peer.connection != Connection::connected || !peer.driver ||
            !peer.driver->needs_write()) return Status::success();
        if (io_backend && io_backend->supports_proactor() && !peer.tls) {
            return proactor_kick_send(peer, now);
        }
        const auto status = peer.driver->flush(&socket_writer, &peer, now, io_byte_budget);
        if (!status.ok()) {
            drop_peer(peer, status, now, false);
            return status;
        }
        if (peer.session) peer.session->sender->unblock_commands();
        return Status::success();
    }

    static WriteResult uring_buffer_writer(void* context, std::span<const char> bytes) noexcept {
        auto& peer = *static_cast<Peer*>(context);
        const std::size_t avail = peer.uring_tx_buf.size() - peer.uring_tx_size;
        if (avail == 0) return {0, true, Status::success()};
        const std::size_t n = std::min(avail, bytes.size());
        std::memcpy(peer.uring_tx_buf.data() + peer.uring_tx_size, bytes.data(), n);
        peer.uring_tx_size += n;
        if (peer.session) peer.session->written_bytes.fetch_add(n, std::memory_order_relaxed);
        return {n, n < bytes.size(), Status::success()};
    }

    void proactor_arm_recv(Peer& peer) noexcept {
        if (!io_backend || !io_backend->supports_proactor() || peer.closed ||
            peer.connection != Connection::connected || peer.tls || !peer.stream.valid() ||
            peer.uring_rx_in_flight || !peer.driver || !peer.driver->can_receive()) {
            return;
        }
        if (peer.unread_size() >= config.max_frame_bytes) return;
        if (peer.uring_rx_buf.empty()) {
            try {
                peer.uring_rx_buf.resize(read_chunk_size);
            } catch (...) {
                return;
            }
        }
        io_remove(peer.io_tag);
        peer.io_tag.pointer = &peer;
        const std::size_t high_water = config.max_frame_bytes + read_chunk_size;
        const std::size_t avail = high_water > peer.unread_size() ? high_water - peer.unread_size() : 0;
        const std::size_t want = std::min(peer.uring_rx_buf.size(), avail);
        if (want == 0) return;
        if (io_backend->submit_recv(peer.io_tag, peer.stream.native_handle(),
                std::span<char>{peer.uring_rx_buf.data(), want})) {
            peer.uring_rx_in_flight = true;
        }
    }

    [[nodiscard]] Status proactor_kick_send(Peer& peer, std::uint64_t now) noexcept {
        if (!io_backend || !io_backend->supports_proactor() || peer.closed ||
            peer.connection != Connection::connected || peer.tls || !peer.stream.valid() ||
            peer.uring_tx_in_flight || !peer.driver || !peer.driver->needs_write()) {
            return Status::success();
        }
        if (peer.uring_tx_buf.empty()) {
            try {
                peer.uring_tx_buf.resize(std::max<std::size_t>(io_byte_budget, config.max_frame_bytes));
            } catch (...) {
                drop_peer(peer, Status{ErrorCode::capacity_exceeded}, now, true);
                return {ErrorCode::capacity_exceeded};
            }
        }
        peer.uring_tx_size = 0;
        peer.uring_tx_offset = 0;
        const auto status = peer.driver->flush(&uring_buffer_writer, &peer, now, peer.uring_tx_buf.size());
        if (!status.ok()) {
            peer.uring_tx_size = 0;
            drop_peer(peer, status, now, false);
            return status;
        }
        if (peer.uring_tx_size == 0) return Status::success();
        peer.io_tag.pointer = &peer;
        if (io_backend->submit_send(peer.io_tag, peer.stream.native_handle(),
                std::span<const char>{peer.uring_tx_buf.data(), peer.uring_tx_size})) {
            peer.uring_tx_in_flight = true;
        }
        return Status::success();
    }

    void handle_proactor_recv(Peer& peer, std::int32_t res, std::uint64_t now) noexcept {
        peer.uring_rx_in_flight = false;
        if (peer.closed || peer.connection != Connection::connected || !peer.driver) return;
        if (res == 0) {
            drop_peer(peer, Status{ErrorCode::connection_closed}, now, false);
            return;
        }
        if (res < 0) {
            const int err = -res;
            if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR || err == ECANCELED) return;
            drop_peer(peer, Status{ErrorCode::io_error, 0, static_cast<std::size_t>(err)}, now, true);
            return;
        }
        const auto n = static_cast<std::size_t>(res);
        const std::size_t high_water = config.max_frame_bytes + read_chunk_size;
        if (peer.receive_buffer.size() >= high_water) peer.compact_receive();
        try {
            peer.receive_buffer.append(peer.uring_rx_buf.data(), n);
        } catch (...) {
            drop_peer(peer, Status{ErrorCode::capacity_exceeded}, now, true);
            return;
        }
        const auto processed = process_peer_buffer(peer, now);
        if (!processed.ok() || peer.closed) return;
        if (peer.driver && peer.driver->can_receive() &&
            !has_complete_buffered_frame(peer) && peer.unread_size() >= config.max_frame_bytes) {
            drop_peer(peer, Status{ErrorCode::too_large}, now, true);
            return;
        }
        if (!peer.closed && peer.driver && peer.driver->needs_write()) {
            (void)proactor_kick_send(peer, now);
        }
        if (!peer.closed && peer.driver && peer.driver->can_receive() &&
            !has_complete_buffered_frame(peer)) {
            proactor_arm_recv(peer);
        }
    }

    void handle_proactor_send(Peer& peer, std::int32_t res, std::uint64_t now) noexcept {
        peer.uring_tx_in_flight = false;
        if (peer.closed || peer.connection != Connection::connected || !peer.driver) return;
        if (res < 0) {
            const int err = -res;
            if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR) {
                if (peer.uring_tx_offset < peer.uring_tx_size && peer.stream.valid()) {
                    peer.io_tag.pointer = &peer;
                    if (io_backend->submit_send(peer.io_tag, peer.stream.native_handle(),
                            std::span<const char>{peer.uring_tx_buf.data() + peer.uring_tx_offset,
                                                  peer.uring_tx_size - peer.uring_tx_offset})) {
                        peer.uring_tx_in_flight = true;
                    }
                }
                return;
            }
            if (err == ECANCELED) return;
            drop_peer(peer, Status{ErrorCode::io_error, 0, static_cast<std::size_t>(err)}, now, true);
            return;
        }
        peer.uring_tx_offset += static_cast<std::size_t>(res);
        if (peer.uring_tx_offset < peer.uring_tx_size) {
            if (peer.stream.valid()) {
                peer.io_tag.pointer = &peer;
                if (io_backend->submit_send(peer.io_tag, peer.stream.native_handle(),
                        std::span<const char>{peer.uring_tx_buf.data() + peer.uring_tx_offset,
                                              peer.uring_tx_size - peer.uring_tx_offset})) {
                    peer.uring_tx_in_flight = true;
                }
            }
            return;
        }
        peer.uring_tx_size = 0;
        peer.uring_tx_offset = 0;
        if (peer.session) peer.session->sender->unblock_commands();
        if (peer.driver->needs_write()) {
            (void)proactor_kick_send(peer, now);
        }
    }

    void tick_sessions(std::uint64_t now) noexcept {
        const auto wall = wall_timestamp();
        for (auto& peer_ptr : peers) {
            auto& peer = *peer_ptr;
            if (peer.closed) continue;
            if (peer.connection == Connection::connecting && now >= peer.connect_deadline) {
                drop_peer(peer, Status{ErrorCode::io_error}, now, true);
                continue;
            }
            if (peer.connection == Connection::handshaking && now >= peer.handshake_deadline) {
                drop_peer(peer, Status{ErrorCode::tls_handshake_timeout}, now, true);
                continue;
            }
            if (peer.connection != Connection::connected || !peer.driver) continue;
            if (!peer.settings.schedule.contains(std::chrono::system_clock::now()) &&
                !peer.schedule_logout_sent && peer.driver->state() == SessionState::active) {
                peer.schedule_logout_sent = true;
                (void)peer.driver->logout("session schedule closed", now,
                    timestamp_view(wall, peer.settings.timestamp_precision));
            }
            const auto flow = peer.settings.flow_control.value_or(FlowControlConfig{});
            if (!peer.driver->can_receive()) {
                if (peer.read_pause_since == 0) peer.read_pause_since = now;
                if (now - peer.read_pause_since >= flow.receive_pause_timeout_ms) {
                    drop_peer(peer, Status{ErrorCode::backlog_timeout}, now, true);
                    continue;
                }
            } else peer.read_pause_since = 0;
            if (peer.session && peer.session->callback_in_flight.load(std::memory_order_acquire) &&
                peer.session->callback_started_ms != 0 &&
                now - peer.session->callback_started_ms >= flow.max_delivery_age_ms) {
                drop_peer(peer, Status{ErrorCode::backlog_timeout}, now, true);
                continue;
            }
            const auto status = peer.driver->tick(now,
                timestamp_view(wall, peer.settings.timestamp_precision));
            if (!status.ok()) {
                if (shutdown_started && shutdown_status.ok()) shutdown_status = status;
                drop_peer(peer, status, now, false);
                continue;
            }
            if (peer.session) {
                const auto state = peer.driver->state();
                peer.session->state.store(state, std::memory_order_release);
                if (state != SessionState::active) peer.session->sender->set_active(false);
            }
            if (peer.driver->should_close()) {
                drop_peer(peer, Status{ErrorCode::connection_closed}, now, false);
            }
        }
    }

    void sweep_closed() noexcept {
        pending.erase(std::remove_if(pending.begin(), pending.end(),
            [](const auto& item) { return item->closed && item->io_tag.in_flight_polls == 0; }), pending.end());
        peers.erase(std::remove_if(peers.begin(), peers.end(),
            [](const auto& item) {
                return item->closed && item->io_tag.in_flight_polls == 0 &&
                       !item->uring_rx_in_flight && !item->uring_tx_in_flight;
            }), peers.end());
    }

    [[nodiscard]] std::uint64_t compute_deadline(std::uint64_t now) const noexcept {
        std::uint64_t deadline = now + 1000;
        for (const auto& peer : peers) {
            if (peer->closed) continue;
            if (peer->connection == Connection::disconnected && peer->initiator)
                deadline = std::min(deadline, peer->reconnect_at);
            else if (peer->connection == Connection::connecting)
                deadline = std::min(deadline, peer->connect_deadline);
            else if (peer->connection == Connection::handshaking)
                deadline = std::min(deadline, peer->handshake_deadline);
            else if (peer->connection == Connection::connected && peer->driver) {
                const auto session_deadline = peer->driver->next_deadline_ms();
                if (session_deadline != 0) deadline = std::min(deadline, session_deadline);
            }
        }
        for (const auto& session : sessions) {
            if (!session || !session->sender) continue;
            const auto sender_deadline = session->sender->next_expiry_deadline();
            if (sender_deadline != 0) deadline = std::min(deadline, sender_deadline);
            for (const auto& lane : session->sender->producer_lanes) {
                if (!lane) continue;
                const auto lane_deadline = lane->next_expiry_deadline();
                if (lane_deadline != 0) deadline = std::min(deadline, lane_deadline);
            }
        }
        for (const auto& listener : listeners) {
            if (listener.accept_backoff_until != 0)
                deadline = std::min(deadline, listener.accept_backoff_until);
        }
        for (const auto& item : pending)
            if (!item->closed) deadline = std::min(deadline, item->deadline);
        if (shutdown_started) deadline = std::min(deadline, shutdown_deadline);
        return deadline;
    }

    [[nodiscard]] Status begin_shutdown(std::uint64_t now) noexcept {
        if (!shutdown_started) {
            shutdown_started = true;
            shutdown_deadline = now + config.logout_timeout_ms;
            for (auto& listener : listeners) {
                io_remove(listener.io_tag);
                if (listener.socket) listener.socket->close();
            }
            for (auto& item : pending) {
                close_pending(*item);
            }
        }
        const auto wall = wall_timestamp();
        for (auto& peer_ptr : peers) {
            auto& peer = *peer_ptr;
            if (peer.connection == Connection::connected && peer.driver &&
                !peer.shutdown_logout_sent) {
                if (peer.session && peer.session->callback_in_flight.load(std::memory_order_acquire))
                    continue;
                // A peer Logout may already have moved the session to
                // logout_pending (or completed it) just before request_stop()
                // was observed. Preserve that in-flight handshake and avoid
                // asking the core to start a second local Logout from a state
                // where it is not valid.
                if (peer.driver->should_close()) continue;
                const auto state = peer.driver->state();
                if (state != SessionState::active &&
                    state != SessionState::awaiting_logon) continue;
                const auto status = peer.driver->logout("engine shutdown", now,
                    timestamp_view(wall, peer.settings.timestamp_precision));
                peer.shutdown_logout_sent = true;
                if (!status.ok()) {
                    if (shutdown_status.ok()) shutdown_status = status;
                    const auto reported = peer.driver->status();
                    if (reported.code != status.code || reported.tag != status.tag ||
                        reported.offset != status.offset) report_error(peer.name, status);
                }
            } else if (peer.connection == Connection::connecting || peer.connection == Connection::handshaking) {
                drop_peer(peer, Status{ErrorCode::engine_stopped}, now, false);
            }
        }
        return Status::success();
    }

    bool shutdown_complete(std::uint64_t now) noexcept {
        if (!shutdown_started) return false;
        bool callbacks_pending = false;
        for (const auto& session : sessions) {
            if (session && session->callback_in_flight.load(std::memory_order_acquire)) {
                callbacks_pending = true;
                break;
            }
        }
        if (callbacks_pending && now < shutdown_deadline) return false;
        for (auto& peer_ptr : peers) {
            auto& peer = *peer_ptr;
            if (peer.connection != Connection::connected) continue;
            if (peer.driver && peer.driver->should_close()) {
                if (peer.uring_tx_in_flight && now < shutdown_deadline) return false;
                drop_peer(peer, Status{ErrorCode::engine_stopped}, now, false);
                continue;
            }
            if (now >= shutdown_deadline) {
                const Status timeout{ErrorCode::logout_timeout};
                if (shutdown_status.ok()) shutdown_status = timeout;
                drop_peer(peer, timeout, now, true);
                continue;
            }
            return false;
        }
        return true;
    }

    [[nodiscard]] Status initialize_run() noexcept {
        const auto initial_validation = validate_config();
        if (!initial_validation.ok()) return initial_validation;
        try {
            peers.reserve(config.max_sessions);
            pending.reserve(config.max_pending_connections);
        } catch (...) {
            return {ErrorCode::capacity_exceeded};
        }
        auto status = create_storage_directory();
        if (!status.ok()) return status;
        status = start_diagnostics();
        if (!status.ok()) return status;
        status = bind_listeners();
        if (!status.ok()) return status;
        try {
            const auto scratch_bytes = align_up(config.max_pending_sender_bytes,
                alignof(std::max_align_t));
            sender_scratch_words.resize(scratch_bytes / sizeof(std::max_align_t));
        } catch (...) {
            return {ErrorCode::capacity_exceeded};
        }
        status = start_workers();
        if (!status.ok()) return status;
        status = start_initiators();
        if (!status.ok()) return status;

        io_completions.resize(std::max<std::size_t>(
            64, 1 + listeners.size() + config.max_pending_connections + config.max_sessions * 2));
        return Status::success();
    }

    [[nodiscard]] Status owner_iteration(std::uint32_t maximum_wait_ms) {
        std::array<char, sizeof(std::uint64_t)> wake_bytes{};
            auto now = monotonic_ms();
            expire_pending_commands(now);
            for (auto& peer : peers) {
                if (!peer->closed && peer->connection == Connection::connected &&
                    peer->driver && peer->session)
                    {
                        process_sender_commands(*peer, now);
                        if (!peer->session->sender->producer_lanes.empty())
                            process_producer_lanes(*peer, now);
                    }
            }
            collect_worker_completions();
            const bool completed_worker = finish_worker_completions(now);
            for (auto& peer : peers) {
                if (peer->closed || !peer->driver || peer->connection != Connection::connected ||
                    !peer->session || peer->session->callback_in_flight.load(std::memory_order_acquire)) continue;
                char reason[256];
                const auto size = peer->session->sender->take_logout(std::span<char>{reason, sizeof(reason)});
                if (size) {
                    const auto wall = wall_timestamp();
                    const auto result = peer->driver->logout({reason, *size}, now,
                        timestamp_view(wall, peer->settings.timestamp_precision));
                    if (!result.ok()) report_error(peer->name, result);
                }
            }
            schedule_pending_active_callbacks();
            if (worker_failure.load(std::memory_order_acquire) ||
                diagnostic_failure.load(std::memory_order_acquire)) {
                fatal_status = Status{ErrorCode::callback_failed};
                observer_failed = true;
            }
            for (auto& peer : peers) {
                if (peer->initiator && peer->connection == Connection::disconnected &&
                    !peer->uring_rx_in_flight && !peer->uring_tx_in_flight &&
                    peer->io_tag.in_flight_polls == 0 &&
                    !stopping.load(std::memory_order_acquire))
                    (void)begin_connect(*peer, now);
            }
            if (completed_worker || !last_tick_valid || now != last_tick_ms || shutdown_started) {
                tick_sessions(now);
                last_tick_ms = now;
                last_tick_valid = true;
            }
            publish_snapshots();
            bool buffered_work = false;
            for (auto& peer : peers) {
                if (!peer->closed && peer->connection == Connection::connected &&
                    peer->tls && peer->tls->pending_plaintext() != 0 &&
                    peer->driver && peer->driver->can_receive()) {
                    (void)read_peer(*peer, now);
                    if (!peer->closed && peer->tls && peer->tls->pending_plaintext() != 0 &&
                        peer->driver && peer->driver->can_receive())
                        buffered_work = true;
                }
            }
            for (auto& item : pending) {
                if (!item->closed && item->tls && item->tls->pending_plaintext() != 0) {
                    (void)read_pending(*item, now);
                    if (!item->closed && item->tls && item->tls->pending_plaintext() != 0)
                        buffered_work = true;
                }
            }
            for (const auto& peer : peers) {
                if (!peer->closed && peer->connection == Connection::connected &&
                    peer->session && peer->session->sender->has_ready_commands()) {
                    buffered_work = true;
                    break;
                }
            }
            for (auto& peer_ptr : peers) {
                auto& peer = *peer_ptr;
                if (peer.closed || peer.connection != Connection::connected ||
                    peer.unread_size() == 0) continue;
                const auto buffered_status = process_peer_buffer(peer, now);
                if (!buffered_status.ok() || peer.closed) continue;
                if (has_complete_buffered_frame(peer)) buffered_work = true;
            }
            for (auto& item : pending) {
                if (!item->closed && now >= item->deadline) {
                    close_pending(*item);
                    report_error({}, Status{item->handshaking ? ErrorCode::tls_handshake_timeout : ErrorCode::logon_timeout});
                }
            }
            if (stopping.load(std::memory_order_acquire)) (void)begin_shutdown(now);
            sweep_closed();
            if (observer_failed) { iteration_finished = true; return fatal_status; }
            if (shutdown_complete(now)) { iteration_finished = true; return shutdown_status; }

            // Synchronize I/O backend registrations and arm Proactor async RECV/SEND ops.
            const bool proactor = io_backend && io_backend->supports_proactor();
            io_sync(wake_tag, wake_fd, io_event_read);
            for (auto& listener : listeners) {
                listener.io_tag.pointer = &listener;
                const bool can_accept = listener.socket && listener.socket->valid() &&
                    (listener.accept_backoff_until == 0 || now >= listener.accept_backoff_until);
                io_sync(listener.io_tag,
                    can_accept ? listener.socket->native_handle() : -1,
                    can_accept ? io_event_read : 0U);
            }
            for (auto& item : pending) {
                item->io_tag.pointer = item.get();
                if (!item->closed && item->stream.valid()) {
                    const std::uint32_t interest = item->tls && item->tls->wants_write() ? io_event_write : io_event_read;
                    io_sync(item->io_tag, item->stream.native_handle(), interest);
                } else {
                    io_remove(item->io_tag);
                }
            }
            for (auto& peer : peers) {
                peer->io_tag.pointer = peer.get();
                if (peer->closed || !peer->stream.valid()) {
                    io_remove(peer->io_tag);
                    continue;
                }
                if (proactor && !peer->tls && peer->connection == Connection::connected) {
                    if (peer->driver && peer->driver->needs_write() && !peer->uring_tx_in_flight) {
                        (void)proactor_kick_send(*peer, now);
                    }
                    if (!peer->closed && !peer->uring_rx_in_flight &&
                        peer->driver && peer->driver->can_receive() &&
                        !has_complete_buffered_frame(*peer)) {
                        proactor_arm_recv(*peer);
                    }
                    continue;
                }
                std::uint32_t events = 0;
                if (peer->connection == Connection::connecting) events = io_event_write;
                else if (peer->connection == Connection::handshaking) {
                    events = peer->tls->wants_write() ? io_event_write : io_event_read;
                } else if (peer->connection == Connection::connected) {
                    if (peer->driver && peer->driver->can_receive())
                        events = peer->tls && peer->tls->read_wants_write() ? io_event_write : io_event_read;
                    if (peer->driver && peer->driver->needs_write())
                        events |= peer->tls && peer->tls->write_wants_read() ? io_event_read : io_event_write;
                }
                io_sync(peer->io_tag, peer->stream.native_handle(), events);
            }

            now = monotonic_ms();
            const auto deadline = compute_deadline(now);
            const auto wait_ms = buffered_work || deadline <= now ? 0U : static_cast<unsigned>(
                std::min<std::uint64_t>(deadline - now, maximum_wait_ms));
            const int ready = io_backend->wait(io_completions, wait_ms);
            if (ready < 0) {
                if (errno == EINTR) return Status::success();
                iteration_finished = true;
                return {ErrorCode::io_error, 0, static_cast<std::size_t>(errno)};
            }
            if (ready == 0) return Status::success();

            const auto count = static_cast<std::size_t>(ready);
            const auto start = rotation % count;
            for (std::size_t offset = 0; offset < count; ++offset) {
                const auto index = (start + offset) % count;
                const auto& completion = io_completions[index];
                auto* target = completion.tag;
                if (target == nullptr) continue;
                now = monotonic_ms();
                const auto kind = static_cast<TargetKind>(target->kind);
                if (kind == TargetKind::wake) {
                    while (::read(wake_fd, wake_bytes.data(), wake_bytes.size()) > 0) {}
                    continue;
                }
                if (kind == TargetKind::listener) {
                    auto* listener = static_cast<Listener*>(target->pointer);
                    if (completion.events & io_event_error) {
                        io_remove(listener->io_tag);
                        if (listener->socket) listener->socket->close();
                        report_error(listener->config.name, Status{ErrorCode::io_error});
                        continue;
                    }
                    const auto listener_index = static_cast<std::size_t>(listener - listeners.data());
                    const auto accepted = start_accepts(*listener, listener_index, now);
                    if (!accepted.ok() && observer_failed) return fatal_status;
                    continue;
                }
                if (kind == TargetKind::pending) {
                    auto* item = static_cast<Pending*>(target->pointer);
                    if (item->closed) continue;
                    if (completion.events & io_event_error) {
                        close_pending(*item);
                        report_error({}, Status{ErrorCode::io_error});
                        if (observer_failed) { iteration_finished = true; return fatal_status; }
                        continue;
                    }
                    if (completion.events & (io_event_read | io_event_write | io_event_hup)) {
                        (void)read_pending(*item, now);
                        if (observer_failed) { iteration_finished = true; return fatal_status; }
                    }
                    continue;
                }
                auto* peer = static_cast<Peer*>(target->pointer);
                if (completion.kind == IoCompletionKind::recv_done) {
                    handle_proactor_recv(*peer, completion.result, now);
                    if (observer_failed) { iteration_finished = true; return fatal_status; }
                    continue;
                }
                if (completion.kind == IoCompletionKind::send_done) {
                    handle_proactor_send(*peer, completion.result, now);
                    if (observer_failed) { iteration_finished = true; return fatal_status; }
                    continue;
                }
                if (peer->closed) continue;
                const auto events = completion.events;
                if (peer->connection == Connection::connecting &&
                    (events & (io_event_write | io_event_error | io_event_hup))) {
                    const auto completed = peer->stream.finish_connect();
                    if (!completed.ok()) {
                        drop_peer(*peer, completed, now, true);
                        continue;
                    }
                    const auto connected = connected_peer(*peer, now);
                    if (!connected.ok()) continue;
                }
                if (peer->connection == Connection::handshaking) {
                    const auto progress = connected_peer(*peer, now);
                    if (!progress.ok()) continue;
                }
                if (peer->connection != Connection::connected) continue;
                if (events & io_event_error) {
                    drop_peer(*peer, Status{ErrorCode::io_error}, now, true);
                    continue;
                }
                if ((events & (io_event_read | io_event_hup)) ||
                    (peer->tls && peer->tls->read_wants_write() && (events & io_event_write)))
                    (void)read_peer(*peer, now);
                if (!peer->closed &&
                    ((peer->driver && peer->driver->needs_write() && !(peer->tls && peer->tls->write_wants_read())) ||
                     (events & io_event_write) ||
                     (peer->tls && peer->tls->write_wants_read() && (events & io_event_read))))
                    (void)write_peer(*peer, now);
                if (observer_failed) { iteration_finished = true; return fatal_status; }
            }
            rotation = (rotation + 1) % count;
        return Status::success();
    }

    std::unique_ptr<IoBackend> io_backend;
    Status io_backend_status{};
    std::vector<IoCompletion> io_completions;
    IoTag wake_tag{make_tag(TargetKind::wake)};
    std::size_t rotation{};
    std::uint64_t last_tick_ms{};
    bool last_tick_valid{};
    bool iteration_finished{};
    Status final_status{};
    std::thread::id owner_thread;

    std::vector<DiagnosticRecord> diagnostic_records;
    std::thread diagnostic_thread;
    std::mutex diagnostic_mutex;
    std::condition_variable diagnostic_cv;
    std::size_t diagnostic_head{}, diagnostic_tail{}, diagnostic_count{};
    bool diagnostic_stopping{};
    std::atomic<bool> diagnostic_failure{false};
    std::atomic<std::uint64_t> diagnostics_dropped{0};

    EngineConfig config;
    std::vector<InitiatorConfig> initiators;
    std::vector<std::shared_ptr<SessionControl>> initiator_sessions;
    std::vector<Listener> listeners;
    std::vector<Acceptor> acceptors;
    std::vector<std::shared_ptr<SessionControl>> sessions;
    std::size_t pending_expiry_session_cursor{};
    std::vector<std::unique_ptr<Peer>> peers;
    std::vector<std::unique_ptr<Pending>> pending;
    std::vector<FieldView> field_scratch;
    std::vector<std::max_align_t> sender_scratch_words;
    EngineMessageCallback message_callback;
    std::vector<RegisteredMessageHandler> typed_handlers;
    std::vector<IgnoredMessageType> ignored_messages;
    EngineActiveCallback active_callback;
    EngineErrorCallback error_callback;
    bool queued{};
    const SchemaDescriptor* schema{};
    std::vector<LengthDataPair> wire_hint_pairs;
    WireHints wire_hints{};
    std::shared_ptr<std::atomic<EngineLife>> life;
    std::shared_ptr<WakeControl> wake_control;
    int wake_fd{-1};
    std::atomic<bool> stopping{false};
    std::atomic<bool> has_run{false};
    std::mutex endpoint_mutex;
    bool shutdown_started{};
    bool observer_failed{};
    std::uint64_t shutdown_deadline{};
    Status shutdown_status{};
    Status fatal_status{ErrorCode::callback_failed};
    std::vector<DeliveryTask> worker_tasks;
    std::vector<char> worker_ring_arena;
    std::vector<std::size_t> worker_ready;
    std::vector<std::size_t> worker_free_indices;
    std::vector<DeliveryCompletion> worker_completions;
    std::vector<std::thread> workers;
    mutable std::mutex worker_mutex;
    std::condition_variable worker_cv;
    std::size_t worker_ready_head{};
    std::size_t worker_ready_tail{};
    std::size_t worker_ready_count{};
    std::size_t worker_free_count{};
    std::size_t worker_completion_head{};
    std::size_t worker_completion_tail{};
    std::size_t worker_completion_count{};
    std::size_t worker_payload_bytes{};
    std::atomic<bool> worker_failure{false};
    bool workers_started{};
    bool worker_stopping{};
};

EngineRuntime::EngineRuntime(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
EngineRuntime::~EngineRuntime() = default;

Result<std::unique_ptr<EngineRuntime>> EngineRuntime::create(EngineConfig config,
    bool queued, const SchemaDescriptor* schema, WireHints wire_hints) noexcept {
    try {
        auto impl = std::make_unique<Impl>(std::move(config), queued, schema, wire_hints);
        if (!impl->io_backend_status.ok()) return impl->io_backend_status;
        if (impl->wake_fd < 0 || !impl->io_backend) return Status{ErrorCode::io_error, 0, static_cast<std::size_t>(errno)};
        auto runtime = std::unique_ptr<EngineRuntime>(new EngineRuntime(std::move(impl)));
        return runtime;
    } catch (...) {
        return Status{ErrorCode::capacity_exceeded};
    }
}

Status EngineRuntime::on_message(EngineMessageCallback callback) noexcept {
    if (!impl_ || impl_->has_run.load(std::memory_order_acquire)) return {ErrorCode::invalid_config};
    if (!callback) return {ErrorCode::invalid_config};
    if (impl_->message_callback) return {ErrorCode::duplicate_field};
    if (!impl_->ignored_messages.empty()) return {ErrorCode::duplicate_field};
    try { impl_->message_callback = std::move(callback); }
    catch (...) { return {ErrorCode::capacity_exceeded}; }
    return Status::success();
}

Status EngineRuntime::on_typed_message(std::string_view msg_type,
    EngineMessageCallback callback) noexcept {
    if (!impl_ || impl_->has_run.load(std::memory_order_acquire) || impl_->schema == nullptr ||
        !callback || msg_type.empty() || !detail::is_valid_text(msg_type) || msg_type.size() > 32)
        return {ErrorCode::invalid_config};
    if (impl_->schema->find_message(msg_type) == nullptr) return {ErrorCode::schema_mismatch, 35};
    for (const auto& current : impl_->typed_handlers)
        if (current.msg_type == msg_type) return {ErrorCode::duplicate_field, 35};
    for (const auto& current : impl_->ignored_messages)
        if (current.msg_type == msg_type) return {ErrorCode::duplicate_field, 35};
    try {
        impl_->typed_handlers.push_back(RegisteredMessageHandler{std::string{msg_type}, std::move(callback)});
        return Status::success();
    } catch (...) { return {ErrorCode::capacity_exceeded}; }
}

Status EngineRuntime::ignore_message(std::string_view msg_type,
    std::string_view reason) noexcept {
    if (!impl_ || impl_->has_run.load(std::memory_order_acquire) ||
        msg_type.empty() || msg_type.size() > 32 || !detail::is_valid_text(msg_type) ||
        reason.empty() || !detail::is_valid_text(reason)) return {ErrorCode::invalid_config};
    if (impl_->schema == nullptr && impl_->message_callback)
        return {ErrorCode::duplicate_field, 35};
    if (impl_->schema != nullptr && impl_->schema->find_message(msg_type) == nullptr)
        return {ErrorCode::schema_mismatch, 35};
    for (const auto& current : impl_->typed_handlers)
        if (current.msg_type == msg_type) return {ErrorCode::duplicate_field, 35};
    for (const auto& current : impl_->ignored_messages)
        if (current.msg_type == msg_type) return {ErrorCode::duplicate_field, 35};
    try {
        impl_->ignored_messages.push_back(IgnoredMessageType{std::string{msg_type}, std::string{reason}});
        return Status::success();
    } catch (...) { return {ErrorCode::capacity_exceeded}; }
}

Status EngineRuntime::on_session_active(EngineActiveCallback callback) noexcept {
    if (!impl_ || impl_->has_run.load(std::memory_order_acquire)) return {ErrorCode::invalid_config};
    if (!callback) return {ErrorCode::invalid_config};
    if (impl_->active_callback) return {ErrorCode::duplicate_field};
    try { impl_->active_callback = std::move(callback); }
    catch (...) { return {ErrorCode::capacity_exceeded}; }
    return Status::success();
}

Status EngineRuntime::on_error(EngineErrorCallback callback) noexcept {
    if (!impl_ || impl_->has_run.load(std::memory_order_acquire)) return {ErrorCode::invalid_config};
    if (!callback) return {ErrorCode::invalid_config};
    if (impl_->error_callback) return {ErrorCode::duplicate_field};
    try { impl_->error_callback = std::move(callback); }
    catch (...) { return {ErrorCode::capacity_exceeded}; }
    return Status::success();
}

Result<SessionHandle> EngineRuntime::add_initiator(InitiatorConfig config) noexcept {
    if (!impl_ || impl_->has_run.load(std::memory_order_acquire) ||
        !plain_identifier(config.name, 128) ||
        !plain_identifier(config.sender_comp_id, 64) ||
        !plain_identifier(config.target_comp_id, 64) ||
        config.sender_comp_id == config.target_comp_id ||
        !is_numeric_host(config.endpoint.host) ||
        config.endpoint.port == 0 ||
        impl_->initiators.size() + impl_->acceptors.size() >= impl_->config.max_sessions)
        return Status{ErrorCode::invalid_config};
    for (const auto& current : impl_->initiators)
        if (current.name == config.name ||
            (current.sender_comp_id == config.sender_comp_id &&
             current.target_comp_id == config.target_comp_id &&
             same_identity_options(current.settings, config.settings)))
            return Status{ErrorCode::invalid_config};
    for (const auto& current : impl_->acceptors)
        if (current.config.name == config.name ||
            (current.config.local_comp_id == config.sender_comp_id &&
             current.config.remote_comp_id == config.target_comp_id &&
             same_identity_options(current.config.settings, config.settings)))
            return Status{ErrorCode::invalid_config};
    try {
        const auto name = config.name;
        impl_->initiators.reserve(impl_->initiators.size() + 1);
        impl_->initiator_sessions.reserve(impl_->initiator_sessions.size() + 1);
        impl_->sessions.reserve(impl_->sessions.size() + 1);
        auto control = impl_->make_session_control(name, config.sender_comp_id, config.target_comp_id, config.settings);
        impl_->initiators.push_back(std::move(config));
        impl_->initiator_sessions.push_back(control);
        return SessionHandle{name, control->sender};
    } catch (...) { return Status{ErrorCode::capacity_exceeded}; }
}

Result<ListenerHandle> EngineRuntime::add_listener(ListenerConfig config) noexcept {
    if (!impl_ || impl_->has_run.load(std::memory_order_acquire) ||
        !plain_identifier(config.name, 128) || !is_numeric_host(config.endpoint.host, true) ||
        config.backlog == 0 || config.backlog > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        impl_->listeners.size() >= impl_->config.max_sessions)
        return Status{ErrorCode::invalid_config};
    for (const auto& listener : impl_->listeners)
        if (listener.config.name == config.name) return Status{ErrorCode::invalid_config};
    try {
        const auto name = config.name;
        Impl::Listener listener;
        listener.config = std::move(config);
        impl_->listeners.push_back(std::move(listener));
        return ListenerHandle{name};
    } catch (...) { return Status{ErrorCode::capacity_exceeded}; }
}

Result<SessionHandle> EngineRuntime::add_acceptor(AcceptorConfig config) noexcept {
    if (!impl_ || impl_->has_run.load(std::memory_order_acquire) ||
        !plain_identifier(config.name, 128) ||
        !plain_identifier(config.local_comp_id, 64) ||
        !plain_identifier(config.remote_comp_id, 64) ||
        config.local_comp_id == config.remote_comp_id ||
        impl_->initiators.size() + impl_->acceptors.size() >= impl_->config.max_sessions)
        return Status{ErrorCode::invalid_config};
    std::size_t listener_index = no_index;
    for (std::size_t i = 0; i < impl_->listeners.size(); ++i)
        if (impl_->listeners[i].config.name == config.listener) listener_index = i;
    if (listener_index == no_index) return Status{ErrorCode::invalid_config};
    for (const auto& current : impl_->initiators)
        if (current.name == config.name) return Status{ErrorCode::invalid_config};
    for (const auto& current : impl_->acceptors)
        if (current.config.name == config.name ||
            (current.config.local_comp_id == config.local_comp_id &&
             current.config.remote_comp_id == config.remote_comp_id &&
             (same_identity_options(current.config.settings, config.settings) ||
              (current.listener_index == listener_index && current.config.settings.version == config.settings.version))))
            return Status{ErrorCode::invalid_config};
    for (const auto& current : impl_->initiators)
        if (current.name == config.name ||
            (current.sender_comp_id == config.local_comp_id &&
             current.target_comp_id == config.remote_comp_id &&
             same_identity_options(current.settings, config.settings)))
            return Status{ErrorCode::invalid_config};
    try {
        const auto name = config.name;
        impl_->acceptors.reserve(impl_->acceptors.size() + 1);
        impl_->sessions.reserve(impl_->sessions.size() + 1);
        auto control = impl_->make_session_control(name, config.local_comp_id, config.remote_comp_id, config.settings);
        impl_->acceptors.push_back(Impl::Acceptor{std::move(config), listener_index, nullptr, control});
        return SessionHandle{name, control->sender};
    } catch (...) { return Status{ErrorCode::capacity_exceeded}; }
}

Status EngineRuntime::validate() const noexcept {
    if (!impl_) return {ErrorCode::invalid_config};
    const auto status = impl_->validate_config();
    if (!status.ok()) return status;
    if (impl_->schema != nullptr) {
        if (impl_->schema->messages.empty()) return {ErrorCode::schema_mismatch};
        for (const auto& handler : impl_->typed_handlers)
            if (impl_->schema->find_message(handler.msg_type) == nullptr)
                return {ErrorCode::schema_mismatch, 35};
        for (const auto& ignored : impl_->ignored_messages)
            if (impl_->schema->find_message(ignored.msg_type) == nullptr || ignored.reason.empty())
                return {ErrorCode::schema_mismatch, 35};
    }
    if (!impl_->initiators.empty() && impl_->initiators.size() > impl_->config.max_sessions)
        return {ErrorCode::invalid_config};
    for (const auto& acceptor : impl_->acceptors)
        if (acceptor.listener_index >= impl_->listeners.size()) return {ErrorCode::invalid_config};
    return Status::success();
}

Status EngineRuntime::start() noexcept {
    if (!impl_) return {ErrorCode::invalid_config};
    const auto checked = validate();
    if (!checked.ok()) return checked;
    bool expected = false;
    if (!impl_->has_run.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
        return {ErrorCode::invalid_config};
    impl_->owner_thread = std::this_thread::get_id();
    for (const auto& session : impl_->sessions)
        if (session && session->sender) session->sender->freeze_producer_lanes();
    try {
        std::sort(impl_->typed_handlers.begin(), impl_->typed_handlers.end(),
            [](const auto& left, const auto& right) { return left.msg_type < right.msg_type; });
        std::sort(impl_->ignored_messages.begin(), impl_->ignored_messages.end(),
            [](const auto& left, const auto& right) { return left.msg_type < right.msg_type; });
        impl_->life->store(EngineLife::running, std::memory_order_release);
        const auto status = impl_->initialize_run();
        if (status.ok()) return status;
        impl_->final_status = status;
    } catch (...) { impl_->final_status = {ErrorCode::capacity_exceeded}; }
    impl_->iteration_finished = true;
    return poll_once();
}

Status EngineRuntime::poll_once(std::uint32_t max_wait_ms) noexcept {
    if (!impl_ || !impl_->has_run.load(std::memory_order_acquire)) return {ErrorCode::not_ready};
    if (impl_->owner_thread != std::this_thread::get_id()) return {ErrorCode::wrong_thread};
    if (impl_->life->load(std::memory_order_acquire) == EngineLife::stopped) return impl_->final_status;
    if (!impl_->iteration_finished) {
        try {
            impl_->final_status = impl_->owner_iteration(std::min(max_wait_ms, 1000U));
            impl_->publish_snapshots();
            if (!impl_->final_status.ok()) impl_->iteration_finished = true;
        } catch (...) {
            impl_->iteration_finished = true;
            impl_->final_status = {ErrorCode::capacity_exceeded};
        }
    }
    if (!impl_->iteration_finished) return Status::success();
    if (!impl_->final_status.ok() && !impl_->observer_failed) {
        const auto& shutdown = impl_->shutdown_status;
        const bool already_reported = !shutdown.ok() &&
            shutdown.code == impl_->final_status.code && shutdown.tag == impl_->final_status.tag &&
            shutdown.offset == impl_->final_status.offset;
        if (!already_reported) impl_->report_error({}, impl_->final_status);
    }
    impl_->life->store(EngineLife::stopping, std::memory_order_release);
    impl_->stop_workers();
    impl_->discard_stopped_worker_state();
    impl_->publish_snapshots();
    impl_->close_all();
    for (auto& session : impl_->sessions) if (session && session->sender) session->sender->mark_stopped();
    impl_->stop_diagnostics();
    if (impl_->diagnostic_failure.load(std::memory_order_acquire))
        impl_->final_status = {ErrorCode::callback_failed};
    impl_->life->store(EngineLife::stopped, std::memory_order_release);
    return impl_->final_status;
}

Status EngineRuntime::run() noexcept {
    const auto status = start();
    if (!status.ok()) return status;
    while (running()) {
        const auto polled = poll_once(1000);
        if (!polled.ok()) return polled;
    }
    return impl_->final_status;
}

bool EngineRuntime::running() const noexcept {
    if (!impl_) return false;
    const auto life = impl_->life->load(std::memory_order_acquire);
    return life == EngineLife::running || life == EngineLife::stopping;
}

Result<SessionSnapshot> EngineRuntime::snapshot(std::string_view name) const noexcept {
    if (!impl_) return Status{ErrorCode::invalid_config};
    try {
        for (const auto& session : impl_->sessions) if (session && session->name == name) {
            SessionSnapshot result;
            result.name = session->name;
            result.state = session->state.load(std::memory_order_acquire);
            result.generation = session->generation.load(std::memory_order_acquire);
            result.last_error = {session->last_error.load(std::memory_order_acquire),
                session->last_error_tag.load(std::memory_order_relaxed),
                session->last_error_offset.load(std::memory_order_relaxed)};
            result.next_inbound_seq = session->next_inbound_seq.load(std::memory_order_relaxed);
            result.next_outbound_seq = session->next_outbound_seq.load(std::memory_order_relaxed);
            result.received_frames = session->received_frames.load(std::memory_order_relaxed);
            result.received_bytes = session->received_bytes.load(std::memory_order_relaxed);
            result.written_bytes = session->written_bytes.load(std::memory_order_relaxed);
            result.business_deliveries = session->business_deliveries.load(std::memory_order_relaxed);
            result.pending_messages = session->pending_messages.load(std::memory_order_relaxed);
            result.pending_bytes = session->pending_bytes.load(std::memory_order_relaxed);
            result.pending_inbound_messages = session->pending_inbound_messages.load(std::memory_order_relaxed);
            result.pending_inbound_bytes = session->pending_inbound_bytes.load(std::memory_order_relaxed);
            result.read_paused = session->read_paused.load(std::memory_order_relaxed);
            return result;
        }
        return Status{ErrorCode::invalid_value};
    } catch (...) { return Status{ErrorCode::capacity_exceeded}; }
}

std::uint64_t EngineRuntime::dropped_diagnostics() const noexcept {
    return impl_ ? impl_->diagnostics_dropped.load(std::memory_order_relaxed) : 0;
}

void EngineRuntime::request_stop() noexcept {
    if (!impl_) return;
    impl_->stopping.store(true, std::memory_order_release);
    auto expected = EngineLife::running;
    (void)impl_->life->compare_exchange_strong(expected, EngineLife::stopping,
        std::memory_order_acq_rel);
    impl_->wake();
}

Result<TcpEndpoint> EngineRuntime::listener_endpoint(std::string_view name) const noexcept {
    if (!impl_) return Status{ErrorCode::invalid_config};
    try {
        std::lock_guard lock(impl_->endpoint_mutex);
        for (const auto& listener : impl_->listeners) {
            if (listener.config.name != name) continue;
            if (listener.published.port == 0) return Status{ErrorCode::need_more};
            return listener.published;
        }
        return Status{ErrorCode::invalid_config};
    } catch (...) {
        return Status{ErrorCode::io_error};
    }
}

} // namespace detail

std::string ErrorEvent::format() const {
    std::string result;
    if (!session_name.empty()) {
        result.append(session_name);
        result.append(": ");
    }
    result.append(status.message());
    if (status.tag != 0) {
        result.append(" (tag ");
        result.append(std::to_string(status.tag));
        result.push_back(')');
    }
    if (status.offset != 0) {
        result.append(status.code == ErrorCode::io_error ? " (system error " : " at byte ");
        result.append(std::to_string(status.offset));
        if (status.code == ErrorCode::io_error) result.push_back(')');
    }
    return result;
}

} // namespace nimblefix
