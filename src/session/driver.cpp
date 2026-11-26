#include <nimblefix/session.hpp>
#include <nimblefix/sender.hpp>

#include "core.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <charconv>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <limits>
#include <new>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nimblefix {
namespace {

constexpr std::size_t admin_field_capacity = 32;
constexpr std::size_t max_msg_type_bytes = 32;

void secure_clear(std::span<char> bytes) noexcept {
    volatile char* output = bytes.data();
    for (std::size_t i = 0; i < bytes.size(); ++i) output[i] = 0;
}

void secure_clear(std::string& value) noexcept {
    secure_clear(std::span<char>{value.data(), value.size()});
    value.clear();
}

struct SecureBufferGuard {
    std::span<char> bytes;
    ~SecureBufferGuard() { secure_clear(bytes); }
};

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
    if (config.settings) {
        const auto settings_status = validate_session_settings(*config.settings,
            config.role == SessionRole::acceptor, config.tls_verified);
        if (!settings_status.ok()) return settings_status;
        if (config.role == SessionRole::acceptor && config.settings->logon.request_sequence_reset)
            return Status{ErrorCode::invalid_config, 141};
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
    std::span<const AdminField> fields, AdminMessageStorage& storage,
    std::span<char> arena) noexcept {
    if (msg_type.empty() || msg_type.size() > max_msg_type_bytes || fields.size() > admin_field_capacity) {
        return Status{ErrorCode::invalid_value, 35};
    }
    std::size_t arena_used = 0;
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
            if (source.text.size() > arena.size() - arena_used ||
                arena_used > std::numeric_limits<std::uint32_t>::max() ||
                source.text.size() > std::numeric_limits<std::uint32_t>::max())
                return Status{ErrorCode::too_large, static_cast<int>(source.tag)};
            if (!source.text.empty())
                std::memcpy(arena.data() + arena_used, source.text.data(), source.text.size());
            slot.offset = static_cast<std::uint32_t>(arena_used);
            slot.size = static_cast<std::uint32_t>(source.text.size());
            arena_used += source.text.size();
            break;
        case FieldKind::decimal:
            return Status{ErrorCode::unsupported};
        }
    }
    storage.view = detail::MessageView{storage.slots.data(), arena.data(), arena_used, storage.plan.data(),
        nullptr, fields.size(), nullptr, msg_type, Status::success()};
    return Status::success();
}

[[nodiscard]] bool parse_utc_millis(std::string_view value, std::int64_t& millis) noexcept {
    if (!detail::is_valid_utc_timestamp(value)) return false;
    const auto number = [&value](std::size_t at, std::size_t count) noexcept {
        unsigned result = 0;
        for (std::size_t i = 0; i < count; ++i)
            result = result * 10U + static_cast<unsigned>(value[at + i] - '0');
        return result;
    };
    using namespace std::chrono;
    const year_month_day date{year{static_cast<int>(number(0, 4))},
        month{number(4, 2)}, day{number(6, 2)}};
    if (!date.ok()) return false;
    const auto second = std::min(number(15, 2), 59U);
    const auto whole = duration_cast<milliseconds>(sys_days{date}.time_since_epoch() +
        hours{number(9, 2)} + minutes{number(12, 2)} + seconds{second}).count();
    unsigned fraction_ms = 0;
    const auto fractional_digits = value.size() > 17 ? value.size() - 18 : 0;
    if (fractional_digits != 0) {
        const auto digits = std::min<std::size_t>(fractional_digits, 3);
        fraction_ms = number(18, digits);
        for (std::size_t i = digits; i < 3; ++i) fraction_ms *= 10U;
    }
    millis = whole + static_cast<std::int64_t>(fraction_ms);
    return true;
}

struct ReplayRange {
    std::uint64_t begin{};
    std::uint64_t end{};
    RecordId dependency{};
    std::vector<RecordId> merged_dependencies{};
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
    bool ephemeral{};
};

struct ReceiptSlot {
    SendReceipt value{};
    bool occupied{};
};

struct RecoverySlot {
    RecordId id{};
    std::size_t bytes{};
    bool is_recovery{};
    bool validated{};
};

struct InboundTags {
    const FieldView* begin{};          // 8
    const FieldView* msg_type{};       // 35
    const FieldView* seq{};            // 34
    const FieldView* sender{};         // 49
    const FieldView* target{};         // 56
    const FieldView* sending_time{};   // 52
    const FieldView* poss_dup{};       // 43
    const FieldView* orig_time{};      // 122
    const FieldView* test_id{};        // 112
    const FieldView* peer_ack{};       // 369
    const FieldView* begin_seq{};      // 7
    const FieldView* end_seq{};        // 16
    const FieldView* new_seq{};        // 36
    const FieldView* gap_fill{};       // 123
    const FieldView* encrypt{};        // 98
    const FieldView* hb_int{};         // 108
    const FieldView* reset_flag{};     // 141
    const FieldView* appl_ver{};       // 1137
    const FieldView* text{};           // 58
    std::uint32_t duplicate_tag{};
};

[[nodiscard]] InboundTags extract_inbound_tags(const FlatMessageView& message) noexcept {
    InboundTags out{};
    auto assign = [&](const FieldView*& slot, const FieldView& field) noexcept {
        if (slot != nullptr && out.duplicate_tag == 0) out.duplicate_tag = field.tag;
        else if (slot == nullptr) slot = &field;
    };
    for (const auto& field : message.fields()) {
        if (field.tag > 141) {
            if (field.tag == 369) assign(out.peer_ack, field);
            else if (field.tag == 1137) assign(out.appl_ver, field);
            continue;
        }
        switch (field.tag) {
        case 8: assign(out.begin, field); break;
        case 35: assign(out.msg_type, field); break;
        case 34: assign(out.seq, field); break;
        case 49: assign(out.sender, field); break;
        case 56: assign(out.target, field); break;
        case 52: assign(out.sending_time, field); break;
        case 43: assign(out.poss_dup, field); break;
        case 122: assign(out.orig_time, field); break;
        case 112: assign(out.test_id, field); break;
        case 7: assign(out.begin_seq, field); break;
        case 16: assign(out.end_seq, field); break;
        case 36: assign(out.new_seq, field); break;
        case 123: assign(out.gap_fill, field); break;
        case 98: assign(out.encrypt, field); break;
        case 108: assign(out.hb_int, field); break;
        case 141: assign(out.reset_flag, field); break;
        case 58: assign(out.text, field); break;
        default: break;
        }
    }
    return out;
}

} // namespace

struct SessionDriver::Impl {
    Impl(SessionConfig requested_config, SessionCallbacks requested_callbacks, Journal requested_journal)
        : config(std::move(requested_config)), callbacks(std::move(requested_callbacks)),
          journal(std::move(requested_journal)),
          core(config.role, config.heartbeat_ms, config.logon_timeout_ms, config.logout_timeout_ms,
              journal.next_inbound_seq(), journal.next_outbound_seq()),
          tx_slots(config.max_pending_messages), encode_buffer(config.max_frame_bytes),
          replay_buffer(config.max_frame_bytes + 128), deferred_logon_buffer(config.max_frame_bytes),
          logon_tx_buffer(config.max_frame_bytes), admin_arena(config.max_frame_bytes),
          parse_fields(config.max_frame_bytes / 4 + 2),
          aux_parse_fields(config.max_frame_bytes / 4 + 2),
          receipt_slots(config.receipt_capacity), replay_slots(config.max_pending_messages),
          recovery_slots(config.max_pending_inbound_messages) {
        const auto retained_floor = journal.lowest_retained_outbound_seq();
        std::uint32_t ack_epoch = journal.records().empty()
            ? journal.checkpoint().epoch : journal.records().front().epoch;
        std::uint64_t scanned_ack = 0;
        std::uint64_t current_epoch_ack = 0;
        const auto current_epoch = journal.checkpoint().epoch;
        const auto current_max_outbound = journal.next_outbound_seq() == 0
            ? 0 : journal.next_outbound_seq() - 1;
        peer_ack_seq = 0;
        for (const auto& record : journal.records()) {
            if (record.epoch != ack_epoch) {
                ack_epoch = record.epoch;
                scanned_ack = 0;
            }
            if (record.kind == RecordKind::epoch_reset) {
                ack_epoch = record.epoch + 1U;
                scanned_ack = 0;
            }
            if (record.kind == RecordKind::outbound && record.receipt >= next_receipt_id) {
                next_receipt_id = record.receipt == std::numeric_limits<std::uint64_t>::max()
                    ? record.receipt : record.receipt + 1;
            }
            if (record.kind == RecordKind::outbound && record.receipt != 0) {
                put_receipt(record.receipt, record.seq, ReceiptState::appended);
                if (record.dependency != 0) increment_dependent_reply(record.dependency);
            }
            if (record.kind == RecordKind::inbound) {
                FlatMessageView message{};
                if (!parse_frame(record.payload, message).ok()) {
                    recovery_status = Status{ErrorCode::corrupt_store};
                    break;
                }
                bool duplicate = false;
                const auto* ack = unique_field(message, 369, duplicate);
                if (duplicate) {
                    recovery_status = Status{ErrorCode::corrupt_store, 369};
                    break;
                }
                if (ack != nullptr) {
                    std::uint64_t parsed_ack{};
                    if (!parse_unsigned(ack->value, parsed_ack) || parsed_ack < scanned_ack ||
                        (record.epoch == current_epoch && parsed_ack > current_max_outbound)) {
                        recovery_status = Status{ErrorCode::corrupt_store, 369};
                        break;
                    }
                    scanned_ack = parsed_ack;
                    if (record.epoch == current_epoch) current_epoch_ack = parsed_ack;
                }
            }
            if (record.kind != RecordKind::inbound || journal.has_delivery(record.id)) continue;
            if (recovery_count == recovery_slots.size() ||
                record.payload.size() > config.max_pending_inbound_bytes - recovery_bytes) {
                recovery_overflow = true;
                break;
            }
            recovery_slots[recovery_count++] = RecoverySlot{record.id, record.payload.size(), true, false};
            recovery_bytes += record.payload.size();
        }
        peer_ack_seq = std::max(current_epoch_ack,
            retained_floor > 1 ? retained_floor - 1 : 0);
        last_retention_inbound_segment = segment_index(journal.checkpoint().delivery_cursor);
        if (const auto* retained = journal.find_outbound(retained_floor); retained != nullptr) {
            last_retention_outbound_segment = segment_index(retained->id);
        } else if (current_max_outbound != 0) {
            const auto* latest = journal.find_outbound(current_max_outbound);
            if (latest != nullptr) last_retention_outbound_segment = segment_index(latest->id);
        }
        last_appended_outbound_segment = last_retention_outbound_segment;
        if (current_max_outbound != 0) {
            if (const auto* latest = journal.find_outbound(current_max_outbound); latest != nullptr) {
                last_appended_outbound_segment = std::max(last_appended_outbound_segment, segment_index(latest->id));
            }
        }
        (void)session_prefix.init(fix_version(), config.local_comp_id, config.remote_comp_id);
        if (next_receipt_id == 0) next_receipt_id = 1;
    }

    ~Impl() {
        secure_clear(logon_tx_buffer);
        secure_clear(deferred_logon_buffer);
        if (config.settings) {
            secure_clear(config.settings->logon.username);
            secure_clear(config.settings->logon.password);
            secure_clear(config.settings->authentication.username);
            secure_clear(config.settings->authentication.password);
            for (auto& field : config.settings->logon.extra_fields) secure_clear(field.value);
        }
    }

    SessionConfig config;
    SessionCallbacks callbacks;
    Journal journal;
    detail::SessionCore core;
    std::vector<TxDescriptor> tx_slots;
    std::vector<char> encode_buffer;
    std::vector<char> replay_buffer;
    std::vector<char> deferred_logon_buffer;
    std::vector<char> logon_tx_buffer;
    std::vector<char> admin_arena;
    std::vector<FieldView> parse_fields;
    std::vector<FieldView> aux_parse_fields;
    detail::ParsedFieldIndex parse_index{};
    std::vector<ReceiptSlot> receipt_slots;
    std::vector<ReplayRange> replay_slots;
    std::vector<RecoverySlot> recovery_slots;
    RecordId hot_dependent_reply_id{};
    std::size_t hot_dependent_reply_count{};
    std::unordered_map<RecordId, std::size_t> dependent_reply_counts;
    SessionDriver* public_owner{};
    std::thread::id owner_thread{};
    Status last_status{};
    Status callback_observer_status{};
    std::uint64_t next_receipt_id{1};
    std::uint64_t peer_ack_seq{};
    std::uint32_t last_retention_outbound_segment{};
    std::uint32_t last_appended_outbound_segment{};
    std::uint32_t last_retention_inbound_segment{};
    detail::StaticSessionPrefix session_prefix{};
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
    std::uint64_t replay_origin_begin{};
    std::uint64_t replay_next{};
    std::uint64_t replay_end{};
    std::uint64_t replay_advance{};
    std::uint64_t replay_receipt_id{};
    std::uint64_t replay_sequence{};
    RecordId replay_dependency{};
    std::vector<RecordId> replay_merged_dependencies{};
    std::uint64_t deferred_logon_sequence{};
    std::size_t replay_bytes{};
    std::size_t replay_offset{};
    std::size_t deferred_logon_size{};
    std::size_t logon_tx_size{};
    std::array<char, max_msg_type_bytes> replay_msg_type{};
    std::uint8_t replay_msg_type_size{};
    std::array<char, 32> wall_time_storage{};
    std::size_t wall_time_size{};
    std::int64_t cached_wall_time_ms{};
    bool cached_wall_time_ms_valid{};
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
    RecordId deferred_delivery_id{};
    std::uint64_t deferred_delivery_sequence{};
    bool deferred_active_callback{};
    bool peer_reset_requested{};
    bool local_epoch_reset_started{};
    bool one_shot_sequence_reset{};
    bool one_shot_reset_active{};
    std::string one_shot_reset_reason{};
    bool staged_tx_active{};
    bool staged_tx_is_replay{};
    std::size_t staged_tx_bytes{};
    bool retention_deferred{};
    Status recovery_status{};

