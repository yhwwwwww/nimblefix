#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>

namespace nimble::bench_profile {

inline auto
NowNs() -> std::uint64_t
{
  return static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline auto
DiffNs(std::uint64_t start, std::uint64_t end) -> std::uint64_t
{
  return end - start;
}

struct InboundProfile
{
  std::uint64_t decode_ns = 0;
  std::uint64_t session_snapshot_ns = 0;
  std::uint64_t validate_comp_ids_ns = 0;
  std::uint64_t validate_poss_dup_ns = 0;
  std::uint64_t observe_seq_ns = 0;
  std::uint64_t record_liveness_ns = 0;
  std::uint64_t validate_app_msg_ns = 0;
  std::uint64_t adopt_ns = 0;
  std::uint64_t adopt_copy_raw_ns = 0;
  std::uint64_t adopt_prelude_ns = 0;
  std::uint64_t adopt_emplace_ns = 0;
  std::uint64_t adopt_raw_move_ns = 0;
  std::uint64_t adopt_rebind_ns = 0;
  std::uint64_t adopt_parsed_move_ns = 0;
  std::uint64_t adopt_reborrow_ns = 0;
  std::uint64_t drain_deferred_ns = 0;
  std::uint64_t other_ns = 0;
  std::uint64_t count = 0;

  void dump() const
  {
    if (count == 0) {
      return;
    }

    const auto total = decode_ns + session_snapshot_ns + validate_comp_ids_ns + validate_poss_dup_ns + observe_seq_ns +
                       record_liveness_ns + validate_app_msg_ns + adopt_ns + drain_deferred_ns + other_ns;

    std::printf("\n=== Inbound Profile (%lu iterations) ===\n", static_cast<unsigned long>(count));
    std::printf("  %-28s %8.1f ns/op\n", "decode:", double(decode_ns) / double(count));
    std::printf("  %-28s %8.1f ns/op\n", "session_snapshot:", double(session_snapshot_ns) / double(count));
    std::printf("  %-28s %8.1f ns/op\n", "validate_comp_ids:", double(validate_comp_ids_ns) / double(count));
    std::printf("  %-28s %8.1f ns/op\n", "validate_poss_dup:", double(validate_poss_dup_ns) / double(count));
    std::printf("  %-28s %8.1f ns/op\n", "observe_seq:", double(observe_seq_ns) / double(count));
    std::printf("  %-28s %8.1f ns/op\n", "record_liveness:", double(record_liveness_ns) / double(count));
    std::printf("  %-28s %8.1f ns/op\n", "validate_app_msg:", double(validate_app_msg_ns) / double(count));
    std::printf("  %-28s %8.1f ns/op\n", "adopt:", double(adopt_ns) / double(count));
    if (adopt_copy_raw_ns != 0 || adopt_prelude_ns != 0 || adopt_emplace_ns != 0 || adopt_raw_move_ns != 0 ||
        adopt_rebind_ns != 0 || adopt_parsed_move_ns != 0 || adopt_reborrow_ns != 0) {
      std::printf("    %-26s %8.1f ns/op\n", "adopt.copy_raw:", double(adopt_copy_raw_ns) / double(count));
      std::printf("    %-26s %8.1f ns/op\n", "adopt.prelude:", double(adopt_prelude_ns) / double(count));
      std::printf("    %-26s %8.1f ns/op\n", "adopt.emplace:", double(adopt_emplace_ns) / double(count));
      std::printf("    %-26s %8.1f ns/op\n", "adopt.raw_move:", double(adopt_raw_move_ns) / double(count));
      std::printf("    %-26s %8.1f ns/op\n", "adopt.rebind:", double(adopt_rebind_ns) / double(count));
      std::printf("    %-26s %8.1f ns/op\n", "adopt.parsed_move:", double(adopt_parsed_move_ns) / double(count));
      std::printf("    %-26s %8.1f ns/op\n", "adopt.reborrow:", double(adopt_reborrow_ns) / double(count));
    }
    std::printf("  %-28s %8.1f ns/op\n", "drain_deferred:", double(drain_deferred_ns) / double(count));
    std::printf("  %-28s %8.1f ns/op\n", "other:", double(other_ns) / double(count));
    std::printf("  %-28s %8.1f ns/op\n", "TOTAL accounted:", double(total) / double(count));
    std::printf("==========================================\n\n");
  }
};

inline auto
GetInboundProfile() -> InboundProfile&
{
  static InboundProfile instance;
  return instance;
}

} // namespace nimble::bench_profile
