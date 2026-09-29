#!/bin/bash
ulimit -c 0
r=/root/sym_cangjie_runtime_1331_implement_r5895529148
python3 "$r/header-cuts.py"
for arm in cut-count cut-allocation; do
 (
  out=$r/unit-$arm; mkdir -p "$out"
  lib=$r/default/build/runtime-staging/lib/x86_64_Release
  start=$SECONDS
  uptime > "$out/uptime-before.txt"
  diff -u "$r/default/runtime/src/Heap/z/zValue.hpp" "$r/$arm/runtime/src/Heap/z/zValue.hpp" > "$out/cut.diff"
  diff -u "$r/default/runtime/src/Heap/z/zValue.inline.hpp" "$r/$arm/runtime/src/Heap/z/zValue.inline.hpp" >> "$out/cut.diff"
  env GC_UNIT_OUT="$out" GCV2_RUNTIME_LIB_DIR="$lib" GCV2_RUNTIME_OUTPUT_ROOT="$lib/../.." bash "$r/$arm/runtime/tests/gc_unit/run_standalone.sh" > "$out/run.log" 2>&1
  rc=$?; echo "$rc" > "$out/run.rc"
  sha256sum "$out/cj_gc_unit" "$lib"/*.so > "$out/hashes.txt"
  uptime > "$out/uptime-after.txt"
  echo "$arm rc=$rc wall=$((SECONDS-start))"
  /usr/bin/grep -E 'FAILED|INCOMPLETE|GC_UNIT_PARALLEL' "$out/run.log" | tail -12
 ) &
done
wait
