#!/usr/bin/env bash
# package.sh - cross-build Launch.exe (Windows x64, MinGW-w64) and assemble the app folder:
#   dist/<CANDIDATE_ID>_Workbench/{Launch.exe, web/, data/}
# Only builds and copies; no application logic here. The final ZIP (with src/, docs/ ...)
# is assembled separately.
#   CANDIDATE_ID=S1234567 scripts/package.sh
set -euo pipefail
cd "$(dirname "$0")/.."

ID="${CANDIDATE_ID:-CANDIDATE}"
OUT="dist/${ID}_Workbench"
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)"
STRIP="${WB_MINGW_PREFIX:-x86_64-w64-mingw32}-strip"

cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-win --target Launch -j"$JOBS"

rm -rf "$OUT"
mkdir -p "$OUT/web" "$OUT/data"
cp build-win/Launch.exe "$OUT/Launch.exe"
"$STRIP" "$OUT/Launch.exe"
cp -R src/frontend/. "$OUT/web/"
cp -R data/. "$OUT/data/"

echo "Packaged: $OUT"
ls -l "$OUT/Launch.exe"
