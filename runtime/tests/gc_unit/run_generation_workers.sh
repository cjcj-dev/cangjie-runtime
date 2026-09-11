#!/usr/bin/env bash
# Product-linked worker core tests. No product source is compiled into the ELF.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SRC="$ROOT/runtime/tests/gc_unit"
OUT="${GC_WORKERS_OUT:?set GC_WORKERS_OUT}"
LIB="${GCV2_RUNTIME_LIB_DIR:?set GCV2_RUNTIME_LIB_DIR to the product pair}"
mkdir -p "$OUT"
HEADERS=$(python3 "$ROOT/runtime/build/resolve_runtime_headers.py" "$ROOT/runtime" "$LIB")
"${CXX:-clang++}" -std=gnu++17 -O0 -g -Wall -Wextra -pthread -fno-rtti \
  -fvisibility-inlines-hidden \
  -I"$SRC" -I"$ROOT/runtime/src" -I"$ROOT/runtime/src/Heap" \
  -I"$ROOT/runtime/src/CJThread/src/runtime/schedule/include" \
  -I"$ROOT/runtime/include" -I"$HEADERS/include" \
  -I"$ROOT/runtime/third_party/third_party_bounds_checking_function/include" \
  "$SRC/test_generation_workers.cpp" \
  -L"$LIB" -Wl,-rpath,"$LIB" -Wl,--exclude-libs,ALL \
  -lcangjie-runtime -lboundscheck -ldl -o "$OUT/generation_workers"
sha256sum "$OUT/generation_workers" "$LIB/libcangjie-runtime.so" "$LIB/libboundscheck.so"
LD_LIBRARY_PATH="$LIB${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" "$OUT/generation_workers"
