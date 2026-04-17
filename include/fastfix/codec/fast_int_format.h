#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace fastfix::codec {

// Digit pair lookup table for 00-99.
// Each entry is two ASCII chars: tens digit and units digit.
inline constexpr auto
BuildDigitPairTable() -> std::array<std::array<char, 2>, 100>
{
  std::array<std::array<char, 2>, 100> table{};
  for (int i = 0; i < 100; ++i) {
    table[i][0] = static_cast<char>('0' + i / 10);
    table[i][1] = static_cast<char>('0' + i % 10);
  }
  return table;
}

inline constexpr auto kDigitPairs = BuildDigitPairTable();

// Format a uint32 into buf. Returns the number of chars written.
// buf must have at least 10 bytes of space.
// Uses digit-pair decomposition for speed.
inline auto
FormatUint32(char* buf, std::uint32_t value) -> std::size_t
{
  if (value < 10U) {
    buf[0] = static_cast<char>('0' + value);
    return 1;
  }
  if (value < 100U) {
    const auto& p = kDigitPairs[value];
    buf[0] = p[0];
    buf[1] = p[1];
    return 2;
  }
  if (value < 1000U) {
    const auto& p = kDigitPairs[value % 100U];
    buf[0] = static_cast<char>('0' + value / 100U);
    buf[1] = p[0];
    buf[2] = p[1];
    return 3;
  }
  if (value < 10000U) {
    const auto& p1 = kDigitPairs[value / 100U];
    const auto& p2 = kDigitPairs[value % 100U];
    buf[0] = p1[0];
    buf[1] = p1[1];
    buf[2] = p2[0];
    buf[3] = p2[1];
    return 4;
  }
  if (value < 100000U) {
    const auto q = value / 100U;
    const auto& p1 = kDigitPairs[q % 100U];
    const auto& p2 = kDigitPairs[value % 100U];
    buf[0] = static_cast<char>('0' + q / 100U);
    buf[1] = p1[0];
    buf[2] = p1[1];
    buf[3] = p2[0];
    buf[4] = p2[1];
    return 5;
  }
  if (value < 1000000U) {
    const auto q = value / 100U;
    const auto& p0 = kDigitPairs[q / 100U];
    const auto& p1 = kDigitPairs[q % 100U];
    const auto& p2 = kDigitPairs[value % 100U];
    buf[0] = p0[0];
    buf[1] = p0[1];
    buf[2] = p1[0];
    buf[3] = p1[1];
    buf[4] = p2[0];
    buf[5] = p2[1];
    return 6;
  }
  // 7+ digits: rare for FIX, fall back to similar decomposition
  if (value < 10000000U) {
    const auto q = value / 10000U;
    const auto r = value % 10000U;
    const auto& p1 = kDigitPairs[q % 100U];
    const auto& p2 = kDigitPairs[r / 100U];
    const auto& p3 = kDigitPairs[r % 100U];
    buf[0] = static_cast<char>('0' + q / 100U);
    buf[1] = p1[0];
    buf[2] = p1[1];
    buf[3] = p2[0];
    buf[4] = p2[1];
    buf[5] = p3[0];
    buf[6] = p3[1];
    return 7;
  }
  // For 8+ digits, use sequential decomposition
  char tmp[10];
  std::size_t len = 0;
  auto v = value;
  while (v > 0) {
    tmp[len++] = static_cast<char>('0' + v % 10U);
    v /= 10U;
  }
  for (std::size_t i = 0; i < len; ++i) {
    buf[i] = tmp[len - 1 - i];
  }
  return len;
}

// Format an int64 into buf. Returns the number of chars written.
// buf must have at least 20 bytes of space.
inline auto
FormatInt64(char* buf, std::int64_t value) -> std::size_t
{
  if (value < 0) {
    buf[0] = '-';
    // Handle INT64_MIN carefully
    auto uval = static_cast<std::uint64_t>(-(value + 1)) + 1ULL;
    // For negative values, format the absolute value after the minus sign
    // Reuse uint32 path if it fits
    if (uval <= UINT32_MAX) {
      return 1 + FormatUint32(buf + 1, static_cast<std::uint32_t>(uval));
    }
    // Large negative -- use sequential decomposition
    char tmp[20];
    std::size_t len = 0;
    while (uval > 0) {
      tmp[len++] = static_cast<char>('0' + uval % 10ULL);
      uval /= 10ULL;
    }
    for (std::size_t i = 0; i < len; ++i) {
      buf[1 + i] = tmp[len - 1 - i];
    }
    return 1 + len;
  }
  if (value <= UINT32_MAX) {
    return FormatUint32(buf, static_cast<std::uint32_t>(value));
  }
  // Large positive -- decompose
  char tmp[20];
  std::size_t len = 0;
  auto uval = static_cast<std::uint64_t>(value);
  while (uval > 0) {
    tmp[len++] = static_cast<char>('0' + uval % 10ULL);
    uval /= 10ULL;
  }
  for (std::size_t i = 0; i < len; ++i) {
    buf[i] = tmp[len - 1 - i];
  }
  return len;
}

} // namespace fastfix::codec
