#include <nimblefix/decimal.hpp>

#include <limits>

namespace nimblefix {

Result<Decimal> Decimal::parse(std::string_view text) noexcept {
    const auto parsed = detail::parse_decimal_constexpr(text.data(), text.size());
    if (!parsed.valid) return Status{ErrorCode::invalid_decimal};
    return Decimal{parsed.coefficient, parsed.scale};
}

Status Decimal::format(std::span<char> output, std::size_t& written) const noexcept {
    written = 0;
    if (scale_ > 18) return Status{ErrorCode::invalid_decimal};
    const std::size_t required = formatted_size();
    if (output.size() < required) return Status{ErrorCode::capacity_exceeded};

    std::uint64_t magnitude = coefficient_ < 0
        ? std::uint64_t{0} - static_cast<std::uint64_t>(coefficient_)
        : static_cast<std::uint64_t>(coefficient_);
    char reverse_digits[20];
    std::size_t digit_count = 0;
    do {
        reverse_digits[digit_count++] = static_cast<char>('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude != 0);

    std::size_t out = 0;
    if (coefficient_ < 0) output[out++] = '-';

    const auto scale = static_cast<std::size_t>(scale_);
    if (scale == 0) {
        while (digit_count != 0) output[out++] = reverse_digits[--digit_count];
    } else {
        if (digit_count > scale) {
            for (std::size_t i = digit_count; i > scale; --i) output[out++] = reverse_digits[i - 1];
        } else {
            output[out++] = '0';
        }
        output[out++] = '.';
        for (std::size_t i = scale; i != 0; --i) {
            const auto digit_index = i - 1;
            output[out++] = digit_index < digit_count ? reverse_digits[digit_index] : '0';
        }
    }
    written = out;
    return Status::success();
}

} // namespace nimblefix
