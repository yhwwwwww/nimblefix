# Third-party sources

The files below are vendored for offline builds. Normal builds do not download
dependencies.

| Source | Version | Files | License |
| --- | --- | --- | --- |
| [QuickFIX](https://github.com/quickfix/quickfix/tree/v1.16.0) | v1.16.0, `2ce8a60667d95a55cdc57a210f165e19cb757126` | `schemas/standard/FIX42.xml`, `FIX43.xml`, `FIX44.xml`, `FIXT11.xml`, `FIX50SP2.xml` | [QuickFIX Software License 1.0](quickfix-LICENSE) |
| [toml++](https://github.com/marzer/tomlplusplus/tree/v3.4.0) | v3.4.0 | `tomlplusplus/toml.hpp` | [MIT](tomlplusplus/LICENSE) |

This product includes software developed by quickfixengine.org
(http://www.quickfixengine.org/).

The XML definitions use the same schema generator as custom dictionaries.
The optional CLI/configuration component uses toml++; the core flat codec and
session library do not require it. Optional TLS and runtime XML components
use the system OpenSSL and libxml2 packages, respectively, with dependencies
exported by their installed CMake targets.
