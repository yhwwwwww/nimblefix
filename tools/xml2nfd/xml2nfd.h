#pragma once

#include <cstdint>
#include <string>

namespace nimble::tools {

// Convert QuickFIX XML content string to .nfd format string.
auto
ConvertXmlToNfd(const std::string& xml_content, std::uint64_t profile_id) -> std::string;

} // namespace nimble::tools
