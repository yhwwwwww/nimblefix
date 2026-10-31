#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace nimblefix {

struct TlsConfig {
    bool enabled{};
    std::filesystem::path ca_file;
    std::filesystem::path certificate_file;
    std::filesystem::path private_key_file;
    std::string server_name;
    bool require_client_certificate{};
    std::uint16_t minimum_version{12}; // 12 = TLS 1.2, 13 = TLS 1.3.
    std::uint32_t handshake_timeout_ms{10'000};
    std::size_t max_certificate_bytes{65'536};
};

[[nodiscard]] bool tls_available() noexcept;

} // namespace nimblefix
