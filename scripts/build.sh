#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR="${BUILD_DIR:-build}"
BUILD_TYPE="${BUILD_TYPE:-Release}"

cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
cmake --build "$BUILD_DIR" -j"$(nproc)"

echo "build OK"
echo "  server:    $BUILD_DIR/mycache-server"
echo "  tests:     $BUILD_DIR/mycache_tests"
echo "  benchmark: $BUILD_DIR/mycache_bench"
