#pragma once

#include <cstdint>
#include <initializer_list>
#include <string_view>

#include <nimblefix/status.hpp>

namespace nimblefix {

enum class FixVersion : std::uint8_t { fix42, fix43, fix44, fixt11 };

[[nodiscard]] constexpr std::string_view begin_string(FixVersion version) noexcept {
    switch (version) {
    case FixVersion::fix42:
        return "FIX.4.2";
    case FixVersion::fix43:
        return "FIX.4.3";
    case FixVersion::fix44:
        return "FIX.4.4";
    case FixVersion::fixt11:
        return "FIXT.1.1";
    }
    return {};
}

[[nodiscard]] inline Result<FixVersion> parse_fix_version(std::string_view text) noexcept {
    for (const auto version : {FixVersion::fix42, FixVersion::fix43, FixVersion::fix44, FixVersion::fixt11})
        if (begin_string(version) == text)
            return version;
    return Status{ErrorCode::unsupported_protocol, 8};
}

[[nodiscard]] constexpr bool valid_appl_ver_id(std::string_view value) noexcept {
    // FIXT DefaultApplVerID: FIX 4.2, 4.3, 4.4, 5.0, 5.0SP1, 5.0SP2.
    return value == "4" || value == "5" || value == "6" || value == "7" || value == "8" || value == "9";
}

} // namespace nimblefix
