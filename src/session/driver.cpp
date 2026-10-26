#include <nimblefix/session.hpp>

#include "core.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <limits>
#include <new>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace nimblefix {
namespace {

constexpr std::size_t admin_field_capacity = 4;
constexpr std::size_t max_msg_type_bytes = 32;

[[nodiscard]] bool parse_unsigned(std::string_view text, std::uint64_t& value) noexcept {
    if (text.empty()) return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

[[nodiscard]] const FieldView* unique_field(const FlatMessageView& message,
    std::uint32_t tag, bool& duplicate) noexcept {
    const FieldView* result = nullptr;
    duplicate = false;
    for (const auto& field : message.fields()) {
        if (field.tag != tag) continue;
        if (result != nullptr) {
            duplicate = true;
            return nullptr;
        }
        result = &field;
    }
    return result;
}

[[nodiscard]] bool is_session_admin(std::string_view msg_type) noexcept {
    return msg_type == "A" || msg_type == "0" || msg_type == "1" || msg_type == "2" ||
        msg_type == "3" || msg_type == "4" || msg_type == "5";
}

[[nodiscard]] Status validate_config(const SessionConfig& config) noexcept {
    if ((config.role != SessionRole::initiator && config.role != SessionRole::acceptor) ||
        config.storage_directory.empty() || config.session_key.empty() ||
        config.session_key.size() > journal_max_session_key_bytes ||
        !detail::is_valid_text(config.local_comp_id) || !detail::is_valid_text(config.remote_comp_id) ||
        config.heartbeat_ms == 0 || config.logon_timeout_ms == 0 || config.logout_timeout_ms == 0 ||
        config.max_frame_bytes < 128 || config.max_frame_bytes > std::numeric_limits<std::size_t>::max() - 8 ||
        config.max_pending_messages == 0 || config.admin_reserve_messages < 2 ||
        config.admin_reserve_messages >= config.max_pending_messages ||
        config.max_pending_bytes == 0 || config.max_frame_bytes > std::numeric_limits<std::size_t>::max() / 2 ||
        config.admin_reserve_bytes < config.max_frame_bytes * 2 ||
        config.admin_reserve_bytes >= config.max_pending_bytes ||
        config.max_pending_bytes - config.admin_reserve_bytes < config.max_frame_bytes ||
        config.max_pending_inbound_messages == 0 ||
        config.max_pending_inbound_bytes < config.max_frame_bytes ||
        config.receipt_capacity == 0 || config.journal_max_records == 0 ||
        config.journal_capacity_bytes == 0) {
        return Status{ErrorCode::invalid_config};
    }
    return Status::success();
}

[[nodiscard]] constexpr std::uint64_t saturating_add(std::uint64_t value,
    std::uint64_t amount) noexcept {
    return amount > std::numeric_limits<std::uint64_t>::max() - value
        ? std::numeric_limits<std::uint64_t>::max() : value + amount;
}

[[nodiscard]] std::string journal_path_for(const SessionConfig& config,
    Status& status) {
    if (config.storage_directory.empty() || config.session_key.empty() ||
        config.session_key.size() > journal_max_session_key_bytes) {
        status = Status{ErrorCode::invalid_config};
        return {};
    }
    std::string hex_key;
    hex_key.reserve(config.session_key.size() * 2);
    constexpr char digits[] = "0123456789abcdef";
    for (const unsigned char ch : config.session_key) {
        hex_key.push_back(digits[ch >> 4]);
        hex_key.push_back(digits[ch & 0x0f]);
    }

    std::filesystem::path directory{config.storage_directory};
    directory /= "sessions";
    for (std::size_t offset = 0; offset < hex_key.size(); offset += 64) {
        directory /= hex_key.substr(offset, std::min<std::size_t>(64, hex_key.size() - offset));
    }
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        status = Status{ErrorCode::io_error};
        return {};
    }
    status = Status::success();
    return (directory / "session.nfxj").string();
}

struct AdminField {
    std::uint32_t tag{};
    FieldKind kind{};
    std::uint64_t scalar{};
    std::string_view text{};
};

struct AdminMessageStorage {
    std::array<detail::SlotValue, admin_field_capacity> slots{};
    std::array<detail::PlanField, admin_field_capacity> plan{};
    detail::MessageView view{};
};

void format_tag_prefix(std::uint32_t tag, detail::PlanField& field) noexcept {
    std::array<char, 10> reversed{};
    std::size_t size = 0;
    do {
        reversed[size++] = static_cast<char>('0' + tag % 10);
        tag /= 10;
    } while (tag != 0 && size < reversed.size() - 1);
    field.prefix_size = static_cast<std::uint8_t>(size + 1);
    for (std::size_t i = 0; i < size; ++i) field.tag_prefix[i] = reversed[size - i - 1];
    field.tag_prefix[size] = '=';
}

[[nodiscard]] Status make_admin_view(std::string_view msg_type,
    std::span<const AdminField> fields, AdminMessageStorage& storage) noexcept {
    if (msg_type.empty() || msg_type.size() > max_msg_type_bytes || fields.size() > admin_field_capacity) {
        return Status{ErrorCode::invalid_value, 35};
    }
    std::string_view arena{};
    for (std::size_t i = 0; i < fields.size(); ++i) {
        const auto& source = fields[i];
        if (source.tag == 0 || detail::is_managed_tag(source.tag)) {
            return Status{ErrorCode::invalid_value, static_cast<int>(source.tag)};
        }
        for (std::size_t previous = 0; previous < i; ++previous) {
            if (fields[previous].tag == source.tag) {
                return Status{ErrorCode::duplicate_field, static_cast<int>(source.tag)};
            }
        }
        auto& field = storage.plan[i];
        field.tag = source.tag;
        field.slot_index = static_cast<std::uint32_t>(i);
        field.kind = source.kind;
        format_tag_prefix(source.tag, field);
        auto& slot = storage.slots[i];
        switch (source.kind) {
        case FieldKind::character:
            slot.scalar = source.scalar;
            break;
        case FieldKind::signed_integer:
        case FieldKind::unsigned_integer:
            slot.scalar = source.scalar;
            break;
        case FieldKind::string:
            if (!detail::is_valid_text(source.text)) return Status{ErrorCode::invalid_value, static_cast<int>(source.tag)};
            if (arena.data() != nullptr) return Status{ErrorCode::unsupported};
            arena = source.text;
            slot.offset = 0;
            slot.size = static_cast<std::uint32_t>(source.text.size());
            break;
        case FieldKind::decimal:
            return Status{ErrorCode::unsupported};
        }
    }
    storage.view = detail::MessageView{storage.slots.data(), arena.data(), arena.size(), storage.plan.data(),
        nullptr, fields.size(), nullptr, msg_type, Status::success()};
    return Status::success();
}

struct ReplayRange {
    std::uint64_t begin{};
    std::uint64_t end{};
    RecordId dependency{};
};

struct TxDescriptor {
    RecordId record_id{};
    std::size_t offset{};
    std::size_t frame_size{};
    std::uint64_t sequence{};
    std::uint64_t receipt_id{};
    std::uint64_t appended_at_ms{};
    std::array<char, max_msg_type_bytes> msg_type{};
    std::uint8_t msg_type_size{};
    bool business{};
};

struct ReceiptSlot {
    SendReceipt value{};
    bool occupied{};
};

struct RecoverySlot {
    RecordId id{};
    std::size_t bytes{};
};

} // namespace

struct SessionDriver::Impl {
    Impl(SessionConfig requested_config, SessionCallbacks requested_callbacks, Journal requested_journal)
        : config(std::move(requested_config)), callbacks(std::move(requested_callbacks)),
          journal(std::move(requested_journal)),
          core(config.role, config.heartbeat_ms, config.logon_timeout_ms, config.logout_timeout_ms,
              journal.next_inbound_seq(), journal.next_outbound_seq()),
          tx_slots(config.max_pending_messages), encode_buffer(config.max_frame_bytes),
          replay_buffer(config.max_frame_bytes), deferred_logon_buffer(config.max_frame_bytes),
          parse_fields(config.max_frame_bytes / 4 + 2),
          receipt_slots(config.receipt_capacity), replay_slots(config.max_pending_messages),
          recovery_slots(config.max_pending_inbound_messages) {
        for (const auto& record : journal.records()) {
            if (record.kind == RecordKind::outbound && record.receipt >= next_receipt_id) {
                next_receipt_id = record.receipt == std::numeric_limits<std::uint64_t>::max()
                    ? record.receipt : record.receipt + 1;
            }
            if (record.kind == RecordKind::outbound && record.receipt != 0) {
                put_receipt(record.receipt, record.seq, ReceiptState::appended);
            }
            if (record.kind != RecordKind::inbound || journal.has_delivery(record.id)) continue;
            if (recovery_count == recovery_slots.size() ||
                record.payload.size() > config.max_pending_inbound_bytes - recovery_bytes) {
                recovery_overflow = true;
                break;
            }
            recovery_slots[recovery_count++] = RecoverySlot{record.id, record.payload.size()};
            recovery_bytes += record.payload.size();
        }
        if (next_receipt_id == 0) next_receipt_id = 1;
    }

