# Dependency Submodules

This repository pins the required third-party sources as Git submodules so both CMake and xmake builds can run without network access after initialization.

Run `git submodule update --init --recursive` after cloning.

- `src/Catch2`: Catch2 `v3.13.0`
- `src/pugixml`: pugixml `v1.15`
- `../bench/vendor/quickfix`: QuickFIX commit `00dd20837c97578e725072e5514c8ffaa0e141d4`

The benchmark dictionary source is the pinned `bench/vendor/quickfix/spec/FIX44.xml`.