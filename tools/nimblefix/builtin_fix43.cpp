#include "builtin_schemas.hpp"

#if defined(NIMBLEFIX_BUILTIN_STANDARD_SCHEMAS)
#include <nimblefix/fix43.hpp>

namespace nimblefix::cli_builtin {

BuiltinSchemaEntry builtin_fix43() noexcept {
    return {&fix43::Schema::descriptor(), fix43::Schema::wire_hints()};
}

} // namespace nimblefix::cli_builtin
#endif
