# Third-party sources

Third-party repositories are pinned as git submodules so clones (`git clone --recurse-submodules` or `git submodule update --init --recursive`) work offline without manual downloads.

| Source | Version | Submodule / Files | License |
| --- | --- | --- | --- |
| [QuickFIX](https://github.com/quickfix/quickfix/tree/v1.16.0) | v1.16.0, `2ce8a60667d95a55cdc57a210f165e19cb757126` | `third_party/quickfix` (submodule) and standard XML dictionaries in `schemas/standard/` | [QuickFIX Software License 1.0](quickfix-LICENSE) |
| [toml++](https://github.com/marzer/tomlplusplus/tree/v3.4.0) | v3.4.0, `30172438cee64926dc41fdd9c11fb3ba5b2ba9de` | `third_party/tomlplusplus` (submodule, `toml.hpp`) | [MIT](tomlplusplus/LICENSE) |

This product includes software developed by quickfixengine.org
(http://www.quickfixengine.org/).

The XML definitions use the same schema generator as custom dictionaries.
The optional CLI/configuration component uses toml++; the core flat codec and
session library do not require it. The QuickFIX submodule is used only for
optional oracle interoperability tests and same-methodology comparison
benchmarks; NimbleFIX libraries themselves have no dependency on QuickFIX.
Optional TLS and runtime XML components use the system OpenSSL and libxml2
packages, respectively, with dependencies exported by their installed CMake
targets.
