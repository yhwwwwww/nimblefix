#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

#include <nimblefix/status.hpp>

namespace nimblefix {

// An exact base-10 number represented as coefficient * 10^-scale.
// The scale is preserved, including trailing zeroes parsed from text.
class Decimal {
public:
    constexpr Decimal() noexcept = default;
    constexpr explicit Decimal(std::int64_t coefficient, std::uint8_t scale = 0) noexcept
        : coefficient_(coefficient), scale_(scale) {}

    [[nodiscard]] constexpr std::int64_t coefficient() const noexcept { return coefficient_; }
    [[nodiscard]] constexpr std::uint8_t scale() const noexcept { return scale_; }

    [[nodiscard]] static Result<Decimal> parse(std::string_view text) noexcept;
    [[nodiscard]] constexpr std::size_t formatted_size() const noexcept;
    [[nodiscard]] Status format(std::span<char> output, std::size_t& written) const noexcept;

    friend constexpr bool operator==(Decimal, Decimal) noexcept = default;

private:
    std::int64_t coefficient_{};
    std::uint8_t scale_{};
};

namespace detail {

struct DecimalParseResult {
    std::int64_t coefficient{};
    std::uint8_t scale{};
    bool valid{};
};

[[nodiscard]] constexpr DecimalParseResult parse_decimal_constexpr(const char* text, std::size_t size) noexcept {
    if (text == nullptr || size == 0) return {};

    std::size_t pos = 0;
    bool negative = false;
    if (text[pos] == '-' || text[pos] == '+') {
        negative = text[pos] == '-';
        if (++pos == size) return {};
    }

    constexpr std::uint64_t positive_limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    constexpr std::uint64_t negative_limit = positive_limit + 1;
    const std::uint64_t limit = negative ? negative_limit : positive_limit;
    std::uint64_t magnitude = 0;
    std::uint8_t scale = 0;
    bool point_seen = false;
    bool digit_seen = false;

    for (; pos < size; ++pos) {
        const char ch = text[pos];
        if (ch == '.') {
            if (point_seen) return {};
            point_seen = true;
            continue;
        }
        if (ch < '0' || ch > '9') return {};
        digit_seen = true;
        const auto digit = static_cast<std::uint64_t>(ch - '0');
        if (magnitude > (limit - digit) / 10) return {};
        magnitude = magnitude * 10 + digit;
        if (point_seen) {
            if (scale == 18) return {};
            ++scale;
        }
    }
    if (!digit_seen) return {};

    std::int64_t coefficient{};
    if (negative) {
        coefficient = magnitude == negative_limit
            ? std::numeric_limits<std::int64_t>::min()
            : -static_cast<std::int64_t>(magnitude);
    } else {
        coefficient = static_cast<std::int64_t>(magnitude);
    }
    return {coefficient, scale, true};
}

} // namespace detail

namespace literals {

consteval Decimal operator""_dec(const char* text, std::size_t size) {
    const auto parsed = detail::parse_decimal_constexpr(text, size);
    if (!parsed.valid) throw "invalid fixed-point decimal literal";
    return Decimal{parsed.coefficient, parsed.scale};
}

} // namespace literals

[[nodiscard]] constexpr std::size_t Decimal::formatted_size() const noexcept {
    std::uint64_t magnitude = coefficient_ < 0
        ? std::uint64_t{0} - static_cast<std::uint64_t>(coefficient_)
        : static_cast<std::uint64_t>(coefficient_);
    std::size_t digits = 1;
    while (magnitude >= 10) {
        magnitude /= 10;
        ++digits;
    }
    if (scale_ == 0) return digits + (coefficient_ < 0 ? 1 : 0);
    const auto integer_digits = digits > scale_ ? digits - scale_ : std::size_t{1};
    const auto sign = coefficient_ < 0 ? std::size_t{1} : std::size_t{0};
    return sign + integer_digits + 1 + scale_;
}

} // namespace nimblefix
