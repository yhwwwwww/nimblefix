#include "fastfix/runtime/trace.h"

#include <algorithm>
#include <cstring>

namespace fastfix::runtime {

auto TraceRecorder::Configure(TraceMode mode, std::uint32_t capacity) -> void {
    mode_ = mode;
    ring_.clear();
    if (mode == TraceMode::kRing && capacity != 0) {
        ring_.resize(capacity);
    }
    next_sequence_ = 1;
    next_index_ = 0;
    size_ = 0;
}

auto TraceRecorder::Clear() -> void {
    std::fill(ring_.begin(), ring_.end(), TraceEvent{});
    next_sequence_ = 1;
    next_index_ = 0;
    size_ = 0;
}

auto TraceRecorder::Record(
    TraceEventKind kind,
    std::uint64_t session_id,
    std::uint32_t worker_id,
    std::uint64_t timestamp_ns,
    std::uint64_t arg0,
    std::uint64_t arg1,
    std::string_view text) -> void {
    if (!enabled()) {
        return;
    }

    auto& event = ring_[next_index_];
    event = TraceEvent{};
    event.sequence = next_sequence_++;
    event.timestamp_ns = timestamp_ns;
    event.kind = kind;
    event.session_id = session_id;
    event.worker_id = worker_id;
    event.arg0 = arg0;
    event.arg1 = arg1;

    const auto copy_size = std::min(text.size(), event.text.size() - 1U);
    if (copy_size != 0) {
        std::memcpy(event.text.data(), text.data(), copy_size);
    }
    event.text[copy_size] = '\0';

    next_index_ = (next_index_ + 1U) % ring_.size();
    if (size_ < ring_.size()) {
        ++size_;
    }
}

auto TraceRecorder::Snapshot() const -> std::vector<TraceEvent> {
    std::vector<TraceEvent> snapshot;
    snapshot.reserve(size_);
    if (!enabled() || size_ == 0) {
        return snapshot;
    }

    const auto start = size_ == ring_.size() ? next_index_ : 0U;
    for (std::size_t offset = 0; offset < size_; ++offset) {
        snapshot.push_back(ring_[(start + offset) % ring_.size()]);
    }
    return snapshot;
}

}  // namespace fastfix::runtime