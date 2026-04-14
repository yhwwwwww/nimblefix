#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_SYSTEM="${FASTFIX_BUILD_SYSTEM:-auto}"
XMAKE_MODE="${FASTFIX_XMAKE_MODE:-release}"
PRESET="${FASTFIX_CMAKE_PRESET:-dev-release}"
CMAKE_GENERATOR_KIND="${FASTFIX_CMAKE_GENERATOR:-auto}"
BENCH_MODE="smoke"
XMAKE_BIN_DIR="${FASTFIX_XMAKE_BIN_DIR:-$ROOT_DIR/build/linux/x86_64/$XMAKE_MODE}"
RESOLVED_BUILD_SYSTEM=""
RESOLVED_CMAKE_GENERATOR=""
RESOLVED_CMAKE_GENERATOR_KIND=""
RESOLVED_CMAKE_PRESET=""

usage() {
    cat <<'EOF'
usage: ./scripts/offline_build.sh [--build-system auto|xmake|cmake] [--mode debug|release] [--cmake-generator auto|ninja|make] [--preset <name>] [--bench skip|smoke|full]

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
    smoke  Run reduced FastFix + QuickFIX benchmark passes after tests
    full   Run the full benchmark compare suite after tests
EOF
}

command_exists() {
    command -v "$1" >/dev/null 2>&1
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
                RESOLVED_BUILD_SYSTEM="xmake"
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

XMAKE_BIN_DIR="${FASTFIX_XMAKE_BIN_DIR:-$ROOT_DIR/build/linux/x86_64/$XMAKE_MODE}"

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

resolve_build_system

if [ "$RESOLVED_BUILD_SYSTEM" = "xmake" ]; then
    echo "info: using xmake ($XMAKE_MODE)" >&2
    xmake f -m "$XMAKE_MODE" -y
    xmake build fastfix-tests
    "$XMAKE_BIN_DIR/fastfix-tests"
else
    resolve_cmake_generator
    reset_incompatible_cmake_cache
    echo "info: using cmake + $RESOLVED_CMAKE_GENERATOR (preset $RESOLVED_CMAKE_PRESET)" >&2
    cmake --preset "$RESOLVED_CMAKE_PRESET"
    cmake --build --preset "$RESOLVED_CMAKE_PRESET"
    ctest --preset "$RESOLVED_CMAKE_PRESET"
fi

if [ "$BENCH_MODE" = "skip" ]; then
    exit 0
fi

export FASTFIX_BUILD_SYSTEM="$RESOLVED_BUILD_SYSTEM"

if [ "$RESOLVED_BUILD_SYSTEM" = "xmake" ]; then
    export FASTFIX_XMAKE_MODE="$XMAKE_MODE"
    export FASTFIX_XMAKE_BIN_DIR="$XMAKE_BIN_DIR"
else
    export FASTFIX_CMAKE_GENERATOR="$RESOLVED_CMAKE_GENERATOR_KIND"
    export FASTFIX_CMAKE_PRESET="$RESOLVED_CMAKE_PRESET"
fi

if [ "$BENCH_MODE" = "smoke" ]; then
    ./bench/bench.sh fastfix --iterations 5000 --loopback 100 --replay 100
    ./bench/bench.sh quickfix --iterations 5000 --replay 100 --replay-span 32 --loopback 100
    exit 0
fi

./bench/bench.sh compare