#include <nimblefix/decimal.hpp>

#include <bit>
#include <cstring>
#include <limits>

namespace nimblefix {
namespace {

inline constexpr char kDigitPairs[201] =
    "0001020304050607080910111213141516171819"
    "2021222324252627282930313233343536373839"
    "4041424344454647484950515253545556575859"
    "6061626364656667686970717273747576777879"
    "8081828384858687888990919293949596979899";

} // namespace

Result<Decimal> Decimal::parse(std::string_view text) noexcept {
    const auto parsed = detail::parse_decimal_constexpr(text.data(), text.size());
    if (!parsed.valid) return Status{ErrorCode::invalid_decimal};
    return Decimal{parsed.coefficient, parsed.scale};
}

Status Decimal::format(std::span<char> output, std::size_t& written) const noexcept {
    written = 0;
    if (scale_ > 18) return Status{ErrorCode::invalid_decimal};

    const bool negative = coefficient_ < 0;
    std::uint64_t magnitude = negative
        ? std::uint64_t{0} - std::bit_cast<std::uint64_t>(coefficient_)
        : static_cast<std::uint64_t>(coefficient_);

    char tmp[24];
    char* const end = tmp + sizeof(tmp);
    char* cur = end;
    while (magnitude >= 100ULL) {
        const auto idx = static_cast<std::size_t>((magnitude % 100ULL) * 2ULL);
        magnitude /= 100ULL;
        cur -= 2;
        cur[0] = kDigitPairs[idx];
        cur[1] = kDigitPairs[idx + 1];
    }
    if (magnitude >= 10ULL) {
        const auto idx = static_cast<std::size_t>(magnitude * 2ULL);
        cur -= 2;
        cur[0] = kDigitPairs[idx];
        cur[1] = kDigitPairs[idx + 1];
    } else {
        *--cur = static_cast<char>('0' + magnitude);
    }

    const std::size_t digits = static_cast<std::size_t>(end - cur);
    const std::size_t scale = static_cast<std::size_t>(scale_);
    const std::size_t sign = negative ? 1U : 0U;
    const std::size_t required = scale == 0
        ? (sign + digits)
        : (sign + (digits > scale ? digits - scale : 1U) + 1U + scale);
    if (output.size() < required) return Status{ErrorCode::capacity_exceeded};

    char* p = output.data();
    if (negative) *p++ = '-';
    if (scale == 0) {
        std::memcpy(p, cur, digits);
    } else if (digits > scale) {
        const std::size_t int_len = digits - scale;
        std::memcpy(p, cur, int_len);
        p += int_len;
        *p++ = '.';
        std::memcpy(p, cur + int_len, scale);
    } else {
        *p++ = '0';
        *p++ = '.';
        const std::size_t zeros = scale - digits;
        if (zeros != 0) {
            std::memset(p, '0', zeros);
            p += zeros;
        }
        std::memcpy(p, cur, digits);
    }
    written = required;
    return Status::success();
}

} // namespace nimblefix
