#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
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
PRESET="$(env_or_default dev-release NIMBLEFIX_CMAKE_PRESET)"
CMAKE_GENERATOR_KIND="$(env_or_default auto NIMBLEFIX_CMAKE_GENERATOR)"
BENCH_MODE="smoke"
XMAKE_BIN_DIR="$(env_or_default "$ROOT_DIR/build/linux/x86_64/$XMAKE_MODE" NIMBLEFIX_XMAKE_BIN_DIR)"
OFFICIAL_FIX_SESSION_MANIFEST="$ROOT_DIR/tests/data/fix-session/official-session-cases.ffcases"
RESOLVED_BUILD_SYSTEM=""
RESOLVED_CMAKE_GENERATOR=""
RESOLVED_CMAKE_GENERATOR_KIND=""
RESOLVED_CMAKE_PRESET=""
XMAKE_CONFIGURED=0
DETECTED_XMAKE_VERSION=""

usage() {
    cat <<'EOF'
usage: ./scripts/offline_build.sh [--build-system auto|xmake|cmake] [--mode debug|release] [--cmake-generator auto|ninja|make] [--preset <name>] [--bench skip|smoke|full]

environment overrides:
    NIMBLEFIX_BUILD_SYSTEM
    NIMBLEFIX_XMAKE_MODE
    NIMBLEFIX_XMAKE_CCACHE
    NIMBLEFIX_XMAKE_BIN_DIR
    NIMBLEFIX_CMAKE_GENERATOR
    NIMBLEFIX_CMAKE_PRESET

build systems:
    auto         Prefer xmake, then cmake + Ninja, then cmake + make
    xmake        Force the xmake path
    cmake        Force the CMake path

xmake modes:
    release      Default xmake build mode
    debug        Debug xmake build mode

cmake generators:
    auto         Prefer Ninja, then Unix Makefiles
    ninja        Force Ninja
    make         Force Unix Makefiles

presets:
    dev-release  Developer default CMake preset base name
    rhel8-gcc12  RHEL 8.10 host with GCC 12 already enabled in the shell
    rhel9-gcc14  RHEL 9.7 host with GCC 14 already enabled in the shell

bench modes:
    skip   Configure, build, and run tests only
    smoke  Run reduced NimbleFIX + QuickFIX benchmark passes after tests
    full   Run the full benchmark compare suite after tests
EOF
}

command_exists() {
    command -v "$1" >/dev/null 2>&1
}

run_xmake() {
    env -u CC -u CXX xmake "$@"
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
                exit 1
            fi
            if ! xmake_is_supported; then
                echo "error: xmake $(detect_xmake_version) is too old for this project; install xmake >= 3.0.0 or rerun with --build-system cmake" >&2
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
            usage
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
            usage
            exit 1
            ;;
    esac

    RESOLVED_CMAKE_PRESET="$(cmake_preset_for_generator "$PRESET" "$RESOLVED_CMAKE_GENERATOR_KIND")"
}

reset_incompatible_cmake_cache() {
    local build_dir="$ROOT_DIR/build/cmake/$RESOLVED_CMAKE_PRESET"
    local cache_file="$build_dir/CMakeCache.txt"
    local cached_generator=""

    if [ ! -f "$cache_file" ]; then
        return
    fi

    cached_generator="$(sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' "$cache_file" | head -n 1)"
    if [ -z "$cached_generator" ] || [ "$cached_generator" = "$RESOLVED_CMAKE_GENERATOR" ]; then
        return
    fi

    echo "info: resetting stale CMake cache in $build_dir ($cached_generator -> $RESOLVED_CMAKE_GENERATOR)" >&2
    cmake -E rm -f "$build_dir/CMakeCache.txt" "$build_dir/CTestTestfile.cmake" "$build_dir/cmake_install.cmake" "$build_dir/Makefile" "$build_dir/build.ninja" "$build_dir/compile_commands.json"
    cmake -E rm -rf "$build_dir/CMakeFiles" "$build_dir/Testing"
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

fallback_to_cmake() {
    if ! command_exists cmake; then
        return 1
    fi

    echo "info: xmake auto path is unavailable in this environment; falling back to cmake" >&2
    RESOLVED_BUILD_SYSTEM="cmake"
    XMAKE_CONFIGURED=0
    return 0
}

run_official_fix_session_gate() {
    local runner_path=""

    case "$RESOLVED_BUILD_SYSTEM" in
        xmake)
            run_xmake build nimblefix-fix-session-testcases
            runner_path="$XMAKE_BIN_DIR/nimblefix-fix-session-testcases"
            ;;
        cmake)
            runner_path="$ROOT_DIR/build/cmake/$RESOLVED_CMAKE_PRESET/bin/nimblefix-fix-session-testcases"
            ;;
        *)
            echo "error: official FIX session gate requires a resolved build system" >&2
            exit 1
            ;;
    esac

    if [ ! -x "$runner_path" ]; then
        echo "error: official FIX session runner is missing at $runner_path" >&2
        exit 1
    fi

    echo "info: running official FIX session testcase gate" >&2
    "$runner_path" --manifest "$OFFICIAL_FIX_SESSION_MANIFEST"
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --build-system)
            BUILD_SYSTEM="$2"
            shift 2
            ;;
        --mode)
            XMAKE_MODE="$2"
            shift 2
            ;;
        --cmake-generator)
            CMAKE_GENERATOR_KIND="$2"
            shift 2
            ;;
        --preset)
            PRESET="$2"
            shift 2
            ;;
        --bench)
            BENCH_MODE="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage
            exit 1
            ;;
    esac
