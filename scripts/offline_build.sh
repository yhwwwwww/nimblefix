#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PRESET="dev-release"
BENCH_MODE="smoke"

usage() {
    cat <<'EOF'
usage: ./scripts/offline_build.sh [--preset <name>] [--bench skip|smoke|full]

presets:
  dev-release  Developer default CMake build
  rhel8-gcc12  RHEL 8.10 host with GCC 12 already enabled in the shell
  rhel9-gcc14  RHEL 9.7 host with GCC 14 already enabled in the shell

bench modes:
  skip   Configure, build, and run tests only
  smoke  Run reduced FastFix + QuickFIX benchmark passes after tests
  full   Run the full benchmark compare suite after tests
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
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

case "$BENCH_MODE" in
    skip|smoke|full)
        ;;
    *)
        usage
        exit 1
        ;;
esac

cd "$ROOT_DIR"

cmake --preset "$PRESET"
cmake --build --preset "$PRESET"
ctest --preset "$PRESET"

if [ "$BENCH_MODE" = "skip" ]; then
    exit 0
fi

export FASTFIX_BUILD_SYSTEM=cmake
export FASTFIX_CMAKE_PRESET="$PRESET"

if [ "$BENCH_MODE" = "smoke" ]; then
    ./bench/bench.sh fastfix --iterations 5000 --loopback 100 --replay 100
    ./bench/bench.sh quickfix --iterations 5000 --replay 100 --replay-span 32 --loopback 100
    exit 0
fi

./bench/bench.sh compare