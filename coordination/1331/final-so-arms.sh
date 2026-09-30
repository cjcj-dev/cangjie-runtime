#!/bin/bash
# Phase 3: SO-only cuts reuse the green test ELF byte-for-byte (protocol 4.2).
ulimit -c 0
r=/root/sym_cangjie_runtime_1331_implement_r5895529148
cd "$r" || exit 2
GL=$r/default/build/runtime-staging/lib/x86_64_Release
uptime > final-uptime-before3.txt

run_so_arm() { # $1 arm, $2 lib dir
  out=$r/arm-$1; rm -rf "$out"; mkdir -p "$out"
  start=$SECONDS
  uptime > "$out/uptime-before.txt"
  sha256sum "$r/arm-green/cj_gc_unit" "$r/arm-green/cj_gc_forwarding_publication_unit" "$2"/libcangjie-runtime.so > "$out/hashes.txt"
  ( cd "$r/default" && env CJRT_HEAP_FILLER=0 true
    bash "$r/default/runtime/tests/gc_unit/run_parallel_tests.sh" \
      "$r/arm-green/cj_gc_unit" "$r/arm-green/cj_gc_forwarding_publication_unit" "$out" "$2" ) > "$out/run.log" 2>&1
  rc=$?; echo "$rc" > "$out/run.rc"; echo "$((SECONDS-start))" > "$out/wall.txt"
  uptime > "$out/uptime-after.txt"
  echo "== $1 rc=$rc wall=$(cat $out/wall.txt)s $(/usr/bin/grep -oE 'GC_UNIT_PARALLEL jobs=[0-9]+ tests=[0-9]+ wall=[0-9.]+' "$out/run.log" | tail -1) $(/usr/bin/grep -oE 'GC_UNIT_INCOMPLETE tests=[0-9]+' "$out/run.log" | tail -1)"
  /usr/bin/grep -E '^\[  FAILED  \]' "$out/run.log" | sort -u | head -10
  return 0
}

# green and restored share the unmodified product SO and the same test ELF.
( run_so_arm restored  "$GL" ) &
( run_so_arm cut-page     "$r/cut-page/build/runtime-staging/lib/x86_64_Release"  ) &
( run_so_arm cut-phase    "$r/cut-phase/build/runtime-staging/lib/x86_64_Release" ) &
( run_so_arm cut-consumer "$r/cut-consumer/build/runtime-staging/lib/x86_64_Release" ) &
wait
uptime > final-uptime-after3.txt
echo "== PHASE3 DONE"
