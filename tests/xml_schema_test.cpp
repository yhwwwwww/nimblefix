#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#include <nimblefix/xml_schema.hpp>

namespace {

int failures = 0;

#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #condition << '\n'; \
    ++failures; \
} } while (false)

void write_file(const std::filesystem::path& path, std::string_view value) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(value.data(), static_cast<std::streamsize>(value.size()));
}

constexpr std::string_view fields_document =
    "<fix type='FIX' major='4' minor='4'><fields>"
    "<field number='11' name='ClOrdID' type='STRING'/>"
    "</fields></fix>";

constexpr std::string_view message_document =
    "<fix type='FIX' major='4' minor='4'><messages>"
    "<message name='NewOrderSingle' msgtype='D' msgcat='app'>"
    "<field name='ClOrdID' required='Y'/></message>"
    "</messages></fix>";

constexpr std::string_view conflicting_field_document =
    "<fix type='FIX' major='4' minor='4'><fields>"
    "<field number='11' name='ClOrdID' type='CHAR'/>"
    "</fields></fix>";

constexpr std::string_view dtd_document =
    "<!DOCTYPE fix [<!ENTITY injected 'bad'>]>"
    "<fix type='FIX' major='4' minor='4'><fields>"
    "<field number='11' name='ClOrdID' type='STRING'><value enum='&injected;'/></field>"
    "</fields><messages><message name='NewOrderSingle' msgtype='D'>"
    "<field name='ClOrdID' required='Y'/></message></messages></fix>";

constexpr std::string_view unpaired_data_document =
    "<fix type='FIX' major='4' minor='4'><fields>"
    "<field number='900001' name='UnpairedData' type='DATA'/>"
    "</fields><messages><message name='RawMessage' msgtype='Z'>"
    "<field name='UnpairedData' required='N'/></message></messages></fix>";

} // namespace

int main() {
    using namespace nimblefix;
    const auto directory = std::filesystem::temp_directory_path() / "nimblefix-xml-schema-test";
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::filesystem::create_directories(directory);
    const auto fields_path = directory / "fields.xml";
    const auto messages_path = directory / "messages.xml";
    const auto complete_path = directory / "complete.xml";
    const auto conflict_path = directory / "conflict.xml";
    const auto dtd_path = directory / "dtd.xml";
    const auto data_path = directory / "unpaired-data.xml";
    const auto fields_name = fields_path.string();
    const auto messages_name = messages_path.string();
    const auto complete_name = complete_path.string();
    const auto conflict_name = conflict_path.string();
    const auto dtd_name = dtd_path.string();
    const auto data_name = data_path.string();

    write_file(fields_path, fields_document);
    write_file(messages_path, message_document);
    write_file(complete_path,
        "<fix type='FIX' major='4' minor='4'><fields>"
        "<field number='11' name='ClOrdID' type='STRING'/>"
        "</fields><messages><message name='NewOrderSingle' msgtype='D' msgcat='app'>"
        "<field name='ClOrdID' required='Y'/></message></messages></fix>");
    write_file(conflict_path, conflicting_field_document);
    write_file(dtd_path, dtd_document);
    write_file(data_path, unpaired_data_document);

    XmlSchemaDiagnostic diagnostic{};
    XmlSchema split{};
    const std::array<std::string_view, 2> split_paths{fields_name, messages_name};
    CHECK(load_xml_schema(split_paths, split, diagnostic).ok());
    CHECK(split.valid());
    CHECK(split.descriptor().find_field(11) != nullptr);
    CHECK(split.find_message("D") != nullptr);

    XmlSchema complete{};
    const std::array<std::string_view, 1> complete_paths{complete_name};
    CHECK(load_xml_schema(complete_paths, complete, diagnostic).ok());
    CHECK(split.fingerprint() == complete.fingerprint());

    XmlSchema duplicate{};
    const std::array<std::string_view, 2> duplicate_paths{complete_name, complete_name};
    CHECK(load_xml_schema(duplicate_paths, duplicate, diagnostic).ok());
    CHECK(duplicate.fingerprint() == complete.fingerprint());

    XmlSchema preserved{};
    CHECK(load_xml_schema(complete_paths, preserved, diagnostic).ok());
    const std::string original_fingerprint{preserved.fingerprint()};
    const std::array<std::string_view, 2> conflict_paths{complete_name, conflict_name};
    const auto conflict = load_xml_schema(conflict_paths, preserved, diagnostic);
    CHECK(conflict.code == ErrorCode::schema_mismatch);
    CHECK(!diagnostic.source.empty() && diagnostic.source == conflict_name);
    CHECK(diagnostic.line != 0 && !diagnostic.message.empty());
    CHECK(preserved.fingerprint() == original_fingerprint);

    const std::array<std::string_view, 1> dtd_paths{dtd_name};
    const auto dtd = load_xml_schema(dtd_paths, preserved, diagnostic);
    CHECK(!dtd.ok() && dtd.code == ErrorCode::invalid_value);
    CHECK(diagnostic.message.find("DOCTYPE") != std::string::npos ||
        diagnostic.message.find("entity") != std::string::npos ||
        diagnostic.message.find("external") != std::string::npos);
    CHECK(preserved.fingerprint() == original_fingerprint);

    const std::array<std::string_view, 1> unpaired_paths{data_name};
    CHECK(load_xml_schema(unpaired_paths, preserved, diagnostic).code == ErrorCode::invalid_value);
    CHECK(diagnostic.message.find("LENGTH") != std::string::npos);

    XmlSchemaLimits tiny{};
    tiny.max_input_file_bytes = 8;
    CHECK(load_xml_schema(complete_paths, preserved, diagnostic, tiny).code == ErrorCode::capacity_exceeded);
    CHECK(diagnostic.source == complete_name);
    CHECK(preserved.fingerprint() == original_fingerprint);

    std::filesystem::remove_all(directory, ignored);
    return failures == 0 ? 0 : 1;
}
