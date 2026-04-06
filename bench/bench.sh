#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
BIN_DIR="$BUILD_DIR/linux/x86_64/release"
BENCH_DIR="$BUILD_DIR/bench"
QUICKFIX_SRC_DIR="$BUILD_DIR/_deps/quickfix-src"
QUICKFIX_BUILD_DIR="$BUILD_DIR/_deps/quickfix-build"

FASTFIX_BENCH_BIN="$BIN_DIR/fastfix-bench"
XML2FFD_BIN="$BIN_DIR/fastfix-xml2ffd"
DICTGEN_BIN="$BIN_DIR/fastfix-dictgen"
QUICKFIX_BENCH_BIN="$BENCH_DIR/quickfix-cpp-bench"

QUICKFIX_XML="$QUICKFIX_SRC_DIR/spec/FIX44.xml"
QUICKFIX_FFD="$BENCH_DIR/quickfix_FIX44.ffd"
QUICKFIX_ART="$BENCH_DIR/quickfix_FIX44.art"
SAMPLE_OVERLAY_ART="$BUILD_DIR/sample-basic-overlay.art"

usage() {
    cat <<'EOF'
usage: ./bench/bench.sh <command> [args...]

commands:
  build         Build all benchmark binaries and benchmark input artifacts
  fastfix       Run FastFix benchmark against build/bench/quickfix_FIX44.art
  fastfix-ffd   Run FastFix benchmark against build/bench/quickfix_FIX44.ffd
  quickfix      Run QuickFIX comparison benchmark
  builder       Run the sample overlay builder matrix benchmark
  compare       Run the default FastFix and QuickFIX comparison suites
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

ensure_quickfix_source() {
    if [ -f "$QUICKFIX_XML" ]; then
        return
    fi

    mkdir -p "$BUILD_DIR/_deps"
    git clone --depth 1 https://github.com/quickfix/quickfix.git "$QUICKFIX_SRC_DIR"
}

build_fastfix_tools() {
    xmake build fastfix-bench
    xmake build fastfix-xml2ffd
    xmake build fastfix-dictgen
}

prepare_quickfix_dictionary() {
    ensure_quickfix_source
    mkdir -p "$BENCH_DIR"

    "$XML2FFD_BIN" \
        --xml "$QUICKFIX_XML" \
        --output "$QUICKFIX_FFD" \
        --profile-id 4400

    "$DICTGEN_BIN" \
        --input "$QUICKFIX_FFD" \
        --output "$QUICKFIX_ART"
}

prepare_builder_dictionary() {
    "$DICTGEN_BIN" \
        --input "$ROOT_DIR/samples/basic_profile.ffd" \
        --merge "$ROOT_DIR/samples/basic_overlay.ffd" \
        --output "$SAMPLE_OVERLAY_ART"
}

build_quickfix_bench() {
    ensure_quickfix_source
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
    build_fastfix_tools
    prepare_quickfix_dictionary
    prepare_builder_dictionary
    build_quickfix_bench
}

run_fastfix_artifact() {
    build_fastfix_tools
    prepare_quickfix_dictionary
    "$FASTFIX_BENCH_BIN" --artifact "$QUICKFIX_ART" "$@"
}

run_fastfix_ffd() {
    build_fastfix_tools
    prepare_quickfix_dictionary
    "$FASTFIX_BENCH_BIN" --dictionary "$QUICKFIX_FFD" "$@"
}

run_builder() {
    build_fastfix_tools
    prepare_quickfix_dictionary
    "$FASTFIX_BENCH_BIN" --artifact "$QUICKFIX_ART" "$@"
}

run_quickfix() {
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
            set -- --iterations 100000
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
        run_quickfix --iterations 100000
        ;;
    *)
        usage
        exit 1
        ;;
esac