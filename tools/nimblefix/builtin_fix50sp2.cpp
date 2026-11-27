#include "builtin_schemas.hpp"

#if defined(NIMBLEFIX_BUILTIN_STANDARD_SCHEMAS)
#include <nimblefix/fix50sp2.hpp>

namespace nimblefix::cli_builtin {

BuiltinSchemaEntry builtin_fix50sp2() noexcept {
    return {&fix50sp2::Schema::descriptor(), fix50sp2::Schema::wire_hints()};
}

} // namespace nimblefix::cli_builtin
#endif
