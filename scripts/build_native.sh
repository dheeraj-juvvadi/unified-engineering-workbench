#!/usr/bin/env bash
# build_native.sh - build and test on Linux/macOS (build/ folder), then print how to run.
set -euo pipefail
cd "$(dirname "$0")/.."
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)"

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$JOBS"
ctest --test-dir build --output-on-failure

echo
echo "OK. Run the app with:  build/Launch        (add --no-browser to skip opening a browser)"
