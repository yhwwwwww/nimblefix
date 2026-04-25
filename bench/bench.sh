#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
BENCH_DIR="$BUILD_DIR/bench"
env_or_default() {
    local default_value="$1"
    shift

    local variable_name
    for variable_name in "$@"; do
        if [ -n "${!variable_name:-}" ]; then
            printf '%s\n' "${!variable_name}"
            return
        fi
    done

    printf '%s\n' "$default_value"
}

BUILD_SYSTEM="$(env_or_default auto NIMBLEFIX_BUILD_SYSTEM)"
XMAKE_MODE="$(env_or_default release NIMBLEFIX_XMAKE_MODE)"
XMAKE_CCACHE="$(env_or_default n NIMBLEFIX_XMAKE_CCACHE)"
XMAKE_BIN_DIR="$(env_or_default "$ROOT_DIR/build/linux/x86_64/$XMAKE_MODE" NIMBLEFIX_XMAKE_BIN_DIR)"
CMAKE_PRESET="$(env_or_default dev-release NIMBLEFIX_CMAKE_PRESET)"
CMAKE_GENERATOR_KIND="$(env_or_default auto NIMBLEFIX_CMAKE_GENERATOR)"
CMAKE_BUILD_DIR=""
CMAKE_BIN_DIR=""
BIN_DIR=""
RESOLVED_BUILD_SYSTEM=""
RESOLVED_CMAKE_GENERATOR=""
RESOLVED_CMAKE_GENERATOR_KIND=""
RESOLVED_CMAKE_PRESET=""
DETECTED_XMAKE_VERSION=""

QUICKFIX_SRC_DIR="$ROOT_DIR/bench/vendor/quickfix"
QUICKFIX_BUILD_DIR="$BENCH_DIR/quickfix-build"

QUICKFIX_XML="$QUICKFIX_SRC_DIR/spec/FIX44.xml"
QUICKFIX_NFD="$BENCH_DIR/quickfix_FIX44.nfd"
QUICKFIX_NFA="$BENCH_DIR/quickfix_FIX44.nfa"
NIMBLEFIX_BENCH_BIN=""
XML2NFD_BIN=""
DICTGEN_BIN=""
QUICKFIX_BENCH_BIN=""
XMAKE_CONFIGURED=0

usage() {
    cat <<'EOF'
usage: ./bench/bench.sh <command> [args...]

commands:
  build         Build all benchmark binaries and benchmark input artifacts
    nimblefix     Run NimbleFIX benchmark against build/bench/quickfix_FIX44.nfa
    nimblefix-nfd Run NimbleFIX benchmark against build/bench/quickfix_FIX44.nfd
  quickfix      Run QuickFIX comparison benchmark (parse, encode, session-inbound, replay, loopback)
  builder       Run the FIX44 encode-focused NimbleFIX benchmark variant
  compare       Run the default NimbleFIX and QuickFIX comparison suites side by side

environment overrides:
    NIMBLEFIX_BUILD_SYSTEM=auto|xmake|cmake  Preferred build system selector
    NIMBLEFIX_XMAKE_MODE=<mode>              Preferred xmake build mode override
    NIMBLEFIX_XMAKE_CCACHE=y|n               Preferred xmake compiler cache toggle
    NIMBLEFIX_XMAKE_BIN_DIR=<path>           Preferred xmake binary directory override
    NIMBLEFIX_CMAKE_GENERATOR=auto|ninja|make
                                            Preferred CMake generator selector
    NIMBLEFIX_CMAKE_PRESET=<name>            Preferred CMake preset override
EOF
}

command_exists() {
    command -v "$1" >/dev/null 2>&1
}

run_xmake() {
    env -u CC -u CXX xmake "$@"
}

detect_xmake_version() {
    if [ -n "$DETECTED_XMAKE_VERSION" ]; then
        printf '%s\n' "$DETECTED_XMAKE_VERSION"
        return
    fi

    DETECTED_XMAKE_VERSION="$(xmake --version 2>/dev/null | sed -n '1s/.*v\([0-9][0-9.]*\).*/\1/p')"
    printf '%s\n' "$DETECTED_XMAKE_VERSION"
}