    SessionConfig config;
    SessionCallbacks callbacks;
    Journal journal;
    detail::SessionCore core;
    std::vector<TxDescriptor> tx_slots;
    std::vector<char> encode_buffer;
    std::vector<char> replay_buffer;
    std::vector<char> deferred_logon_buffer;
    std::vector<FieldView> parse_fields;
    std::vector<ReceiptSlot> receipt_slots;
    std::vector<ReplayRange> replay_slots;
    std::vector<RecoverySlot> recovery_slots;
    SessionDriver* public_owner{};
    std::thread::id owner_thread{};
    Status last_status{};
    Status callback_observer_status{};
    std::uint64_t next_receipt_id{1};
    std::uint64_t last_byte_progress_ms{};
    std::uint64_t pending_since_ms{};
    std::size_t tx_head{};
    std::size_t tx_count{};
    std::size_t pending_bytes_count{};
    std::size_t business_count{};
    std::size_t business_bytes{};
    std::size_t admin_count{};
    std::size_t admin_bytes{};
    std::size_t replay_head{};
    std::size_t replay_count{};
    std::size_t recovery_count{};
    std::size_t recovery_bytes{};
    std::uint64_t replay_next{};
    std::uint64_t replay_end{};
    std::uint64_t replay_advance{};
    std::uint64_t replay_receipt_id{};
    std::uint64_t replay_sequence{};
    RecordId replay_dependency{};
    std::uint64_t deferred_logon_sequence{};
    std::size_t replay_bytes{};
    std::size_t replay_offset{};
    std::size_t deferred_logon_size{};
    std::array<char, max_msg_type_bytes> replay_msg_type{};
    std::uint8_t replay_msg_type_size{};
    std::array<char, 22> wall_time_storage{};
    std::size_t wall_time_size{};
    bool owner_set{};
    bool replay_active{};
    bool replay_prepared{};
    bool deferred_logon_pending{};
    bool processing_deferred_logon{};
    bool capacity_blocked{};
    bool write_timer_active{};
    bool should_close{};
    bool fatal{};
    bool callback_active{};
    bool recovery_overflow{};
    bool stall_logout_attempted{};
    std::uint64_t callback_now_ms{};
    std::string_view callback_wall_time{};
    std::uint64_t stall_logout_deadline{};

    [[nodiscard]] Status set_wall_time(std::string_view value) noexcept {
        if (!detail::is_valid_utc_timestamp(value) || value.size() > wall_time_storage.size()) {
            return Status{ErrorCode::invalid_timestamp, 52};
        }
        std::memcpy(wall_time_storage.data(), value.data(), value.size());
        wall_time_size = value.size();
        callback_wall_time = {wall_time_storage.data(), wall_time_size};
        return Status::success();
    }

    [[nodiscard]] Status claim_owner() noexcept {
        const auto current = std::this_thread::get_id();
        if (!owner_set) {
            owner_thread = current;
            owner_set = true;
            return Status::success();
        }
        return owner_thread == current ? Status::success() : Status{ErrorCode::wrong_thread};
    }

    [[nodiscard]] bool is_active() const noexcept { return core.state() == SessionState::active && !fatal; }
    [[nodiscard]] bool can_send_now() const noexcept {
        if (!is_active()) return false;
        const auto message_limit = config.max_pending_messages - config.admin_reserve_messages;
        const auto byte_limit = config.max_pending_bytes - config.admin_reserve_bytes;
        return business_count < message_limit && business_bytes < byte_limit &&
            tx_count < tx_slots.size() && pending_bytes_count + replay_reservation_bytes() < config.max_pending_bytes;
    }

    [[nodiscard]] std::size_t replay_reservation_bytes() const noexcept {
        return replay_active || replay_count != 0 ? config.max_frame_bytes : 0;
    }

    [[nodiscard]] std::size_t tx_index(std::size_t logical) const noexcept {
        return (tx_head + logical) % tx_slots.size();
    }

    [[nodiscard]] TxDescriptor* front_tx() noexcept {
        return tx_count == 0 ? nullptr : &tx_slots[tx_head];
    }

    [[nodiscard]] const TxDescriptor* front_tx() const noexcept {
        return tx_count == 0 ? nullptr : &tx_slots[tx_head];
    }

    void put_receipt(std::uint64_t id, std::uint64_t sequence, ReceiptState state,
        Status status = {}) noexcept {
        if (id == 0 || receipt_slots.empty()) return;
        auto& slot = receipt_slots[static_cast<std::size_t>(id % receipt_slots.size())];
        slot.value = SendReceipt{id, sequence, state, status};
        slot.occupied = true;
    }

    void update_receipt(std::uint64_t id, ReceiptState state, Status status = {}) noexcept {
        if (id == 0 || receipt_slots.empty()) return;
        auto& slot = receipt_slots[static_cast<std::size_t>(id % receipt_slots.size())];
        if (slot.occupied && slot.value.id == id) {
            slot.value.state = state;
            slot.value.status = status;
        }
    }

    void fail_receipts(Status reason) noexcept {
        for (std::size_t i = 0; i < tx_count; ++i) {
            auto& descriptor = tx_slots[tx_index(i)];
            if (!descriptor.business) continue;
            const auto& current = receipt_slots[static_cast<std::size_t>(descriptor.receipt_id % receipt_slots.size())];
            if (current.occupied && current.value.id == descriptor.receipt_id &&
                current.value.state == ReceiptState::expired) continue;
            update_receipt(descriptor.receipt_id, ReceiptState::failed, reason);
        }
        if (replay_prepared && replay_receipt_id != 0) {
            update_receipt(replay_receipt_id, ReceiptState::failed, reason);
        }
    }

    void notify_error(Status error) noexcept {
        if (callbacks.on_error) {
            try {
                callbacks.on_error(error);
            } catch (...) {
                callback_observer_status = Status{ErrorCode::callback_failed};
            }
        }
    }

    [[nodiscard]] Status fail(Status error) noexcept {
        if (fatal) return last_status.ok() ? error : last_status;
        if (error.ok()) error = Status{ErrorCode::invalid_value};
        if (last_status.ok()) last_status = error;
        fatal = true;
        should_close = true;
        fail_receipts(error);
        notify_error(error);
        if (!callback_observer_status.ok()) last_status = callback_observer_status;
        return last_status;
    }

    [[nodiscard]] Status transport_failure(Status error) noexcept {
        if (error.ok()) error = Status{ErrorCode::connection_closed};
        last_status = error;
        should_close = true;
        notify_error(error);
        if (!callback_observer_status.ok()) last_status = callback_observer_status;
        return error;
    }

    [[nodiscard]] Status enqueue(RecordId record_id, std::size_t frame_size,
        std::uint64_t sequence, std::uint64_t receipt_id, std::string_view msg_type,
        std::uint64_t now_ms, bool business) noexcept {
        if (tx_count >= tx_slots.size() || frame_size > config.max_pending_bytes - pending_bytes_count ||
            msg_type.size() > max_msg_type_bytes) {
            return Status{ErrorCode::queue_full};
        }
        if (tx_count == 0 && !replay_active && replay_count == 0) {
            pending_since_ms = now_ms;
            last_byte_progress_ms = now_ms;
            write_timer_active = true;
        }
        const auto tail = tx_index(tx_count);
        TxDescriptor descriptor{};
        descriptor.record_id = record_id;
        descriptor.frame_size = frame_size;
        descriptor.sequence = sequence;
        descriptor.receipt_id = receipt_id;
        descriptor.appended_at_ms = now_ms;
        descriptor.msg_type_size = static_cast<std::uint8_t>(msg_type.size());
        std::memcpy(descriptor.msg_type.data(), msg_type.data(), msg_type.size());
        descriptor.business = business;
        tx_slots[tail] = descriptor;
        ++tx_count;
        pending_bytes_count += frame_size;
        if (business) {
            ++business_count;
            business_bytes += frame_size;
        } else {
            ++admin_count;
            admin_bytes += frame_size;
        }
        return Status::success();
    }

    [[nodiscard]] Status pop_tx(bool written, std::uint64_t now_ms, Status failure = {}) noexcept {
        auto* descriptor = front_tx();
        if (descriptor == nullptr) return Status{ErrorCode::invalid_value};
        const auto message_type = std::string_view{descriptor->msg_type.data(), descriptor->msg_type_size};
        if (written) {
            update_receipt(descriptor->receipt_id, ReceiptState::written);
        } else if (!failure.ok()) {
            update_receipt(descriptor->receipt_id, ReceiptState::failed, failure);
        }
        pending_bytes_count -= descriptor->frame_size;
        if (descriptor->business) {
            --business_count;
            business_bytes -= descriptor->frame_size;
        } else {
            --admin_count;
            admin_bytes -= descriptor->frame_size;
        }
        tx_head = (tx_head + 1) % tx_slots.size();
        --tx_count;
        if (written) {
            detail::CoreEvent event{};
            event.kind = detail::CoreEventKind::outbound_written;
            event.now_ms = now_ms;
            event.sequence = descriptor->sequence;
            event.msg_type = message_type;
            const auto output = core.process(event);
            if (!output.status.ok()) return fail(output.status);
            for (std::size_t i = 0; i < output.action_count; ++i) {
                if (output.actions[i].kind == detail::CoreActionKind::close_transport) should_close = true;
            }
        }
        if (tx_count == 0 && !replay_active && replay_count == 0) write_timer_active = false;
        maybe_notify_capacity();
        return fatal ? last_status : Status::success();
    }

