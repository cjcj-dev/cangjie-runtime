#!/bin/bash
ulimit -c 0
r=/root/sym_cangjie_runtime_1322_implement_r5899654110
uptime
for arm in default testable; do
 (
  out=$r/unit-$arm
  lib=$r/$arm/build/runtime-staging/lib/x86_64_Release
  mkdir -p "$out"
  start=$SECONDS
  uptime > "$out/uptime-before.txt"
  sha256sum "$lib"/*.so > "$out/so.sha256"
  env GC_UNIT_OUT="$out" GCV2_RUNTIME_LIB_DIR="$lib" GCV2_RUNTIME_OUTPUT_ROOT="$lib/../.." bash "$r/$arm/runtime/tests/gc_unit/run_standalone.sh" > "$out/run.log" 2>&1
  echo $? > "$out/run.rc"
  sha256sum "$out/cj_gc_unit" "$out/cj_gc_forwarding_publication_unit" > "$out/elf.sha256"
  uptime > "$out/uptime-after.txt"
  echo $((SECONDS-start)) > "$out/wall.txt"
  echo "$arm rc=$(cat "$out/run.rc") wall=$(cat "$out/wall.txt")"
  if [ "$arm" = default ] && [ -x "$out/cj_gc_unit" ]; then
    mkdir -p "$r/unit-filler"
    CJRT_HEAP_FILLER=0 bash "$r/$arm/runtime/tests/gc_unit/run_parallel_tests.sh" "$out/cj_gc_unit" "$out/cj_gc_forwarding_publication_unit" "$r/unit-filler" "$lib" > "$r/unit-filler/run.log" 2>&1
    echo $? > "$r/unit-filler/run.rc"
    echo "filler rc=$(cat "$r/unit-filler/run.rc")"
  fi
 ) &
done
wait
uptime
