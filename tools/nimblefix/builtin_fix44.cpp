#include "builtin_schemas.hpp"

#if defined(NIMBLEFIX_BUILTIN_STANDARD_SCHEMAS)
#include <nimblefix/fix44.hpp>

namespace nimblefix::cli_builtin {

BuiltinSchemaEntry builtin_fix44() noexcept {
    return {&fix44::Schema::descriptor(), fix44::Schema::wire_hints()};
}

} // namespace nimblefix::cli_builtin
#endif
