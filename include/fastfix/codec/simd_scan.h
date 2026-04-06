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

inline auto FindByteScalar(const std::byte* data, std::size_t len, std::byte needle) -> const std::byte* {
    auto* p = static_cast<const std::byte*>(std::memchr(data, static_cast<int>(needle), len));
    return p ? p : data + len;
}

#if FASTFIX_HAS_SSE2
inline auto FindByteSse2(const std::byte* data, std::size_t len, std::byte needle) -> const std::byte* {
    const auto target = _mm_set1_epi8(static_cast<char>(needle));
    std::size_t i = 0;
    for (; i + 16 <= len; i += 16) {
        auto chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i));
        auto cmp = _mm_cmpeq_epi8(chunk, target);
        int mask = _mm_movemask_epi8(cmp);
        if (mask != 0) {
            return data + i + __builtin_ctz(static_cast<unsigned>(mask));
        }
    }
    // Scalar fallback for remaining bytes
    for (; i < len; ++i) {
        if (data[i] == needle) return data + i;
    }
    return data + len;
}
#endif

inline auto FindByte(const std::byte* data, std::size_t len, std::byte needle) -> const std::byte* {
#if FASTFIX_HAS_SSE2
    return FindByteSse2(data, len, needle);
#else
    return FindByteScalar(data, len, needle);
#endif
}

}  // namespace fastfix::codec
