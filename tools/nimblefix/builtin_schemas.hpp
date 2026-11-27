#pragma once

#include <string_view>

#include <nimblefix/codec.hpp>
#include <nimblefix/schema.hpp>

namespace nimblefix::cli_builtin {

struct BuiltinSchemaEntry {
    const SchemaDescriptor* descriptor{};
    WireHints hints{};
};

[[nodiscard]] bool has_standard_schemas() noexcept;
[[nodiscard]] BuiltinSchemaEntry find_standard_schema(std::string_view name) noexcept;

#if defined(NIMBLEFIX_BUILTIN_STANDARD_SCHEMAS)
[[nodiscard]] BuiltinSchemaEntry builtin_fix42() noexcept;
[[nodiscard]] BuiltinSchemaEntry builtin_fix43() noexcept;
[[nodiscard]] BuiltinSchemaEntry builtin_fix44() noexcept;
[[nodiscard]] BuiltinSchemaEntry builtin_fix50sp2() noexcept;
#endif

} // namespace nimblefix::cli_builtin
