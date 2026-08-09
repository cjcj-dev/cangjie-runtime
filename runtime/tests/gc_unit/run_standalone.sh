#!/usr/bin/env bash
# Standalone build+run of GC unit tests without full runtime cmake.
# Usage: bash runtime/tests/gc_unit/run_standalone.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SRC="$ROOT/runtime/tests/gc_unit"
OUT="${GC_UNIT_OUT:-$ROOT/runtime/tests/gc_unit/build_standalone}"
mkdir -p "$OUT"
CXX="${CXX:-clang++}"
$CXX -std=gnu++17 -O0 -g -Wall -Wextra \
  -I"$SRC" -I"$ROOT/runtime/src" \
  "$SRC/gc_unit_main.cpp" \
  "$SRC/test_colour_address.cpp" \
  "$SRC/test_route_info.cpp" \
  "$SRC/test_live_map.cpp" \
  "$SRC/test_object_gate.cpp" \
  -o "$OUT/cj_gc_unit"
"$OUT/cj_gc_unit"
echo "GC_UNIT_OK rc=$?"