    void maybe_notify_capacity() noexcept {
        if (!capacity_blocked) return;
        const auto message_limit = config.max_pending_messages - config.admin_reserve_messages;
        const auto byte_limit = config.max_pending_bytes - config.admin_reserve_bytes;
        if (business_count > message_limit / 2 || business_bytes > byte_limit / 2) return;
        capacity_blocked = false;
        if (callbacks.on_capacity) {
            try {
                callbacks.on_capacity();
            } catch (...) {
                (void)fail(Status{ErrorCode::callback_failed});
            }
        }
    }

    [[nodiscard]] Status make_admin_and_append(std::string_view msg_type,
        std::span<const AdminField> fields, std::uint64_t now_ms, std::string_view wall_time,
        bool is_logon_reply = false, bool poss_dup = false, std::string_view orig_time = {},
        std::uint64_t forced_sequence = 0, bool persist = true, RecordId dependency = 0) noexcept {
        const auto sequence = forced_sequence == 0 ? core.next_outbound() : forced_sequence;
        if (sequence == 0) return Status{ErrorCode::too_large, 34};
        AdminMessageStorage storage{};
        if (const auto status = make_admin_view(msg_type, fields, storage); !status.ok()) return status;
        SessionHeader header{config.local_comp_id, config.remote_comp_id, sequence, wall_time,
            poss_dup, orig_time};
        const auto encoded = encode_fix44(storage.view, header, encode_buffer);
        if (!encoded.ok()) return encoded.status();
        if (encoded.value() > config.max_frame_bytes) return Status{ErrorCode::too_large};
        const auto replay_reserve = replay_reservation_bytes();
        if (tx_count == tx_slots.size() || admin_count >= config.admin_reserve_messages ||
            encoded.value() > config.admin_reserve_bytes - admin_bytes ||
            replay_reserve > config.admin_reserve_bytes - admin_bytes - encoded.value() ||
            encoded.value() > config.max_pending_bytes - pending_bytes_count ||
            replay_reserve > config.max_pending_bytes - pending_bytes_count - encoded.value()) {
            return Status{ErrorCode::queue_full};
        }
        RecordId record_id = 0;
        if (persist) {
            const auto appended = journal.append(RecordKind::outbound, sequence, 0, dependency,
                std::string_view{encode_buffer.data(), encoded.value()}, 0);
            if (!appended.ok()) return appended.status();
            record_id = appended.value().id;
        }
        if (const auto status = enqueue(record_id, encoded.value(), sequence, 0, msg_type,
                now_ms, false); !status.ok()) {
            if (persist) return fail(Status{ErrorCode::protocol_error, 34});
            return status;
        }
        if (persist) {
            detail::CoreEvent event{};
            event.kind = detail::CoreEventKind::outbound_appended;
            event.now_ms = now_ms;
            event.sequence = sequence;
            event.msg_type = msg_type;
            const auto output = core.process(event);
            if (!output.status.ok()) return output.status;
        }
        if (is_logon_reply) {
            detail::CoreEvent event{};
            event.kind = detail::CoreEventKind::logon_reply_appended;
            event.now_ms = now_ms;
            const auto output = core.process(event);
            if (!output.status.ok()) return output.status;
            if (const auto status = apply_actions(output, nullptr, 0, 0, now_ms, wall_time, 1); !status.ok()) return status;
        }
        return Status::success();
    }

    [[nodiscard]] Status send_admin_action(const detail::CoreAction& action,
        std::uint64_t now_ms, std::string_view wall_time, RecordId dependency = 0) noexcept {
        std::array<AdminField, admin_field_capacity> fields{};
        std::size_t count = 0;
        const auto add_unsigned = [&](std::uint32_t tag, std::uint64_t value) {
            fields[count++] = AdminField{tag, FieldKind::unsigned_integer, value, {}};
        };
        const auto add_text = [&](std::uint32_t tag, std::string_view value) {
            fields[count++] = AdminField{tag, FieldKind::string, 0, value};
        };

        std::string_view msg_type;
        bool logon_reply = false;
        switch (action.kind) {
        case detail::CoreActionKind::send_logon: {
            msg_type = "A";
            add_unsigned(98, 0);
            const auto seconds = std::max<std::uint64_t>(1,
                (static_cast<std::uint64_t>(config.heartbeat_ms) + 999) / 1000);
            add_unsigned(108, seconds);
            logon_reply = config.role == SessionRole::acceptor && action.first == 0;
            break;
        }
        case detail::CoreActionKind::send_logout:
            msg_type = "5";
            if (!action.text.empty()) add_text(58, action.text);
            break;
        case detail::CoreActionKind::send_heartbeat:
            msg_type = "0";
            if (!action.text.empty()) add_text(112, action.text);
            break;
        case detail::CoreActionKind::send_test_request:
            msg_type = "1";
            add_text(112, action.text);
            break;
        case detail::CoreActionKind::send_resend_request:
            msg_type = "2";
            add_unsigned(7, action.first);
            add_unsigned(16, action.last);
            break;
        default:
            return Status{ErrorCode::invalid_value};
        }
        return make_admin_and_append(msg_type, std::span<const AdminField>{fields.data(), count},
            now_ms, wall_time, logon_reply, false, {}, 0, true, dependency);
    }

    [[nodiscard]] Status queue_replay(std::uint64_t begin, std::uint64_t end,
        RecordId dependency) noexcept {
        const auto next_out = journal.next_outbound_seq();
        if (next_out <= 1) return Status::success();
        const auto maximum = next_out - 1;
        if (end == 0 || end > maximum) end = maximum;
        if (begin == 0 || begin > end) return Status{ErrorCode::invalid_value, 7};
        if (replay_active && dependency == replay_dependency &&
            begin <= replay_end_queued() && end >= replay_begin_queued()) {
            if (begin < replay_next) begin = replay_next;
            if (end > replay_end) replay_end = end;
            return Status::success();
        }
        if (replay_count != 0) {
            const auto last_index = (replay_head + replay_count - 1) % replay_slots.size();
            auto& last = replay_slots[last_index];
            if (last.dependency == dependency &&
                begin <= last.end + (last.end != std::numeric_limits<std::uint64_t>::max()) && end >= last.begin) {
                last.begin = std::min(last.begin, begin);
                last.end = std::max(last.end, end);
                return Status::success();
            }
        }
        if (admin_bytes > config.admin_reserve_bytes ||
            config.max_frame_bytes > config.admin_reserve_bytes - admin_bytes) {
            return Status{ErrorCode::queue_full};
        }
        if (replay_count == replay_slots.size()) return Status{ErrorCode::queue_full};
        replay_slots[(replay_head + replay_count) % replay_slots.size()] = ReplayRange{begin, end, dependency};
        ++replay_count;
        if (!write_timer_active) {
            pending_since_ms = callback_now_ms;
            last_byte_progress_ms = callback_now_ms;
            write_timer_active = true;
        }
        return Status::success();
    }

    [[nodiscard]] std::uint64_t replay_begin_queued() const noexcept {
        return replay_active ? replay_next : 0;
    }

    [[nodiscard]] std::uint64_t replay_end_queued() const noexcept {
        return replay_active ? replay_end : 0;
    }

    void start_next_replay() noexcept {
        if (replay_active || replay_count == 0) return;
        const auto range = replay_slots[replay_head];
        replay_head = (replay_head + 1) % replay_slots.size();
        --replay_count;
        replay_next = range.begin;
        replay_end = range.end;
        replay_dependency = range.dependency;
        replay_active = true;
        replay_prepared = false;
    }

    [[nodiscard]] Status parse_frame(std::string_view bytes, FlatMessageView& message) noexcept {
        if (bytes.size() > config.max_frame_bytes) return Status{ErrorCode::too_large};
        FrameView frame{};
        if (const auto status = inspect_frame(bytes, frame, {}, CodecLimits{config.max_frame_bytes}); !status.ok()) {
            return status;
        }
        if (frame.consumed != bytes.size()) return Status{ErrorCode::invalid_frame};
        return parse_flat(frame, parse_fields, message, {}, CodecLimits{config.max_frame_bytes});
    }

    [[nodiscard]] Status parse_inbound_header(const FlatMessageView& message,
        std::string_view& msg_type, std::uint64_t& sequence, bool& poss_dup,
        bool& original_time_valid, std::string_view& test_id) const noexcept {
        bool duplicate = false;
        const auto* type = unique_field(message, 35, duplicate);
        if (duplicate || type == nullptr || type->value.empty()) return Status{ErrorCode::invalid_header, 35};
        const auto* seq = unique_field(message, 34, duplicate);
        if (duplicate || seq == nullptr || !parse_unsigned(seq->value, sequence) || sequence == 0) {
            return Status{ErrorCode::invalid_header, 34};
        }
        const auto* sender = unique_field(message, 49, duplicate);
        if (duplicate || sender == nullptr || sender->value != config.remote_comp_id) {
            return Status{ErrorCode::identity_mismatch, 49};
        }
        const auto* target = unique_field(message, 56, duplicate);
        if (duplicate || target == nullptr || target->value != config.local_comp_id) {
            return Status{ErrorCode::identity_mismatch, 56};
        }
        const auto* sending_time = unique_field(message, 52, duplicate);
        if (duplicate || sending_time == nullptr || !detail::is_valid_utc_timestamp(sending_time->value)) {
            return Status{ErrorCode::invalid_timestamp, 52};
        }
        const auto* poss = unique_field(message, 43, duplicate);
        if (duplicate) return Status{ErrorCode::duplicate_field, 43};
        poss_dup = poss != nullptr && poss->value == "Y";
        if (poss != nullptr && poss->value != "Y" && poss->value != "N") {
            return Status{ErrorCode::invalid_value, 43};
        }
        const auto* original = unique_field(message, 122, duplicate);
        if (duplicate) return Status{ErrorCode::duplicate_field, 122};
        original_time_valid = original != nullptr && detail::is_valid_utc_timestamp(original->value);
        if (poss_dup && (!original_time_valid || original->value > sending_time->value)) {
            return Status{ErrorCode::invalid_timestamp, 122};
        }
        if (!poss_dup && original != nullptr) return Status{ErrorCode::invalid_header, 122};
        msg_type = type->value;
        test_id = {};
        if (msg_type == "1" || msg_type == "0") {
            const auto* test = unique_field(message, 112, duplicate);
            if (duplicate) return Status{ErrorCode::duplicate_field, 112};
            if (test != nullptr) test_id = test->value;
            if (msg_type == "1" && test == nullptr) return Status{ErrorCode::missing_field, 112};
        }
        return Status::success();
    }