version_at_least() {
    local current="$1"
    local minimum="$2"
    local current_major current_minor current_patch
    local minimum_major minimum_minor minimum_patch

    IFS=. read -r current_major current_minor current_patch <<EOF
$current
EOF
    IFS=. read -r minimum_major minimum_minor minimum_patch <<EOF
$minimum
EOF

    current_major="${current_major:-0}"
    current_minor="${current_minor:-0}"
    current_patch="${current_patch:-0}"
    minimum_major="${minimum_major:-0}"
    minimum_minor="${minimum_minor:-0}"
    minimum_patch="${minimum_patch:-0}"

    if [ "$current_major" -gt "$minimum_major" ]; then
        return 0
    fi
    if [ "$current_major" -lt "$minimum_major" ]; then
        return 1
    fi
    if [ "$current_minor" -gt "$minimum_minor" ]; then
        return 0
    fi
    if [ "$current_minor" -lt "$minimum_minor" ]; then
        return 1
    fi
    [ "$current_patch" -ge "$minimum_patch" ]
}

xmake_is_supported() {
    local version
    version="$(detect_xmake_version)"
    [ -n "$version" ] && version_at_least "$version" "3.0.0"
}

validate_xmake_ccache() {
    case "$XMAKE_CCACHE" in
        y|n)
            ;;
        *)
            echo "error: NIMBLEFIX_XMAKE_CCACHE must be 'y' or 'n' (got '$XMAKE_CCACHE')" >&2
            exit 1
            ;;
    esac
}

ensure_submodules_ready() {
    local required_files=(
        "deps/src/Catch2/extras/catch_amalgamated.cpp"
        "deps/src/pugixml/src/pugixml.cpp"
        "bench/vendor/quickfix/spec/FIX44.xml"
    )
    local missing_files=()
    local relative_path

    for relative_path in "${required_files[@]}"; do
        if [ ! -f "$ROOT_DIR/$relative_path" ]; then
            missing_files+=("$relative_path")
        fi
    done

    if [ "${#missing_files[@]}" -eq 0 ]; then
        return
    fi

    if [ ! -f "$ROOT_DIR/.gitmodules" ]; then
        echo "error: missing vendored dependency files:" >&2
        printf '  %s\n' "${missing_files[@]}" >&2
        exit 1
    fi

    if ! command_exists git; then
        echo "error: git is required to initialize repository submodules" >&2
        printf 'missing: %s\n' "${missing_files[*]}" >&2
        exit 1
    fi

    if ! git -C "$ROOT_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        echo "error: missing vendored dependency files:" >&2
        printf '  %s\n' "${missing_files[@]}" >&2
        echo "hint: source archive is incomplete; populate submodules before building" >&2
        exit 1
    fi

    echo "info: initializing git submodules" >&2
    git -C "$ROOT_DIR" submodule sync --recursive
    git -C "$ROOT_DIR" submodule update --init --recursive

    for relative_path in "${missing_files[@]}"; do
        if [ ! -f "$ROOT_DIR/$relative_path" ]; then
            echo "error: submodule initialization did not provide $relative_path" >&2
            exit 1
        fi
    done
}

cmake_preset_for_generator() {
    local preset_name="$1"
    local generator_kind="$2"

    if [ "$generator_kind" = "make" ]; then
        case "$preset_name" in
            *-make)
                printf '%s\n' "$preset_name"
                ;;
            *)
                printf '%s-make\n' "$preset_name"
                ;;
        esac
        return
    fi

    case "$preset_name" in
        *-make)
            printf '%s\n' "${preset_name%-make}"
            ;;
        *)
            printf '%s\n' "$preset_name"
            ;;
    esac
}

has_flag() {
    local needle="$1"
    shift
    for arg in "$@"; do
        if [ "$arg" = "$needle" ]; then
            return 0
        fi
    done
    return 1
}

