#pragma once

#include <cstddef>

namespace nimblefix {

struct FlatSchema {};

// Engine execution strategy. Both strategies use the same session driver and
// journal; Queued only moves business delivery to a bounded worker executor.
struct Inline {};
struct Queued {};

enum class QueuedDeliveryStrategy {
    journal_cursor,
    descriptor_ring,
};

struct QueuedConfig {
    std::size_t worker_threads{2};
    std::size_t max_pending_callbacks{128};
    std::size_t max_pending_callback_bytes{4U * 1024U * 1024U};
    // Kept selectable so both real delivery paths can be compared under the
    // same workload. journal_cursor borrows committed mmap payloads; the ring
    // path copies only into its bounded descriptor payload arena.
    QueuedDeliveryStrategy delivery_strategy{QueuedDeliveryStrategy::descriptor_ring};
};

} // namespace nimblefix
