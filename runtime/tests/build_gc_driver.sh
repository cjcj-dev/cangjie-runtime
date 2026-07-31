#!/usr/bin/env bash
# Build gc_driver against a built libcangjie-runtime (same recipe as covverify/gcstress harnesses).
set -euo pipefail

ROOT="${1:?usage: build_gc_driver.sh <runtime-src-root> <lib-dir> <out-bin>}"
LIBDIR="${2:?}"
OUT="${3:?}"
SRC="$ROOT/tests/gc_driver.cpp"
INCLUDES=(
  -I"$ROOT/src"
  -I"$ROOT/src/Heap"
  -I"$ROOT/src/Common"
  -I"$ROOT/src/ObjectModel"
  -I"$ROOT/src/Mutator"
  -I"$ROOT/src/Base"
  -I"$ROOT/include"
  -I"$ROOT/src/Heap/Allocator"
  -I"$ROOT/src/Heap/Collector"
  -I"$ROOT/src/Heap/WCollector"
  -I"$ROOT/src/Heap/Barrier"
  -I"$ROOT/third_party/boundscheck-v1.1.16/include"
)

# Match runtime build defines used on linux x86_64 release.
DEFINES=(
  -DMRT_LINUX
  -DMRT_DEBUG_SYMBOLS_ENABLE=0
  -DCANGJIE_SANITIZER_SUPPORT=0
)

CXX="${CXX:-clang++}"
"$CXX" -std=gnu++17 -O2 -fPIC -pthread \
  "${DEFINES[@]}" "${INCLUDES[@]}" \
  "$SRC" \
  -L"$LIBDIR" -Wl,-rpath,"$LIBDIR" \
  -lcangjie-runtime -lboundscheck -ldl -lpthread \
  -o "$OUT"
echo "BUILT $OUT"
