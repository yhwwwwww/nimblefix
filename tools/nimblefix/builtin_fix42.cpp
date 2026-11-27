#include "builtin_schemas.hpp"

#if defined(NIMBLEFIX_BUILTIN_STANDARD_SCHEMAS)
#include <nimblefix/fix42.hpp>
#endif

namespace nimblefix::cli_builtin {

#if defined(NIMBLEFIX_BUILTIN_STANDARD_SCHEMAS)
BuiltinSchemaEntry builtin_fix42() noexcept {
    return {&fix42::Schema::descriptor(), fix42::Schema::wire_hints()};
}
#endif

bool has_standard_schemas() noexcept {
#if defined(NIMBLEFIX_BUILTIN_STANDARD_SCHEMAS)
    return true;
#else
    return false;
#endif
}

BuiltinSchemaEntry find_standard_schema(std::string_view name) noexcept {
#if defined(NIMBLEFIX_BUILTIN_STANDARD_SCHEMAS)
    if (name == "fix42") return builtin_fix42();
    if (name == "fix43") return builtin_fix43();
    if (name == "fix44") return builtin_fix44();
    if (name == "fix50sp2") return builtin_fix50sp2();
#else
    (void)name;
#endif
    return {};
}

} // namespace nimblefix::cli_builtin
