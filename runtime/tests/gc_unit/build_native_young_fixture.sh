#!/usr/bin/env bash
# Build the debugger-only young fixture against the same product SO as gc_unit.
set -euo pipefail
src=$(cd "$(dirname "$0")" && pwd)
rt=$(cd "$src/../.." && pwd)
lib=$(realpath "$1")
out=$2
mkdir -p "$out"
start=$SECONDS
clang++ -std=gnu++17 -O0 -g -Wall -Wextra -pthread -fno-rtti -fvisibility-inlines-hidden \
  -DMRT_TESTABLE_INTERNALS=1 -DMRT_PRODUCT_TESTABLE_INTERNALS=1 \
  -I"$src" -I"$rt/src" -I"$rt/src/Loader/BinaryFile" -I"$rt/src/Heap" \
  -I"$rt/src/Heap/z/os/linux" -I"$rt/src/CJThread/src/runtime/schedule/include" \
  -I"$rt/include" -I"$rt/third_party/third_party_bounds_checking_function/include" \
  -I"$lib/../../include" "$src/native_young_fixture.cpp" \
  -L"$lib" -Wl,-rpath,"$lib" -Wl,--exclude-libs,ALL \
  -lcangjie-runtime -lboundscheck -o "$out/cj_gc_native_young_fixture"
sha256sum "$out/cj_gc_native_young_fixture" "$lib/libcangjie-runtime.so" "$lib/libboundscheck.so" > "$out/young-artifacts.sha256"
echo "wall=$((SECONDS-start))"
