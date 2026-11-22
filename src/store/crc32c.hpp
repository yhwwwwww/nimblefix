#pragma once

#include <cstddef>
#include <cstdint>

namespace nimblefix::store {

// Updates an inverted running CRC32C state (start with 0xffffffffU, finish with ~crc).
// Automatically dispatches to SSE4.2 / ARMv8 hardware CRC32C when available,
// falling back to slicing-by-8 software CRC32C.
[[nodiscard]] std::uint32_t crc32c_update(std::uint32_t crc,
                                          const unsigned char* data,
                                          std::size_t size) noexcept;

// Feeds `count` zero bytes into the running CRC32C state without touching memory.
[[nodiscard]] std::uint32_t crc32c_zero(std::uint32_t crc, std::size_t count) noexcept;

// Computes CRC32C over [0, size) treating [zero_offset, zero_offset + zero_length) as zeros.
[[nodiscard]] std::uint32_t crc32c_with_hole(const unsigned char* bytes,
                                             std::size_t size,
                                             std::size_t zero_offset,
                                             std::size_t zero_length) noexcept;

// Computes CRC32C over [0, size) treating two disjoint holes as zeros.
[[nodiscard]] std::uint32_t crc32c_with_two_holes(const unsigned char* bytes,
                                                  std::size_t size,
                                                  std::size_t hole1_offset,
                                                  std::size_t hole1_len,
                                                  std::size_t hole2_offset,
                                                  std::size_t hole2_len) noexcept;

} // namespace nimblefix::store