    [[nodiscard]] Status build_replay_frame() noexcept {
        if (!replay_active || replay_prepared) return Status::success();
        while (replay_next <= replay_end) {
            const auto* record = journal.find_outbound(replay_next);
            if (record == nullptr || record->payload.empty()) return Status{ErrorCode::corrupt_store, 34};
            FlatMessageView parsed{};
            if (const auto status = parse_frame(record->payload, parsed); !status.ok()) return status;
            bool duplicate = false;
            const auto* type = unique_field(parsed, 35, duplicate);
            if (duplicate || type == nullptr) return Status{ErrorCode::corrupt_store, 35};
            if (type->value.size() > max_msg_type_bytes) return Status{ErrorCode::unsupported};
            if (!is_session_admin(type->value)) {
                const auto* time = unique_field(parsed, 52, duplicate);
                if (duplicate || time == nullptr) return Status{ErrorCode::corrupt_store, 52};
                const auto* original = unique_field(parsed, 122, duplicate);
                if (duplicate) return Status{ErrorCode::corrupt_store, 122};
                const auto original_time = original == nullptr ? time->value : original->value;
                SessionHeader header{config.local_comp_id, config.remote_comp_id,
                    record->seq, callback_wall_time, true, original_time};
                const auto encoded = encode_fix44_replay(parsed, header, replay_buffer);
                if (!encoded.ok()) return encoded.status();
                replay_bytes = encoded.value();
                replay_offset = 0;
                replay_sequence = record->seq;
                replay_advance = record->seq + 1;
                replay_receipt_id = record->receipt;
                replay_msg_type_size = static_cast<std::uint8_t>(type->value.size());
                std::memcpy(replay_msg_type.data(), type->value.data(), type->value.size());
                replay_prepared = true;
                return Status::success();
            }

            const auto gap_begin = replay_next;
            auto gap_end = replay_next + 1;
            std::string_view original_time{};
            const auto* first_time = unique_field(parsed, 52, duplicate);
            if (duplicate || first_time == nullptr) return Status{ErrorCode::corrupt_store, 52};
            const auto* first_original = unique_field(parsed, 122, duplicate);
            if (duplicate) return Status{ErrorCode::corrupt_store, 122};
            original_time = first_original == nullptr ? first_time->value : first_original->value;
            while (gap_end <= replay_end) {
                const auto* next_record = journal.find_outbound(gap_end);
                if (next_record == nullptr || next_record->payload.empty()) return Status{ErrorCode::corrupt_store, 34};
                FlatMessageView next_parsed{};
                if (const auto status = parse_frame(next_record->payload, next_parsed); !status.ok()) return status;
                const auto* next_type = unique_field(next_parsed, 35, duplicate);
                if (duplicate || next_type == nullptr) return Status{ErrorCode::corrupt_store, 35};
                if (!is_session_admin(next_type->value)) break;
                ++gap_end;
            }
            std::array<AdminField, 2> gap_fields{
                AdminField{123, FieldKind::character, static_cast<unsigned char>('Y'), {}},
                AdminField{36, FieldKind::unsigned_integer, gap_end, {}},
            };
            AdminMessageStorage storage{};
            if (const auto status = make_admin_view("4", gap_fields, storage); !status.ok()) return status;
            SessionHeader header{config.local_comp_id, config.remote_comp_id,
                gap_begin, callback_wall_time, true, original_time};
            const auto encoded = encode_fix44(storage.view, header, replay_buffer);
            if (!encoded.ok()) return encoded.status();
            replay_bytes = encoded.value();
            replay_offset = 0;
            replay_sequence = gap_begin;
            replay_advance = gap_end;
            replay_receipt_id = 0;
            replay_msg_type_size = 1;
            replay_msg_type[0] = '4';
            replay_prepared = true;
            return Status::success();
        }
        if (const auto status = finish_replay_range(); !status.ok()) return status;
        start_next_replay();
        return build_replay_frame();
    }

    [[nodiscard]] Status finish_replay_range() noexcept {
        const auto dependency = replay_dependency;
        replay_active = false;
        replay_prepared = false;
        replay_offset = 0;
        replay_bytes = 0;
        replay_receipt_id = 0;
        replay_dependency = 0;
        if (dependency != 0) {
            const auto* inbound = journal.find_record(dependency);
            if (inbound == nullptr || inbound->kind != RecordKind::inbound) return Status{ErrorCode::corrupt_store};
            const auto sequence = inbound->seq;
            if (const auto status = append_delivery(dependency, sequence); !status.ok()) return status;
            if (core.state() == SessionState::active) {
                return deliver_recovery(callback_now_ms, callback_wall_time);
            }
        }
        return Status::success();
    }

    [[nodiscard]] Status queue_replay_range(std::uint64_t begin, std::uint64_t end,
        RecordId dependency = 0) noexcept {
        const auto next_outbound = journal.next_outbound_seq();
        const auto maximum = next_outbound == 0 ? 0 : next_outbound - 1;
        if (end == 0 || end > maximum) end = maximum;
        if (maximum == 0 || begin > end) {
            if (dependency == 0) return Status::success();
            const auto* inbound = journal.find_record(dependency);
            if (inbound == nullptr || inbound->kind != RecordKind::inbound) {
                return Status{ErrorCode::corrupt_store};
            }
            return append_delivery(dependency, inbound->seq);
        }
        const auto status = queue_replay(begin, end, dependency);
        if (!status.ok()) return status;
        start_next_replay();
        if (!write_timer_active && (replay_active || replay_count != 0)) {
            pending_since_ms = callback_now_ms;
            last_byte_progress_ms = callback_now_ms;
            write_timer_active = true;
        }
        return Status::success();
    }

    [[nodiscard]] Status append_delivery(RecordId inbound_id, std::uint64_t sequence) noexcept {
        if (journal.has_delivery(inbound_id)) {
            forget_recovery(inbound_id);
            return Status::success();
        }
        const auto appended = journal.append(RecordKind::delivery, sequence, 0, inbound_id, {});
        if (!appended.ok()) return appended.status();
        forget_recovery(inbound_id);
        return Status::success();
    }

    void forget_recovery(RecordId inbound_id) noexcept {
        for (std::size_t i = 0; i < recovery_count; ++i) {
            if (recovery_slots[i].id != inbound_id) continue;
            recovery_bytes -= recovery_slots[i].bytes;
            for (std::size_t move = i + 1; move < recovery_count; ++move) {
                recovery_slots[move - 1] = recovery_slots[move];
            }
            --recovery_count;
            recovery_slots[recovery_count] = RecoverySlot{};
            break;
        }
    }

    [[nodiscard]] Status remember_inbound(RecordId id, std::size_t bytes) noexcept {
        if (recovery_count == recovery_slots.size() || bytes > config.max_pending_inbound_bytes - recovery_bytes) {
            return Status{ErrorCode::queue_full};
        }
        recovery_slots[recovery_count++] = RecoverySlot{id, bytes};
        recovery_bytes += bytes;
        return Status::success();
    }

    [[nodiscard]] Status deliver_message(const FlatMessageView& message, DeliveryInfo info,
        std::uint64_t now_ms, std::string_view wall_time) noexcept {
        if (!callbacks.on_message) return fail(Status{ErrorCode::callback_failed});
        SessionContext context{*public_owner, now_ms, wall_time, info};
        Status callback_status{};
        callback_active = true;
        callback_now_ms = now_ms;
        callback_wall_time = wall_time;
        try {
            callback_status = callbacks.on_message(context, message, info);
        } catch (...) {
            callback_status = Status{ErrorCode::callback_failed};
        }
        callback_active = false;
        if (!callback_status.ok()) return fail(Status{ErrorCode::callback_failed,
            callback_status.tag, callback_status.offset});
        if (info.is_recovery && dependent_application_replies(info.record_id) > context.send_ordinal_) {
            return fail(Status{ErrorCode::callback_failed});
        }
        const auto completed = append_delivery(info.record_id, info.msg_seq_num);
        return completed.ok() ? completed : fail(completed);
    }

    [[nodiscard]] Status run_active(std::uint64_t now_ms, std::string_view wall_time) noexcept {
        if (callbacks.on_active) {
            SessionContext context{*public_owner, now_ms, wall_time, {}};
            Status callback_status{};
            callback_active = true;
            callback_now_ms = now_ms;
            callback_wall_time = wall_time;
            try {
                callback_status = callbacks.on_active(context);
            } catch (...) {
                callback_status = Status{ErrorCode::callback_failed};
            }
            callback_active = false;
            if (!callback_status.ok()) return fail(Status{ErrorCode::callback_failed,
                callback_status.tag, callback_status.offset});
        }
        if (core.state() != SessionState::active) return Status::success();
        return deliver_recovery(now_ms, wall_time);
    }

