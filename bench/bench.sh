#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
BENCH_DIR="$BUILD_DIR/bench"
XMAKE_BIN_DIR="$BUILD_DIR/linux/x86_64/release"
CMAKE_PRESET="${FASTFIX_CMAKE_PRESET:-dev-release}"
CMAKE_BUILD_DIR="${FASTFIX_CMAKE_BUILD_DIR:-$ROOT_DIR/build/cmake/$CMAKE_PRESET}"
CMAKE_BIN_DIR="$CMAKE_BUILD_DIR/bin"

QUICKFIX_SRC_DIR="$ROOT_DIR/bench/vendor/quickfix"
QUICKFIX_BUILD_DIR="$BENCH_DIR/quickfix-build"

QUICKFIX_XML="$QUICKFIX_SRC_DIR/spec/FIX44.xml"
QUICKFIX_FFD="$BENCH_DIR/quickfix_FIX44.ffd"
QUICKFIX_ART="$BENCH_DIR/quickfix_FIX44.art"

build_system="${FASTFIX_BUILD_SYSTEM:-auto}"
FASTFIX_BENCH_BIN=""
XML2FFD_BIN=""
DICTGEN_BIN=""
QUICKFIX_BENCH_BIN=""

usage() {
    cat <<'EOF'
usage: ./bench/bench.sh <command> [args...]

commands:
  build         Build all benchmark binaries and benchmark input artifacts
  fastfix       Run FastFix benchmark against build/bench/quickfix_FIX44.art
  fastfix-ffd   Run FastFix benchmark against build/bench/quickfix_FIX44.ffd
  quickfix      Run QuickFIX comparison benchmark (parse, encode, session-inbound, replay, loopback)
    builder       Run the FIX44 encode-focused FastFix benchmark variant
  compare       Run the default FastFix and QuickFIX comparison suites side by side

environment overrides:
    FASTFIX_BUILD_SYSTEM=cmake|xmake   Select the build system (default: auto, prefers cmake)
    FASTFIX_CMAKE_PRESET=<name>        CMake preset used when FASTFIX_BUILD_SYSTEM=cmake
    FASTFIX_CMAKE_BUILD_DIR=<path>     Explicit CMake build directory override
EOF
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
    if [ "$build_system" = "auto" ]; then
        if command -v cmake >/dev/null 2>&1; then
            build_system="cmake"
        elif command -v xmake >/dev/null 2>&1; then
            build_system="xmake"
        else
            echo "error: neither cmake nor xmake is available" >&2
            exit 1
        fi
    fi

    case "$build_system" in
        cmake|xmake)
            ;;
        *)
            echo "error: unsupported FASTFIX_BUILD_SYSTEM '$build_system'" >&2
            exit 1
            ;;
    esac

    if [ "$build_system" = "cmake" ]; then
        FASTFIX_BENCH_BIN="$CMAKE_BIN_DIR/fastfix-bench"
        XML2FFD_BIN="$CMAKE_BIN_DIR/fastfix-xml2ffd"
        DICTGEN_BIN="$CMAKE_BIN_DIR/fastfix-dictgen"
        QUICKFIX_BENCH_BIN="$CMAKE_BIN_DIR/quickfix-cpp-bench"
    else
        FASTFIX_BENCH_BIN="$XMAKE_BIN_DIR/fastfix-bench"
        XML2FFD_BIN="$XMAKE_BIN_DIR/fastfix-xml2ffd"
        DICTGEN_BIN="$XMAKE_BIN_DIR/fastfix-dictgen"
        QUICKFIX_BENCH_BIN="$BENCH_DIR/quickfix-cpp-bench"
    fi
}

ensure_quickfix_source() {
    if [ ! -f "$QUICKFIX_XML" ]; then
        echo "error: missing QuickFIX submodule checkout at $QUICKFIX_XML" >&2
        echo "hint: run 'git submodule update --init --recursive'" >&2
        exit 1
    fi
}

configure_cmake() {
    if [ -f "$CMAKE_BUILD_DIR/CMakeCache.txt" ]; then
        return
    fi

    if [ -n "${FASTFIX_CMAKE_PRESET:-}" ]; then
        cmake --preset "$CMAKE_PRESET"
    else
        cmake -S "$ROOT_DIR" -B "$CMAKE_BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
    fi
}