resolve_build_system() {
    if [ -n "$RESOLVED_BUILD_SYSTEM" ]; then
        return
    fi

    case "$BUILD_SYSTEM" in
        auto)
            if command_exists xmake; then
                if xmake_is_supported; then
                    RESOLVED_BUILD_SYSTEM="xmake"
                elif command_exists cmake; then
                    echo "info: xmake $(detect_xmake_version) is too old for this project; falling back to cmake" >&2
                    RESOLVED_BUILD_SYSTEM="cmake"
                else
                    echo "error: xmake $(detect_xmake_version) is too old for this project and cmake is not available" >&2
                    exit 1
                fi
            elif command_exists cmake; then
                RESOLVED_BUILD_SYSTEM="cmake"
            else
                echo "error: neither xmake nor cmake is available" >&2
                exit 1
            fi
            ;;
        xmake)
            if ! command_exists xmake; then
                echo "error: xmake is not available" >&2
                echo "hint: install xmake or rerun with NIMBLEFIX_BUILD_SYSTEM=cmake" >&2
                exit 1
            fi
            if ! xmake_is_supported; then
                echo "error: xmake $(detect_xmake_version) is too old for this project; install xmake >= 3.0.0 or rerun with NIMBLEFIX_BUILD_SYSTEM=cmake" >&2
                exit 1
            fi
            RESOLVED_BUILD_SYSTEM="xmake"
            ;;
        cmake)
            if ! command_exists cmake; then
                echo "error: cmake is not available" >&2
                exit 1
            fi
            RESOLVED_BUILD_SYSTEM="cmake"
            ;;
        *)
            echo "error: unsupported build system '$BUILD_SYSTEM'" >&2
            exit 1
            ;;
    esac
}

resolve_cmake_generator() {
    if [ -n "$RESOLVED_CMAKE_GENERATOR" ]; then
        return
    fi

    if ! command_exists cmake; then
        echo "error: cmake is not available" >&2
        exit 1
    fi

    case "$CMAKE_GENERATOR_KIND" in
        auto)
            if command_exists ninja; then
                RESOLVED_CMAKE_GENERATOR_KIND="ninja"
                RESOLVED_CMAKE_GENERATOR="Ninja"
            elif command_exists make; then
                RESOLVED_CMAKE_GENERATOR_KIND="make"
                RESOLVED_CMAKE_GENERATOR="Unix Makefiles"
            else
                echo "error: cmake is available but neither ninja nor make is installed" >&2
                exit 1
            fi
            ;;
        ninja)
            if ! command_exists ninja; then
                echo "error: ninja is not available" >&2
                exit 1
            fi
            RESOLVED_CMAKE_GENERATOR_KIND="ninja"
            RESOLVED_CMAKE_GENERATOR="Ninja"
            ;;
        make)
            if ! command_exists make; then
                echo "error: make is not available" >&2
                exit 1
            fi
            RESOLVED_CMAKE_GENERATOR_KIND="make"
            RESOLVED_CMAKE_GENERATOR="Unix Makefiles"
            ;;
        *)
            echo "error: unsupported CMake generator '$CMAKE_GENERATOR_KIND'" >&2
            exit 1
            ;;
    esac

    RESOLVED_CMAKE_PRESET="$(cmake_preset_for_generator "$CMAKE_PRESET" "$RESOLVED_CMAKE_GENERATOR_KIND")"
    CMAKE_BUILD_DIR="$ROOT_DIR/build/cmake/$RESOLVED_CMAKE_PRESET"
    CMAKE_BIN_DIR="$CMAKE_BUILD_DIR/bin"
}

