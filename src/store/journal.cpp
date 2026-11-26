#include "journal_internal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <unistd.h>

namespace nimblefix {

using namespace store;

JournalReader::JournalReader(JournalReader&& other) noexcept
    : state_(std::move(other.state_)) {}

JournalReader& JournalReader::operator=(JournalReader&& other) noexcept {
    if (this == &other) return *this;
    state_ = std::move(other.state_);
    return *this;
}

JournalReader::~JournalReader() = default;

Status JournalReader::next(JournalRecord& record) noexcept {
    if (!state_ || !state_->registry || state_->slot >= state_->registry->capacity)
        return {ErrorCode::reader_expired, 0, 0};
    auto registry = state_->registry;
    auto& slot = registry->slots[state_->slot];
    slot.active_calls.fetch_add(1, std::memory_order_seq_cst);
    struct ActiveCall {
        Registry::Slot& slot;
        ~ActiveCall() { slot.active_calls.fetch_sub(1, std::memory_order_seq_cst); }
    } active_call{slot};
    auto* owner = slot.owner.load(std::memory_order_seq_cst);
    if (owner == nullptr) return {ErrorCode::reader_expired, 0, 0};
    return owner->reader_next(*state_, record);
}

RecordId JournalReader::cursor() const noexcept {
    return state_ ? state_->cursor.load(std::memory_order_acquire) : 0;
}

std::uint64_t JournalReader::lag_records() const noexcept {
    if (!state_ || !state_->registry || state_->slot >= state_->registry->capacity) return 0;
    auto registry = state_->registry;
    auto& slot = registry->slots[state_->slot];
    slot.active_calls.fetch_add(1, std::memory_order_seq_cst);
    struct ActiveCall {
        Registry::Slot& slot;
        ~ActiveCall() { slot.active_calls.fetch_sub(1, std::memory_order_seq_cst); }
    } active_call{slot};
    auto* owner = slot.owner.load(std::memory_order_seq_cst);
    if (owner == nullptr) return 0;
    const auto count = owner->published_record_count_.load(std::memory_order_acquire);
    const auto position = slot.position.load(std::memory_order_acquire);
    return count > position ? count - position : 0;
}

Journal::Journal(Journal&& other) noexcept { *this = std::move(other); }

Journal& Journal::operator=(Journal&& other) noexcept {
    if (this == &other) return *this;
    reset();
    other.quiesce_readers();
    fd_ = std::exchange(other.fd_, -1);
    capacity_bytes_ = std::exchange(other.capacity_bytes_, 0);
    next_inbound_seq_ = std::exchange(other.next_inbound_seq_, 1);
    next_outbound_seq_ = std::exchange(other.next_outbound_seq_, 1);
    max_records_ = std::exchange(other.max_records_, 0);
    test_hook_ = std::exchange(other.test_hook_, {});
    records_ = std::move(other.records_);
    outbound_index_ = std::move(other.outbound_index_);
    records_data_ = records_.data();
    segments_ = std::move(other.segments_);
    reader_registry_ = std::move(other.reader_registry_);
    published_record_count_.store(
        other.published_record_count_.exchange(0, std::memory_order_acq_rel), std::memory_order_release);
    active_segment_ = std::exchange(other.active_segment_, 0);
    segment_capacity_bytes_ = std::exchange(other.segment_capacity_bytes_, 0);
    max_segments_ = std::exchange(other.max_segments_, 0);
    next_segment_index_ = std::exchange(other.next_segment_index_, 0);
    first_segment_index_ = std::exchange(other.first_segment_index_, 0);
    used_total_bytes_ = std::exchange(other.used_total_bytes_, 0);
    max_readers_ = std::exchange(other.max_readers_, 0);
    max_reader_lag_records_ = std::exchange(other.max_reader_lag_records_, 0);
    lowest_retained_outbound_seq_ = std::exchange(other.lowest_retained_outbound_seq_, 1);
    lowest_retained_record_id_.store(
        other.lowest_retained_record_id_.exchange(superblock_size, std::memory_order_acq_rel),
        std::memory_order_release);
    completed_inbound_seq_ = std::exchange(other.completed_inbound_seq_, 0);
    completed_inbound_record_id_ = std::exchange(other.completed_inbound_record_id_, 0);
    completed_scan_index_ = std::exchange(other.completed_scan_index_, 0);
    checkpoint_ = std::exchange(other.checkpoint_, {});
    checkpoint_stale_ = std::exchange(other.checkpoint_stale_, false);
    maintenance_needed_ = std::exchange(other.maintenance_needed_, false);
    current_epoch_ = std::exchange(other.current_epoch_, journal_initial_epoch);
    epoch_reset_armed_ = std::exchange(other.epoch_reset_armed_, false);
    base_path_ = std::move(other.base_path_);
    session_key_ = std::move(other.session_key_);
    audit_fd_ = std::exchange(other.audit_fd_, -1);
    audit_mapping_ = std::exchange(other.audit_mapping_, nullptr);
    audit_file_size_ = std::exchange(other.audit_file_size_, 0);
    audit_bytes_ = std::exchange(other.audit_bytes_, 0);
    audit_count_ = std::exchange(other.audit_count_, 0);
    epoch_audit_records_ = std::move(other.epoch_audit_records_);
    if (reader_registry_) {
        for (std::size_t i = 0; i < reader_registry_->capacity; ++i)
            if (reader_registry_->slots[i].claimed.load(std::memory_order_acquire))
                reader_registry_->slots[i].owner.store(this, std::memory_order_seq_cst);
    }
    return *this;
}

Journal::~Journal() { reset(); }

void Journal::reset() noexcept {
    quiesce_readers();
    records_.clear();
    outbound_index_.clear();
    records_data_ = nullptr;
    reader_registry_.reset();
    published_record_count_.store(0, std::memory_order_release);
    segments_.clear();
    epoch_audit_records_.clear();
    if (audit_mapping_ != nullptr && audit_file_size_ != 0)
        ::munmap(audit_mapping_, audit_file_size_);
    audit_mapping_ = nullptr;
    audit_file_size_ = 0;
    if (audit_fd_ >= 0) ::close(audit_fd_);
    audit_fd_ = -1;
    audit_bytes_ = 0;
    audit_count_ = 0;
    if (fd_ >= 0) {
        ::flock(fd_, LOCK_UN);
        ::close(fd_);
    }
    fd_ = -1;
    capacity_bytes_ = 0;
    next_inbound_seq_ = 1;
    next_outbound_seq_ = 1;
    max_records_ = 0;
    test_hook_ = {};
    active_segment_ = 0;
    segment_capacity_bytes_ = 0;
    max_segments_ = 0;
    next_segment_index_ = 0;
    first_segment_index_ = 0;
    used_total_bytes_ = 0;
    max_readers_ = 0;
    max_reader_lag_records_ = 0;
    lowest_retained_outbound_seq_ = 1;
    lowest_retained_record_id_.store(superblock_size, std::memory_order_release);
    completed_inbound_seq_ = 0;
    completed_inbound_record_id_ = 0;
    completed_scan_index_ = 0;
    checkpoint_ = {};
    checkpoint_stale_ = false;
    maintenance_needed_ = false;
    current_epoch_ = journal_initial_epoch;
    epoch_reset_armed_ = false;
    base_path_.clear();
    session_key_.clear();
}

void Journal::quiesce_readers() noexcept {
    if (!reader_registry_) return;
    for (std::size_t i = 0; i < reader_registry_->capacity; ++i)
        reader_registry_->slots[i].owner.store(nullptr, std::memory_order_seq_cst);
    for (std::size_t i = 0; i < reader_registry_->capacity; ++i) {
        while (reader_registry_->slots[i].active_calls.load(std::memory_order_seq_cst) != 0)
            std::this_thread::yield();
    }
}

Result<ReservedRecord> Journal::reserve(RecordKind kind,
                                        std::uint64_t seq,
                                        std::uint64_t receipt,
                                        RecordId dependency,
                                        std::size_t max_payload_bytes,
                                        std::uint64_t next_inbound) noexcept try {
    if (fd_ < 0 || segments_.empty()) return Status{ErrorCode::invalid_config, 0, 0};
    if (records_.size() >= max_records_) return Status{ErrorCode::storage_full, 0, used_total_bytes_};

    // Zero-overhead fast path when no readers are active.
    if (reader_registry_ && reader_registry_->active_count.load(std::memory_order_relaxed) != 0) {
        const auto published_count = published_record_count_.load(std::memory_order_relaxed);
        for (std::size_t i = 0; i < reader_registry_->capacity; ++i) {
            auto& slot = reader_registry_->slots[i];
            if (!slot.active.load(std::memory_order_acquire)) continue;
            const auto position = slot.position.load(std::memory_order_acquire);
            if (published_count >= position && published_count - position >= max_reader_lag_records_)
                return Status{ErrorCode::queue_full, 0, used_total_bytes_};
        }
    }

    if (seq == 0 || max_payload_bytes > std::numeric_limits<std::uint32_t>::max())
        return Status{ErrorCode::invalid_value, 0, used_total_bytes_};

    std::uint64_t persisted_next = 0;
    if (kind == RecordKind::inbound) {
        if (seq != next_inbound_seq_) return Status{ErrorCode::sequence_gap, 0, used_total_bytes_};
        if (seq == std::numeric_limits<std::uint64_t>::max()) return Status{ErrorCode::invalid_value, 0, used_total_bytes_};
        persisted_next = next_inbound == 0 ? seq + 1 : next_inbound;
        if (persisted_next <= seq || receipt != 0 || dependency != 0 || max_payload_bytes == 0)
            return Status{ErrorCode::invalid_config, 0, used_total_bytes_};
    } else if (kind == RecordKind::outbound) {
        if (seq != next_outbound_seq_) return Status{ErrorCode::sequence_gap, 0, used_total_bytes_};
        if (seq == std::numeric_limits<std::uint64_t>::max() || next_inbound != 0 || max_payload_bytes == 0)
            return Status{ErrorCode::invalid_config, 0, used_total_bytes_};
        if (dependency != 0) {
            const auto* inbound = find_record(dependency);
            if (inbound == nullptr || inbound->kind != RecordKind::inbound)
                return Status{ErrorCode::missing_field, 0, used_total_bytes_};
        }
    } else if (kind == RecordKind::delivery) {
        if (next_inbound != 0 || receipt != 0 || max_payload_bytes != 0)
            return Status{ErrorCode::invalid_config, 0, used_total_bytes_};
        const auto* inbound = find_record(dependency);
        if (inbound == nullptr) return Status{ErrorCode::history_unavailable, 0, used_total_bytes_};
        if (inbound->kind != RecordKind::inbound || inbound->seq != seq || inbound->delivered)
            return Status{ErrorCode::invalid_config, 0, used_total_bytes_};
    } else if (kind == RecordKind::epoch_reset) {
        if (!epoch_reset_armed_ || current_epoch_ == std::numeric_limits<std::uint32_t>::max() ||
            seq != current_epoch_ || next_inbound != 0 || receipt != 0 || dependency != 0 ||
            max_payload_bytes == 0 || max_payload_bytes > 256)
            return Status{ErrorCode::invalid_config, 0, used_total_bytes_};
        persisted_next = static_cast<std::uint64_t>(current_epoch_) + 1U;
    } else {
        return Status{ErrorCode::invalid_config, 0, used_total_bytes_};
    }

    if (max_payload_bytes > std::numeric_limits<std::size_t>::max() - record_header_size - 7U)
        return Status{ErrorCode::too_large, 0, used_total_bytes_};
    const std::size_t needed_size = align_record(record_header_size + max_payload_bytes);
    if (needed_size > std::numeric_limits<std::uint32_t>::max() ||
        needed_size > segment_capacity_bytes_ - superblock_size)
        return Status{ErrorCode::storage_full, 0, used_total_bytes_};

    std::size_t target_slot = active_segment_;
    bool rotated = false;
    auto* segment = segments_[target_slot].get();
    if (needed_size > segment->mapping_size - segment->write_offset) {
        if (target_slot + 1 < segments_.size()) {
            target_slot += 1;
            segment = segments_[target_slot].get();
        } else {
            if (segments_.size() >= max_segments_ || next_segment_index_ > maximum_segment_index)
                return Status{ErrorCode::storage_full, 0, used_total_bytes_};
            auto created = create_segment_atomic(base_path_, next_segment_index_,
                segment_capacity_bytes_, session_key_, capacity_bytes_, max_segments_,
                audit_bytes_, test_hook_);
            if (!created.ok()) return created.status();
            segments_.push_back(std::move(created).value());
            used_total_bytes_ += superblock_size;
            ++next_segment_index_;
            target_slot = segments_.size() - 1;
            segment = segments_[target_slot].get();
        }
        rotated = true;
        if (needed_size > segment->mapping_size - segment->write_offset)
            return Status{ErrorCode::storage_full, 0, used_total_bytes_};
    }

    auto* record = static_cast<unsigned char*>(segment->mapping) + segment->write_offset;
    ReservedRecord reserved{};
    reserved.payload = {reinterpret_cast<char*>(record + record_header_size), max_payload_bytes};
    reserved.kind = kind;
    reserved.seq = seq;
    reserved.receipt = receipt;
    reserved.dependency = dependency;
    reserved.persisted_next = persisted_next;
    reserved.segment_slot = target_slot;
    reserved.offset = segment->write_offset;
    reserved.max_payload_bytes = max_payload_bytes;
    reserved.rotated = rotated;
    return reserved;
} catch (const std::bad_alloc&) {
    return Status{ErrorCode::capacity_exceeded, 0, 0};
} catch (...) {
    return Status{ErrorCode::io_error, 0, 0};
}

Result<JournalRecord> Journal::commit(const ReservedRecord& reserved,
                                      std::size_t actual_payload_bytes) noexcept try {
    if (reserved.segment_slot >= segments_.size())
        return Status{ErrorCode::invalid_config, 0, 0};
    auto* segment = segments_[reserved.segment_slot].get();
    if (reserved.offset != segment->write_offset || actual_payload_bytes > reserved.max_payload_bytes)
        return Status{ErrorCode::invalid_value, 0, 0};
    if ((reserved.kind == RecordKind::inbound || reserved.kind == RecordKind::outbound ||
         reserved.kind == RecordKind::epoch_reset) && actual_payload_bytes == 0)
        return Status{ErrorCode::invalid_config, 0, 0};
    if (reserved.kind == RecordKind::epoch_reset &&
        std::memchr(reserved.payload.data(), 0, actual_payload_bytes) != nullptr)
        return Status{ErrorCode::invalid_config, 0, 0};

    const std::size_t total_size = align_record(record_header_size + actual_payload_bytes);
    if (total_size > segment->mapping_size - segment->write_offset)
        return Status{ErrorCode::storage_full, 0, used_total_bytes_};

    active_segment_ = reserved.segment_slot;
    auto* record = static_cast<unsigned char*>(segment->mapping) + reserved.offset;

    // Zero only the 64-byte header and the 0..7 trailing alignment padding bytes
    // (never wastefully memset the payload region).
    std::memset(record, 0, record_header_size);
    const std::size_t pad_bytes = total_size - (record_header_size + actual_payload_bytes);
    if (pad_bytes != 0)
        std::memset(record + record_header_size + actual_payload_bytes, 0, pad_bytes);

    put_u32(record, static_cast<std::uint32_t>(total_size));
    put_u32(record + 4, static_cast<std::uint32_t>(actual_payload_bytes));
    put_u32(record + 8, static_cast<std::uint32_t>(reserved.kind));
    put_u32(record + 12, current_epoch_);
    put_u64(record + 16, reserved.seq);
    put_u64(record + 24, reserved.receipt);
    put_u64(record + 32, reserved.dependency);
    put_u64(record + 40, reserved.persisted_next);
    auto* marker_word = reinterpret_cast<std::uint32_t*>(record + record_commit_offset);
    std::atomic_ref<std::uint32_t> marker(*marker_word);
    marker.store(to_little_endian(0), std::memory_order_relaxed);

    if (test_hook_.callback != nullptr) test_hook_.callback(JournalFaultPoint::header, test_hook_.context);
    if (test_hook_.callback != nullptr) test_hook_.callback(JournalFaultPoint::payload, test_hook_.context);
    put_u32(record + record_crc_offset, record_crc(record, total_size));
    if (test_hook_.callback != nullptr) test_hook_.callback(JournalFaultPoint::precommit, test_hook_.context);
    marker.store(to_little_endian(record_commit_marker), std::memory_order_release);
    if (test_hook_.callback != nullptr) test_hook_.callback(JournalFaultPoint::postcommit, test_hook_.context);

    const auto id = segment_record_id(segment->index, reserved.offset);
    JournalRecord appended{id, reserved.kind, reserved.seq, reserved.receipt, reserved.dependency,
        reserved.persisted_next,
        std::string_view(reinterpret_cast<const char*>(record + record_header_size), actual_payload_bytes),
        reserved.kind == RecordKind::inbound && reserved.persisted_next > reserved.seq + 1,
        current_epoch_};
    const std::size_t record_index = records_.size();
    records_.push_back(appended);
    if (reserved.kind == RecordKind::outbound) outbound_index_.push_back(record_index);
    if (reserved.kind == RecordKind::delivery) {
        JournalRecord* inbound = nullptr;
        const std::size_t n = records_.size();
        const std::size_t scan_min = n > 5 ? n - 5 : 0;
        for (std::size_t idx = n - 1; idx > scan_min; --idx) {
            if (records_[idx - 1].id == reserved.dependency) {
                inbound = &records_[idx - 1];
                break;
            }
        }
        if (inbound == nullptr) {
            auto found = std::lower_bound(records_.begin(), records_.end(), reserved.dependency,
                [](const JournalRecord& item, RecordId value) { return item.id < value; });
            if (found != records_.end() && found->id == reserved.dependency) inbound = &*found;
        }
        if (inbound != nullptr) inbound->delivered = true;
    }
    published_record_count_.store(records_.size(), std::memory_order_release);
    segment->write_offset += total_size;
    used_total_bytes_ += total_size;
    if (reserved.kind == RecordKind::inbound) next_inbound_seq_ = reserved.persisted_next;
    if (reserved.kind == RecordKind::outbound) next_outbound_seq_ = reserved.seq + 1;
    if (reserved.kind == RecordKind::epoch_reset) {
        current_epoch_ = static_cast<std::uint32_t>(reserved.persisted_next);
        next_inbound_seq_ = 1;
        next_outbound_seq_ = 1;
        completed_inbound_seq_ = 0;
        completed_scan_index_ = records_.size();
        lowest_retained_outbound_seq_ = 1;
        outbound_index_.clear();
    }
    if (reserved.kind == RecordKind::inbound || reserved.kind == RecordKind::delivery)
        advance_completed_inbound_cursor();

    checkpoint_.epoch = current_epoch_;
    checkpoint_.next_inbound_seq = next_inbound_seq_;
    checkpoint_.next_outbound_seq = next_outbound_seq_;
    checkpoint_.completed_inbound_seq = completed_inbound_seq_;
    checkpoint_.delivery_cursor = completed_inbound_record_id_;
    checkpoint_.last_record_id = id;
    checkpoint_.first_segment_index = first_segment_index_;
    checkpoint_.lowest_retained_outbound_seq = lowest_retained_outbound_seq_;
    checkpoint_.lowest_retained_record_id = lowest_retained_record_id_.load(std::memory_order_relaxed);
    checkpoint_stale_ = true;
    if (reserved.rotated) maintenance_needed_ = true;
    return appended;
} catch (const std::bad_alloc&) {
    return Status{ErrorCode::capacity_exceeded, 0, 0};
} catch (...) {
    return Status{ErrorCode::io_error, 0, 0};
}

Result<JournalRecord> Journal::append(RecordKind kind,
                                      std::uint64_t seq,
                                      std::uint64_t receipt,
                                      RecordId dependency,
                                      std::string_view payload,
                                      std::uint64_t next_inbound) noexcept {
    if (kind == RecordKind::epoch_reset && payload.find('\0') != std::string_view::npos)
        return Status{ErrorCode::invalid_config, 0, used_total_bytes_};
    auto reserved = reserve(kind, seq, receipt, dependency, payload.size(), next_inbound);
    if (!reserved.ok()) return reserved.status();
    if (!payload.empty()) {
        std::memcpy(reserved.value().payload.data(), payload.data(), payload.size());
    }
    return commit(reserved.value(), payload.size());
}

Status Journal::refill_spare_segments() noexcept {
    while (segments_.size() < max_segments_ && next_segment_index_ <= maximum_segment_index) {
        auto created = create_segment_atomic(base_path_, next_segment_index_,
            segment_capacity_bytes_, session_key_, capacity_bytes_, max_segments_,
            audit_bytes_, test_hook_);
        if (!created.ok()) return created.status();
        segments_.push_back(std::move(created).value());
        used_total_bytes_ += superblock_size;
        ++next_segment_index_;
    }
    return Status::success();
}

Status Journal::poll_maintenance() noexcept {
    if (fd_ < 0) return Status{ErrorCode::invalid_config, 0, 0};
    if (!maintenance_needed_ && segments_.size() >= max_segments_) return Status::success();
    if (const auto st = refill_spare_segments(); !st.ok()) return st;
    if (maintenance_needed_) {
        auto next = checkpoint();
        ++next.generation;
        if (const auto st = write_checkpoint_file(next); !st.ok()) {
            checkpoint_stale_ = true;
            return st;
        }
        checkpoint_ = next;
        checkpoint_stale_ = false;
        maintenance_needed_ = false;
    }
    return Status::success();
}

Status Journal::write_checkpoint_file(const JournalCheckpoint& checkpoint) noexcept {
    if (base_path_.empty() || session_key_.empty())
        return {ErrorCode::unsupported, 0, 0};
    if (outbound_index_.size() > std::numeric_limits<std::uint32_t>::max())
        return {ErrorCode::too_large, 0, 0};
    std::vector<unsigned char> bytes;
    try {
        bytes.resize(checkpoint_fixed_size + session_key_.size() + outbound_index_.size() * 16U);
    } catch (...) {
        return {ErrorCode::capacity_exceeded, 0, 0};
    }
    std::memcpy(bytes.data(), checkpoint_magic.data(), checkpoint_magic.size());
    put_u32(bytes.data() + 8, 2);
    put_u64(bytes.data() + 16, checkpoint.generation);
    put_u64(bytes.data() + 24, checkpoint.next_inbound_seq);
    put_u64(bytes.data() + 32, checkpoint.next_outbound_seq);
    put_u64(bytes.data() + 40, checkpoint.delivery_cursor);
    put_u64(bytes.data() + 48, checkpoint.replay_cursor);
    put_u64(bytes.data() + 56, checkpoint.last_record_id);
    put_u32(bytes.data() + 64, checkpoint.first_segment_index);
    put_u64(bytes.data() + 68, checkpoint.lowest_retained_outbound_seq);
    put_u64(bytes.data() + 76, checkpoint.lowest_retained_record_id);
    put_u32(bytes.data() + 84, static_cast<std::uint32_t>(outbound_index_.size()));
    put_u32(bytes.data() + 88, static_cast<std::uint32_t>(session_key_.size()));
    put_u32(bytes.data() + 92, checkpoint.epoch);
    put_u64(bytes.data() + 96, checkpoint.completed_inbound_seq);
    std::memcpy(bytes.data() + checkpoint_fixed_size, session_key_.data(), session_key_.size());
    std::size_t offset = checkpoint_fixed_size + session_key_.size();
    for (const auto index : outbound_index_) {
        put_u64(bytes.data() + offset, records_[index].seq);
        put_u64(bytes.data() + offset + 8, records_[index].id);
        offset += 16;
    }
    put_u32(bytes.data() + checkpoint_crc_offset,
        crc32c_with_hole(bytes.data(), bytes.size(), checkpoint_crc_offset, sizeof(std::uint32_t)));

    std::string target;
    std::string temporary;
    try {
        target = base_path_ + ".ckp";
        temporary = make_temp_path(target);
    } catch (...) {
        return {ErrorCode::capacity_exceeded, 0, 0};
    }
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return io_status(errno);
    const auto write_status = write_all(fd, bytes.data(), bytes.size());
    const int close_result = ::close(fd);
    if (!write_status.ok() || close_result != 0) {
        const auto status = write_status.ok() ? io_status(errno) : write_status;
        (void)::unlink(temporary.c_str());
        return status;
    }
    if (::rename(temporary.c_str(), target.c_str()) != 0) {
        const int saved = errno;
        (void)::unlink(temporary.c_str());
        return io_status(saved);
    }
    if (test_hook_.callback != nullptr)
        test_hook_.callback(JournalFaultPoint::checkpoint_published, test_hook_.context);
    return Status::success();
}

Status Journal::append_epoch_audit(std::uint32_t previous_epoch,
                                   std::uint32_t next_epoch,
                                   RecordId record_id,
                                   std::string_view reason) noexcept {
    if (audit_mapping_ == nullptr || audit_count_ >= audit_entry_count)
        return {ErrorCode::storage_full, 0, audit_count_};
    if (previous_epoch == 0 || next_epoch != previous_epoch + 1U || record_id == 0 ||
        reason.empty() || reason.size() > 256 || reason.find('\0') != std::string_view::npos)
        return {ErrorCode::invalid_value, 0, 0};
    auto* entry = static_cast<unsigned char*>(audit_mapping_) + superblock_size + audit_count_ * audit_entry_size;
    std::memset(entry, 0, audit_entry_size);
    put_u32(entry, static_cast<std::uint32_t>(audit_entry_size));
    put_u32(entry + 4, previous_epoch);
    put_u32(entry + 8, next_epoch);
    put_u32(entry + 12, static_cast<std::uint32_t>(reason.size()));
    put_u64(entry + 16, record_id);
    std::memcpy(entry + 64, reason.data(), reason.size());
    put_u32(entry + 24, audit_entry_crc(entry));
    auto* marker_word = reinterpret_cast<std::uint32_t*>(entry + audit_commit_offset);
    std::atomic_ref<std::uint32_t> marker(*marker_word);
    marker.store(to_little_endian(0), std::memory_order_relaxed);
    marker.store(to_little_endian(audit_commit_marker), std::memory_order_release);
    epoch_audit_records_.push_back(EpochAuditRecord{previous_epoch, next_epoch, record_id,
        std::string_view(reinterpret_cast<const char*>(entry + 64), reason.size())});
    ++audit_count_;
    if (test_hook_.callback != nullptr)
        test_hook_.callback(JournalFaultPoint::audit_published, test_hook_.context);
    return Status::success();
}

JournalCheckpoint Journal::checkpoint() const noexcept {
    auto result = checkpoint_;
    result.epoch = current_epoch_;
    result.next_inbound_seq = next_inbound_seq_;
    result.next_outbound_seq = next_outbound_seq_;
    result.completed_inbound_seq = completed_inbound_seq_;
    result.delivery_cursor = completed_inbound_record_id_;
    result.last_record_id = records_.empty() ? 0 : records_.back().id;
    result.first_segment_index = first_segment_index_;
    result.lowest_retained_outbound_seq = lowest_retained_outbound_seq_;
    result.lowest_retained_record_id = lowest_retained_record_id_.load(std::memory_order_acquire);
    return result;
}

Status Journal::publish_checkpoint(RecordId delivery_cursor,
                                   std::uint64_t replay_cursor) noexcept {
    if (fd_ < 0) return {ErrorCode::invalid_config, 0, 0};
    const auto lowest_id = lowest_retained_record_id_.load(std::memory_order_acquire);
    if (replay_cursor < lowest_retained_outbound_seq_ || replay_cursor > next_outbound_seq_ ||
        replay_cursor < checkpoint_.replay_cursor || delivery_cursor < checkpoint_.delivery_cursor)
        return {ErrorCode::invalid_value, 0, 0};
    const bool persisted_pruned_cursor = delivery_cursor != 0 &&
        delivery_cursor == checkpoint_.delivery_cursor && delivery_cursor < lowest_id;
    if (delivery_cursor != 0 && !persisted_pruned_cursor) {
        const auto* record = find_record(delivery_cursor);
        if (record == nullptr || record->kind != RecordKind::inbound || !record->delivered)
            return {ErrorCode::invalid_value, 0, static_cast<std::size_t>(delivery_cursor)};
        for (const auto& item : records_) {
            if (item.kind == RecordKind::inbound && item.id <= delivery_cursor && !item.delivered)
                return {ErrorCode::invalid_value, 0, static_cast<std::size_t>(item.id)};
        }
    }
    auto next = checkpoint();
    next.delivery_cursor = delivery_cursor;
    next.replay_cursor = replay_cursor;
    ++next.generation;
    const auto status = write_checkpoint_file(next);
    if (!status.ok()) {
        checkpoint_stale_ = true;
        return status;
    }
    checkpoint_ = next;
    checkpoint_stale_ = false;
    maintenance_needed_ = false;
    return Status::success();
}

Status Journal::begin_epoch(std::string_view reason) noexcept {
    if (fd_ < 0) return {ErrorCode::invalid_config, 0, 0};
    if (audit_mapping_ == nullptr || audit_bytes_ != audit_file_size)
        return {ErrorCode::storage_full, 0, 0};
    if (reason.empty() || reason.size() > 256 || reason.find('\0') != std::string_view::npos ||
        current_epoch_ == std::numeric_limits<std::uint32_t>::max())
        return {ErrorCode::invalid_value, 0, 0};
    if (audit_count_ >= audit_entry_count) return {ErrorCode::storage_full, 0, audit_count_};
    if ((reader_registry_ && reader_registry_->active_count.load(std::memory_order_acquire) != 0) ||
        std::any_of(segments_.begin(), segments_.end(), [](const auto& segment) {
            return segment->active_leases.load(std::memory_order_acquire) != 0;
        }))
        return {ErrorCode::sequence_reset_denied, 0, 0};
    RecordId delivery_cursor = checkpoint_.delivery_cursor;
    for (const auto& record : records_) {
        if (record.kind != RecordKind::inbound) continue;
        if (!record.delivered)
            return {ErrorCode::sequence_reset_denied, 0, static_cast<std::size_t>(record.id)};
        delivery_cursor = record.id;
    }
    epoch_reset_armed_ = true;
    const auto appended = append(RecordKind::epoch_reset, current_epoch_, 0, 0, reason, 0);
    epoch_reset_armed_ = false;
    if (!appended.ok()) return appended.status();
    const auto audit_status = append_epoch_audit(appended.value().epoch,
        appended.value().epoch + 1U, appended.value().id, reason);
    if (!audit_status.ok()) return audit_status;

    auto next = checkpoint();
    next.epoch = current_epoch_;
    next.delivery_cursor = delivery_cursor;
    next.replay_cursor = 1;
    ++next.generation;
    if (write_checkpoint_file(next).ok()) {
        checkpoint_ = next;
        checkpoint_stale_ = false;
        maintenance_needed_ = false;
    } else {
        checkpoint_ = next;
        checkpoint_stale_ = true;
    }
    return Status::success();
}

Status Journal::set_retention_floor(std::uint64_t lowest_outbound_seq,
                                    RecordId delivery_cursor) noexcept try {
    if (fd_ < 0) return {ErrorCode::invalid_config, 0, 0};
    const auto current_lowest_id = lowest_retained_record_id_.load(std::memory_order_acquire);
    if (lowest_outbound_seq < lowest_retained_outbound_seq_ ||
        lowest_outbound_seq > next_outbound_seq_ ||
        lowest_outbound_seq < checkpoint_.replay_cursor ||
        delivery_cursor < checkpoint_.delivery_cursor)
        return {ErrorCode::invalid_value, 0, 0};
    const bool persisted_pruned_cursor = delivery_cursor != 0 &&
        delivery_cursor == checkpoint_.delivery_cursor && delivery_cursor < current_lowest_id;
    if (delivery_cursor != 0 && !persisted_pruned_cursor) {
        const auto* record = find_record(delivery_cursor);
        if (record == nullptr || record->kind != RecordKind::inbound || !record->delivered)
            return {ErrorCode::invalid_value, 0, static_cast<std::size_t>(delivery_cursor)};
        for (const auto& item : records_) {
            if (item.kind == RecordKind::inbound && item.id <= delivery_cursor && !item.delivered)
                return {ErrorCode::invalid_value, 0, static_cast<std::size_t>(item.id)};
        }
    }

    RecordId latest_current_inbound = 0;
    RecordId latest_current_outbound = 0;
    for (const auto& record : records_) {
        if (record.epoch != current_epoch_) continue;
        if (record.kind == RecordKind::inbound) latest_current_inbound = record.id;
        else if (record.kind == RecordKind::outbound) latest_current_outbound = record.id;
    }

    std::size_t safe_segments = 0;
    const bool readers_active = reader_registry_ &&
        reader_registry_->active_count.load(std::memory_order_acquire) != 0;
    if (!readers_active) {
        for (std::size_t slot = 0; slot < active_segment_ && slot < segments_.size(); ++slot) {
            const auto& segment = segments_[slot];
            // Acquire-synchronized lease check replaces relaxed shared_ptr::use_count().
            if (segment->active_leases.load(std::memory_order_acquire) != 0) break;
            bool safe = true;
            for (const auto& record : records_) {
                if (segment_index_from_id(record.id) != segment->index) continue;
                if (record.id == latest_current_inbound || record.id == latest_current_outbound) {
                    safe = false; break;
                }
                if (record.kind == RecordKind::epoch_reset) {
                    const auto audited = std::any_of(epoch_audit_records_.begin(), epoch_audit_records_.end(),
                        [&record](const auto& item) { return item.reset_record_id == record.id; });
                    if (!audited) { safe = false; break; }
                }
                if (record.kind == RecordKind::outbound && record.seq >= lowest_outbound_seq &&
                    record.epoch >= current_epoch_) {
                    safe = false; break;
                }
                if (record.kind == RecordKind::inbound &&
                    (record.id > delivery_cursor || !record.delivered)) {
                    safe = false; break;
                }
                if (record.kind == RecordKind::delivery && record.dependency > delivery_cursor) {
                    safe = false; break;
                }
            }
            if (!safe) break;
            ++safe_segments;
        }
    }

    const std::uint32_t next_first_segment = safe_segments != 0
        ? segments_[safe_segments]->index : first_segment_index_;
    RecordId next_lowest_record_id = current_lowest_id;
    if (safe_segments != 0) {
        const auto first_kept = std::find_if(records_.begin(), records_.end(),
            [next_first_segment](const auto& r) {
                return segment_index_from_id(r.id) >= next_first_segment;
            });
        next_lowest_record_id = first_kept == records_.end()
            ? segment_record_id(next_first_segment, superblock_size) : first_kept->id;
    }

    auto next_checkpoint = checkpoint();
    next_checkpoint.delivery_cursor = delivery_cursor;
    next_checkpoint.replay_cursor = lowest_outbound_seq;
    next_checkpoint.first_segment_index = next_first_segment;
    next_checkpoint.lowest_retained_outbound_seq = lowest_outbound_seq;
    next_checkpoint.lowest_retained_record_id = next_lowest_record_id;
    next_checkpoint.last_record_id = (!records_.empty() &&
        segment_index_from_id(records_.back().id) >= next_first_segment)
        ? records_.back().id : 0;
    ++next_checkpoint.generation;

    // Publish the new retention floor before unlinking retired segments so a crash
    // before unlink recovers cleanly with the checkpoint's first_segment_index.
    if (safe_segments != 0) {
        // Temporarily build pruned outbound_index view for single accurate checkpoint write.
        std::vector<std::size_t> kept_outbound;
        kept_outbound.reserve(outbound_index_.size());
        for (const auto idx : outbound_index_) {
            if (segment_index_from_id(records_[idx].id) >= next_first_segment)
                kept_outbound.push_back(idx);
        }
        auto saved_outbound = std::move(outbound_index_);
        outbound_index_ = std::move(kept_outbound);
        const auto cp_status = write_checkpoint_file(next_checkpoint);
        outbound_index_ = std::move(saved_outbound);
        if (!cp_status.ok()) return cp_status;
    } else {
        const auto cp_status = write_checkpoint_file(next_checkpoint);
        if (!cp_status.ok()) return cp_status;
    }

    checkpoint_ = next_checkpoint;
    checkpoint_stale_ = false;
    maintenance_needed_ = false;
    lowest_retained_outbound_seq_ = lowest_outbound_seq;
    first_segment_index_ = next_first_segment;
    lowest_retained_record_id_.store(next_lowest_record_id, std::memory_order_release);
    if (test_hook_.callback != nullptr)
        test_hook_.callback(JournalFaultPoint::retention_published, test_hook_.context);

    if (safe_segments != 0) {
        std::size_t unlinked = 0;
        Status unlink_error{};
        for (std::size_t i = 0; i < safe_segments; ++i) {
            const auto& segment = segments_[i];
            if (::unlink(segment->path.c_str()) != 0 && errno != ENOENT) {
                unlink_error = io_status(errno, used_total_bytes_);
                break;
            }
            used_total_bytes_ -= std::min(used_total_bytes_, segment->write_offset);
            ++unlinked;
        }
        if (unlinked != 0) {
            const std::uint32_t actual_first = segments_[unlinked]->index;
            active_segment_ -= unlinked;
            segments_.erase(segments_.begin(), segments_.begin() + static_cast<std::ptrdiff_t>(unlinked));
            records_.erase(std::remove_if(records_.begin(), records_.end(), [actual_first](const auto& r) {
                return segment_index_from_id(r.id) < actual_first;
            }), records_.end());
            records_data_ = records_.data();
            completed_scan_index_ = 0;
            advance_completed_inbound_cursor();
            outbound_index_.clear();
            for (std::size_t i = 0; i < records_.size(); ++i)
                if (records_[i].kind == RecordKind::outbound && records_[i].epoch == current_epoch_)
                    outbound_index_.push_back(i);
            published_record_count_.store(records_.size(), std::memory_order_release);
            first_segment_index_ = actual_first;
            const RecordId actual_lowest = records_.empty()
                ? segment_record_id(actual_first, superblock_size) : records_.front().id;
            lowest_retained_record_id_.store(actual_lowest, std::memory_order_release);
            checkpoint_.first_segment_index = actual_first;
            checkpoint_.lowest_retained_record_id = actual_lowest;
            checkpoint_.last_record_id = records_.empty() ? 0 : records_.back().id;
        }
        if (!unlink_error.ok()) return unlink_error;
    }

    return refill_spare_segments();
} catch (const std::bad_alloc&) {
    return {ErrorCode::capacity_exceeded, 0, 0};
} catch (...) {
    return {ErrorCode::io_error, 0, 0};
}

Result<JournalReader> Journal::create_reader(RecordId start_id) noexcept {
    if (!reader_registry_ || reader_registry_->capacity == 0)
        return Status{ErrorCode::invalid_config, 0, 0};
    const auto published = published_record_count_.load(std::memory_order_acquire);
    const auto lowest_id = lowest_retained_record_id_.load(std::memory_order_acquire);
    std::size_t position = published;
    if (start_id != 0) {
        if (start_id < lowest_id)
            return Status{ErrorCode::history_unavailable, 0, static_cast<std::size_t>(start_id)};
        std::size_t lower = 0;
        std::size_t upper = published;
        while (lower < upper) {
            const auto middle = lower + (upper - lower) / 2;
            if (records_data_[middle].id < start_id) lower = middle + 1;
            else upper = middle;
        }
        position = lower;
    }
    if (published - position > max_reader_lag_records_)
        return Status{ErrorCode::reader_expired, 0, static_cast<std::size_t>(start_id)};
    try {
        auto state = std::make_shared<JournalReader::State>();
        state->registry = reader_registry_;
        state->mapping_leases.reserve(segments_.size());
        for (const auto& segment : segments_) {
            auto lease = acquire_segment_lease(segment);
            if (!lease) return Status{ErrorCode::capacity_exceeded, 0, 0};
            state->mapping_leases.push_back(std::move(lease));
        }
        bool acquired = false;
        for (std::size_t i = 0; i < reader_registry_->capacity; ++i) {
            auto& slot = reader_registry_->slots[i];
            bool expected = false;
            if (!slot.claimed.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) continue;
            state->slot = i;
            slot.position.store(position, std::memory_order_relaxed);
            slot.state.store(state.get(), std::memory_order_relaxed);
            slot.owner.store(this, std::memory_order_relaxed);
            reader_registry_->active_count.fetch_add(1, std::memory_order_acq_rel);
            slot.active.store(true, std::memory_order_release);
            acquired = true;
            break;
        }
        if (!acquired) return Status{ErrorCode::queue_full, 0, 0};
        state->cursor.store(start_id == 0
            ? (published == 0 ? 0 : records_data_[published - 1].id) : start_id,
            std::memory_order_release);
        state->min_record_id.store(start_id, std::memory_order_release);
        return JournalReader{std::move(state)};
    } catch (...) {
        return Status{ErrorCode::capacity_exceeded, 0, 0};
    }
}

Status Journal::reader_next(JournalReader::State& state, JournalRecord& record) noexcept {
    if (state.registry != reader_registry_ || state.slot >= state.registry->capacity)
        return {ErrorCode::reader_expired, 0, 0};
    auto& slot = state.registry->slots[state.slot];
    if (slot.owner.load(std::memory_order_acquire) != this ||
        !slot.active.load(std::memory_order_acquire))
        return {ErrorCode::reader_expired, 0, 0};
    const auto cursor = state.cursor.load(std::memory_order_acquire);
    const auto lowest_id = lowest_retained_record_id_.load(std::memory_order_acquire);
    if (cursor != 0 && cursor < lowest_id)
        return {ErrorCode::reader_expired, 0, static_cast<std::size_t>(cursor)};
    auto position = slot.position.load(std::memory_order_relaxed);
    const auto published = published_record_count_.load(std::memory_order_acquire);
    if (position > published) return {ErrorCode::reader_expired, 0, 0};
    if (published - position > max_reader_lag_records_)
        return {ErrorCode::reader_expired, 0, static_cast<std::size_t>(cursor)};
    if (position >= published) return {ErrorCode::need_more, 0, 0};

    const RecordId min_id = state.min_record_id.load(std::memory_order_relaxed);
    if (min_id != 0) [[unlikely]] {
        while (position < published && records_data_[position].id < min_id) {
            ++position;
        }
        if (position >= published) {
            slot.position.store(position, std::memory_order_release);
            return {ErrorCode::need_more, 0, 0};
        }
        state.min_record_id.store(0, std::memory_order_relaxed);
    }

    const auto& source = records_data_[position];
    record.id = source.id;
    record.kind = source.kind;
    record.seq = source.seq;
    record.receipt = source.receipt;
    record.dependency = source.dependency;
    record.next_seq = source.next_seq;
    record.payload = source.payload;
    record.delivered = false;
    record.epoch = source.epoch;
    slot.position.store(position + 1, std::memory_order_release);
    state.cursor.store(record.id, std::memory_order_release);
    return Status::success();
}

void Journal::advance_completed_inbound_cursor() noexcept {
    if (completed_scan_index_ > records_.size()) completed_scan_index_ = records_.size();
    const auto old_seq = completed_inbound_seq_;
    const auto old_id = completed_inbound_record_id_;
    std::size_t i = completed_scan_index_;
    for (; i < records_.size(); ++i) {
        const auto& record = records_[i];
        if (record.kind != RecordKind::inbound || record.epoch != current_epoch_) {
            completed_scan_index_ = i + 1;
            continue;
        }
        if (record.seq <= completed_inbound_seq_) {
            if (!record.delivered) { completed_scan_index_ = i; break; }
            completed_scan_index_ = i + 1;
            continue;
        }
        if (completed_inbound_seq_ == std::numeric_limits<std::uint64_t>::max() ||
            record.seq != completed_inbound_seq_ + 1 || !record.delivered) {
            completed_scan_index_ = i;
            break;
        }
        completed_inbound_seq_ = record.next_seq > record.seq + 1
            ? record.next_seq - 1 : record.seq;
        completed_inbound_record_id_ = record.id;
        completed_scan_index_ = i + 1;
    }
    if (i == records_.size()) completed_scan_index_ = records_.size();
    if (completed_inbound_seq_ != old_seq || completed_inbound_record_id_ != old_id) {
        checkpoint_.completed_inbound_seq = completed_inbound_seq_;
        checkpoint_.delivery_cursor = completed_inbound_record_id_;
        checkpoint_stale_ = true;
    }
}

const JournalRecord* Journal::find_record(RecordId id) const noexcept {
    const std::size_t n = records_.size();
    const std::size_t tail_scan = n < 4 ? n : 4;
    for (std::size_t i = 0; i < tail_scan; ++i) {
        const auto& rec = records_[n - 1 - i];
        if (rec.id == id) return &rec;
        if (rec.id < id) return nullptr;
    }
    const auto found = std::lower_bound(records_.begin(), records_.end(), id,
        [](const JournalRecord& record, RecordId value) { return record.id < value; });
    return found != records_.end() && found->id == id ? &*found : nullptr;
}

Result<JournalLease> Journal::pin_record(RecordId id) const noexcept {
    const auto* record = find_record(id);
    if (record == nullptr) {
        const auto lowest_id = lowest_retained_record_id_.load(std::memory_order_acquire);
        return Status{id < lowest_id ? ErrorCode::history_unavailable : ErrorCode::missing_field,
                      0, static_cast<std::size_t>(id)};
    }
    JournalLease lease;
    lease.record_ = *record;
    const auto index = segment_index_from_id(id);
    for (const auto& segment : segments_) {
        if (segment->index == index) {
            auto mapping_lease = acquire_segment_lease(segment);
            if (!mapping_lease) return Status{ErrorCode::capacity_exceeded, 0, static_cast<std::size_t>(id)};
            lease.mapping_lease_ = std::move(mapping_lease);
            return lease;
        }
    }
    return Status{ErrorCode::history_unavailable, 0, static_cast<std::size_t>(id)};
}

const JournalRecord* Journal::find_outbound(std::uint64_t seq) const noexcept {
    if (outbound_index_.empty()) return nullptr;
    const auto& back = records_[outbound_index_.back()];
    if (back.seq == seq) return &back;
    if (back.seq < seq) return nullptr;
    const auto& front = records_[outbound_index_.front()];
    if (front.seq == seq) return &front;
    if (front.seq > seq) return nullptr;
    const auto found = std::lower_bound(outbound_index_.begin(), outbound_index_.end(), seq,
        [this](std::size_t index, std::uint64_t value) { return records_[index].seq < value; });
    if (found == outbound_index_.end() || records_[*found].seq != seq) return nullptr;
    return &records_[*found];
}

bool Journal::has_delivery(RecordId inbound_id) const noexcept {
    const auto* inbound = find_record(inbound_id);
    if (inbound == nullptr || inbound->kind != RecordKind::inbound) return false;
    return inbound->delivered;
}

} // namespace nimblefix