    [[nodiscard]] Status process_recovered_admin(const FlatMessageView& message,
        RecordId id, std::uint64_t seq, std::uint64_t now_ms, std::string_view wall_time) noexcept {
        bool duplicate = false;
        const auto* type = unique_field(message, 35, duplicate);
        if (duplicate || type == nullptr) return Status{ErrorCode::corrupt_store, 35};
        if (type->value == "2") {
            const auto* begin_field = unique_field(message, 7, duplicate);
            if (duplicate || begin_field == nullptr) return Status{ErrorCode::corrupt_store, 7};
            const auto* end_field = unique_field(message, 16, duplicate);
            if (duplicate || end_field == nullptr) return Status{ErrorCode::corrupt_store, 16};
            std::uint64_t begin{}, end{};
            if (!parse_unsigned(begin_field->value, begin) || !parse_unsigned(end_field->value, end)) {
                return Status{ErrorCode::corrupt_store, 7};
            }
            if (const auto status = queue_replay_range(begin, end, id); !status.ok()) return status;
            return Status::success(); // The replay range appends delivery after its last byte is written.
        } else if (type->value == "1") {
            const auto* request_id = unique_field(message, 112, duplicate);
            if (duplicate || request_id == nullptr) return Status{ErrorCode::corrupt_store, 112};
            if (!has_dependent_outbound(id)) {
                detail::CoreAction action{detail::CoreActionKind::send_heartbeat, 0, 0, request_id->value};
                if (const auto status = send_admin_action(action, now_ms, wall_time, id); !status.ok()) return status;
            }
        } else if (type->value == "5") {
            const auto* text = unique_field(message, 58, duplicate);
            if (duplicate) return Status{ErrorCode::corrupt_store, 58};
            if (!has_dependent_outbound(id)) {
                detail::CoreAction action{detail::CoreActionKind::send_logout, 0, 0,
                    text == nullptr ? std::string_view{} : text->value};
                if (const auto status = send_admin_action(action, now_ms, wall_time, id); !status.ok()) return status;
            }
        }
        return append_delivery(id, seq);
    }

    [[nodiscard]] bool has_dependent_outbound(RecordId inbound_id) const noexcept {
        for (const auto& record : journal.records()) {
            if (record.kind == RecordKind::outbound && record.dependency == inbound_id) return true;
        }
        return false;
    }

    [[nodiscard]] std::size_t dependent_application_replies(RecordId inbound_id) const noexcept {
        std::size_t count = 0;
        for (const auto& record : journal.records()) {
            if (record.kind == RecordKind::outbound && record.dependency == inbound_id && record.receipt != 0) {
                ++count;
            }
        }
        return count;
    }

    [[nodiscard]] Status deliver_recovery(std::uint64_t now_ms, std::string_view wall_time) noexcept {
        std::size_t index = 0;
        while (index < recovery_count) {
            const auto entry = recovery_slots[index];
            if (entry.id == 0 || journal.has_delivery(entry.id)) {
                ++index;
                continue;
            }
            const auto* record = journal.find_record(entry.id);
            if (record == nullptr || record->kind != RecordKind::inbound) return fail(Status{ErrorCode::corrupt_store});
            const auto record_id = record->id;
            const auto sequence = record->seq;
            const auto payload = record->payload;
            FlatMessageView message{};
            if (const auto status = parse_frame(payload, message); !status.ok()) return fail(status);
            bool duplicate = false;
            const auto* type = unique_field(message, 35, duplicate);
            if (duplicate || type == nullptr) return fail(Status{ErrorCode::corrupt_store, 35});
            if (is_session_admin(type->value)) {
                if (type->value != "A") {
                    if (const auto status = process_recovered_admin(message, record_id,
                            sequence, now_ms, wall_time); !status.ok()) return fail(status);
                } else if (const auto status = append_delivery(record_id, sequence); !status.ok()) {
                    return fail(status);
                }
            } else {
                if (const auto status = deliver_message(message,
                        DeliveryInfo{record_id, sequence, true}, now_ms, wall_time); !status.ok()) return status;
            }
            // ResendRequest completion is deferred until its replay range is
            // fully written. Preserve journal ordering and stop until flush.
            if (!journal.has_delivery(entry.id)) break;
        }
        return Status::success();
    }

    [[nodiscard]] Result<SendReceipt> send_application(detail::MessageView message,
        std::uint64_t now_ms, std::string_view wall_time, RecordId dependency,
        std::size_t dependency_ordinal, bool recovery) noexcept {
        if (fatal) return last_status.ok() ? Status{ErrorCode::engine_stopped} : last_status;
        if (!is_active()) return Status{ErrorCode::not_ready};
        if (message.msg_type.empty() || is_session_admin(message.msg_type)) {
            return Status{ErrorCode::managed_field, 35};
        }
        if (message.msg_type.size() > max_msg_type_bytes) return Status{ErrorCode::unsupported, 35};
        if (dependency != 0 && recovery) {
            std::size_t ordinal = 0;
            for (const auto& record : journal.records()) {
                if (record.kind != RecordKind::outbound || record.dependency != dependency || record.receipt == 0) continue;
                if (ordinal++ != dependency_ordinal) continue;
                FlatMessageView original{};
                if (const auto status = parse_frame(record.payload, original); !status.ok()) {
                    return Status{ErrorCode::corrupt_store};
                }
                bool duplicate = false;
                const auto* original_time = unique_field(original, 52, duplicate);
                if (duplicate || original_time == nullptr) return Status{ErrorCode::corrupt_store, 52};
                const SessionHeader original_header{config.local_comp_id, config.remote_comp_id,
                    record.seq, original_time->value};
                const auto candidate = encode_fix44(message, original_header, encode_buffer);
                if (!candidate.ok()) return candidate.status();
                if (candidate.value() != record.payload.size() ||
                    std::memcmp(encode_buffer.data(), record.payload.data(), candidate.value()) != 0) {
                    return Status{ErrorCode::invalid_value};
                }
                const auto& stored = receipt_slots[static_cast<std::size_t>(record.receipt % receipt_slots.size())];
                if (stored.occupied && stored.value.id == record.receipt) return stored.value;
                return SendReceipt{record.receipt, record.seq, ReceiptState::expired,
                    Status{ErrorCode::receipt_expired}};
            }
        }
        const auto message_limit = config.max_pending_messages - config.admin_reserve_messages;
        const auto byte_limit = config.max_pending_bytes - config.admin_reserve_bytes;
        if (business_count >= message_limit || tx_count >= tx_slots.size() ||
            business_bytes >= byte_limit || pending_bytes_count + replay_reservation_bytes() >=
                config.max_pending_bytes) {
            capacity_blocked = true;
            return Status{ErrorCode::queue_full};
        }
        if (next_receipt_id == 0 || next_receipt_id == std::numeric_limits<std::uint64_t>::max()) {
            return Status{ErrorCode::too_large};
        }
        const auto sequence = core.next_outbound();
        SessionHeader header{config.local_comp_id, config.remote_comp_id, sequence, wall_time};
        const auto encoded = encode_fix44(message, header, encode_buffer);
        if (!encoded.ok()) return encoded.status();
        if (encoded.value() > config.max_frame_bytes ||
            encoded.value() > byte_limit - business_bytes ||
            encoded.value() > config.max_pending_bytes - pending_bytes_count ||
            replay_reservation_bytes() > config.max_pending_bytes - pending_bytes_count - encoded.value()) {
            capacity_blocked = true;
            return Status{ErrorCode::queue_full};
        }
        const auto receipt_id = next_receipt_id;
        const auto appended = journal.append(RecordKind::outbound, sequence, receipt_id, dependency,
            std::string_view{encode_buffer.data(), encoded.value()});
        if (!appended.ok()) return fail(appended.status());
        if (const auto status = enqueue(appended.value().id, encoded.value(), sequence,
                receipt_id, message.msg_type, now_ms, true); !status.ok()) {
            return fail(Status{ErrorCode::protocol_error, 34});
        }
        put_receipt(receipt_id, sequence, ReceiptState::appended);
        ++next_receipt_id;
        detail::CoreEvent event{};
        event.kind = detail::CoreEventKind::outbound_appended;
        event.now_ms = now_ms;
        event.sequence = sequence;
        event.msg_type = message.msg_type;
        const auto output = core.process(event);
        if (!output.status.ok()) return fail(output.status);
        return SendReceipt{receipt_id, sequence, ReceiptState::appended, Status::success()};
    }

    void clear_transmit_state() noexcept {
        for (std::size_t i = 0; i < tx_count; ++i) {
            auto& descriptor = tx_slots[tx_index(i)];
            descriptor.offset = 0;
            // An appended receipt remains accepted; the peer will decide which
            // original sequences it needs after the next Logon.
        }
        tx_head = 0;
        tx_count = 0;
        pending_bytes_count = 0;
        business_count = 0;
        business_bytes = 0;
        admin_count = 0;
        admin_bytes = 0;
        replay_head = 0;
        replay_count = 0;
        replay_active = false;
        replay_prepared = false;
        replay_offset = 0;
        replay_bytes = 0;
        replay_receipt_id = 0;
        write_timer_active = false;
        stall_logout_attempted = false;
        stall_logout_deadline = 0;
    }