resolve_paths() {
    if [ -n "$BIN_DIR" ]; then
        return
    fi

    resolve_build_system
    if [ "$RESOLVED_BUILD_SYSTEM" = "xmake" ]; then
        BIN_DIR="$XMAKE_BIN_DIR"
    else
        resolve_cmake_generator
        BIN_DIR="$CMAKE_BIN_DIR"
    fi

    NIMBLEFIX_BENCH_BIN="$BIN_DIR/nimblefix-bench"
    XML2NFD_BIN="$BIN_DIR/nimblefix-xml2nfd"
    DICTGEN_BIN="$BIN_DIR/nimblefix-dictgen"
    QUICKFIX_BENCH_BIN="$BIN_DIR/quickfix-cpp-bench"
}

reset_incompatible_cmake_cache() {
    local cache_file="$CMAKE_BUILD_DIR/CMakeCache.txt"
    local cached_generator=""

    if [ ! -f "$cache_file" ]; then
        return
    fi

    cached_generator="$(sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' "$cache_file" | head -n 1)"
    if [ -z "$cached_generator" ] || [ "$cached_generator" = "$RESOLVED_CMAKE_GENERATOR" ]; then
        return
    fi

    echo "info: resetting stale CMake cache in $CMAKE_BUILD_DIR ($cached_generator -> $RESOLVED_CMAKE_GENERATOR)" >&2
    cmake -E rm -f "$CMAKE_BUILD_DIR/CMakeCache.txt" "$CMAKE_BUILD_DIR/CTestTestfile.cmake" "$CMAKE_BUILD_DIR/cmake_install.cmake" "$CMAKE_BUILD_DIR/Makefile" "$CMAKE_BUILD_DIR/build.ninja" "$CMAKE_BUILD_DIR/compile_commands.json"
    cmake -E rm -rf "$CMAKE_BUILD_DIR/CMakeFiles" "$CMAKE_BUILD_DIR/Testing"
}

fallback_to_cmake() {
    if ! command_exists cmake; then
        return 1
    fi

    echo "info: xmake auto path is unavailable in this environment; falling back to cmake" >&2
    RESOLVED_BUILD_SYSTEM="cmake"
    XMAKE_CONFIGURED=0
    BIN_DIR=""
    NIMBLEFIX_BENCH_BIN=""
    XML2NFD_BIN=""
    DICTGEN_BIN=""
    QUICKFIX_BENCH_BIN=""
    resolve_paths
    return 0
}

ensure_quickfix_source() {
    if [ ! -f "$QUICKFIX_XML" ]; then
        echo "error: missing QuickFIX submodule checkout at $QUICKFIX_XML" >&2
        echo "hint: run 'git submodule update --init --recursive'" >&2
        exit 1
    fi
}

configure_cmake() {
    resolve_paths
    reset_incompatible_cmake_cache
    if [ -f "$CMAKE_BUILD_DIR/CMakeCache.txt" ]; then
        return
    fi

    cmake --preset "$RESOLVED_CMAKE_PRESET"
}

configure_xmake() {
    if [ "$XMAKE_CONFIGURED" = "1" ]; then
        return 0
    fi

    validate_xmake_ccache
    local -a args=(f -c -m "$XMAKE_MODE" --ccache="$XMAKE_CCACHE" -y)

    if [ -n "${CC:-}" ]; then
        if command_exists "$CC"; then
            args+=("--cc=$CC")
        elif [ "$BUILD_SYSTEM" = "xmake" ]; then
            echo "error: CC compiler '$CC' is not available" >&2
            return 1
        else
            echo "info: ignoring unavailable CC='$CC' for xmake auto path" >&2
        fi
    fi
    if [ -n "${CXX:-}" ]; then
        if command_exists "$CXX"; then
            args+=("--cxx=$CXX")
        elif [ "$BUILD_SYSTEM" = "xmake" ]; then
            echo "error: CXX compiler '$CXX' is not available" >&2
            return 1
        else
            echo "info: ignoring unavailable CXX='$CXX' for xmake auto path" >&2
        fi
    fi

    if run_xmake "${args[@]}"; then
        XMAKE_CONFIGURED=1
        return 0
    fi

    XMAKE_CONFIGURED=0
    return 1
}

cmake_build() {
    configure_cmake
    cmake --build --preset "$RESOLVED_CMAKE_PRESET" --target "$@"
}

