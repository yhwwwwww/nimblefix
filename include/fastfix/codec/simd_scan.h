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

inline auto FindByteShortScalar(const std::byte* data, std::size_t len, std::byte needle) -> const std::byte* {
    for (std::size_t i = 0; i < len; ++i) {
        if (data[i] == needle) {
            return data + i;
        }
    }
    return data + len;
}

inline auto FindByteScalar(const std::byte* data, std::size_t len, std::byte needle) -> const std::byte* {
    auto* p = static_cast<const std::byte*>(std::memchr(data, static_cast<int>(needle), len));
    return p ? p : data + len;
}

#if FASTFIX_HAS_SSE2
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#endif
inline auto FindByteSse2(const std::byte* data, std::size_t len, std::byte needle) -> const std::byte* {
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

inline auto FindByte(const std::byte* data, std::size_t len, std::byte needle) -> const std::byte* {
#if FASTFIX_HAS_SSE2
    if (len < 16U) {
        return FindByteShortScalar(data, len, needle);
    }
    return FindByteSse2(data, len, needle);
#else
    return FindByteScalar(data, len, needle);
#endif
}

}  // namespace fastfix::codec