    [[nodiscard]] Status execute_core_output(const detail::CoreOutput& output,
        const FlatMessageView* message, RecordId inbound_id, std::uint64_t inbound_seq,
        std::uint64_t now_ms, std::string_view wall_time) noexcept {
        callback_now_ms = now_ms;
        callback_wall_time = wall_time;
        return apply_actions(output, message, inbound_id, inbound_seq, now_ms, wall_time);
    }

    [[nodiscard]] Status on_connected(std::uint64_t now_ms, std::string_view wall_time) noexcept {
        if (fatal) return last_status.ok() ? Status{ErrorCode::engine_stopped} : last_status;
        if (const auto status = set_wall_time(wall_time); !status.ok()) return status;
        callback_now_ms = now_ms;
        clear_transmit_state();
        last_status = Status::success();
        detail::CoreEvent event{};
        event.kind = detail::CoreEventKind::connected;
        event.now_ms = now_ms;
        const auto output = core.process(event);
        if (!output.status.ok()) return output.status;
        return execute_core_output(output, nullptr, 0, 0, now_ms, callback_wall_time);
    }

    [[nodiscard]] Status on_disconnected(Status reason, std::uint64_t now_ms) noexcept {
        detail::CoreEvent event{};
        event.kind = detail::CoreEventKind::disconnected;
        event.now_ms = now_ms;
        (void)core.process(event);
        clear_transmit_state();
        should_close = false;
        if (!reason.ok() && reason.code != ErrorCode::connection_closed) {
            const bool already_reported = last_status.code == reason.code &&
                last_status.tag == reason.tag && last_status.offset == reason.offset;
            last_status = reason;
            if (!already_reported) notify_error(reason);
            if (!callback_observer_status.ok()) last_status = callback_observer_status;
        }
        return Status::success();
    }

    [[nodiscard]] Status on_tick(std::uint64_t now_ms, std::string_view wall_time) noexcept {
        if (fatal) return last_status.ok() ? Status{ErrorCode::engine_stopped} : last_status;
        if (const auto status = set_wall_time(wall_time); !status.ok()) return status;
        callback_now_ms = now_ms;
        if (write_timer_active && config.write_stall_ms != 0 &&
            now_ms >= last_byte_progress_ms && now_ms - last_byte_progress_ms >= config.write_stall_ms) {
            if (!stall_logout_attempted && core.state() == SessionState::active) {
                stall_logout_attempted = true;
                stall_logout_deadline = saturating_add(now_ms, config.logout_timeout_ms);
                detail::CoreEvent logout_event{};
                logout_event.kind = detail::CoreEventKind::local_logout;
                logout_event.now_ms = now_ms;
                logout_event.text = "Write stalled";
                const auto logout_output = core.process(logout_event);
                if (!logout_output.status.ok()) return fail(logout_output.status);
                if (const auto status = execute_core_output(logout_output, nullptr, 0, 0,
                        now_ms, callback_wall_time); !status.ok()) return status;
            } else if (!stall_logout_attempted) {
                return transport_failure(Status{ErrorCode::write_stalled});
            }
            if (stall_logout_attempted && now_ms >= stall_logout_deadline) {
                return transport_failure(Status{ErrorCode::write_stalled});
            }
        }
        if (config.max_pending_age_ms != 0 && tx_count != 0) {
            const auto* first = front_tx();
            if (first != nullptr && now_ms >= first->appended_at_ms &&
                now_ms - first->appended_at_ms >= config.max_pending_age_ms) {
                for (std::size_t i = 0; i < tx_count; ++i) {
                    const auto& descriptor = tx_slots[tx_index(i)];
                    if (descriptor.business) update_receipt(descriptor.receipt_id,
                        ReceiptState::expired, Status{ErrorCode::receipt_expired});
                }
                return fail(Status{ErrorCode::receipt_expired});
            }
        }
        detail::CoreEvent event{};
        event.kind = detail::CoreEventKind::tick;
        event.now_ms = now_ms;
        event.flag = tx_count != 0 || replay_active || replay_count != 0;
        const auto output = core.process(event);
        return execute_core_output(output, nullptr, 0, 0, now_ms, callback_wall_time);
    }

    [[nodiscard]] Status on_logout(std::string_view reason, std::uint64_t now_ms,
        std::string_view wall_time) noexcept {
        if (fatal) return last_status.ok() ? Status{ErrorCode::engine_stopped} : last_status;
        if (const auto status = set_wall_time(wall_time); !status.ok()) return status;
        callback_now_ms = now_ms;
        detail::CoreEvent event{};
        event.kind = detail::CoreEventKind::local_logout;
        event.now_ms = now_ms;
        event.text = reason;
        const auto output = core.process(event);
        return execute_core_output(output, nullptr, 0, 0, now_ms, callback_wall_time);
    }
    [[nodiscard]] Status apply_actions(const detail::CoreOutput& output,
        const FlatMessageView* delivery_message, RecordId inbound_id,
        std::uint64_t inbound_seq, std::uint64_t now_ms, std::string_view wall_time,
        std::size_t depth = 0) noexcept {
        if (depth > 8) return fail(Status{ErrorCode::protocol_error});
        if (!output.status.ok()) return fail(output.status);
        for (std::size_t i = 0; i < output.action_count; ++i) {
            const auto& action = output.actions[i];
            switch (action.kind) {
            case detail::CoreActionKind::send_logon:
            case detail::CoreActionKind::send_logout:
            case detail::CoreActionKind::send_heartbeat:
            case detail::CoreActionKind::send_test_request:
            case detail::CoreActionKind::send_resend_request: {
                if (const auto status = send_admin_action(action, now_ms, wall_time, inbound_id); !status.ok()) {
                    return fail(status);
                }
                break;
            }
            case detail::CoreActionKind::start_replay:
                if (const auto status = queue_replay_range(action.first, action.last, inbound_id); !status.ok()) {
                    return fail(status);
                }
                break;
            case detail::CoreActionKind::become_active:
                if (const auto status = run_active(now_ms, wall_time); !status.ok()) return status;
                break;
            case detail::CoreActionKind::deliver_message:
                if (delivery_message == nullptr || inbound_id == 0) return fail(Status{ErrorCode::protocol_error});
                if (const auto status = deliver_message(*delivery_message,
                        DeliveryInfo{inbound_id, inbound_seq, false}, now_ms, wall_time); !status.ok()) return status;
                break;
            case detail::CoreActionKind::close_transport:
                should_close = true;
                break;
            }
        }
        return Status::success();
    }

