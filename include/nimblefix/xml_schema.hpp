#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <nimblefix/schema.hpp>

namespace nimblefix {

struct XmlSchemaLimits {
    std::size_t max_input_file_bytes{4 * 1024 * 1024};
    std::size_t max_total_input_bytes{16 * 1024 * 1024};
    std::size_t max_input_files{32};
    std::size_t max_xml_depth{64};
    std::size_t max_xml_items{50'000};
    std::size_t max_fields{16'384};
    std::size_t max_messages{4096};
    std::size_t max_components{4096};
    std::size_t max_groups{16'384};
};

struct XmlSchemaDiagnostic {
    Status status{};
    std::string source{};
    std::string message{};
    std::size_t line{};
    std::size_t column{};
};

class XmlSchema {
public:
    struct Storage;
    XmlSchema() noexcept;
    XmlSchema(const XmlSchema&) = delete;
    XmlSchema& operator=(const XmlSchema&) = delete;
    XmlSchema(XmlSchema&&) noexcept;
    XmlSchema& operator=(XmlSchema&&) noexcept;
    ~XmlSchema();

    [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(storage_); }
    [[nodiscard]] const SchemaDescriptor& descriptor() const noexcept;
    [[nodiscard]] WireHints wire_hints() const noexcept;
    [[nodiscard]] const MessageDescriptor* find_message(std::string_view msg_type) const noexcept {
        return descriptor().find_message(msg_type);
    }
    [[nodiscard]] std::string_view fingerprint() const noexcept {
        return descriptor().fingerprint;
    }

private:
    explicit XmlSchema(std::unique_ptr<Storage> storage) noexcept;
    std::unique_ptr<Storage> storage_{};
    friend Status load_xml_schema(std::span<const std::string_view>, XmlSchema&,
        XmlSchemaDiagnostic&, XmlSchemaLimits) noexcept;
};

// Input files are merged as peers. Identical definitions are deduplicated and
// conflicting definitions report the source location of the rejected input.
// On failure, output remains unchanged and diagnostic owns its source/message.
[[nodiscard]] Status load_xml_schema(std::span<const std::string_view> paths,
    XmlSchema& output, XmlSchemaDiagnostic& diagnostic,
    XmlSchemaLimits limits = {}) noexcept;

[[nodiscard]] inline Status check_schema_fingerprint(const SchemaDescriptor& generated,
    const XmlSchema& loaded) noexcept {
    if (!loaded.valid() || generated.fingerprint.empty() ||
        generated.fingerprint != loaded.fingerprint()) return {ErrorCode::schema_mismatch};
    return Status::success();
}

} // namespace nimblefix
