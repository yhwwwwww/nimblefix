#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include "fastfix/runtime/config.h"

namespace fastfix::runtime {

enum class TraceEventKind : std::uint32_t {
    kConfigLoaded = 0,
    kProfileLoaded,
    kSessionRegistered,
    kPendingConnectionRegistered,
    kSessionEvent,
    kStoreEvent,
};

struct TraceEvent {
    std::uint64_t sequence{0};
    std::uint64_t timestamp_ns{0};
    TraceEventKind kind{TraceEventKind::kConfigLoaded};
    std::uint64_t session_id{0};
    std::uint32_t worker_id{0};
    std::uint64_t arg0{0};
    std::uint64_t arg1{0};
    std::array<char, 96> text{};
};

class TraceRecorder {
  public:
    auto Configure(TraceMode mode, std::uint32_t capacity) -> void;
    auto Clear() -> void;

    [[nodiscard]] bool enabled() const {
        return mode_ != TraceMode::kDisabled && !ring_.empty();
    }

    [[nodiscard]] std::uint32_t capacity() const {
        return static_cast<std::uint32_t>(ring_.size());
    }

    auto Record(
        TraceEventKind kind,
        std::uint64_t session_id,
        std::uint32_t worker_id,
        std::uint64_t timestamp_ns,
        std::uint64_t arg0,
        std::uint64_t arg1,
        std::string_view text) -> void;

    [[nodiscard]] auto Snapshot() const -> std::vector<TraceEvent>;

  private:
    TraceMode mode_{TraceMode::kDisabled};
    std::vector<TraceEvent> ring_;
    std::uint64_t next_sequence_{1};
    std::size_t next_index_{0};
    std::size_t size_{0};
};

}  // namespace fastfix::runtime