xmake_build() {
    if ! configure_xmake; then
        if [ "$BUILD_SYSTEM" = "auto" ] && fallback_to_cmake; then
            cmake_build "$@"
            return
        fi
        return 1
    fi

    for target_name in "$@"; do
        run_xmake build "$target_name"
    done
}

build_nimblefix_tools() {
    resolve_paths
    if [ "$RESOLVED_BUILD_SYSTEM" = "xmake" ]; then
        xmake_build nimblefix-bench nimblefix-xml2nfd nimblefix-dictgen
    else
        cmake_build nimblefix-bench nimblefix-xml2nfd nimblefix-dictgen
    fi
}

prepare_quickfix_dictionary() {
    ensure_quickfix_source
    resolve_paths
    if [ "$RESOLVED_BUILD_SYSTEM" = "xmake" ]; then
        xmake_build nimblefix-fix44-assets
    else
        cmake_build nimblefix-fix44-assets
    fi
}

build_quickfix_bench() {
    ensure_quickfix_source
    resolve_paths
    if [ "$RESOLVED_BUILD_SYSTEM" = "xmake" ]; then
        xmake_build nimblefix-quickfix-cpp-bench nimblefix-fix44-assets
    else
        cmake_build nimblefix-quickfix-cpp-bench nimblefix-fix44-assets
    fi
}

build_all() {
    resolve_paths
    build_nimblefix_tools
    prepare_quickfix_dictionary
    build_quickfix_bench
}

run_nimblefix_artifact() {
    resolve_paths
    build_nimblefix_tools
    prepare_quickfix_dictionary
    "$NIMBLEFIX_BENCH_BIN" --artifact "$QUICKFIX_NFA" "$@"
}

run_nimblefix_nfd() {
    resolve_paths
    build_nimblefix_tools
    prepare_quickfix_dictionary
    "$NIMBLEFIX_BENCH_BIN" --dictionary "$QUICKFIX_NFD" "$@"
}

run_builder() {
    resolve_paths
    build_nimblefix_tools
    prepare_quickfix_dictionary
    "$NIMBLEFIX_BENCH_BIN" --artifact "$QUICKFIX_NFA" "$@"
}

run_quickfix() {
    resolve_paths
    build_quickfix_bench
    "$QUICKFIX_BENCH_BIN" --xml "$QUICKFIX_XML" "$@"
}

if [ "$#" -lt 1 ]; then
    usage
    exit 1
fi

ensure_submodules_ready

command_name="$1"
shift

case "$command_name" in
    build)
        build_all
        ;;
    nimblefix)
        if [ "$#" -eq 0 ]; then
            set -- --iterations 100000 --loopback 1000 --replay 1000
        fi
        run_nimblefix_artifact "$@"
        ;;
    nimblefix-nfd)
        if [ "$#" -eq 0 ]; then
            set -- --iterations 30000 --loopback 200 --replay 200
        fi
        run_nimblefix_nfd "$@"
        ;;
    quickfix)
        if [ "$#" -eq 0 ]; then
            set -- --iterations 100000 --replay 1000 --replay-span 128 --loopback 1000
        fi
        run_quickfix "$@"
        ;;
    builder)
        builder_args=("$@")
        if ! has_flag "--iterations" "${builder_args[@]}"; then
            builder_args+=(--iterations 100000)
        fi
        if ! has_flag "--loopback" "${builder_args[@]}"; then
            builder_args+=(--loopback 0)
        fi
        if ! has_flag "--replay" "${builder_args[@]}"; then
            builder_args+=(--replay 0)
        fi
        run_builder "${builder_args[@]}"
        ;;
    compare)
        if [ "$#" -ne 0 ]; then
            usage
            exit 1
        fi
        run_nimblefix_artifact --iterations 100000 --loopback 1000 --replay 1000
        run_quickfix --iterations 100000 --replay 1000 --replay-span 128 --loopback 1000
        ;;
    *)
        usage
        exit 1
        ;;
esac