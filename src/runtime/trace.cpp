#include "fastfix/runtime/trace.h"

#include <algorithm>
#include <cstring>

namespace fastfix::runtime {

auto
TraceRecorder::Configure(TraceMode mode, std::uint32_t capacity, std::uint32_t worker_count) -> void
{
  mode_ = mode;
  capacity_ = capacity;
  buffers_.clear();
  if (mode == TraceMode::kRing && capacity != 0U) {
    const auto buffer_count = std::max<std::uint32_t>(1U, worker_count);
    buffers_.reserve(buffer_count);
    for (std::uint32_t index = 0; index < buffer_count; ++index) {
      auto buffer = std::make_unique<TraceBuffer>();
      buffer->ring.resize(capacity);
      buffers_.push_back(std::move(buffer));
    }
  }
  next_sequence_.store(1U, std::memory_order_relaxed);
}

auto
TraceRecorder::Clear() -> void
{
  for (const auto& buffer : buffers_) {
    if (buffer == nullptr) {
      continue;
    }
    std::lock_guard lock(buffer->mutex);
    std::fill(buffer->ring.begin(), buffer->ring.end(), TraceEvent{});
    buffer->next_index = 0U;
    buffer->size = 0U;
  }
  next_sequence_.store(1U, std::memory_order_relaxed);
}

auto
TraceRecorder::Record(TraceEventKind kind,
                      std::uint64_t session_id,
                      std::uint32_t worker_id,
                      std::uint64_t timestamp_ns,
                      std::uint64_t arg0,
                      std::uint64_t arg1,
                      std::string_view text) -> void
{
  if (!enabled()) {
    return;
  }

  auto* buffer = buffers_[ResolveBufferIndex(worker_id)].get();
  if (buffer == nullptr || buffer->ring.empty()) {
    return;
  }

  const auto sequence = next_sequence_.fetch_add(1U, std::memory_order_relaxed);
  std::lock_guard lock(buffer->mutex);

  auto& event = buffer->ring[buffer->next_index];
  event = TraceEvent{};
  event.sequence = sequence;
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

  buffer->next_index = (buffer->next_index + 1U) % buffer->ring.size();
  if (buffer->size < buffer->ring.size()) {
    ++buffer->size;
  }
}

auto
TraceRecorder::Snapshot() const -> std::vector<TraceEvent>
{
  std::vector<TraceEvent> snapshot;
  if (!enabled()) {
    return snapshot;
  }

  for (const auto& buffer : buffers_) {
    if (buffer == nullptr) {
      continue;
    }
    std::lock_guard lock(buffer->mutex);
    snapshot.reserve(snapshot.size() + buffer->size);
    if (buffer->size == 0U) {
      continue;
    }

    const auto start = buffer->size == buffer->ring.size() ? buffer->next_index : 0U;
    for (std::size_t offset = 0; offset < buffer->size; ++offset) {
      snapshot.push_back(buffer->ring[(start + offset) % buffer->ring.size()]);
    }
  }

  std::sort(snapshot.begin(), snapshot.end(), [](const TraceEvent& lhs, const TraceEvent& rhs) {
    return lhs.sequence < rhs.sequence;
  });
  return snapshot;
}

auto
TraceRecorder::ResolveBufferIndex(std::uint32_t worker_id) const -> std::size_t
{
  if (buffers_.empty()) {
    return 0U;
  }
  return static_cast<std::size_t>(worker_id) % buffers_.size();
}

} // namespace fastfix::runtime