#include <nimblefix/xml_schema.hpp>

#include <array>
#include <string_view>

int main(int argc, char** argv) {
    if (argc != 2) return 1;
    const std::string_view inputs[]{argv[1]};
    nimblefix::XmlSchema schema;
    nimblefix::XmlSchemaDiagnostic diagnostic;
    const auto status = nimblefix::load_xml_schema(inputs, schema, diagnostic);
    if (!status.ok() || !schema.valid()) return 2;
    const auto* message = schema.find_message("Z");
    const auto* field = schema.descriptor().find_field(900001);
    if (message == nullptr || message->name != "CustomOrder" ||
        field == nullptr || field->name != "ExternalID" || schema.fingerprint().empty()) return 3;
    return 0;
}
