#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

#include "nimblefix/runtime/config.h"

namespace nimble::runtime {

enum class TraceEventKind : std::uint32_t
{
  kConfigLoaded = 0,
  kProfileLoaded,
  kSessionRegistered,
  kPendingConnectionRegistered,
  kSessionEvent,
  kStoreEvent,
};

struct TraceEvent
{
  std::uint64_t sequence{ 0 };
  std::uint64_t timestamp_ns{ 0 };
  TraceEventKind kind{ TraceEventKind::kConfigLoaded };
  std::uint64_t session_id{ 0 };
  std::uint32_t worker_id{ 0 };
  std::uint64_t arg0{ 0 };
  std::uint64_t arg1{ 0 };
  std::array<char, 96> text{};
};

class TraceRecorder
{
public:
  auto Configure(TraceMode mode, std::uint32_t capacity, std::uint32_t worker_count = 1U) -> void;
  auto Clear() -> void;

  [[nodiscard]] bool enabled() const { return mode_ != TraceMode::kDisabled && capacity_ != 0U && !buffers_.empty(); }

  [[nodiscard]] std::uint32_t capacity() const { return capacity_; }

  auto Record(TraceEventKind kind,
              std::uint64_t session_id,
              std::uint32_t worker_id,
              std::uint64_t timestamp_ns,
              std::uint64_t arg0,
              std::uint64_t arg1,
              std::string_view text) -> void;

  [[nodiscard]] auto Snapshot() const -> std::vector<TraceEvent>;

private:
  struct TraceBuffer
  {
    mutable std::mutex mutex;
    std::vector<TraceEvent> ring;
    std::size_t next_index{ 0U };
    std::size_t size{ 0U };
  };

  [[nodiscard]] auto ResolveBufferIndex(std::uint32_t worker_id) const -> std::size_t;

  TraceMode mode_{ TraceMode::kDisabled };
  std::uint32_t capacity_{ 0U };
  std::vector<std::unique_ptr<TraceBuffer>> buffers_;
  std::atomic<std::uint64_t> next_sequence_{ 1U };
};

} // namespace nimble::runtime