cmake_build() {
    configure_cmake

    if [ -n "${FASTFIX_CMAKE_PRESET:-}" ]; then
        cmake --build --preset "$CMAKE_PRESET" --target "$@"
    else
        cmake --build "$CMAKE_BUILD_DIR" --target "$@"
    fi
}

build_fastfix_tools() {
    if [ "$build_system" = "cmake" ]; then
        cmake_build fastfix-bench fastfix-xml2ffd fastfix-dictgen
    else
        xmake build fastfix-bench
        xmake build fastfix-xml2ffd
        xmake build fastfix-dictgen
    fi
}

prepare_quickfix_dictionary() {
    ensure_quickfix_source

    if [ "$build_system" = "cmake" ]; then
        cmake_build fastfix-fix44-assets
        return
    fi

    mkdir -p "$BENCH_DIR"

    "$XML2FFD_BIN" \
        --xml "$QUICKFIX_XML" \
        --output "$QUICKFIX_FFD" \
        --profile-id 4400

    "$DICTGEN_BIN" \
        --input "$QUICKFIX_FFD" \
        --output "$QUICKFIX_ART"
}

build_quickfix_bench() {
    ensure_quickfix_source

    if [ "$build_system" = "cmake" ]; then
        cmake_build fastfix-quickfix-cpp-bench fastfix-fix44-assets
        return
    fi

    mkdir -p "$BENCH_DIR"

    cmake -S "$QUICKFIX_SRC_DIR" -B "$QUICKFIX_BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DQUICKFIX_SHARED_LIBS=OFF \
        -DQUICKFIX_EXAMPLES=OFF \
        -DQUICKFIX_TESTS=OFF
    cmake --build "$QUICKFIX_BUILD_DIR" -j

    g++ -O2 -DNDEBUG -std=c++20 \
        -I"$QUICKFIX_SRC_DIR/include/quickfix" \
        -I"$QUICKFIX_SRC_DIR/src/C++" \
        -I"$ROOT_DIR/bench" \
        "$ROOT_DIR/bench/quickfix_main.cpp" \
        "$QUICKFIX_BUILD_DIR/src/C++/libquickfix.a" \
        -pthread \
        -o "$QUICKFIX_BENCH_BIN"
}

build_all() {
    resolve_build_system
    build_fastfix_tools
    prepare_quickfix_dictionary
    build_quickfix_bench
}

run_fastfix_artifact() {
    resolve_build_system
    build_fastfix_tools
    prepare_quickfix_dictionary
    "$FASTFIX_BENCH_BIN" --artifact "$QUICKFIX_ART" "$@"
}

run_fastfix_ffd() {
    resolve_build_system
    build_fastfix_tools
    prepare_quickfix_dictionary
    "$FASTFIX_BENCH_BIN" --dictionary "$QUICKFIX_FFD" "$@"
}

run_builder() {
    resolve_build_system
    build_fastfix_tools
    prepare_quickfix_dictionary
    "$FASTFIX_BENCH_BIN" --artifact "$QUICKFIX_ART" "$@"
}

run_quickfix() {
    resolve_build_system
    build_quickfix_bench
    "$QUICKFIX_BENCH_BIN" --xml "$QUICKFIX_XML" "$@"
}

if [ "$#" -lt 1 ]; then
    usage
    exit 1
fi

command_name="$1"
shift

case "$command_name" in
    build)
        build_all
        ;;
    fastfix)
        if [ "$#" -eq 0 ]; then
            set -- --iterations 100000 --loopback 1000 --replay 1000
        fi
        run_fastfix_artifact "$@"
        ;;
    fastfix-ffd)
        if [ "$#" -eq 0 ]; then
            set -- --iterations 30000 --loopback 200 --replay 200
        fi
        run_fastfix_ffd "$@"
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
        run_fastfix_artifact --iterations 100000 --loopback 1000 --replay 1000
        run_quickfix --iterations 100000 --replay 1000 --replay-span 128 --loopback 1000
        ;;
    *)
        usage
        exit 1
        ;;
esac