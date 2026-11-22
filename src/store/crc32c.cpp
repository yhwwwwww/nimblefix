#include "crc32c.hpp"

#include <array>
#include <cstring>

#if defined(__x86_64__) || defined(_M_X64)
#include <nmmintrin.h>
#endif

#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#endif

namespace nimblefix::store {
namespace {

constexpr std::uint32_t crc32c_poly = 0x82f63b78U;

constexpr std::array<std::array<std::uint32_t, 256>, 8> make_slicing_tables() noexcept {
    std::array<std::array<std::uint32_t, 256>, 8> tables{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t crc = i;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1U) ^ ((crc & 1U) ? crc32c_poly : 0U);
        tables[0][i] = crc;
    }
    for (std::size_t t = 1; t < 8; ++t) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            const std::uint32_t prev = tables[t - 1][i];
            tables[t][i] = (prev >> 8U) ^ tables[0][prev & 0xffU];
        }
    }
    return tables;
}

alignas(64) constexpr auto slicing_tables = make_slicing_tables();

[[nodiscard]] std::uint32_t crc32c_software(std::uint32_t crc,
                                            const unsigned char* data,
                                            std::size_t size) noexcept {
    while (size >= 8) {
        std::uint32_t low = 0;
        std::uint32_t high = 0;
        std::memcpy(&low, data, 4);
        std::memcpy(&high, data + 4, 4);
        low ^= crc;
        crc = slicing_tables[7][low & 0xffU] ^
              slicing_tables[6][(low >> 8U) & 0xffU] ^
              slicing_tables[5][(low >> 16U) & 0xffU] ^
              slicing_tables[4][(low >> 24U) & 0xffU] ^
              slicing_tables[3][high & 0xffU] ^
              slicing_tables[2][(high >> 8U) & 0xffU] ^
              slicing_tables[1][(high >> 16U) & 0xffU] ^
              slicing_tables[0][(high >> 24U) & 0xffU];
        data += 8;
        size -= 8;
    }
    while (size-- > 0) {
        crc = (crc >> 8U) ^ slicing_tables[0][(crc ^ *data++) & 0xffU];
    }
    return crc;
}

#if (defined(__x86_64__) || defined(_M_X64)) && (defined(__GNUC__) || defined(__clang__))
__attribute__((target("sse4.2")))
[[nodiscard]] std::uint32_t crc32c_sse42(std::uint32_t crc,
                                         const unsigned char* data,
                                         std::size_t size) noexcept {
    std::uint64_t state = crc;
    while (size >= 8) {
        std::uint64_t word = 0;
        std::memcpy(&word, data, sizeof(word));
        state = _mm_crc32_u64(state, word);
        data += 8;
        size -= 8;
    }
    std::uint32_t tail = static_cast<std::uint32_t>(state);
    if (size >= 4) {
        std::uint32_t word = 0;
        std::memcpy(&word, data, sizeof(word));
        tail = _mm_crc32_u32(tail, word);
        data += 4;
        size -= 4;
    }
    while (size-- > 0) {
        tail = _mm_crc32_u8(tail, *data++);
    }
    return tail;
}

[[nodiscard]] bool cpu_has_sse42() noexcept {
    static const bool supported = []() noexcept {
        __builtin_cpu_init();
        return __builtin_cpu_supports("sse4.2") != 0;
    }();
    return supported;
}
#endif

} // namespace

std::uint32_t crc32c_update(std::uint32_t crc,
                            const unsigned char* data,
                            std::size_t size) noexcept {
    if (size == 0) return crc;
#if (defined(__x86_64__) || defined(_M_X64)) && (defined(__GNUC__) || defined(__clang__))
    if (cpu_has_sse42()) return crc32c_sse42(crc, data, size);
#elif defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
    while (size >= 8) {
        std::uint64_t word = 0;
        std::memcpy(&word, data, sizeof(word));
        crc = __crc32cd(crc, word);
        data += 8;
        size -= 8;
    }
    while (size-- > 0) crc = __crc32cb(crc, *data++);
    return crc;
#endif
    return crc32c_software(crc, data, size);
}

std::uint32_t crc32c_zero(std::uint32_t crc, std::size_t count) noexcept {
    static constexpr std::array<unsigned char, 16> zeros{};
    while (count >= zeros.size()) {
        crc = crc32c_update(crc, zeros.data(), zeros.size());
        count -= zeros.size();
    }
    if (count != 0) crc = crc32c_update(crc, zeros.data(), count);
    return crc;
}

std::uint32_t crc32c_with_hole(const unsigned char* bytes,
                               std::size_t size,
                               std::size_t zero_offset,
                               std::size_t zero_length) noexcept {
    std::uint32_t crc = 0xffffffffU;
    if (zero_offset > size) zero_offset = size;
    if (zero_length > size - zero_offset) zero_length = size - zero_offset;
    crc = crc32c_update(crc, bytes, zero_offset);
    crc = crc32c_zero(crc, zero_length);
    const std::size_t tail = zero_offset + zero_length;
    crc = crc32c_update(crc, bytes + tail, size - tail);
    return ~crc;
}

std::uint32_t crc32c_with_two_holes(const unsigned char* bytes,
                                    std::size_t size,
                                    std::size_t hole1_offset,
                                    std::size_t hole1_len,
                                    std::size_t hole2_offset,
                                    std::size_t hole2_len) noexcept {
    std::uint32_t crc = 0xffffffffU;
    crc = crc32c_update(crc, bytes, hole1_offset);
    crc = crc32c_zero(crc, hole1_len);
    const std::size_t mid_start = hole1_offset + hole1_len;
    crc = crc32c_update(crc, bytes + mid_start, hole2_offset - mid_start);
    crc = crc32c_zero(crc, hole2_len);
    const std::size_t tail_start = hole2_offset + hole2_len;
    crc = crc32c_update(crc, bytes + tail_start, size - tail_start);
    return ~crc;
}

} // namespace nimblefix::store