    [[nodiscard]] Status process_inbound(std::string_view frame_bytes, std::uint64_t now_ms,
        std::string_view wall_time, bool recovery = false) noexcept {
        FlatMessageView message{};
        if (const auto status = parse_frame(frame_bytes, message); !status.ok()) return fail(status);
        std::string_view msg_type;
        std::string_view test_id;
        std::uint64_t sequence{};
        bool poss_dup = false;
        bool orig_valid = false;
        if (const auto status = parse_inbound_header(message, msg_type, sequence,
                poss_dup, orig_valid, test_id); !status.ok()) return fail(status);

        std::uint64_t peer_heartbeat_ms = 0;
        std::uint64_t resend_begin = 0;
        std::uint64_t resend_end = 0;
        std::uint64_t next_inbound = 0;
        std::string_view logout_text{};
        if (msg_type == "A") {
            bool duplicate = false;
            const auto* encryption = unique_field(message, 98, duplicate);
            if (duplicate || encryption == nullptr || encryption->value != "0") {
                return fail(Status{ErrorCode::protocol_error, 98});
            }
            const auto* heartbeat = unique_field(message, 108, duplicate);
            std::uint64_t seconds{};
            if (duplicate || heartbeat == nullptr || !parse_unsigned(heartbeat->value, seconds) || seconds == 0 ||
                seconds > std::numeric_limits<std::uint32_t>::max() / 1000U) {
                return fail(Status{ErrorCode::protocol_error, 108});
            }
            peer_heartbeat_ms = seconds * 1000;
        } else if (msg_type == "2") {
            bool duplicate = false;
            const auto* begin_field = unique_field(message, 7, duplicate);
            if (duplicate || begin_field == nullptr || !parse_unsigned(begin_field->value, resend_begin)) {
                return fail(Status{ErrorCode::protocol_error, 7});
            }
            const auto* end_field = unique_field(message, 16, duplicate);
            if (duplicate || end_field == nullptr || !parse_unsigned(end_field->value, resend_end)) {
                return fail(Status{ErrorCode::protocol_error, 16});
            }
            if (resend_begin == 0 || (resend_end != 0 && resend_end < resend_begin)) {
                return fail(Status{ErrorCode::protocol_error, 7});
            }
        } else if (msg_type == "4") {
            bool duplicate = false;
            const auto* flag = unique_field(message, 123, duplicate);
            if (duplicate || flag == nullptr) return fail(Status{ErrorCode::missing_field, 123});
            const auto* new_seq = unique_field(message, 36, duplicate);
            if (duplicate || new_seq == nullptr || !parse_unsigned(new_seq->value, next_inbound) ||
                next_inbound <= sequence) {
                return fail(Status{ErrorCode::protocol_error, 36});
            }
            if (flag->value != "Y") {
                return fail(Status{ErrorCode::unsupported, 123});
            }
        } else if (msg_type == "5") {
            bool duplicate = false;
            const auto* text = unique_field(message, 58, duplicate);
            if (duplicate) return fail(Status{ErrorCode::duplicate_field, 58});
            if (text != nullptr) logout_text = text->value;
        }

        bool deferred_logon = recovery;
        if (msg_type == "A" && !deferred_logon && core.state() == SessionState::awaiting_logon &&
            sequence > core.next_inbound()) {
            if (deferred_logon_pending) {
                if (sequence == deferred_logon_sequence &&
                    frame_bytes == std::string_view{deferred_logon_buffer.data(), deferred_logon_size}) {
                    return Status::success();
                }
                return fail(Status{ErrorCode::protocol_error, 35});
            }
            if (frame_bytes.size() > deferred_logon_buffer.size()) return fail(Status{ErrorCode::too_large});
            std::memcpy(deferred_logon_buffer.data(), frame_bytes.data(), frame_bytes.size());
            deferred_logon_size = frame_bytes.size();
            deferred_logon_sequence = sequence;
            deferred_logon_pending = true;

            detail::CoreEvent candidate{};
            candidate.kind = detail::CoreEventKind::inbound_candidate;
            candidate.now_ms = now_ms;
            candidate.sequence = sequence;
            candidate.last = sequence - 1; // The high Logon is already in hand; request only its gap.
            candidate.msg_type = msg_type;
            candidate.flag = poss_dup;
            candidate.flag2 = orig_valid;
            const auto gap_output = core.process(candidate);
            if (!gap_output.status.ok() || gap_output.inbound != detail::InboundDisposition::gap) {
                deferred_logon_pending = false;
                return fail(gap_output.status.ok() ? Status{ErrorCode::protocol_error, 34} : gap_output.status);
            }
            detail::CoreEvent out_of_order{};
            out_of_order.kind = detail::CoreEventKind::out_of_order_logon;
            out_of_order.now_ms = now_ms;
            out_of_order.first = peer_heartbeat_ms;
            const auto logon_output = core.process(out_of_order);
            if (!logon_output.status.ok()) {
                deferred_logon_pending = false;
                return fail(logon_output.status);
            }
            callback_now_ms = now_ms;
            callback_wall_time = wall_time;
            if (const auto status = apply_actions(logon_output, nullptr, 0, 0,
                    now_ms, wall_time); !status.ok()) return status;
            return apply_actions(gap_output, nullptr, 0, 0, now_ms, wall_time);
        }
        if (msg_type == "A" && deferred_logon_pending && sequence == deferred_logon_sequence) {
            deferred_logon = true;
        }

        detail::CoreEvent candidate{};
        candidate.kind = detail::CoreEventKind::inbound_candidate;
        candidate.now_ms = now_ms;
        candidate.sequence = sequence;
        candidate.msg_type = msg_type;
        candidate.flag = poss_dup;
        candidate.flag2 = orig_valid;
        candidate.flag3 = deferred_logon;
        const auto disposition = core.process(candidate);
        if (!disposition.status.ok()) return fail(disposition.status);
        if (disposition.inbound == detail::InboundDisposition::duplicate) return Status::success();
        if (disposition.inbound == detail::InboundDisposition::gap) {
            callback_now_ms = now_ms;
            callback_wall_time = wall_time;
            return apply_actions(disposition, nullptr, 0, 0, now_ms, wall_time);
        }
        if (disposition.inbound != detail::InboundDisposition::accept) return fail(Status{ErrorCode::protocol_error});
        if (recovery_count == recovery_slots.size() ||
            frame_bytes.size() > config.max_pending_inbound_bytes - recovery_bytes) {
            return fail(Status{ErrorCode::queue_full});
        }

        const auto appended = journal.append(RecordKind::inbound, sequence, 0, 0,
            frame_bytes, next_inbound);
        if (!appended.ok()) return fail(appended.status());
        const auto inbound_id = appended.value().id;
        if (const auto status = remember_inbound(inbound_id, frame_bytes.size()); !status.ok()) return fail(status);

        detail::CoreEvent committed{};
        committed.kind = detail::CoreEventKind::inbound_committed;
        committed.now_ms = now_ms;
        committed.sequence = sequence;
        committed.next_sequence = appended.value().next_seq;
        committed.msg_type = msg_type;
        committed.first = peer_heartbeat_ms;
        committed.last = resend_end;
        committed.text = msg_type == "1" || msg_type == "0" ? test_id : logout_text;
        committed.flag = poss_dup;
        committed.flag2 = deferred_logon;
        committed.flag3 = orig_valid;
        if (msg_type == "2") committed.first = resend_begin;
        const auto actions = core.process(committed);
        if (!actions.status.ok()) return fail(actions.status);
        callback_now_ms = now_ms;
        callback_wall_time = wall_time;
        if (const auto status = apply_actions(actions,
                is_session_admin(msg_type) ? nullptr : &message,
                inbound_id, sequence, now_ms, wall_time); !status.ok()) return status;
        if (is_session_admin(msg_type) && msg_type != "2") {
            if (const auto status = append_delivery(inbound_id, sequence); !status.ok()) return fail(status);
        }
        if (deferred_logon) {
            deferred_logon_pending = false;
            deferred_logon_size = 0;
            deferred_logon_sequence = 0;
        } else if (deferred_logon_pending && core.next_inbound() == deferred_logon_sequence) {
            processing_deferred_logon = true;
            const auto deferred = std::string_view{deferred_logon_buffer.data(), deferred_logon_size};
            const auto status = process_inbound(deferred, now_ms, wall_time, true);
            processing_deferred_logon = false;
            if (!status.ok()) return status;
            deferred_logon_pending = false;
            deferred_logon_size = 0;
            deferred_logon_sequence = 0;
        } else if (deferred_logon_pending && core.next_inbound() > deferred_logon_sequence) {
            return fail(Status{ErrorCode::protocol_error, 34});
        }
        (void)processing_deferred_logon;
        return Status::success();
    }

    [[nodiscard]] Status finish_tx_frame(std::uint64_t now_ms) noexcept {
        if (!replay_active || !replay_prepared) return Status{ErrorCode::invalid_value};
        const auto written_sequence = replay_sequence;
        const auto written_advance = replay_advance;
        const auto written_receipt = replay_receipt_id;
        const auto written_type = std::string_view{replay_msg_type.data(), replay_msg_type_size};
        update_receipt(written_receipt, ReceiptState::written);
        detail::CoreEvent event{};
        event.kind = detail::CoreEventKind::outbound_written;
        event.now_ms = now_ms;
        event.sequence = written_sequence;
        event.msg_type = written_type;
        const auto output = core.process(event);
        if (!output.status.ok()) return fail(output.status);
        for (std::size_t i = 0; i < output.action_count; ++i) {
            if (output.actions[i].kind == detail::CoreActionKind::close_transport) should_close = true;
        }

        if (written_type == "4") {
            while (tx_count != 0) {
                const auto* front = front_tx();
                if (front == nullptr || front->sequence < written_sequence || front->sequence >= written_advance ||
                    front->business) break;
                detail::CoreEvent skipped{};
                skipped.kind = detail::CoreEventKind::outbound_skipped;
                skipped.sequence = front->sequence;
                skipped.msg_type = {front->msg_type.data(), front->msg_type_size};
                (void)core.process(skipped);
                if (const auto status = pop_tx(false, now_ms); !status.ok()) return status;
            }
        } else if (tx_count != 0) {
            const auto* front = front_tx();
            if (front != nullptr && front->sequence == written_sequence) {
                if (const auto status = pop_tx(false, now_ms); !status.ok()) return status;
            }
        }

        replay_next = written_advance;
        replay_prepared = false;
        replay_offset = 0;
        replay_bytes = 0;
        replay_receipt_id = 0;
        if (replay_next > replay_end) {
            if (const auto status = finish_replay_range(); !status.ok()) return fail(status);
            start_next_replay();
        }
        if (tx_count == 0 && !replay_active && replay_count == 0) write_timer_active = false;
        maybe_notify_capacity();
        return Status::success();
    }

    [[nodiscard]] Status on_flush(WriteFn write, void* context, std::uint64_t now_ms,
        std::size_t byte_budget) noexcept {
        if (write == nullptr) return Status{ErrorCode::invalid_config};
        if (fatal) return last_status.ok() ? Status{ErrorCode::engine_stopped} : last_status;
        if (core.state() == SessionState::disconnected || core.state() == SessionState::closed ||
            core.state() == SessionState::failed) return Status{ErrorCode::not_ready};
        callback_now_ms = now_ms;
        std::size_t written_this_turn = 0;
        while (written_this_turn < byte_budget && !should_close) {
            start_next_replay();
            if (replay_active && !replay_prepared) {
                if (const auto status = build_replay_frame(); !status.ok()) return fail(status);
                if (!replay_active) continue;
            }

            auto* queued = front_tx();
            const bool replay_due = replay_active && replay_prepared &&
                (queued == nullptr || (queued->offset == 0 && replay_sequence <= queued->sequence));
            if (!replay_due && queued == nullptr) break;

            std::span<const char> remaining;
            if (replay_due) {
                remaining = {replay_buffer.data() + replay_offset, replay_bytes - replay_offset};
            } else {
                const auto* record = journal.find_record(queued->record_id);
                if (record == nullptr || record->kind != RecordKind::outbound ||
                    record->payload.size() != queued->frame_size || queued->offset > queued->frame_size) {
                    return fail(Status{ErrorCode::corrupt_store});
                }
                remaining = {record->payload.data() + queued->offset, queued->frame_size - queued->offset};
            }
            const auto allowance = std::min(remaining.size(), byte_budget - written_this_turn);
            const auto result = write(context, remaining.first(allowance));
            if (result.bytes_written > allowance) return fail(Status{ErrorCode::protocol_error});
            if (result.bytes_written != 0) {
                written_this_turn += result.bytes_written;
                last_byte_progress_ms = now_ms;
                write_timer_active = true;
                stall_logout_attempted = false;
                stall_logout_deadline = 0;
                if (replay_due) replay_offset += result.bytes_written;
                else queued->offset += result.bytes_written;
            }
            if (replay_due && replay_offset == replay_bytes) {
                if (const auto status = finish_tx_frame(now_ms); !status.ok()) return status;
            } else if (!replay_due && queued->offset == queued->frame_size) {
                if (const auto status = pop_tx(true, now_ms); !status.ok()) return status;
                if (core.should_close()) should_close = true;
            }
            if (!result.status.ok()) return transport_failure(result.status);
            if (result.bytes_written == 0 || result.would_block) break;
        }
        return fatal ? last_status : Status::success();
    }

};