done

XMAKE_BIN_DIR="$(env_or_default "$ROOT_DIR/build/linux/x86_64/$XMAKE_MODE" NIMBLEFIX_XMAKE_BIN_DIR)"

case "$BUILD_SYSTEM" in
    auto|xmake|cmake)
        ;;
    *)
        usage
        exit 1
        ;;
esac

case "$XMAKE_MODE" in
    debug|release)
        ;;
    *)
        usage
        exit 1
        ;;
esac

case "$CMAKE_GENERATOR_KIND" in
    auto|ninja|make)
        ;;
    *)
        usage
        exit 1
        ;;
esac

case "$BENCH_MODE" in
    skip|smoke|full)
        ;;
    *)
        usage
        exit 1
        ;;
esac

cd "$ROOT_DIR"

ensure_submodules_ready

resolve_build_system

if [ "$RESOLVED_BUILD_SYSTEM" = "xmake" ]; then
    if configure_xmake; then
        echo "info: using xmake ($XMAKE_MODE)" >&2
        run_xmake build nimblefix-tests
        "$XMAKE_BIN_DIR/nimblefix-tests"
        run_official_fix_session_gate
    elif [ "$BUILD_SYSTEM" = "auto" ] && fallback_to_cmake; then
        :
    else
        exit 1
    fi
elif [ "$RESOLVED_BUILD_SYSTEM" = "cmake" ]; then
    resolve_cmake_generator
    reset_incompatible_cmake_cache
    echo "info: using cmake + $RESOLVED_CMAKE_GENERATOR (preset $RESOLVED_CMAKE_PRESET)" >&2
    cmake --preset "$RESOLVED_CMAKE_PRESET"
    cmake --build --preset "$RESOLVED_CMAKE_PRESET"
    ctest --preset "$RESOLVED_CMAKE_PRESET"
    run_official_fix_session_gate
else
    echo "error: unresolved build system '$RESOLVED_BUILD_SYSTEM'" >&2
    exit 1
fi

if [ "$BENCH_MODE" = "skip" ]; then
    exit 0
fi

export NIMBLEFIX_BUILD_SYSTEM="$RESOLVED_BUILD_SYSTEM"

if [ "$RESOLVED_BUILD_SYSTEM" = "xmake" ]; then
    export NIMBLEFIX_XMAKE_MODE="$XMAKE_MODE"
    export NIMBLEFIX_XMAKE_CCACHE="$XMAKE_CCACHE"
    export NIMBLEFIX_XMAKE_BIN_DIR="$XMAKE_BIN_DIR"
else
    export NIMBLEFIX_CMAKE_GENERATOR="$RESOLVED_CMAKE_GENERATOR_KIND"
    export NIMBLEFIX_CMAKE_PRESET="$RESOLVED_CMAKE_PRESET"
fi

if [ "$BENCH_MODE" = "smoke" ]; then
    ./bench/bench.sh nimblefix --iterations 5000 --loopback 100 --replay 100
    ./bench/bench.sh quickfix --iterations 5000 --replay 100 --replay-span 32 --loopback 100
    exit 0
fi

./bench/bench.sh compare