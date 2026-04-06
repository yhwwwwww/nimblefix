#pragma once

#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fastfix::tests {

inline auto Bytes(std::string_view text) -> std::vector<std::byte> {
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (const auto ch : text) {
        bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(ch)));
    }
    return bytes;
}

inline auto EncodeFixFrame(
    std::string_view body_fields,
    std::string_view begin_string = "FIX.4.4",
    char readable_delimiter = '|') -> std::vector<std::byte> {
    std::string body(body_fields);
    for (auto& ch : body) {
        if (ch == readable_delimiter) {
            ch = '\x01';
        }
    }

    std::string full;
    full.append("8=");
    full.append(begin_string);
    full.push_back('\x01');
    full.append("9=");
    full.append(std::to_string(body.size()));
    full.push_back('\x01');
    full.append(body);

    std::uint32_t checksum = 0;
    for (const auto ch : full) {
        checksum += static_cast<unsigned char>(ch);
    }
    checksum %= 256U;

    std::ostringstream stream;
    stream << "10=" << std::setw(3) << std::setfill('0') << checksum << '\x01';
    full.append(stream.str());
    return Bytes(full);
}

}  // namespace fastfix::tests
