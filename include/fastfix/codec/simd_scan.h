#pragma once

#include <cstddef>
#include <cstring>

#if defined(__SSE2__)
#define FASTFIX_HAS_SSE2 1
#include <immintrin.h>
#else
#define FASTFIX_HAS_SSE2 0
#endif

namespace fastfix::codec {

inline auto
FindByteShortScalar(const std::byte* data, std::size_t len, std::byte needle) -> const std::byte*
{
  for (std::size_t i = 0; i < len; ++i) {
    if (data[i] == needle) {
      return data + i;
    }
  }
  return data + len;
}

inline auto
FindByteScalar(const std::byte* data, std::size_t len, std::byte needle) -> const std::byte*
{
  auto* p = static_cast<const std::byte*>(std::memchr(data, static_cast<int>(needle), len));
  return p ? p : data + len;
}

#if FASTFIX_HAS_SSE2
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#endif
inline auto
FindByteSse2(const std::byte* data, std::size_t len, std::byte needle) -> const std::byte*
{
  const auto target = _mm_set1_epi8(static_cast<char>(needle));
  std::size_t i = 0;
  for (; i + 16 <= len; i += 16) {
    auto chunk = _mm_loadu_si128(reinterpret_cast<const __m128i_u*>(data + i));
    auto cmp = _mm_cmpeq_epi8(chunk, target);
    int mask = _mm_movemask_epi8(cmp);
    if (mask != 0) {
      return data + i + __builtin_ctz(static_cast<unsigned>(mask));
    }
  }
  return FindByteShortScalar(data + i, len - i, needle);
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
#endif

inline auto
FindByte(const std::byte* data, std::size_t len, std::byte needle) -> const std::byte*
{
#if FASTFIX_HAS_SSE2
  if (len < 16U) {
    return FindByteShortScalar(data, len, needle);
  }
  return FindByteSse2(data, len, needle);
#else
  return FindByteScalar(data, len, needle);
#endif
}

/// Dual-target SIMD scan: finds the first occurrence of either byte a or byte
/// b. Returns a pointer to the first match, or data + len if neither is found.
/// The caller can inspect which byte was found by comparing *result == a or
/// *result == b.
inline auto
FindEitherByte(const std::byte* data, std::size_t len, std::byte a, std::byte b) -> const std::byte*
{
#if FASTFIX_HAS_SSE2
  const auto target_a = _mm_set1_epi8(static_cast<char>(a));
  const auto target_b = _mm_set1_epi8(static_cast<char>(b));
  std::size_t i = 0;
  for (; i + 16 <= len; i += 16) {
    auto chunk = _mm_loadu_si128(reinterpret_cast<const __m128i_u*>(data + i));
    auto cmp_a = _mm_cmpeq_epi8(chunk, target_a);
    auto cmp_b = _mm_cmpeq_epi8(chunk, target_b);
    int mask = _mm_movemask_epi8(_mm_or_si128(cmp_a, cmp_b));
    if (mask != 0) {
      return data + i + __builtin_ctz(static_cast<unsigned>(mask));
    }
  }
  // Scalar tail
  for (; i < len; ++i) {
    if (data[i] == a || data[i] == b) {
      return data + i;
    }
  }
  return data + len;
#else
  for (std::size_t i = 0; i < len; ++i) {
    if (data[i] == a || data[i] == b) {
      return data + i;
    }
  }
  return data + len;
#endif
}

/// SIMD-accelerated byte-sum checksum used by FIX encode and decode paths.
/// Returns the raw sum (caller applies % 256 as needed).
inline auto
ComputeChecksumSIMD(const char* data, std::size_t len) -> std::uint32_t
{
  std::uint32_t sum = 0;
#if FASTFIX_HAS_SSE2
  const auto zero = _mm_setzero_si128();
  std::size_t i = 0;
  for (; i + 16 <= len; i += 16) {
    auto chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i));
    auto sad = _mm_sad_epu8(chunk, zero);
    sum +=
      static_cast<std::uint32_t>(_mm_extract_epi16(sad, 0)) + static_cast<std::uint32_t>(_mm_extract_epi16(sad, 4));
  }
  for (; i < len; ++i) {
    sum += static_cast<unsigned char>(data[i]);
  }
#else
  for (std::size_t i = 0; i < len; ++i) {
    sum += static_cast<unsigned char>(data[i]);
  }
#endif
  return sum;
}

/// Overload for std::byte pointers.
inline auto
ComputeChecksumSIMD(const std::byte* data, std::size_t len) -> std::uint32_t
{
  return ComputeChecksumSIMD(reinterpret_cast<const char*>(data), len);
}

} // namespace fastfix::codec