SessionDriver::SessionDriver(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {
    if (impl_) impl_->public_owner = this;
}

SessionDriver::SessionDriver(SessionDriver&& other) noexcept : impl_(std::move(other.impl_)) {
    if (impl_) impl_->public_owner = this;
}

SessionDriver& SessionDriver::operator=(SessionDriver&& other) noexcept {
    if (this == &other) return *this;
    impl_ = std::move(other.impl_);
    if (impl_) impl_->public_owner = this;
    return *this;
}

SessionDriver::~SessionDriver() = default;

Result<SessionDriver> SessionDriver::create(SessionConfig config, SessionCallbacks callbacks) noexcept {
    if (const auto status = validate_config(config); !status.ok()) return status;
    try {
        Status path_status{};
        auto path = journal_path_for(config, path_status);
        if (!path_status.ok()) return path_status;
        auto opened = Journal::open(JournalConfig{path, config.session_key,
            config.journal_capacity_bytes, config.journal_max_records, config.journal_test_hook});
        if (!opened.ok()) return opened.status();
        auto journal = std::move(opened).value();
        auto impl = std::make_unique<Impl>(std::move(config), std::move(callbacks), std::move(journal));
        if (impl->recovery_overflow) return Status{ErrorCode::queue_full};
        return SessionDriver{std::move(impl)};
    } catch (const std::bad_alloc&) {
        return Status{ErrorCode::capacity_exceeded};
    } catch (...) {
        return Status{ErrorCode::io_error};
    }
}

Status SessionDriver::connected(std::uint64_t now_ms, std::string_view wall_time) noexcept {
    if (!impl_) return Status{ErrorCode::engine_stopped};
    if (const auto status = impl_->claim_owner(); !status.ok()) return status;
    return impl_->on_connected(now_ms, wall_time);
}

Status SessionDriver::receive(std::string_view complete_frame, std::uint64_t now_ms,
    std::string_view wall_time) noexcept {
    if (!impl_) return Status{ErrorCode::engine_stopped};
    if (const auto status = impl_->claim_owner(); !status.ok()) return status;
    if (const auto status = impl_->set_wall_time(wall_time); !status.ok()) return status;
    impl_->callback_now_ms = now_ms;
    return impl_->process_inbound(complete_frame, now_ms, impl_->callback_wall_time);
}

Status SessionDriver::tick(std::uint64_t now_ms, std::string_view wall_time) noexcept {
    if (!impl_) return Status{ErrorCode::engine_stopped};
    if (const auto status = impl_->claim_owner(); !status.ok()) return status;
    return impl_->on_tick(now_ms, wall_time);
}

Status SessionDriver::disconnected(Status reason, std::uint64_t now_ms) noexcept {
    if (!impl_) return Status{ErrorCode::engine_stopped};
    if (const auto status = impl_->claim_owner(); !status.ok()) return status;
    return impl_->on_disconnected(reason, now_ms);
}

Status SessionDriver::logout(std::string_view reason, std::uint64_t now_ms,
    std::string_view wall_time) noexcept {
    if (!impl_) return Status{ErrorCode::engine_stopped};
    if (const auto status = impl_->claim_owner(); !status.ok()) return status;
    return impl_->on_logout(reason, now_ms, wall_time);
}

Result<SendReceipt> SessionDriver::send(detail::MessageView message,
    std::uint64_t now_ms, std::string_view wall_time) noexcept {
    return send_dependent(message, now_ms, wall_time, 0, 0, false);
}

Result<SendReceipt> SessionDriver::send_dependent(detail::MessageView message,
    std::uint64_t now_ms, std::string_view wall_time, RecordId dependency,
    std::size_t dependency_ordinal, bool recovery) noexcept {
    if (!impl_) return Status{ErrorCode::engine_stopped};
    if (const auto status = impl_->claim_owner(); !status.ok()) return status;
    if (dependency != 0 && !impl_->callback_active) return Status{ErrorCode::not_ready};
    if (const auto status = impl_->set_wall_time(wall_time); !status.ok()) return status;
    impl_->callback_now_ms = now_ms;
    return impl_->send_application(message, now_ms, impl_->callback_wall_time,
        dependency, dependency_ordinal, recovery);
}

Status SessionDriver::flush(WriteFn write, void* context, std::uint64_t now_ms,
    std::size_t byte_budget) noexcept {
    if (!impl_) return Status{ErrorCode::engine_stopped};
    if (const auto status = impl_->claim_owner(); !status.ok()) return status;
    return impl_->on_flush(write, context, now_ms, byte_budget);
}

Result<SendReceipt> SessionDriver::receipt(std::uint64_t id) const noexcept {
    if (!impl_ || id == 0 || impl_->receipt_slots.empty()) return Status{ErrorCode::receipt_expired};
    const auto& slot = impl_->receipt_slots[static_cast<std::size_t>(id % impl_->receipt_slots.size())];
    if (!slot.occupied || slot.value.id != id) return Status{ErrorCode::receipt_expired};
    return slot.value;
}

SessionState SessionDriver::state() const noexcept {
    if (!impl_) return SessionState::closed;
    return impl_->fatal ? SessionState::failed : impl_->core.state();
}

Status SessionDriver::status() const noexcept {
    if (!impl_) return Status{ErrorCode::engine_stopped};
    if (impl_->fatal) return impl_->last_status.ok() ? Status{ErrorCode::engine_stopped} : impl_->last_status;
    return impl_->last_status;
}

bool SessionDriver::needs_write() const noexcept {
    return impl_ && (impl_->tx_count != 0 || impl_->replay_active || impl_->replay_count != 0);
}

bool SessionDriver::should_close() const noexcept {
    return impl_ && (impl_->should_close || impl_->core.should_close());
}

bool SessionDriver::can_send() const noexcept {
    return impl_ && impl_->can_send_now();
}

std::uint64_t SessionDriver::next_deadline_ms() const noexcept {
    if (!impl_) return std::numeric_limits<std::uint64_t>::max();
    auto deadline = impl_->core.next_deadline();
    if (impl_->write_timer_active && impl_->config.write_stall_ms != 0) {
        deadline = std::min(deadline, saturating_add(impl_->last_byte_progress_ms,
            impl_->config.write_stall_ms));
    }
    if (impl_->stall_logout_attempted) deadline = std::min(deadline, impl_->stall_logout_deadline);
    if (impl_->config.max_pending_age_ms != 0 && impl_->tx_count != 0) {
        const auto* front = impl_->front_tx();
        if (front != nullptr) deadline = std::min(deadline, saturating_add(front->appended_at_ms,
            impl_->config.max_pending_age_ms));
    }
    return deadline;
}

std::uint64_t SessionDriver::next_inbound_seq() const noexcept {
    return impl_ ? impl_->core.next_inbound() : 0;
}

std::uint64_t SessionDriver::next_outbound_seq() const noexcept {
    return impl_ ? impl_->core.next_outbound() : 0;
}

std::size_t SessionDriver::pending_messages() const noexcept {
    if (!impl_) return 0;
    return impl_->tx_count + impl_->replay_count + (impl_->replay_active ? 1U : 0U);
}

std::size_t SessionDriver::pending_bytes() const noexcept {
    if (!impl_) return 0;
    const auto replay_bytes = impl_->replay_prepared ? impl_->replay_bytes - impl_->replay_offset : 0;
    return impl_->pending_bytes_count + replay_bytes;
}

Result<SendReceipt> SessionContext::send(detail::MessageView message) noexcept {
    if (driver_ == nullptr || send_ordinal_ == std::numeric_limits<std::size_t>::max()) {
        return Status{ErrorCode::engine_stopped};
    }
    auto result = driver_->send_dependent(message, now_ms_, wall_time_,
        delivery_.record_id, send_ordinal_, delivery_.is_recovery);
    if (result.ok()) ++send_ordinal_;
    return result;
}

Status SessionContext::logout(std::string_view reason) noexcept {
    if (driver_ == nullptr || !driver_->impl_ || !driver_->impl_->callback_active ||
        delivery_.record_id != 0) {
        return Status{ErrorCode::not_ready};
    }
    return driver_->logout(reason, now_ms_, wall_time_);
}

SessionState SessionContext::state() const noexcept {
    return driver_ == nullptr ? SessionState::closed : driver_->state();
}

} // namespace nimblefix