    [[nodiscard]] Status set_wall_time(std::string_view value) noexcept {
        if (wall_time_size != 0 && value == std::string_view{wall_time_storage.data(), wall_time_size}) {
            callback_wall_time = {wall_time_storage.data(), wall_time_size};
            return Status::success();
        }
        if (!detail::is_valid_utc_timestamp(value) || value.size() > wall_time_storage.size()) {
            return Status{ErrorCode::invalid_timestamp, 52};
        }
        std::memcpy(wall_time_storage.data(), value.data(), value.size());
        wall_time_size = value.size();
        callback_wall_time = {wall_time_storage.data(), wall_time_size};
        cached_wall_time_ms_valid = false;
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

    [[nodiscard]] bool can_receive_now() const noexcept {
        if (fatal) return false;
        if (core.state() == SessionState::awaiting_logon) return true;
        return recovery_count < recovery_slots.size() &&
            recovery_bytes <= config.max_pending_inbound_bytes - config.max_frame_bytes;
    }

    [[nodiscard]] FixVersion fix_version() const noexcept {
        return config.settings ? config.settings->version : FixVersion::fix44;
    }

    [[nodiscard]] std::optional<std::uint64_t> last_processed_seq() const noexcept {
        if (!config.settings || !config.settings->emit_last_processed_seq) return std::nullopt;
        const auto completed = journal.completed_inbound_seq();
        if (completed == 0) return std::nullopt;
        return completed;
    }

    [[nodiscard]] SessionHeader make_header(std::uint64_t sequence,
        std::string_view wall_time, bool poss_dup = false,
        std::string_view original_time = {}) const noexcept {
        SessionHeader header{config.local_comp_id, config.remote_comp_id, sequence, wall_time,
            poss_dup, original_time};
        header.version = fix_version();
        header.last_msg_seq_num_processed = last_processed_seq();
        return header;
    }

    [[nodiscard]] static std::uint32_t segment_index(RecordId id) noexcept {
        return static_cast<std::uint32_t>(id >> 48U);
    }

    [[nodiscard]] std::uint64_t min_in_flight_outbound_seq() const noexcept {
        std::uint64_t bound = journal.next_outbound_seq();
        if (tx_count != 0) {
            bound = std::min(bound, tx_slots[tx_head].sequence);
        }
        if (replay_active) {
            const std::uint64_t active_min = replay_prepared
                ? std::min(replay_sequence, replay_next) : replay_next;
            if (active_min != 0) bound = std::min(bound, active_min);
        }
        for (std::size_t i = 0; i < replay_count; ++i) {
            const auto& range = replay_slots[(replay_head + i) % replay_slots.size()];
            if (range.begin != 0) bound = std::min(bound, range.begin);
        }
        return bound;
    }

    [[nodiscard]] Status update_retention_floor(bool force = false) noexcept {
        const auto cursor = journal.completed_inbound_record_id();
        const auto inbound_segment = segment_index(cursor);
        if (!force && !retention_deferred && inbound_segment <= last_retention_inbound_segment &&
            last_appended_outbound_segment <= last_retention_outbound_segment) {
            return Status::success();
        }
        const auto latest = journal.next_outbound_seq() == 0 ? 0 : journal.next_outbound_seq() - 1;
        std::uint64_t window_floor = 1;
        if (config.retain_outbound_messages != 0 && latest >= config.retain_outbound_messages)
            window_floor = latest - config.retain_outbound_messages + 1;
        const auto ack_floor = peer_ack_seq == std::numeric_limits<std::uint64_t>::max()
            ? peer_ack_seq : peer_ack_seq + 1;
        const auto target_floor = std::min(window_floor, ack_floor);
        // Never allow retention to prune outbound records still queued in TX or replay!
        const auto in_flight_floor = min_in_flight_outbound_seq();
        retention_deferred = (in_flight_floor < target_floor);
        auto floor = std::min(target_floor, in_flight_floor);
        floor = std::max(floor, journal.lowest_retained_outbound_seq());
        std::uint32_t outbound_segment = 0;
        if (floor <= latest) {
            if (const auto* boundary = journal.find_outbound(floor); boundary != nullptr)
                outbound_segment = segment_index(boundary->id);
        } else if (latest != 0) {
            if (const auto* boundary = journal.find_outbound(latest); boundary != nullptr)
                outbound_segment = segment_index(boundary->id);
        }
        if (!force && outbound_segment <= last_retention_outbound_segment &&
            inbound_segment <= last_retention_inbound_segment)
            return Status::success();
        const auto status = journal.set_retention_floor(floor, cursor);
        if (status.code == ErrorCode::unsupported) return Status::success();
        if (status.ok()) {
            last_retention_outbound_segment = std::max(last_retention_outbound_segment, outbound_segment);
            last_retention_inbound_segment = std::max(last_retention_inbound_segment, inbound_segment);
        }
        return status;
    }

    [[nodiscard]] Status apply_peer_ack(const FlatMessageView& message,
        std::uint64_t* parsed_value = nullptr) noexcept {
        bool duplicate = false;
        const auto* field = unique_field(message, 369, duplicate);
        if (duplicate) return Status{ErrorCode::duplicate_field, 369};
        if (field == nullptr) return Status::success();
        std::uint64_t value{};
        if (!parse_unsigned(field->value, value)) return Status{ErrorCode::invalid_value, 369};
        const auto last_outbound = journal.next_outbound_seq() == 0
            ? 0 : journal.next_outbound_seq() - 1;
        if (value < peer_ack_seq || value > last_outbound) return Status{ErrorCode::protocol_error, 369};
        if (parsed_value != nullptr) *parsed_value = value;
        return Status::success();
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
        secure_clear(logon_tx_buffer);
        logon_tx_size = 0;
        secure_clear(deferred_logon_buffer);
        deferred_logon_size = 0;
        notify_error(error);
        if (!callback_observer_status.ok()) last_status = callback_observer_status;
        return last_status;
    }

    [[nodiscard]] Status transport_failure(Status error) noexcept {
        if (error.ok()) error = Status{ErrorCode::connection_closed};
        last_status = error;
        should_close = true;
        secure_clear(logon_tx_buffer);
        logon_tx_size = 0;
        notify_error(error);
        if (!callback_observer_status.ok()) last_status = callback_observer_status;
        return error;
    }

    [[nodiscard]] Status enqueue(RecordId record_id, std::size_t frame_size,
        std::uint64_t sequence, std::uint64_t receipt_id, std::string_view msg_type,
        std::uint64_t now_ms, bool business, bool ephemeral = false) noexcept {
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
        descriptor.ephemeral = ephemeral;
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
        const bool ephemeral = descriptor->ephemeral;
        const auto message_type = std::string_view{descriptor->msg_type.data(), descriptor->msg_type_size};
        if (written) {
            update_receipt(descriptor->receipt_id, ReceiptState::written);
        } else if (!failure.ok()) {
            update_receipt(descriptor->receipt_id, ReceiptState::failed, failure);
        }
        if (ephemeral) {
            secure_clear(logon_tx_buffer);
            logon_tx_size = 0;
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
        if (retention_deferred) [[unlikely]] {
            if (const auto rstatus = update_retention_floor(); !rstatus.ok()) return fail(rstatus);
        }
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
        if (const auto status = make_admin_view(msg_type, fields, storage, admin_arena); !status.ok()) return status;
        const auto header = make_header(sequence, wall_time, poss_dup, orig_time);
        RecordId record_id = 0;
        std::size_t encoded_size = 0;
        const auto replay_reserve = replay_reservation_bytes();
        const bool base_capacity_ok =
            tx_count < tx_slots.size() &&
            admin_count < config.admin_reserve_messages &&
            replay_reserve <= config.admin_reserve_bytes - admin_bytes &&
            replay_reserve <= config.max_pending_bytes - pending_bytes_count;
        const std::size_t max_admin_frame = base_capacity_ok
            ? std::min({config.max_frame_bytes,
                        config.admin_reserve_bytes - admin_bytes - replay_reserve,
                        config.max_pending_bytes - pending_bytes_count - replay_reserve})
            : 0U;

        const std::size_t admin_bound = std::min(config.max_frame_bytes,
            std::size_t{256} + storage.view.value_byte_count + storage.view.field_count * 40U);
        bool committed_zero_copy = false;
        if (persist && base_capacity_ok && max_admin_frame > 0) {
            auto reserved = journal.reserve(RecordKind::outbound, sequence, 0, dependency, admin_bound);
            if (reserved.ok()) {
                const auto encoded = encode_fix(storage.view, header, reserved.value().payload);
                if (encoded.ok()) {
                    if (encoded.value() > max_admin_frame) return Status{ErrorCode::queue_full};
                    const auto committed = journal.commit(reserved.value(), encoded.value());
                    if (!committed.ok()) return committed.status();
                    record_id = committed.value().id;
                    encoded_size = encoded.value();
                    last_appended_outbound_segment = std::max(last_appended_outbound_segment, segment_index(record_id));
                    committed_zero_copy = true;
                } else if (encoded.status().code != ErrorCode::capacity_exceeded) {
                    return encoded.status();
                }
            }
        }
        if (!committed_zero_copy) {
            const auto encoded = encode_fix(storage.view, header, encode_buffer);
            if (!encoded.ok()) return encoded.status();
            if (encoded.value() > config.max_frame_bytes) return Status{ErrorCode::too_large};
            if (!base_capacity_ok || encoded.value() > max_admin_frame) {
                return Status{ErrorCode::queue_full};
            }
            encoded_size = encoded.value();
            if (persist) {
                const auto appended = journal.append(RecordKind::outbound, sequence, 0, dependency,
                    std::string_view{encode_buffer.data(), encoded_size}, 0);
                if (!appended.ok()) return appended.status();
                record_id = appended.value().id;
                last_appended_outbound_segment = std::max(last_appended_outbound_segment, segment_index(record_id));
            }
        }
        if (const auto status = enqueue(record_id, encoded_size, sequence, 0, msg_type,
                now_ms, false); !status.ok()) {
            if (persist) return fail(Status{ErrorCode::protocol_error, 34});
            return status;
        }
        if (persist) {
            if (const auto status = update_retention_floor(); !status.ok()) return fail(status);
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

    [[nodiscard]] Status send_logon(std::uint64_t now_ms, std::string_view wall_time,
        bool activate_after_reply, RecordId dependency) noexcept {
        SecureBufferGuard clear_arena{std::span<char>{admin_arena}};
        const auto sequence = core.next_outbound();
        if (sequence == 0) return fail(Status{ErrorCode::too_large, 34});
        std::array<AdminField, admin_field_capacity> safe_fields{};
        std::array<AdminField, admin_field_capacity> wire_fields{};
        std::size_t safe_count = 0;
        const auto add_safe = [&](const AdminField& field) {
            safe_fields[safe_count] = field;
            wire_fields[safe_count] = field;
            ++safe_count;
        };
        add_safe(AdminField{98, FieldKind::unsigned_integer, 0, {}});
        const std::uint32_t effective_hb_ms =
            (config.role == SessionRole::acceptor && core.peer_heartbeat_ms() != 0)
                ? core.peer_heartbeat_ms()
                : config.heartbeat_ms;
        const auto seconds = std::max<std::uint64_t>(1,
            (static_cast<std::uint64_t>(effective_hb_ms) + 999) / 1000);
        add_safe(AdminField{108, FieldKind::unsigned_integer, seconds, {}});

        const bool request_reset = config.role == SessionRole::initiator &&
            ((config.settings && config.settings->logon.request_sequence_reset) || one_shot_reset_active);
        const bool agree_reset = config.role == SessionRole::acceptor && peer_reset_requested;
        if (request_reset || agree_reset)
            add_safe(AdminField{141, FieldKind::character, static_cast<unsigned char>('Y'), {}});
        if (fix_version() == FixVersion::fixt11) {
            const auto& app_version = config.settings->default_appl_ver_id;
            add_safe(AdminField{1137, FieldKind::string, 0, app_version});
        }

        auto storage = AdminMessageStorage{};
        const auto header = make_header(sequence, wall_time);
        if (const auto status = make_admin_view("A",
                std::span<const AdminField>{safe_fields.data(), safe_count}, storage, admin_arena); !status.ok())
            return status;
        const auto persisted = encode_fix(storage.view, header, encode_buffer);
        if (!persisted.ok()) return persisted.status();
        if (persisted.value() > config.max_frame_bytes) return Status{ErrorCode::too_large};

        std::size_t wire_count = safe_count;
        if (config.role == SessionRole::initiator && config.settings) {
            const auto& logon = config.settings->logon;
            if (!logon.username.empty())
                wire_fields[wire_count++] = AdminField{553, FieldKind::string, 0, logon.username};
            if (!logon.password.empty())
                wire_fields[wire_count++] = AdminField{554, FieldKind::string, 0, logon.password};
            for (const auto& field : logon.extra_fields)
                wire_fields[wire_count++] = AdminField{field.tag, FieldKind::string, 0, field.value};
        }
        if (const auto status = make_admin_view("A",
                std::span<const AdminField>{wire_fields.data(), wire_count}, storage, admin_arena); !status.ok())
            return status;
        const auto actual = encode_fix(storage.view, header, logon_tx_buffer);
        if (!actual.ok()) {
            secure_clear(logon_tx_buffer);
            secure_clear(admin_arena);
            return actual.status();
        }
        if (actual.value() > config.max_frame_bytes) {
            secure_clear(logon_tx_buffer);
            secure_clear(admin_arena);
            return Status{ErrorCode::too_large};
        }
        const auto replay_reserve = replay_reservation_bytes();
        if (tx_count == tx_slots.size() || admin_count >= config.admin_reserve_messages ||
            actual.value() > config.admin_reserve_bytes - admin_bytes ||
            replay_reserve > config.admin_reserve_bytes - admin_bytes - actual.value() ||
            actual.value() > config.max_pending_bytes - pending_bytes_count ||
            replay_reserve > config.max_pending_bytes - pending_bytes_count - actual.value()) {
            secure_clear(logon_tx_buffer);
            secure_clear(admin_arena);
            return Status{ErrorCode::queue_full};
        }
        const auto appended = journal.append(RecordKind::outbound, sequence, 0,
            dependency, std::string_view{encode_buffer.data(), persisted.value()}, 0);
        if (!appended.ok()) {
            secure_clear(logon_tx_buffer);
            secure_clear(admin_arena);
            return fail(appended.status());
        }
        logon_tx_size = actual.value();
        if (const auto status = enqueue(appended.value().id, actual.value(), sequence,
                0, "A", now_ms, false, true); !status.ok()) {
            secure_clear(logon_tx_buffer);
            logon_tx_size = 0;
            secure_clear(admin_arena);
            return fail(Status{ErrorCode::protocol_error, 34});
        }
        if (const auto status = update_retention_floor(); !status.ok()) return fail(status);
        detail::CoreEvent event{};
        event.kind = detail::CoreEventKind::outbound_appended;
        event.now_ms = now_ms;
        event.sequence = sequence;
        event.msg_type = "A";
        auto output = core.process(event);
        if (!output.status.ok()) return fail(output.status);
        if (config.role == SessionRole::acceptor && activate_after_reply) {
            detail::CoreEvent reply{};
            reply.kind = detail::CoreEventKind::logon_reply_appended;
            reply.now_ms = now_ms;
            output = core.process(reply);
            if (!output.status.ok()) return fail(output.status);
            if (const auto status = apply_actions(output, nullptr, 0, 0,
                    now_ms, wall_time, 1); !status.ok()) return status;
        }
        secure_clear(admin_arena);
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
            return send_logon(now_ms, wall_time,
                config.role == SessionRole::acceptor && action.first == 0, dependency);
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

    [[nodiscard]] bool attach_merged_dependency(std::vector<RecordId>& merged,
        RecordId primary, RecordId dep) const noexcept {
        if (dep == 0 || dep == primary) return true;
        if (std::find(merged.begin(), merged.end(), dep) != merged.end()) return true;
        const std::size_t cap = std::max<std::size_t>(replay_slots.size() * 4U, 256U);
        if (merged.size() >= cap) return false;
        try {
            merged.push_back(dep);
            return true;
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] Status queue_replay(std::uint64_t begin, std::uint64_t end,
        RecordId dependency) noexcept {
        const auto next_out = journal.next_outbound_seq();
        if (next_out <= 1) return Status::success();
        const auto maximum = next_out - 1;
        if (end == 0 || end > maximum) end = maximum;
        if (begin == 0 || begin > end) return Status{ErrorCode::invalid_value, 7};
        if (replay_active) {
            const std::uint64_t active_cover_begin =
                (dependency == replay_dependency) ? replay_next : replay_origin_begin;
            const std::uint64_t active_adj_end =
                replay_end + (replay_end != std::numeric_limits<std::uint64_t>::max());
            if (begin >= active_cover_begin && begin <= active_adj_end &&
                end >= active_cover_begin) {
                if (!attach_merged_dependency(replay_merged_dependencies, replay_dependency, dependency)) {
                    return Status{ErrorCode::queue_full};
                }
                if (end > replay_end) replay_end = end;
                return Status::success();
            }
        }
        for (std::size_t i = replay_count; i > 0; --i) {
            const auto slot_index = (replay_head + i - 1) % replay_slots.size();
            auto& slot = replay_slots[slot_index];
            const std::uint64_t slot_adj_end =
                slot.end + (slot.end != std::numeric_limits<std::uint64_t>::max());
            const std::uint64_t req_adj_end =
                end + (end != std::numeric_limits<std::uint64_t>::max());
            if (begin <= slot_adj_end && req_adj_end >= slot.begin) {
                if (!attach_merged_dependency(slot.merged_dependencies, slot.dependency, dependency)) {
                    return Status{ErrorCode::queue_full};
                }
                slot.begin = std::min(slot.begin, begin);
                slot.end = std::max(slot.end, end);
                return Status::success();
            }
        }
        if (admin_bytes > config.admin_reserve_bytes ||
            config.max_frame_bytes > config.admin_reserve_bytes - admin_bytes) {
            return Status{ErrorCode::queue_full};
        }
        if (replay_count == replay_slots.size()) return Status{ErrorCode::queue_full};
        auto& target = replay_slots[(replay_head + replay_count) % replay_slots.size()];
        target.begin = begin;
        target.end = end;
        target.dependency = dependency;
        target.merged_dependencies.clear();
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
        auto& range = replay_slots[replay_head];
        replay_origin_begin = range.begin;
        replay_next = range.begin;
        replay_end = range.end;
        replay_dependency = range.dependency;
        replay_merged_dependencies = std::move(range.merged_dependencies);
        range.merged_dependencies.clear();
        replay_head = (replay_head + 1) % replay_slots.size();
        --replay_count;
        replay_active = true;
        replay_prepared = false;
    }

    [[nodiscard]] Status parse_frame(std::string_view bytes, FlatMessageView& message) noexcept {
        if (bytes.size() > config.max_frame_bytes) return Status{ErrorCode::too_large};
        FrameView frame{};
        const bool check_csum = !config.settings || config.settings->verify_checksum;
        if (const auto status = inspect_frame(bytes, frame, callbacks.wire_hints,
                CodecLimits{config.max_frame_bytes, check_csum}); !status.ok()) {
            return status;
        }
        if (frame.consumed != bytes.size()) return Status{ErrorCode::invalid_frame};
        if (const auto status = parse_flat_verified(frame, parse_fields, message, callbacks.wire_hints);
            !status.ok()) {
            return status;
        }
        message.bind_index(&parse_index);
        return Status::success();
    }

    // Auxiliary parser for historical outbound records (replay / recovery reply reuse) that
    // must never overwrite parse_fields or parse_index while an inbound callback is active.
    [[nodiscard]] Status parse_frame_aux(std::string_view bytes, FlatMessageView& message) noexcept {
        if (bytes.size() > config.max_frame_bytes) return Status{ErrorCode::too_large};
        FrameView frame{};
        const bool check_csum = !config.settings || config.settings->verify_checksum;
        if (const auto status = inspect_frame(bytes, frame, callbacks.wire_hints,
                CodecLimits{config.max_frame_bytes, check_csum}); !status.ok()) {
            return status;
        }
        if (frame.consumed != bytes.size()) return Status{ErrorCode::invalid_frame};
        return parse_flat_verified(frame, aux_parse_fields, message, callbacks.wire_hints);
    }

    [[nodiscard]] Status parse_inbound_header(const InboundTags& tags,
        std::string_view& msg_type, std::uint64_t& sequence, bool& poss_dup,
        bool& original_time_valid, std::string_view& test_id) const noexcept {
        if (tags.duplicate_tag != 0) {
            const auto d = tags.duplicate_tag;
            if (d == 8) return Status{ErrorCode::unsupported_protocol, 8};
            if (d == 35 || d == 34) return Status{ErrorCode::invalid_header, static_cast<int>(d)};
            if (d == 49 || d == 56) return Status{ErrorCode::identity_mismatch, static_cast<int>(d)};
            if (d == 52) return Status{ErrorCode::invalid_timestamp, 52};
            return Status{ErrorCode::duplicate_field, static_cast<int>(d)};
        }
        if (tags.begin == nullptr)
            return Status{ErrorCode::unsupported_protocol, 8};
        if (config.settings && tags.begin->value != begin_string(fix_version()))
            return Status{ErrorCode::unsupported_protocol, 8};
        if (tags.msg_type == nullptr || tags.msg_type->value.empty())
            return Status{ErrorCode::invalid_header, 35};
        if (tags.seq == nullptr || !parse_unsigned(tags.seq->value, sequence) || sequence == 0)
            return Status{ErrorCode::invalid_header, 34};
        if (tags.sender == nullptr || tags.sender->value != config.remote_comp_id)
            return Status{ErrorCode::identity_mismatch, 49};
        if (tags.target == nullptr || tags.target->value != config.local_comp_id)
            return Status{ErrorCode::identity_mismatch, 56};
        if (tags.sending_time == nullptr || !detail::is_valid_utc_timestamp(tags.sending_time->value))
            return Status{ErrorCode::invalid_timestamp, 52};
        poss_dup = tags.poss_dup != nullptr && tags.poss_dup->value == "Y";
        if (tags.poss_dup != nullptr && tags.poss_dup->value != "Y" && tags.poss_dup->value != "N")
            return Status{ErrorCode::invalid_value, 43};
        original_time_valid = tags.orig_time != nullptr && detail::is_valid_utc_timestamp(tags.orig_time->value);
        if (poss_dup && (!original_time_valid ||
                detail::compare_utc_timestamps(tags.orig_time->value, tags.sending_time->value) > 0)) {
            return Status{ErrorCode::invalid_timestamp, 122};
        }
        if (!poss_dup && tags.orig_time != nullptr) return Status{ErrorCode::invalid_header, 122};
        msg_type = tags.msg_type->value;
        test_id = {};
        if (msg_type == "1" || msg_type == "0") {
            if (tags.test_id != nullptr) test_id = tags.test_id->value;
            if (msg_type == "1" && tags.test_id == nullptr) return Status{ErrorCode::missing_field, 112};
        }
        return Status::success();
    }

    [[nodiscard]] bool sending_time_within_skew(std::string_view sent,
        std::string_view now) noexcept {
        if (!config.settings || !config.settings->check_sending_time) return true;
        std::int64_t sent_ms{};
        if (!parse_utc_millis(sent, sent_ms)) return false;
        std::int64_t now_ms{};
        if (cached_wall_time_ms_valid && now == callback_wall_time) {
            now_ms = cached_wall_time_ms;
        } else {
            if (!parse_utc_millis(now, now_ms)) return false;
            if (now == callback_wall_time) {
                cached_wall_time_ms = now_ms;
                cached_wall_time_ms_valid = true;
            }
        }
        const auto difference = sent_ms >= now_ms
            ? static_cast<std::uint64_t>(sent_ms - now_ms)
            : static_cast<std::uint64_t>(now_ms - sent_ms);
        return difference <= config.settings->max_sending_time_skew_ms;
    }

    [[nodiscard]] Status validate_logon_policy(const FlatMessageView& message,
        const InboundTags& tags, std::uint64_t sequence, bool& reset_requested) noexcept {
        reset_requested = false;
        if (config.settings && config.settings->version == FixVersion::fixt11) {
            if (tags.appl_ver == nullptr ||
                tags.appl_ver->value != config.settings->default_appl_ver_id)
                return Status{ErrorCode::unsupported_protocol, 1137};
        }
        if (tags.reset_flag != nullptr && tags.reset_flag->value != "Y" && tags.reset_flag->value != "N")
            return Status{ErrorCode::invalid_value, 141};
        reset_requested = tags.reset_flag != nullptr && tags.reset_flag->value == "Y";

        if (config.role == SessionRole::acceptor && config.settings &&
            !config.logon_preauthenticated) {
            const auto authenticated = authenticate_logon(config.settings->authentication,
                message, config.tls_verified);
            if (!authenticated.ok()) return authenticated;
        }
        if (reset_requested) {
            if (!config.settings || !config.settings->allow_sequence_reset || sequence != 1)
                return Status{ErrorCode::sequence_reset_denied, 141};
            if (tags.peer_ack != nullptr) {
                std::uint64_t ack_value{};
                if (!parse_unsigned(tags.peer_ack->value, ack_value) ||
                    (ack_value != 0 && !(config.role == SessionRole::initiator && ack_value == 1)))
                    return Status{ErrorCode::sequence_reset_denied, 369};
            }
            const bool local_requested_reset =
                config.settings->logon.request_sequence_reset || one_shot_reset_active;
            if (config.role == SessionRole::initiator &&
                (!local_requested_reset || !local_epoch_reset_started))
                return Status{ErrorCode::sequence_reset_denied, 141};
            if (config.role == SessionRole::acceptor && core.state() != SessionState::awaiting_logon)
                return Status{ErrorCode::sequence_reset_denied, 141};
        } else if (config.role == SessionRole::initiator &&
            ((config.settings && config.settings->logon.request_sequence_reset) || one_shot_reset_active) &&
            core.state() == SessionState::awaiting_logon) {
            return Status{ErrorCode::sequence_reset_denied, 141};
        }
        return Status::success();
    }

    [[nodiscard]] Result<std::size_t> encode_safe_logon(const FlatMessageView& message,
        std::uint64_t wire_sequence, bool poss_dup, bool original_time_valid,
        std::string_view original_time, std::string_view sending_time,
        std::optional<std::uint64_t> peer_ack) noexcept {
        std::array<AdminField, 4> fields{};
        std::size_t count = 0;
        bool duplicate = false;
        const auto* encryption = unique_field(message, 98, duplicate);
        std::uint64_t encryption_value{};
        if (duplicate || encryption == nullptr || !parse_unsigned(encryption->value, encryption_value))
            return Status{ErrorCode::corrupt_store, 98};
        const auto* heartbeat = unique_field(message, 108, duplicate);
        std::uint64_t heartbeat_value{};
        if (duplicate || heartbeat == nullptr || !parse_unsigned(heartbeat->value, heartbeat_value))
            return Status{ErrorCode::corrupt_store, 108};
        fields[count++] = AdminField{98, FieldKind::unsigned_integer, encryption_value, {}};
        fields[count++] = AdminField{108, FieldKind::unsigned_integer, heartbeat_value, {}};
        const auto* reset = unique_field(message, 141, duplicate);
        if (duplicate) return Status{ErrorCode::corrupt_store, 141};
        if (reset != nullptr)
            fields[count++] = AdminField{141, FieldKind::character,
                static_cast<unsigned char>(reset->value.empty() ? 0 : reset->value[0]), {}};
        std::string_view appl_ver{};
        if (fix_version() == FixVersion::fixt11) {
            const auto* field = unique_field(message, 1137, duplicate);
            if (duplicate || field == nullptr) return Status{ErrorCode::corrupt_store, 1137};
            appl_ver = field->value;
            // The safe body is capped at four fields; Logon's safe fields can
            // include all four entries (98, 108, 141, 1137).
            if (count == fields.size()) return Status{ErrorCode::capacity_exceeded};
            fields[count++] = AdminField{1137, FieldKind::string, 0, appl_ver};
        }
        AdminMessageStorage storage{};
        if (const auto status = make_admin_view("A",
                std::span<const AdminField>{fields.data(), count}, storage, admin_arena); !status.ok())
            return status;
        auto header = make_header(wire_sequence, sending_time, poss_dup,
            original_time_valid ? original_time : std::string_view{});
        // Preserve actual inbound wire direction (remote -> local) in the redacted journal record.
        header.sender_comp_id = config.remote_comp_id;
        header.target_comp_id = config.local_comp_id;
        if (!config.settings) {
            const auto* begin = unique_field(message, 8, duplicate);
            if (duplicate || begin == nullptr) return Status{ErrorCode::corrupt_store, 8};
            const auto version = parse_fix_version(begin->value);
            if (!version.ok()) return Status{ErrorCode::corrupt_store, 8};
            header.version = version.value();
        }
        header.last_msg_seq_num_processed = peer_ack;
        if (!poss_dup) header.orig_sending_time = {};
        const auto encoded = encode_fix(storage.view, header, encode_buffer);
        if (!encoded.ok()) return encoded.status();
        secure_clear(admin_arena);
        return encoded.value();
    }

    [[nodiscard]] Status build_replay_frame() noexcept {
        if (!replay_active || replay_prepared) return Status::success();
        while (replay_next <= replay_end) {
            const auto* record = journal.find_outbound(replay_next);
            if (record == nullptr || record->payload.empty()) return Status{ErrorCode::corrupt_store, 34};
            FlatMessageView parsed{};
            if (const auto status = parse_frame_aux(record->payload, parsed); !status.ok()) return status;
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
                auto header = make_header(record->seq, callback_wall_time, true, original_time);
                const auto encoded = encode_fix_replay(parsed, header, replay_buffer);
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
            auto gap_end = std::max(replay_next + 1,
                record->next_seq > record->seq ? record->next_seq : record->seq + 1);
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
                if (const auto status = parse_frame_aux(next_record->payload, next_parsed); !status.ok()) return status;
                const auto* next_type = unique_field(next_parsed, 35, duplicate);
                if (duplicate || next_type == nullptr) return Status{ErrorCode::corrupt_store, 35};
                if (!is_session_admin(next_type->value)) break;
                const auto adv = next_record->next_seq > next_record->seq
                    ? next_record->next_seq : next_record->seq + 1;
                gap_end = std::max(gap_end + 1, adv);
            }
            std::array<AdminField, 2> gap_fields{
                AdminField{123, FieldKind::character, static_cast<unsigned char>('Y'), {}},
                AdminField{36, FieldKind::unsigned_integer, gap_end, {}},
            };
            AdminMessageStorage storage{};
            if (const auto status = make_admin_view("4", gap_fields, storage, admin_arena); !status.ok()) return status;
            auto header = make_header(gap_begin, callback_wall_time, true, original_time);
            const auto encoded = encode_fix(storage.view, header, replay_buffer);
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
        auto merged = std::move(replay_merged_dependencies);
        replay_merged_dependencies.clear();
        replay_origin_begin = 0;
        replay_active = false;
        replay_prepared = false;
        replay_offset = 0;
        replay_bytes = 0;
        replay_receipt_id = 0;
        replay_dependency = 0;
        bool completed_any = false;
        auto complete_dep = [&](RecordId dep_id) noexcept -> Status {
            if (dep_id == 0) return Status::success();
            const auto* inbound = journal.find_record(dep_id);
            if (inbound == nullptr || inbound->kind != RecordKind::inbound)
                return Status{ErrorCode::corrupt_store};
            const auto sequence = inbound->seq;
            if (const auto status = append_delivery(dep_id, sequence); !status.ok()) return status;
            completed_any = true;
            return Status::success();
        };
        if (const auto status = complete_dep(dependency); !status.ok()) return status;
        for (const auto extra : merged) {
            if (const auto status = complete_dep(extra); !status.ok()) return status;
        }
        if (retention_deferred) [[unlikely]] {
            if (const auto rstatus = update_retention_floor(); !rstatus.ok()) return rstatus;
        }
        if (completed_any && core.state() == SessionState::active) {
            return deliver_recovery(callback_now_ms, callback_wall_time);
        }
        return Status::success();
    }

    [[nodiscard]] Status queue_replay_range(std::uint64_t begin, std::uint64_t end,
        RecordId dependency = 0) noexcept {
        const auto next_outbound = journal.next_outbound_seq();
        const auto maximum = next_outbound == 0 ? 0 : next_outbound - 1;
        if (maximum != 0 && begin < journal.lowest_retained_outbound_seq())
            return fail(Status{ErrorCode::history_unavailable, 7,
                static_cast<std::size_t>(begin)});
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
            clear_dependent_reply(inbound_id);
            return update_retention_floor();
        }
        const auto appended = journal.append(RecordKind::delivery, sequence, 0, inbound_id, {});
        if (!appended.ok()) return appended.status();
        forget_recovery(inbound_id);
        clear_dependent_reply(inbound_id);
        return update_retention_floor();
    }

    [[nodiscard]] std::uint64_t reject_reason(Status status) const noexcept {
        switch (status.code) {
        case ErrorCode::required_field_missing:
        case ErrorCode::missing_field:
            return 1;
        case ErrorCode::message_unhandled:
            return 11;
        case ErrorCode::invalid_timestamp:
            return status.tag == 52 ? 10 : 5;
        case ErrorCode::invalid_value:
        case ErrorCode::duplicate_field:
        case ErrorCode::unsupported:
        case ErrorCode::schema_mismatch:
            return 5;
        default:
            return 0;
        }
    }

    [[nodiscard]] Status send_protocol_reject(std::uint64_t reference_sequence,
        std::string_view reference_type, Status reason, std::uint64_t reject_reason_code,
        std::uint64_t now_ms, std::string_view wall_time) noexcept {
        std::array<AdminField, 5> fields{};
        std::size_t count = 0;
        fields[count++] = AdminField{45, FieldKind::unsigned_integer, reference_sequence, {}};
        if (reason.tag > 0)
            fields[count++] = AdminField{371, FieldKind::unsigned_integer,
                static_cast<std::uint64_t>(reason.tag), {}};
        if (!reference_type.empty())
            fields[count++] = AdminField{372, FieldKind::string, 0, reference_type};
        fields[count++] = AdminField{373, FieldKind::unsigned_integer, reject_reason_code, {}};
        fields[count++] = AdminField{58, FieldKind::string, 0, reason.message()};
        return make_admin_and_append("3", std::span<const AdminField>{fields.data(), count},
            now_ms, wall_time);
    }

    void report_peer_reject(const FlatMessageView& message) noexcept {
        bool duplicate = false;
        const auto* sequence = unique_field(message, 45, duplicate);
        if (duplicate || sequence == nullptr) {
            notify_error(Status{ErrorCode::peer_rejected, 45});
            return;
        }
        std::uint64_t reference_sequence{};
        if (!parse_unsigned(sequence->value, reference_sequence)) {
            notify_error(Status{ErrorCode::peer_rejected, 45});
            return;
        }
        const auto* reference_tag = unique_field(message, 371, duplicate);
        if (duplicate) {
            notify_error(Status{ErrorCode::peer_rejected, 371});
            return;
        }
        int tag = 0;
        if (reference_tag != nullptr) {
            std::uint64_t parsed{};
            if (!parse_unsigned(reference_tag->value, parsed) ||
                parsed > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
                notify_error(Status{ErrorCode::peer_rejected, 371});
                return;
            }
            tag = static_cast<int>(parsed);
        }
        notify_error(Status{ErrorCode::peer_rejected, tag,
            static_cast<std::size_t>(std::min<std::uint64_t>(reference_sequence,
                std::numeric_limits<std::size_t>::max()))});
    }

    void forget_recovery(RecordId inbound_id) noexcept {
        for (std::size_t i = recovery_count; i > 0; --i) {
            const std::size_t idx = i - 1;
            if (recovery_slots[idx].id != inbound_id) continue;
            recovery_bytes -= recovery_slots[idx].bytes;
            for (std::size_t move = idx + 1; move < recovery_count; ++move) {
                recovery_slots[move - 1] = recovery_slots[move];
            }
            --recovery_count;
            recovery_slots[recovery_count] = RecoverySlot{};
            break;
        }
    }

    [[nodiscard]] RecoverySlot* find_recovery_slot(RecordId id) noexcept {
        for (std::size_t i = recovery_count; i > 0; --i) {
            if (recovery_slots[i - 1].id == id) return &recovery_slots[i - 1];
        }
        return nullptr;
    }

    [[nodiscard]] Status remember_inbound(RecordId id, std::size_t bytes,
        bool is_recovery = false, bool validated = false) noexcept {
        if (recovery_count == recovery_slots.size() || bytes > config.max_pending_inbound_bytes - recovery_bytes) {
            return Status{ErrorCode::queue_full};
        }
        recovery_slots[recovery_count++] = RecoverySlot{id, bytes, is_recovery, validated};
        recovery_bytes += bytes;
        return Status::success();
    }

    [[nodiscard]] Status deliver_message(const FlatMessageView& message, DeliveryInfo info,
        std::uint64_t now_ms, std::string_view wall_time) noexcept {
        auto* slot = find_recovery_slot(info.record_id);
        const bool already_validated = slot != nullptr && slot->validated;
        if (callbacks.validate_message && !already_validated) {
            Status validation{};
            try { validation = callbacks.validate_message(message); }
            catch (...) { validation = Status{ErrorCode::callback_failed}; }
            if (!validation.ok()) {
                if (validation.code == ErrorCode::callback_failed) return fail(validation);
                bool duplicate = false;
                const auto* type = unique_field(message, 35, duplicate);
                if (duplicate || type == nullptr) return fail(Status{ErrorCode::invalid_header, 35});
                if (const auto reject = send_protocol_reject(info.msg_seq_num, type->value,
                        validation, reject_reason(validation), now_ms, wall_time); !reject.ok())
                    return fail(reject);
                notify_error(validation);
                const auto completed = append_delivery(info.record_id, info.msg_seq_num);
                return completed.ok() ? completed : fail(completed);
            }
            if (slot != nullptr) slot->validated = true;
        }
        if (deferred_active_callback) return Status::success();
        if (callbacks.deferred_delivery) {
            if (!callbacks.on_deferred_message) return fail(Status{ErrorCode::invalid_config});
            if (deferred_delivery_id != 0) {
                // One callback at a time per session. Later committed records
                // remain in recovery_slots until this delivery completes.
                return Status::success();
            }
            const auto* record = journal.find_record(info.record_id);
            if (record == nullptr || record->kind != RecordKind::inbound ||
                record->seq != info.msg_seq_num || journal.has_delivery(info.record_id))
                return fail(Status{ErrorCode::corrupt_store});
            auto pinned = journal.pin_record(info.record_id);
            if (!pinned.ok()) return fail(pinned.status());
            DeliveryLease lease{info, pinned.value().payload(), std::move(pinned).value()};
            Status queued{};
            callback_active = true;
            callback_now_ms = now_ms;
            callback_wall_time = wall_time;
            try {
                queued = callbacks.on_deferred_message(lease);
            } catch (...) {
                queued = Status{ErrorCode::callback_failed};
            }
            callback_active = false;
            if (queued.code == ErrorCode::queue_full || queued.code == ErrorCode::need_more)
                return Status::success();
            if (!queued.ok()) return fail(queued);
            if (deferred_delivery_id != info.record_id ||
                deferred_delivery_sequence != info.msg_seq_num)
                return fail(Status{ErrorCode::protocol_error});
            return Status::success();
        }
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
        if (info.is_recovery && dependent_application_replies(info.record_id) > context.send_ordinal_)
            return fail(Status{ErrorCode::callback_failed});
        const auto completed = append_delivery(info.record_id, info.msg_seq_num);
        return completed.ok() ? completed : fail(completed);
    }

    [[nodiscard]] Status complete_deferred(DeliveryInfo info, Status callback_status,
        std::size_t accepted_replies, std::uint64_t now_ms,
        std::string_view wall_time) noexcept {
        if (deferred_delivery_id == 0 || deferred_delivery_id != info.record_id ||
            deferred_delivery_sequence != info.msg_seq_num)
            return Status{ErrorCode::not_ready};
        deferred_delivery_id = 0;
        deferred_delivery_sequence = 0;
        if (!callback_status.ok())
            return fail(Status{ErrorCode::callback_failed, callback_status.tag, callback_status.offset});
        if (dependent_application_replies(info.record_id) != accepted_replies)
            return fail(Status{ErrorCode::callback_failed});
        const auto completed = append_delivery(info.record_id, info.msg_seq_num);
        if (!completed.ok()) return fail(completed);
        callback_now_ms = now_ms;
        if (const auto time_status = set_wall_time(wall_time); !time_status.ok()) return fail(time_status);
        // The runtime clears its per-session callback completion state after
        // this returns. The next owner tick advances to a later delivery.
        return Status::success();
    }

    [[nodiscard]] Status run_active(std::uint64_t now_ms, std::string_view wall_time) noexcept {
        if (callbacks.deferred_active) {
            if (!callbacks.on_deferred_active) return fail(Status{ErrorCode::invalid_config});
            Status schedule_status{};
            callback_active = true;
            callback_now_ms = now_ms;
            callback_wall_time = wall_time;
            try {
                schedule_status = callbacks.on_deferred_active();
            } catch (...) {
                schedule_status = Status{ErrorCode::callback_failed};
            }
            callback_active = false;
            if (!schedule_status.ok()) return fail(Status{ErrorCode::callback_failed,
                schedule_status.tag, schedule_status.offset});
            if (!deferred_active_callback) return fail(Status{ErrorCode::protocol_error});
            return Status::success();
        }
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
                detail::CoreEvent logout_event{};
                logout_event.kind = detail::CoreEventKind::local_logout;
                logout_event.now_ms = now_ms;
                logout_event.text = text == nullptr ? std::string_view{} : text->value;
                logout_event.poss_dup = true; // Mark close_after_logout_ = true on peer-initiated Logout.
                const auto logout_output = core.process(logout_event);
                if (!logout_output.status.ok()) return logout_output.status;
                if (const auto status = apply_actions(logout_output, nullptr, id, seq, now_ms, wall_time, 1);
                    !status.ok()) return status;
            }
        } else if (type->value == "3") {
            report_peer_reject(message);
        }
        return append_delivery(id, seq);
    }

    [[nodiscard]] bool has_dependent_outbound(RecordId inbound_id) const noexcept {
        for (const auto& record : journal.records()) {
            if (record.kind == RecordKind::outbound && record.dependency == inbound_id) return true;
        }
        return false;
    }

    void increment_dependent_reply(RecordId inbound_id) noexcept {
        if (hot_dependent_reply_id == inbound_id) {
            ++hot_dependent_reply_count;
            return;
        }
        if (hot_dependent_reply_id == 0 && dependent_reply_counts.empty()) {
            hot_dependent_reply_id = inbound_id;
            hot_dependent_reply_count = 1;
            return;
        }
        if (hot_dependent_reply_id != 0) {
            dependent_reply_counts[hot_dependent_reply_id] += hot_dependent_reply_count;
            hot_dependent_reply_id = 0;
            hot_dependent_reply_count = 0;
        }
        ++dependent_reply_counts[inbound_id];
    }

    void clear_dependent_reply(RecordId inbound_id) noexcept {
        if (hot_dependent_reply_id == inbound_id) {
            hot_dependent_reply_id = 0;
            hot_dependent_reply_count = 0;
            return;
        }
        if (!dependent_reply_counts.empty()) {
            dependent_reply_counts.erase(inbound_id);
        }
    }

    [[nodiscard]] std::size_t dependent_application_replies(RecordId inbound_id) const noexcept {
        if (hot_dependent_reply_id == inbound_id) return hot_dependent_reply_count;
        if (dependent_reply_counts.empty()) return 0U;
        const auto found = dependent_reply_counts.find(inbound_id);
        return found == dependent_reply_counts.end() ? 0U : found->second;
    }

    [[nodiscard]] Status deliver_recovery(std::uint64_t now_ms, std::string_view wall_time) noexcept {
        if (deferred_active_callback ||
            (callbacks.deferred_delivery && deferred_delivery_id != 0)) return Status::success();
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
                        DeliveryInfo{record_id, sequence, entry.is_recovery}, now_ms, wall_time); !status.ok()) return status;
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
                if (const auto status = parse_frame_aux(record.payload, original); !status.ok()) {
                    return Status{ErrorCode::corrupt_store};
                }
                bool duplicate = false;
                const auto* original_time = unique_field(original, 52, duplicate);
                if (duplicate || original_time == nullptr) return Status{ErrorCode::corrupt_store, 52};
                auto original_header = make_header(record.seq, original_time->value);
                const auto* original_ack = unique_field(original, 369, duplicate);
                if (duplicate) return Status{ErrorCode::corrupt_store, 369};
                original_header.last_msg_seq_num_processed.reset();
                if (original_ack != nullptr) {
                    std::uint64_t ack{};
                    if (!parse_unsigned(original_ack->value, ack)) return Status{ErrorCode::corrupt_store, 369};
                    original_header.last_msg_seq_num_processed = ack;
                }
                const auto candidate = encode_fix(message, original_header, encode_buffer);
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
        const auto header = make_header(sequence, wall_time);
        const auto receipt_id = next_receipt_id;
        const auto replay_reserve = replay_reservation_bytes();
        const std::size_t max_allowed = std::min({
            config.max_frame_bytes,
            byte_limit - business_bytes,
            config.max_pending_bytes - pending_bytes_count - replay_reserve
        });

        // Tight upper-bound reserve so small segments pack densely without premature rotation.
        std::size_t reserve_bound = 192U + message.value_byte_count + message.field_count * 40U;
        if (message.group_count != 0 && message.groups != nullptr) {
            for (std::size_t g = 0; g < message.group_count; ++g) {
                const auto& grp = message.groups[g];
                if (grp.entries != nullptr) {
                    for (std::size_t e = 0; e < grp.entry_count; ++e) {
                        reserve_bound += grp.entries[e].value_byte_count + grp.entries[e].field_count * 40U;
                    }
                }
            }
        }
        reserve_bound = std::min(config.max_frame_bytes, reserve_bound);

        // Zero-copy path: encode directly into pre-faulted mmap Journal payload slot.
        RecordId committed_id = 0;
        std::size_t encoded_size = 0;
        auto reserved = journal.reserve(RecordKind::outbound, sequence, receipt_id, dependency, reserve_bound);
        if (reserved.ok()) {
            Result<std::size_t> encoded{Status{ErrorCode::capacity_exceeded}};
            if (message.prebuilt_body && session_prefix.valid) {
                auto buf = detail::StreamBuffer::begin_session_frame(
                    reserved.value().payload, message.msg_type, session_prefix,
                    sequence, wall_time, header.last_msg_seq_num_processed);
                if (buf.ok() && message.value_byte_count != 0) {
                    buf.write_raw(message.value_bytes, message.value_byte_count);
                }
                encoded = buf.finish_frame();
            } else {
                encoded = encode_fix(message, header, reserved.value().payload);
            }
            if (encoded.ok()) {
                if (encoded.value() > max_allowed) {
                    capacity_blocked = true;
                    return Status{ErrorCode::queue_full};
                }
                const auto committed = journal.commit(reserved.value(), encoded.value());
                if (!committed.ok()) return fail(committed.status());
                committed_id = committed.value().id;
                encoded_size = encoded.value();
            } else if (encoded.status().code != ErrorCode::capacity_exceeded) {
                return encoded.status();
            }
        }
        if (committed_id == 0) {
            // Exact-size fallback for tight/nearly-full segments or deeply nested groups.
            const auto encoded = encode_fix(message, header, encode_buffer);
            if (!encoded.ok()) return encoded.status();
            if (encoded.value() > max_allowed) {
                capacity_blocked = true;
                return Status{ErrorCode::queue_full};
            }
            const auto appended = journal.append(RecordKind::outbound, sequence, receipt_id, dependency,
                std::string_view{encode_buffer.data(), encoded.value()});
            if (!appended.ok()) return fail(appended.status());
            committed_id = appended.value().id;
            encoded_size = encoded.value();
        }
        if (dependency != 0 && receipt_id != 0) increment_dependent_reply(dependency);
        last_appended_outbound_segment = std::max(last_appended_outbound_segment, segment_index(committed_id));
        if (const auto status = enqueue(committed_id, encoded_size, sequence,
                receipt_id, message.msg_type, now_ms, true); !status.ok()) {
            return fail(Status{ErrorCode::protocol_error, 34});
        }
        if (const auto status = update_retention_floor(); !status.ok()) return fail(status);
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

    [[nodiscard]] Result<SendReceipt> send_application_stream(std::string_view msg_type,
        SessionDriver::StreamEncodeFn encode_fn, void* encode_ctx,
        std::uint64_t now_ms, std::string_view wall_time, RecordId dependency,
        std::size_t dependency_ordinal, bool recovery) noexcept {
        if (fatal) return last_status.ok() ? Status{ErrorCode::engine_stopped} : last_status;
        if (!is_active()) return Status{ErrorCode::not_ready};
        if (msg_type.empty() || is_session_admin(msg_type)) {
            return Status{ErrorCode::managed_field, 35};
        }
        if (msg_type.size() > max_msg_type_bytes) return Status{ErrorCode::unsupported, 35};
        if (dependency != 0 && recovery) [[unlikely]] {
            std::size_t ordinal = 0;
            for (const auto& record : journal.records()) {
                if (record.kind != RecordKind::outbound || record.dependency != dependency || record.receipt == 0) continue;
                if (ordinal++ != dependency_ordinal) continue;
                FlatMessageView original{};
                if (const auto status = parse_frame_aux(record.payload, original); !status.ok()) {
                    return Status{ErrorCode::corrupt_store};
                }
                bool duplicate = false;
                const auto* original_time = unique_field(original, 52, duplicate);
                if (duplicate || original_time == nullptr) return Status{ErrorCode::corrupt_store, 52};
                const auto* original_ack_field = unique_field(original, 369, duplicate);
                if (duplicate) return Status{ErrorCode::corrupt_store, 369};
                std::optional<std::uint64_t> original_ack{};
                if (original_ack_field != nullptr) {
                    std::uint64_t ack{};
                    if (!parse_unsigned(original_ack_field->value, ack)) return Status{ErrorCode::corrupt_store, 369};
                    original_ack = ack;
                }
                const auto candidate = encode_fn(encode_ctx, encode_buffer, session_prefix,
                    record.seq, original_time->value, original_ack, nullptr, nullptr);
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
        const auto replay_reserve = replay_reservation_bytes();
        if (business_count >= message_limit || tx_count >= tx_slots.size() ||
            business_bytes >= byte_limit ||
            replay_reserve > config.max_pending_bytes - pending_bytes_count) {
            capacity_blocked = true;
            return Status{ErrorCode::queue_full};
        }
        const auto sequence = core.next_outbound();
        if (sequence == 0) return fail(Status{ErrorCode::too_large, 34});
        const auto receipt_id = next_receipt_id;
        if (receipt_id == 0) return fail(Status{ErrorCode::receipt_expired});
        const std::size_t max_allowed = std::min({
            config.max_frame_bytes,
            byte_limit - business_bytes,
            config.max_pending_bytes - pending_bytes_count - replay_reserve
        });
        const auto last_proc = last_processed_seq();
        const std::size_t fast_reserve = std::min<std::size_t>(config.max_frame_bytes, 1024U);

        RecordId committed_id = 0;
        std::size_t encoded_size = 0;
        auto reserved = journal.reserve(RecordKind::outbound, sequence, receipt_id, dependency, fast_reserve);
        if (reserved.ok()) {
            struct SpillCtx {
                std::span<char> fallback;
                bool spilled{false};
            } spill{encode_buffer, false};
            auto grow = +[](void* raw, const char* old_data, std::size_t written,
                            std::size_t min_cap) noexcept -> std::span<char> {
                auto* st = static_cast<SpillCtx*>(raw);
                if (st->spilled || min_cap > st->fallback.size()) return {};
                if (written != 0) std::memcpy(st->fallback.data(), old_data, written);
                st->spilled = true;
                return st->fallback;
            };
            const auto encoded = encode_fn(encode_ctx, reserved.value().payload,
                session_prefix, sequence, wall_time, last_proc, grow, &spill);
            if (!encoded.ok()) return encoded.status();
            if (encoded.value() > max_allowed) {
                capacity_blocked = true;
                return Status{ErrorCode::queue_full};
            }
            if (!spill.spilled) {
                const auto committed = journal.commit(reserved.value(), encoded.value());
                if (!committed.ok()) return fail(committed.status());
                committed_id = committed.value().id;
                encoded_size = encoded.value();
            } else {
                const auto appended = journal.append(RecordKind::outbound, sequence, receipt_id, dependency,
                    std::string_view{encode_buffer.data(), encoded.value()});
                if (!appended.ok()) return fail(appended.status());
                committed_id = appended.value().id;
                encoded_size = encoded.value();
            }
        } else {
            const auto encoded = encode_fn(encode_ctx, encode_buffer,
                session_prefix, sequence, wall_time, last_proc, nullptr, nullptr);
            if (!encoded.ok()) return encoded.status();
            if (encoded.value() > max_allowed) {
                capacity_blocked = true;
                return Status{ErrorCode::queue_full};
            }
            const auto appended = journal.append(RecordKind::outbound, sequence, receipt_id, dependency,
                std::string_view{encode_buffer.data(), encoded.value()});
            if (!appended.ok()) return fail(appended.status());
            committed_id = appended.value().id;
            encoded_size = encoded.value();
        }
        if (dependency != 0 && receipt_id != 0) increment_dependent_reply(dependency);
        last_appended_outbound_segment = std::max(last_appended_outbound_segment, segment_index(committed_id));
        if (const auto status = enqueue(committed_id, encoded_size, sequence,
                receipt_id, msg_type, now_ms, true); !status.ok()) {
            return fail(Status{ErrorCode::protocol_error, 34});
        }
        if (const auto status = update_retention_floor(); !status.ok()) return fail(status);
        put_receipt(receipt_id, sequence, ReceiptState::appended);
        ++next_receipt_id;
        detail::CoreEvent event{};
        event.kind = detail::CoreEventKind::outbound_appended;
        event.now_ms = now_ms;
        event.sequence = sequence;
        event.msg_type = msg_type;
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
        replay_origin_begin = 0;
        replay_active = false;
        replay_prepared = false;
        replay_offset = 0;
        replay_bytes = 0;
        replay_receipt_id = 0;
        replay_dependency = 0;
        replay_merged_dependencies.clear();
        for (auto& slot : replay_slots) slot.merged_dependencies.clear();
        retention_deferred = false;
        write_timer_active = false;
        stall_logout_attempted = false;
        stall_logout_deadline = 0;
        secure_clear(logon_tx_buffer);
        logon_tx_size = 0;
        secure_clear(deferred_logon_buffer);
        deferred_logon_size = 0;
        deferred_logon_sequence = 0;
        deferred_logon_pending = false;
        peer_reset_requested = false;
        local_epoch_reset_started = false;
        one_shot_reset_active = false;
        staged_tx_active = false;
        staged_tx_is_replay = false;
        staged_tx_bytes = 0;
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
        if (!recovery_status.ok()) return fail(recovery_status);
        if (const auto status = set_wall_time(wall_time); !status.ok()) return status;
        callback_now_ms = now_ms;
        clear_transmit_state();
        last_status = Status::success();
        const bool want_reset = config.role == SessionRole::initiator &&
            ((config.settings && config.settings->logon.request_sequence_reset) || one_shot_sequence_reset);
        if (want_reset && !local_epoch_reset_started) {
            if (config.settings && !config.settings->allow_sequence_reset)
                return fail(Status{ErrorCode::sequence_reset_denied, 141});
            const std::string_view reset_reason =
                (one_shot_sequence_reset && !one_shot_reset_reason.empty())
                    ? std::string_view{one_shot_reset_reason}
                    : std::string_view{"local Logon ResetSeqNumFlag=Y"};
            const auto reset = journal.begin_epoch(reset_reason);
            if (!reset.ok()) return fail(reset);
            detail::CoreEvent reset_core{};
            reset_core.kind = detail::CoreEventKind::reset_epoch;
            const auto reset_output = core.process(reset_core);
            if (!reset_output.status.ok()) return fail(reset_output.status);
            peer_ack_seq = 0;
            local_epoch_reset_started = true;
            if (one_shot_sequence_reset) {
                one_shot_sequence_reset = false;
                one_shot_reset_active = true;
            }
            last_retention_outbound_segment = 0;
            last_appended_outbound_segment = 0;
            if (const auto status = update_retention_floor(true); !status.ok()) return fail(status);
        }
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
        if (journal.needs_maintenance()) {
            if (const auto maint = journal.poll_maintenance(); !maint.ok()) return fail(maint);
        }
        detail::CoreEvent event{};
        event.kind = detail::CoreEventKind::tick;
        event.now_ms = now_ms;
        event.has_pending_tx = tx_count != 0 || replay_active || replay_count != 0;
        const auto output = core.process(event);
        if (const auto status = execute_core_output(output, nullptr, 0, 0,
                now_ms, callback_wall_time); !status.ok()) return status;
        if (core.state() == SessionState::active && callbacks.deferred_delivery)
            return deliver_recovery(now_ms, callback_wall_time);
        return Status::success();
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
        std::string_view wall_time, bool recovery = false,
        const FrameView* verified_frame = nullptr) noexcept {
        FlatMessageView message{};
        if (verified_frame != nullptr && verified_frame->consumed == frame_bytes.size() &&
            frame_bytes.size() <= config.max_frame_bytes) {
            if (const auto status = parse_flat_verified(*verified_frame, parse_fields, message, callbacks.wire_hints);
                !status.ok()) {
                return fail(status);
            }
            message.bind_index(&parse_index);
        } else {
            if (const auto status = parse_frame(frame_bytes, message); !status.ok()) return fail(status);
        }
        const auto tags = extract_inbound_tags(message);
        std::string_view msg_type;
        std::string_view test_id;
        std::uint64_t sequence{};
        bool poss_dup = false;
        bool orig_valid = false;
        if (const auto status = parse_inbound_header(tags, msg_type, sequence,
                poss_dup, orig_valid, test_id); !status.ok()) return fail(status);

        std::uint64_t peer_heartbeat_ms = 0;
        std::uint64_t resend_begin = 0;
        std::uint64_t resend_end = 0;
        std::uint64_t next_inbound = 0;
        std::uint64_t effective_sequence = sequence;
        bool sequence_reset_event = false;
        bool gapfill_event = false;
        bool duplicate_gapfill = false;
        bool noop_sequence_reset = false;
        bool reject_sequence_reset = false;
        bool logon_reset_requested = false;
        std::optional<std::uint64_t> inbound_peer_ack{};
        std::string_view logout_text{};

        const auto* sent_time_field = tags.sending_time;
        if (sent_time_field == nullptr) return fail(Status{ErrorCode::invalid_timestamp, 52});
        if (!sending_time_within_skew(sent_time_field->value, wall_time)) {
            const Status stale{ErrorCode::invalid_timestamp, 52};
            if (const auto status = send_protocol_reject(sequence, msg_type, stale, 10,
                    now_ms, wall_time); !status.ok()) return fail(status);
            notify_error(stale);
            if (const auto status = on_logout("SendingTime accuracy problem", now_ms, wall_time); !status.ok())
                return status;
            return Status::success();
        }

        if (msg_type == "A") {
            if (tags.encrypt == nullptr || tags.encrypt->value != "0") {
                return fail(Status{ErrorCode::protocol_error, 98});
            }
            std::uint64_t seconds{};
            if (tags.hb_int == nullptr || !parse_unsigned(tags.hb_int->value, seconds) || seconds == 0 ||
                seconds > std::numeric_limits<std::uint32_t>::max() / 1000U) {
                return fail(Status{ErrorCode::protocol_error, 108});
            }
            peer_heartbeat_ms = seconds * 1000;
            if (!recovery) {
                if (const auto status = validate_logon_policy(message, tags, sequence,
                        logon_reset_requested); !status.ok()) return fail(status);
            } else if (config.settings && config.settings->version == FixVersion::fixt11) {
                if (tags.appl_ver == nullptr ||
                    tags.appl_ver->value != config.settings->default_appl_ver_id)
                    return fail(Status{ErrorCode::unsupported_protocol, 1137});
            }
        } else if (msg_type == "2") {
            if (tags.begin_seq == nullptr || !parse_unsigned(tags.begin_seq->value, resend_begin)) {
                return fail(Status{ErrorCode::protocol_error, 7});
            }
            if (tags.end_seq == nullptr || !parse_unsigned(tags.end_seq->value, resend_end)) {
                return fail(Status{ErrorCode::protocol_error, 16});
            }
            if (resend_begin == 0 || (resend_end != 0 && resend_end < resend_begin)) {
                return fail(Status{ErrorCode::protocol_error, 7});
            }
        } else if (msg_type == "4") {
            // Tag 123 (GapFillFlag) is optional in FIX and defaults to 'N' (Reset mode).
            if (tags.gap_fill != nullptr && tags.gap_fill->value != "N" && tags.gap_fill->value != "Y") {
                return fail(Status{ErrorCode::invalid_value, 123});
            }
            const bool is_gap_fill = tags.gap_fill != nullptr && tags.gap_fill->value == "Y";
            if (tags.new_seq == nullptr || !parse_unsigned(tags.new_seq->value, next_inbound)) {
                return fail(Status{ErrorCode::protocol_error, 36});
            }
            if (!is_gap_fill) {
                if (next_inbound > core.next_inbound()) {
                    sequence_reset_event = true;
                    effective_sequence = core.next_inbound();
                } else if (next_inbound == core.next_inbound() || poss_dup) {
                    // FIX 4.4 / QuickFIX parity: NewSeqNo == expected (no-op) or
                    // PossDup retransmission of an already-applied Reset is ignored cleanly.
                    noop_sequence_reset = true;
                } else {
                    // FIX 4.4 / QuickFIX parity: live attempt to lower expected sequence
                    // replies with Session Reject (373=5) without terminating the session.
                    reject_sequence_reset = true;
                }
            } else {
                if (!poss_dup || !orig_valid || next_inbound <= sequence)
                    return fail(Status{ErrorCode::protocol_error, 123});
                if (next_inbound <= core.next_inbound()) {
                    // Duplicate replayed GapFill whose NewSeqNo is already satisfied: ignore cleanly.
                    duplicate_gapfill = true;
                } else {
                    gapfill_event = true;
                    if (sequence < core.next_inbound()) effective_sequence = core.next_inbound();
                }
            }
        } else if (msg_type == "5") {
            if (tags.text != nullptr) logout_text = tags.text->value;
        }

        // Validate peer 369 syntax early; monotonicity check is applied after duplicate check
        // so replayed PossDup duplicates with older 369 do not falsely fail the session.
        std::optional<std::uint64_t> parsed_ack_candidate{};
        if (tags.peer_ack != nullptr) {
            std::uint64_t ack_val{};
            if (!parse_unsigned(tags.peer_ack->value, ack_val))
                return fail(Status{ErrorCode::invalid_value, 369});
            parsed_ack_candidate = ack_val;
        }

        if (duplicate_gapfill) {
            detail::CoreEvent dup_check{};
            dup_check.kind = detail::CoreEventKind::inbound_candidate;
            dup_check.now_ms = now_ms;
            dup_check.sequence = sequence;
            dup_check.msg_type = msg_type;
            dup_check.poss_dup = poss_dup;
            dup_check.orig_sending_time_valid = orig_valid;
            const auto dup_disp = core.process(dup_check);
            if (!dup_disp.status.ok()) return fail(dup_disp.status);
            return Status::success();
        }

        if (noop_sequence_reset || reject_sequence_reset) {
            detail::CoreEvent reset_check{};
            reset_check.kind = detail::CoreEventKind::inbound_candidate;
            reset_check.now_ms = now_ms;
            reset_check.sequence = core.next_inbound();
            reset_check.msg_type = msg_type;
            reset_check.poss_dup = poss_dup;
            reset_check.orig_sending_time_valid = orig_valid;
            const auto reset_disp = core.process(reset_check);
            if (!reset_disp.status.ok()) return fail(reset_disp.status);
            if (!poss_dup && parsed_ack_candidate) {
                const auto last_outbound = journal.next_outbound_seq() == 0
                    ? 0 : journal.next_outbound_seq() - 1;
                if (*parsed_ack_candidate < peer_ack_seq || *parsed_ack_candidate > last_outbound)
                    return fail(Status{ErrorCode::protocol_error, 369});
            }
            if (reject_sequence_reset) {
                const Status reject_err{ErrorCode::invalid_value, 36};
                callback_now_ms = now_ms;
                callback_wall_time = wall_time;
                if (const auto status = send_protocol_reject(sequence, "4", reject_err, 5,
                        now_ms, wall_time); !status.ok()) {
                    return fail(status);
                }
                notify_error(reject_err);
                if (!callback_observer_status.ok()) return fail(callback_observer_status);
            }
            return Status::success();
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
            if (parsed_ack_candidate) {
                const auto last_outbound = journal.next_outbound_seq() == 0
                    ? 0 : journal.next_outbound_seq() - 1;
                if (*parsed_ack_candidate < peer_ack_seq || *parsed_ack_candidate > last_outbound)
                    return fail(Status{ErrorCode::protocol_error, 369});
            }
            if (frame_bytes.size() > deferred_logon_buffer.size()) return fail(Status{ErrorCode::too_large});
            std::memcpy(deferred_logon_buffer.data(), frame_bytes.data(), frame_bytes.size());
            deferred_logon_size = frame_bytes.size();
            deferred_logon_sequence = sequence;
            deferred_logon_pending = true;

            detail::CoreEvent candidate{};
            candidate.kind = detail::CoreEventKind::inbound_candidate;
            candidate.now_ms = now_ms;
            candidate.sequence = effective_sequence;
            candidate.last = sequence - 1; // The high Logon is already in hand; request only its gap.
            candidate.msg_type = msg_type;
            candidate.poss_dup = poss_dup;
            candidate.orig_sending_time_valid = orig_valid;
            const auto gap_output = core.process(candidate);
            if (!gap_output.status.ok() || gap_output.inbound != detail::InboundDisposition::gap) {
                deferred_logon_pending = false;
                secure_clear(deferred_logon_buffer);
                deferred_logon_size = 0;
                deferred_logon_sequence = 0;
                return fail(gap_output.status.ok() ? Status{ErrorCode::protocol_error, 34} : gap_output.status);
            }
            detail::CoreEvent out_of_order{};
            out_of_order.kind = detail::CoreEventKind::out_of_order_logon;
            out_of_order.now_ms = now_ms;
            out_of_order.first = peer_heartbeat_ms;
            const auto logon_output = core.process(out_of_order);
            if (!logon_output.status.ok()) {
                deferred_logon_pending = false;
                secure_clear(deferred_logon_buffer);
                deferred_logon_size = 0;
                deferred_logon_sequence = 0;
                return fail(logon_output.status);
            }
            callback_now_ms = now_ms;
            callback_wall_time = wall_time;
            if (const auto status = apply_actions(logon_output, nullptr, 0, 0,
                    now_ms, wall_time); !status.ok()) {
                secure_clear(deferred_logon_buffer);
                deferred_logon_size = 0;
                deferred_logon_sequence = 0;
                return status;
            }
            const auto gap_status = apply_actions(gap_output, nullptr, 0, 0, now_ms, wall_time);
            if (!gap_status.ok()) {
                secure_clear(deferred_logon_buffer);
                deferred_logon_size = 0;
                deferred_logon_sequence = 0;
            }
            return gap_status;
        }
        if (msg_type == "A" && deferred_logon_pending && sequence == deferred_logon_sequence) {
            deferred_logon = true;
        }

        // For acceptor 141=Y Logon, commit epoch reset only after all Logon validations have passed.
        if (msg_type == "A" && logon_reset_requested && config.role == SessionRole::acceptor) {
            const auto reset_status = journal.begin_epoch("peer Logon ResetSeqNumFlag=Y");
            if (!reset_status.ok()) return fail(reset_status);
            detail::CoreEvent reset_core{};
            reset_core.kind = detail::CoreEventKind::reset_epoch;
            const auto reset_output = core.process(reset_core);
            if (!reset_output.status.ok()) return fail(reset_output.status);
            peer_ack_seq = 0;
            peer_reset_requested = true;
            last_retention_outbound_segment = 0;
            last_appended_outbound_segment = 0;
            if (const auto status = update_retention_floor(true); !status.ok()) return fail(status);
        }

        detail::CoreEvent candidate{};
        candidate.kind = detail::CoreEventKind::inbound_candidate;
        candidate.now_ms = now_ms;
        candidate.sequence = effective_sequence;
        candidate.msg_type = msg_type;
        candidate.poss_dup = poss_dup;
        candidate.orig_sending_time_valid = orig_valid;
        candidate.is_deferred_logon = deferred_logon;
        if (msg_type == "2") {
            candidate.first = resend_begin;
            candidate.last = resend_end;
        }
        const auto disposition = core.process(candidate);
        if (!disposition.status.ok()) return fail(disposition.status);
        if (disposition.inbound == detail::InboundDisposition::duplicate) return Status::success();
        if (parsed_ack_candidate) {
            const auto last_outbound = journal.next_outbound_seq() == 0
                ? 0 : journal.next_outbound_seq() - 1;
            if (*parsed_ack_candidate < peer_ack_seq || *parsed_ack_candidate > last_outbound)
                return fail(Status{ErrorCode::protocol_error, 369});
            inbound_peer_ack = parsed_ack_candidate;
        }
        if (disposition.inbound == detail::InboundDisposition::gap) {
            callback_now_ms = now_ms;
            callback_wall_time = wall_time;
            return apply_actions(disposition, nullptr, 0, 0, now_ms, wall_time);
        }
        if (disposition.inbound != detail::InboundDisposition::accept) return fail(Status{ErrorCode::protocol_error});
        if (!is_session_admin(msg_type)) {
            const bool pending_capacity = recovery_count < recovery_slots.size() &&
                frame_bytes.size() <= config.max_pending_inbound_bytes - recovery_bytes;
            const bool frame_reserve = core.state() == SessionState::awaiting_logon || can_receive_now();
            if (!pending_capacity || !frame_reserve) return Status{ErrorCode::queue_full};
        }

        std::string_view stored_payload = frame_bytes;
        std::uint64_t persisted_next = 0;
        if (sequence_reset_event || gapfill_event) persisted_next = next_inbound;
        if (msg_type == "A") {
            const auto safe = encode_safe_logon(message, sequence, poss_dup,
                orig_valid, tags.orig_time == nullptr ? std::string_view{} : tags.orig_time->value,
                sent_time_field->value, inbound_peer_ack);
            if (!safe.ok()) return fail(safe.status());
            stored_payload = {encode_buffer.data(), safe.value()};
        }
        const auto appended = journal.append(RecordKind::inbound, effective_sequence, 0, 0,
            stored_payload, persisted_next);
        if (!appended.ok()) return fail(appended.status());
        const auto inbound_id = appended.value().id;
        if (!is_session_admin(msg_type)) {
            const bool is_gap_or_replay_recovery = core.state() == SessionState::awaiting_logon || poss_dup;
            if (const auto status = remember_inbound(inbound_id, stored_payload.size(),
                    is_gap_or_replay_recovery, false); !status.ok())
                return fail(status);
        } else if (msg_type == "2") {
            // Track pending ResendRequest in recovery_slots so disconnect mid-replay
            // replays and completes it upon reconnect without freezing 369/retention.
            if (recovery_count < recovery_slots.size() &&
                stored_payload.size() <= config.max_pending_inbound_bytes - recovery_bytes) {
                (void)remember_inbound(inbound_id, stored_payload.size(), true, true);
            }
        }
        if (inbound_peer_ack) peer_ack_seq = *inbound_peer_ack;

        const bool merged_gapfill_covered_deferred_logon =
            (sequence_reset_event || gapfill_event) && deferred_logon_pending &&
            appended.value().next_seq > deferred_logon_sequence;

        detail::CoreEvent committed{};
        committed.kind = sequence_reset_event || gapfill_event
            ? detail::CoreEventKind::sequence_reset_committed
            : detail::CoreEventKind::inbound_committed;
        committed.now_ms = now_ms;
        committed.sequence = effective_sequence;
        committed.next_sequence = appended.value().next_seq;
        committed.msg_type = msg_type;
        committed.first = peer_heartbeat_ms;
        committed.last = resend_end;
        committed.text = msg_type == "1" || msg_type == "0" ? test_id : logout_text;
        committed.poss_dup = poss_dup;
        committed.orig_sending_time_valid = orig_valid;
        committed.is_deferred_logon = deferred_logon || merged_gapfill_covered_deferred_logon;
        if (msg_type == "2") committed.first = resend_begin;
        const auto actions = core.process(committed);
        if (!actions.status.ok()) return fail(actions.status);
        callback_now_ms = now_ms;
        callback_wall_time = wall_time;
        if (const auto status = apply_actions(actions,
                is_session_admin(msg_type) ? nullptr : &message,
                inbound_id, effective_sequence, now_ms, wall_time); !status.ok()) return status;
        if (is_session_admin(msg_type) && msg_type != "2") {
            if (const auto status = append_delivery(inbound_id, effective_sequence); !status.ok()) return fail(status);
        }
        if (msg_type == "3") report_peer_reject(message);
        if (const auto status = update_retention_floor(); !status.ok()) return fail(status);
        if (deferred_logon || merged_gapfill_covered_deferred_logon) {
            secure_clear(deferred_logon_buffer);
            deferred_logon_pending = false;
            deferred_logon_size = 0;
            deferred_logon_sequence = 0;
        } else if (deferred_logon_pending && core.next_inbound() == deferred_logon_sequence) {
            processing_deferred_logon = true;
            const auto deferred = std::string_view{deferred_logon_buffer.data(), deferred_logon_size};
            const auto status = process_inbound(deferred, now_ms, wall_time, true);
            processing_deferred_logon = false;
            if (!status.ok()) return status;
            secure_clear(deferred_logon_buffer);
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
                skipped.now_ms = now_ms;
                skipped.sequence = front->sequence;
                skipped.msg_type = {front->msg_type.data(), front->msg_type_size};
                const auto skip_output = core.process(skipped);
                if (const auto status = pop_tx(false, now_ms); !status.ok()) return status;
                if (skip_output.action_count != 0 || !skip_output.status.ok()) {
                    if (const auto status = apply_actions(skip_output, nullptr, 0, 0,
                            now_ms, callback_wall_time); !status.ok()) return status;
                }
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
            core.state() == SessionState::failed || staged_tx_active) return Status{ErrorCode::not_ready};
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
            } else if (queued->ephemeral) {
                if (logon_tx_size != queued->frame_size || queued->offset > logon_tx_size)
                    return fail(Status{ErrorCode::corrupt_store});
                remaining = {logon_tx_buffer.data() + queued->offset, logon_tx_size - queued->offset};
            } else {
                const auto* record = journal.find_record(queued->record_id);
                if (record == nullptr && queued->record_id < journal.lowest_retained_record_id()) {
                    return fail(Status{ErrorCode::history_unavailable});
                }
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

    [[nodiscard]] Status on_flush_batch(WriteBatchFn write_batch, WriteFn write_single,
        void* context, std::uint64_t now_ms, std::size_t byte_budget) noexcept {
        if (write_batch == nullptr || write_single == nullptr) return Status{ErrorCode::invalid_config};
        if (fatal) return last_status.ok() ? Status{ErrorCode::engine_stopped} : last_status;
        if (core.state() == SessionState::disconnected || core.state() == SessionState::closed ||
            core.state() == SessionState::failed || staged_tx_active) return Status{ErrorCode::not_ready};
        if (replay_active || replay_count != 0 || tx_count <= 1) {
            return on_flush(write_single, context, now_ms, byte_budget);
        }
        callback_now_ms = now_ms;
        std::size_t written_this_turn = 0;
        while (written_this_turn < byte_budget && !should_close && tx_count != 0) {
            if (replay_active || replay_count != 0 || tx_count == 1 || front_tx()->ephemeral) {
                return on_flush(write_single, context, now_ms, byte_budget - written_this_turn);
            }
            std::array<std::span<const char>, 64> chunks{};
            std::size_t chunk_count = 0;
            std::size_t batch_bytes = 0;
            const std::size_t remaining_budget = byte_budget - written_this_turn;
            for (std::size_t i = 0; i < tx_count && chunk_count < chunks.size() && batch_bytes < remaining_budget; ++i) {
                const auto& desc = tx_slots[tx_index(i)];
                if (desc.ephemeral) break;
                const auto* record = journal.find_record(desc.record_id);
                if (record == nullptr && desc.record_id < journal.lowest_retained_record_id()) {
                    return fail(Status{ErrorCode::history_unavailable});
                }
                if (record == nullptr || record->kind != RecordKind::outbound ||
                    record->payload.size() != desc.frame_size || desc.offset > desc.frame_size) {
                    return fail(Status{ErrorCode::corrupt_store});
                }
                const std::size_t rem = desc.frame_size - desc.offset;
                const std::size_t take = std::min(rem, remaining_budget - batch_bytes);
                chunks[chunk_count++] = std::span<const char>{record->payload.data() + desc.offset, take};
                batch_bytes += take;
            }
            if (chunk_count == 0) break;
            const auto result = chunk_count == 1
                ? write_single(context, chunks[0])
                : write_batch(context, std::span<const std::span<const char>>{chunks.data(), chunk_count});
            if (result.bytes_written > batch_bytes) return fail(Status{ErrorCode::protocol_error});
            if (result.bytes_written != 0) {
                written_this_turn += result.bytes_written;
                last_byte_progress_ms = now_ms;
                write_timer_active = true;
                stall_logout_attempted = false;
                stall_logout_deadline = 0;
                std::size_t left = result.bytes_written;
                while (left != 0 && tx_count != 0) {
                    auto* front = front_tx();
                    const std::size_t rem = front->frame_size - front->offset;
                    if (left >= rem) {
                        left -= rem;
                        front->offset = front->frame_size;
                        if (const auto status = pop_tx(true, now_ms); !status.ok()) return status;
                        if (core.should_close()) should_close = true;
                    } else {
                        front->offset += left;
                        left = 0;
                    }
                }
            }
            if (!result.status.ok()) return transport_failure(result.status);
            if (result.bytes_written == 0 || result.would_block) break;
        }
        return fatal ? last_status : Status::success();
    }

    [[nodiscard]] Result<std::size_t> on_stage_tx(std::span<char> buffer, std::uint64_t now_ms) noexcept {
        if (fatal) return last_status.ok() ? Status{ErrorCode::engine_stopped} : last_status;
        if (core.state() == SessionState::disconnected || core.state() == SessionState::closed ||
            core.state() == SessionState::failed || staged_tx_active) {
            return Status{ErrorCode::not_ready};
        }
        if (buffer.empty() || should_close) return std::size_t{0};
        callback_now_ms = now_ms;
        start_next_replay();
        if (replay_active && !replay_prepared) {
            if (const auto status = build_replay_frame(); !status.ok()) return fail(status);
        }
        auto* queued = front_tx();
        const bool replay_due = replay_active && replay_prepared &&
            (queued == nullptr || (queued->offset == 0 && replay_sequence <= queued->sequence));
        if (!replay_due && queued == nullptr) return std::size_t{0};

        if (replay_due) {
            const std::size_t rem = replay_bytes - replay_offset;
            const std::size_t take = std::min(rem, buffer.size());
            if (take != 0) {
                std::memcpy(buffer.data(), replay_buffer.data() + replay_offset, take);
                staged_tx_active = true;
                staged_tx_is_replay = true;
                staged_tx_bytes = take;
            }
            return take;
        }

        std::size_t copied = 0;
        const bool allow_coalesce = !replay_active && replay_count == 0;
        for (std::size_t i = 0; i < tx_count && copied < buffer.size(); ++i) {
            const auto& desc = tx_slots[tx_index(i)];
            std::span<const char> slice;
            if (desc.ephemeral) {
                if (logon_tx_size != desc.frame_size || desc.offset > logon_tx_size) {
                    return fail(Status{ErrorCode::corrupt_store});
                }
                slice = {logon_tx_buffer.data() + desc.offset, logon_tx_size - desc.offset};
            } else {
                const auto* record = journal.find_record(desc.record_id);
                if (record == nullptr && desc.record_id < journal.lowest_retained_record_id()) {
                    return fail(Status{ErrorCode::history_unavailable});
                }
                if (record == nullptr || record->kind != RecordKind::outbound ||
                    record->payload.size() != desc.frame_size || desc.offset > desc.frame_size) {
                    return fail(Status{ErrorCode::corrupt_store});
                }
                slice = {record->payload.data() + desc.offset, desc.frame_size - desc.offset};
            }
            const std::size_t take = std::min(slice.size(), buffer.size() - copied);
            if (take != 0) {
                std::memcpy(buffer.data() + copied, slice.data(), take);
                copied += take;
            }
            const bool is_logout = desc.msg_type_size == 1 && desc.msg_type[0] == '5';
            if (take < slice.size() || !allow_coalesce || is_logout) break;
        }
        if (copied != 0) {
            staged_tx_active = true;
            staged_tx_is_replay = false;
            staged_tx_bytes = copied;
        }
        return copied;
    }

    [[nodiscard]] Status on_commit_tx_written(std::size_t bytes_written, std::uint64_t now_ms) noexcept {
        if (fatal) return last_status.ok() ? Status{ErrorCode::engine_stopped} : last_status;
        if (bytes_written == 0) return Status::success();
        if (!staged_tx_active || bytes_written > staged_tx_bytes) {
            return fail(Status{ErrorCode::protocol_error});
        }
        callback_now_ms = now_ms;
        const bool is_replay = staged_tx_is_replay;
        last_byte_progress_ms = now_ms;
        write_timer_active = true;
        stall_logout_attempted = false;
        stall_logout_deadline = 0;
        staged_tx_bytes -= bytes_written;
        if (staged_tx_bytes == 0) {
            staged_tx_active = false;
            staged_tx_is_replay = false;
        }
        if (is_replay) {
            if (!replay_active || !replay_prepared || bytes_written > replay_bytes - replay_offset) {
                return fail(Status{ErrorCode::protocol_error});
            }
            replay_offset += bytes_written;
            if (replay_offset == replay_bytes) {
                if (const auto status = finish_tx_frame(now_ms); !status.ok()) return status;
            }
            return fatal ? last_status : Status::success();
        }
        std::size_t left = bytes_written;
        while (left != 0 && tx_count != 0) {
            auto* front = front_tx();
            const std::size_t rem = front->frame_size - front->offset;
            if (left >= rem) {
                left -= rem;
                front->offset = front->frame_size;
                if (const auto status = pop_tx(true, now_ms); !status.ok()) return status;
                if (core.should_close()) should_close = true;
            } else {
                front->offset += left;
                left = 0;
            }
        }
        if (left != 0) return fail(Status{ErrorCode::protocol_error});
        return fatal ? last_status : Status::success();
    }

    void on_abort_staged_tx() noexcept {
        staged_tx_active = false;
        staged_tx_is_replay = false;
        staged_tx_bytes = 0;
    }

    static std::array<char, 24> format_current_utc() noexcept {
        std::array<char, 24> buf{};
        timespec ts{};
        ::clock_gettime(CLOCK_REALTIME, &ts);
        std::tm utc{};
        if (::gmtime_r(&ts.tv_sec, &utc) == nullptr) {
            std::memcpy(buf.data(), "19700101-00:00:00.000", 21);
            return buf;
        }
        auto w2 = [](char* o, unsigned v) noexcept {
            o[0] = static_cast<char>('0' + (v / 10U) % 10U);
            o[1] = static_cast<char>('0' + v % 10U);
        };
        auto w3 = [](char* o, unsigned v) noexcept {
            o[0] = static_cast<char>('0' + (v / 100U) % 10U);
            o[1] = static_cast<char>('0' + (v / 10U) % 10U);
            o[2] = static_cast<char>('0' + v % 10U);
        };
        auto w4 = [](char* o, unsigned v) noexcept {
            o[0] = static_cast<char>('0' + (v / 1000U) % 10U);
            o[1] = static_cast<char>('0' + (v / 100U) % 10U);
            o[2] = static_cast<char>('0' + (v / 10U) % 10U);
            o[3] = static_cast<char>('0' + v % 10U);
        };
        w4(buf.data() + 0, static_cast<unsigned>(utc.tm_year + 1900));
        w2(buf.data() + 4, static_cast<unsigned>(utc.tm_mon + 1));
        w2(buf.data() + 6, static_cast<unsigned>(utc.tm_mday));
        buf[8] = '-';
        w2(buf.data() + 9, static_cast<unsigned>(utc.tm_hour));
        buf[11] = ':';
        w2(buf.data() + 12, static_cast<unsigned>(utc.tm_min));
        buf[14] = ':';
        w2(buf.data() + 15, static_cast<unsigned>(utc.tm_sec));
        buf[17] = '.';
        w3(buf.data() + 18, static_cast<unsigned>(ts.tv_nsec / 1'000'000L));
        return buf;
    }

    [[nodiscard]] Status on_arm_one_shot_sequence_reset(std::string_view reason) noexcept {
        if (fatal) return last_status.ok() ? Status{ErrorCode::engine_stopped} : last_status;
        if (config.role != SessionRole::initiator) return Status{ErrorCode::sequence_reset_denied, 141};
        if (config.settings && !config.settings->allow_sequence_reset)
            return Status{ErrorCode::sequence_reset_denied, 141};
        if (reason.empty() || reason.size() > 200 || !detail::is_valid_text(reason))
            return Status{ErrorCode::invalid_value, 58};
        try {
            one_shot_reset_reason.assign(reason.data(), reason.size());
            one_shot_sequence_reset = true;
            return Status::success();
        } catch (...) {
            return Status{ErrorCode::capacity_exceeded};
        }
    }

    [[nodiscard]] Status on_reset_epoch_offline(std::string_view reason) noexcept {
        if (fatal) return last_status.ok() ? Status{ErrorCode::engine_stopped} : last_status;
        if (core.state() != SessionState::disconnected) return Status{ErrorCode::not_ready};
        if (config.settings && !config.settings->allow_sequence_reset)
            return Status{ErrorCode::sequence_reset_denied, 141};
        if (reason.empty() || reason.size() > 200 || !detail::is_valid_text(reason))
            return Status{ErrorCode::invalid_value, 58};
        const auto reset = journal.begin_epoch(reason);
        if (!reset.ok()) return reset;
        detail::CoreEvent rc{};
        rc.kind = detail::CoreEventKind::reset_epoch;
        const auto rout = core.process(rc);
        if (!rout.status.ok()) return rout.status;
        peer_ack_seq = 0;
        last_retention_outbound_segment = 0;
        last_appended_outbound_segment = 0;
        return update_retention_floor(true);
    }

    [[nodiscard]] Status on_adjust_sequences(std::uint64_t next_inbound,
        std::uint64_t next_outbound, std::string_view reason) noexcept {
        if (fatal) return last_status.ok() ? Status{ErrorCode::engine_stopped} : last_status;
        if (core.state() != SessionState::disconnected) return Status{ErrorCode::not_ready};
        if (next_inbound == 0 || next_outbound == 0) return Status{ErrorCode::invalid_value, 34};
        if (reason.empty() || reason.size() > 200 || !detail::is_valid_text(reason))
            return Status{ErrorCode::invalid_value, 58};
        if (next_inbound == core.next_inbound() && next_outbound == core.next_outbound())
            return Status::success();
        if (next_inbound < core.next_inbound() || next_outbound < core.next_outbound()) {
            if (config.settings && !config.settings->allow_sequence_reset)
                return Status{ErrorCode::sequence_reset_denied, 141};
            const auto reset = journal.begin_epoch(reason);
            if (!reset.ok()) return reset;
            detail::CoreEvent rc{};
            rc.kind = detail::CoreEventKind::reset_epoch;
            const auto rout = core.process(rc);
            if (!rout.status.ok()) return rout.status;
            peer_ack_seq = 0;
            last_retention_outbound_segment = 0;
            last_appended_outbound_segment = 0;
        }
        const auto utc_buf = format_current_utc();
        const std::string_view wt = wall_time_size != 0
            ? std::string_view{wall_time_storage.data(), wall_time_size}
            : std::string_view{utc_buf.data(), 21};

        if (next_inbound > core.next_inbound()) {
            const auto cur_in = core.next_inbound();
            std::array<AdminField, 3> fields{
                AdminField{123, FieldKind::character, static_cast<unsigned char>('N'), {}},
                AdminField{36, FieldKind::unsigned_integer, next_inbound, {}},
                AdminField{58, FieldKind::string, 0, reason},
            };
            AdminMessageStorage storage{};
            if (const auto st = make_admin_view("4", fields, storage, admin_arena); !st.ok()) return st;
            SessionHeader hdr{config.remote_comp_id, config.local_comp_id, cur_in, wt};
            hdr.version = fix_version();
            const auto enc = encode_fix(storage.view, hdr, encode_buffer);
            if (!enc.ok()) return enc.status();
            const auto app = journal.append(RecordKind::inbound, cur_in, 0, 0,
                std::string_view{encode_buffer.data(), enc.value()}, next_inbound);
            if (!app.ok()) return app.status();
            if (!journal.has_delivery(app.value().id)) {
                if (const auto del = append_delivery(app.value().id, cur_in); !del.ok()) return del;
            }
        }
        if (next_outbound > core.next_outbound()) {
            const auto cur_out = core.next_outbound();
            std::array<AdminField, 3> fields{
                AdminField{123, FieldKind::character, static_cast<unsigned char>('Y'), {}},
                AdminField{36, FieldKind::unsigned_integer, next_outbound, {}},
                AdminField{58, FieldKind::string, 0, reason},
            };
            AdminMessageStorage storage{};
            if (const auto st = make_admin_view("4", fields, storage, admin_arena); !st.ok()) return st;
            SessionHeader hdr{config.local_comp_id, config.remote_comp_id, cur_out, wt, true, wt};
            hdr.version = fix_version();
            const auto enc = encode_fix(storage.view, hdr, encode_buffer);
            if (!enc.ok()) return enc.status();
            const std::uint64_t persisted_next = next_outbound > cur_out + 1 ? next_outbound : 0;
            const auto app = journal.append(RecordKind::outbound, cur_out, 0, 0,
                std::string_view{encode_buffer.data(), enc.value()}, persisted_next);
            if (!app.ok()) return app.status();
            last_appended_outbound_segment = std::max(last_appended_outbound_segment, segment_index(app.value().id));
        }
        detail::CoreEvent adj{};
        adj.kind = detail::CoreEventKind::adjust_sequences;
        adj.first = next_inbound;
        adj.last = next_outbound;
        const auto aout = core.process(adj);
        if (!aout.status.ok()) return aout.status;
        return update_retention_floor(true);
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
            config.journal_capacity_bytes, config.journal_max_records, config.journal_test_hook,
            config.journal_segment_bytes, config.journal_max_segments});
        if (!opened.ok()) return opened.status();
        auto journal = std::move(opened).value();
        auto impl = std::make_unique<Impl>(std::move(config), std::move(callbacks), std::move(journal));
        if (!impl->recovery_status.ok()) return impl->recovery_status;
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
    if (impl_->fatal) return impl_->last_status.ok() ? Status{ErrorCode::engine_stopped} : impl_->last_status;
    if (impl_->core.state() == SessionState::disconnected || impl_->core.state() == SessionState::closed ||
        impl_->core.state() == SessionState::failed)
        return Status{ErrorCode::not_ready};
    if (const auto status = impl_->set_wall_time(wall_time); !status.ok()) return status;
    impl_->callback_now_ms = now_ms;
    return impl_->process_inbound(complete_frame, now_ms, impl_->callback_wall_time);
}

Status SessionDriver::receive_verified(FrameView verified_frame, std::uint64_t now_ms,
    std::string_view wall_time) noexcept {
    if (!impl_) return Status{ErrorCode::engine_stopped};
    if (const auto status = impl_->claim_owner(); !status.ok()) return status;
    if (impl_->fatal) return impl_->last_status.ok() ? Status{ErrorCode::engine_stopped} : impl_->last_status;
    if (impl_->core.state() == SessionState::disconnected || impl_->core.state() == SessionState::closed ||
        impl_->core.state() == SessionState::failed)
        return Status{ErrorCode::not_ready};
    if (const auto status = impl_->set_wall_time(wall_time); !status.ok()) return status;
    impl_->callback_now_ms = now_ms;
    return impl_->process_inbound(verified_frame.bytes, now_ms, impl_->callback_wall_time, false, &verified_frame);
}

Status SessionDriver::defer_delivery(DeliveryInfo info) noexcept {
    if (!impl_) return Status{ErrorCode::engine_stopped};
    if (const auto status = impl_->claim_owner(); !status.ok()) return status;
    if (!impl_->callback_active || !impl_->callbacks.deferred_delivery || info.record_id == 0)
        return Status{ErrorCode::not_ready};
    const auto* record = impl_->journal.find_record(info.record_id);
    if (record == nullptr || record->kind != RecordKind::inbound ||
        record->seq != info.msg_seq_num || impl_->journal.has_delivery(info.record_id))
        return Status{ErrorCode::corrupt_store};
    if (impl_->deferred_delivery_id != 0 && impl_->deferred_delivery_id != info.record_id)
        return Status{ErrorCode::queue_full};
    impl_->deferred_delivery_id = info.record_id;
    impl_->deferred_delivery_sequence = info.msg_seq_num;
    return Status::success();
}

Status SessionDriver::defer_active() noexcept {
    if (!impl_) return Status{ErrorCode::engine_stopped};
    if (const auto status = impl_->claim_owner(); !status.ok()) return status;
    if (!impl_->callback_active || !impl_->callbacks.deferred_active)
        return Status{ErrorCode::not_ready};
    if (impl_->deferred_active_callback) return Status{ErrorCode::not_ready};
    impl_->deferred_active_callback = true;
    return Status::success();
}

Status SessionDriver::complete_delivery(DeliveryInfo info, Status callback_status,
    std::size_t accepted_replies, std::uint64_t now_ms,
    std::string_view wall_time) noexcept {
    if (!impl_) return Status{ErrorCode::engine_stopped};
    if (const auto status = impl_->claim_owner(); !status.ok()) return status;
    if (const auto status = impl_->set_wall_time(wall_time); !status.ok()) return status;
    impl_->callback_now_ms = now_ms;
    return impl_->complete_deferred(info, callback_status, accepted_replies,
        now_ms, impl_->callback_wall_time);
}

Status SessionDriver::complete_active(Status callback_status, std::uint64_t now_ms,
    std::string_view wall_time) noexcept {
    if (!impl_) return Status{ErrorCode::engine_stopped};
    if (const auto status = impl_->claim_owner(); !status.ok()) return status;
    if (!impl_->deferred_active_callback) return Status{ErrorCode::not_ready};
    if (const auto status = impl_->set_wall_time(wall_time); !status.ok()) return status;
    impl_->callback_now_ms = now_ms;
    impl_->deferred_active_callback = false;
    if (!callback_status.ok())
        return impl_->fail(Status{ErrorCode::callback_failed,
            callback_status.tag, callback_status.offset});
    // The next owner tick starts queued business delivery after runtime state
    // for this active callback has been cleared.
    return Status::success();
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

Status SessionDriver::arm_one_shot_sequence_reset(std::string_view reason) noexcept {
    if (!impl_) return Status{ErrorCode::engine_stopped};
    if (const auto status = impl_->claim_owner(); !status.ok()) return status;
    return impl_->on_arm_one_shot_sequence_reset(reason);
}

Status SessionDriver::reset_epoch_offline(std::string_view reason) noexcept {
    if (!impl_) return Status{ErrorCode::engine_stopped};
    if (const auto status = impl_->claim_owner(); !status.ok()) return status;
    return impl_->on_reset_epoch_offline(reason);
}

Status SessionDriver::adjust_sequences(std::uint64_t next_inbound,
    std::uint64_t next_outbound, std::string_view reason) noexcept {
    if (!impl_) return Status{ErrorCode::engine_stopped};
    if (const auto status = impl_->claim_owner(); !status.ok()) return status;
    return impl_->on_adjust_sequences(next_inbound, next_outbound, reason);
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
    if (dependency != 0 && !impl_->callback_active &&
        (!impl_->callbacks.deferred_delivery || impl_->deferred_delivery_id != dependency))
        return Status{ErrorCode::not_ready};
    if (const auto status = impl_->set_wall_time(wall_time); !status.ok()) return status;
    impl_->callback_now_ms = now_ms;
    return impl_->send_application(message, now_ms, impl_->callback_wall_time,
        dependency, dependency_ordinal, recovery);
}

Result<SendReceipt> SessionDriver::send_stream_dependent(std::string_view msg_type,
    StreamEncodeFn encode_fn, void* encode_ctx,
    std::uint64_t now_ms, std::string_view wall_time, RecordId dependency,
    std::size_t dependency_ordinal, bool recovery) noexcept {
    if (!impl_) return Status{ErrorCode::engine_stopped};
    if (const auto status = impl_->claim_owner(); !status.ok()) return status;
    if (dependency != 0 && !impl_->callback_active &&
        (!impl_->callbacks.deferred_delivery || impl_->deferred_delivery_id != dependency))
        return Status{ErrorCode::not_ready};
    if (const auto status = impl_->set_wall_time(wall_time); !status.ok()) return status;
    impl_->callback_now_ms = now_ms;
    return impl_->send_application_stream(msg_type, encode_fn, encode_ctx,
        now_ms, impl_->callback_wall_time, dependency, dependency_ordinal, recovery);
}

Status SessionDriver::flush(WriteFn write, void* context, std::uint64_t now_ms,
    std::size_t byte_budget) noexcept {
    if (!impl_) return Status{ErrorCode::engine_stopped};
    if (const auto status = impl_->claim_owner(); !status.ok()) return status;
    return impl_->on_flush(write, context, now_ms, byte_budget);
}

Status SessionDriver::flush_batch(WriteBatchFn write_batch, WriteFn write_single,
    void* context, std::uint64_t now_ms, std::size_t byte_budget) noexcept {
    if (!impl_) return Status{ErrorCode::engine_stopped};
    if (const auto status = impl_->claim_owner(); !status.ok()) return status;
    return impl_->on_flush_batch(write_batch, write_single, context, now_ms, byte_budget);
}

Result<std::size_t> SessionDriver::stage_tx(std::span<char> buffer, std::uint64_t now_ms) noexcept {
    if (!impl_) return Status{ErrorCode::engine_stopped};
    if (const auto status = impl_->claim_owner(); !status.ok()) return status;
    return impl_->on_stage_tx(buffer, now_ms);
}

Status SessionDriver::commit_tx_written(std::size_t bytes_written, std::uint64_t now_ms) noexcept {
    if (!impl_) return Status{ErrorCode::engine_stopped};
    if (const auto status = impl_->claim_owner(); !status.ok()) return status;
    return impl_->on_commit_tx_written(bytes_written, now_ms);
}

void SessionDriver::abort_staged_tx() noexcept {
    if (!impl_) return;
    impl_->on_abort_staged_tx();
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

bool SessionDriver::can_receive() const noexcept {
    return impl_ && impl_->can_receive_now();
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

std::size_t SessionDriver::pending_inbound_messages() const noexcept {
    return impl_ ? impl_->recovery_count : 0;
}

std::size_t SessionDriver::pending_inbound_bytes() const noexcept {
    return impl_ ? impl_->recovery_bytes : 0;
}

SessionContext::SessionContext(SessionDriver& driver, std::uint64_t now_ms,
    std::string_view wall_time, DeliveryInfo delivery) noexcept
    : driver_(&driver), send_context_(&driver), send_function_(&SessionContext::send_inline),
      now_ms_(now_ms), wall_time_(wall_time), delivery_(delivery),
      state_snapshot_(driver.state()),
      session_name_(driver.impl_ ? driver.impl_->config.name : std::string_view{}),
      local_comp_id_(driver.impl_ ? driver.impl_->config.local_comp_id : std::string_view{}),
      remote_comp_id_(driver.impl_ ? driver.impl_->config.remote_comp_id : std::string_view{}) {}

Result<SendReceipt> SessionContext::send_inline(void* context,
    detail::MessageView message, std::uint64_t now_ms, std::string_view wall_time,
    DeliveryInfo delivery, std::size_t ordinal, std::uint64_t) noexcept {
    auto* driver = static_cast<SessionDriver*>(context);
    if (driver == nullptr) return Status{ErrorCode::engine_stopped};
    return driver->send_dependent(message, now_ms, wall_time,
        delivery.record_id, ordinal, delivery.is_recovery);
}

Result<SendReceipt> SessionContext::send(detail::MessageView message) noexcept {
    if (send_function_ == nullptr || send_context_ == nullptr ||
        send_ordinal_ == std::numeric_limits<std::size_t>::max())
        return Status{ErrorCode::engine_stopped};
    auto result = send_function_(send_context_, message, now_ms_, wall_time_,
        delivery_, send_ordinal_, generation_);
    if (result.ok()) ++send_ordinal_;
    return result;
}

Status SessionContext::logout(std::string_view reason) noexcept {
    if (delivery_.record_id != 0) return Status{ErrorCode::not_ready};
    if (driver_ != nullptr) {
        if (!driver_->impl_ || !driver_->impl_->callback_active) return Status{ErrorCode::not_ready};
        return driver_->logout(reason, now_ms_, wall_time_);
    }
    if (sender_lifetime_)
        return detail::enqueue_sender_logout(sender_lifetime_, reason);
    return Status{ErrorCode::not_ready};
}

SessionState SessionContext::state() const noexcept {
    return driver_ == nullptr ? state_snapshot_ : driver_->state();
}

} // namespace nimblefix
