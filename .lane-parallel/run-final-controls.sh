#!/bin/bash
ulimit -c 0
set -u
lane=sym_cangjie_runtime_700_implement_r5723534433
root=/root/${lane}-final-controls
mkdir -p "$root"
cp /root/diff_936c2943d724/unit-testable/cj_gc_unit "$root/cj_gc_unit"
uptime > "$root/uptime-before.txt"
for arm in green producer consumer restored; do
 (
 out="$root/$arm"; mkdir -p "$out"
 lib="/root/${lane}-final-${arm}/testable/build/runtime-staging/lib/x86_64_Release"
 sha256sum "$root/cj_gc_unit" "$lib"/*.so > "$out/artifacts.sha256"
 env LD_LIBRARY_PATH="$lib" ldd "$root/cj_gc_unit" > "$out/ldd.txt"
 nm --defined-only -C "$lib/libcangjie-runtime.so" > "$out/product-symbols.txt"
 nm --undefined-only -C "$root/cj_gc_unit" > "$out/test-imports.txt"
 taskset -pc $$ > "$out/affinity.txt"
 for test in ValueRootCurrentization.MajorDriverPairsExportOwnersBeforeHandoff MarkingStacksProduct.MajorSerialEntersFromDoGarbageCollection; do
  ( start=$SECONDS
    timeout 90 env LD_LIBRARY_PATH="$lib" GC_UNIT_TALLY_FILE="$out/$test.tally" "$root/cj_gc_unit" "--gtest_filter=$test" > "$out/$test.log" 2>&1
    echo "$?" > "$out/$test.rc"
    echo "$((SECONDS-start))" > "$out/$test.wall"
  ) &
 done
 wait
 sha256sum "$root/cj_gc_unit" "$lib"/*.so > "$out/artifacts-after.sha256"
 diff -u "$out/artifacts.sha256" "$out/artifacts-after.sha256" > "$out/artifacts.diff"
 ) &
done
wait
uptime > "$root/uptime-after.txt"
for arm in green producer consumer restored; do
 for test in ValueRootCurrentization.MajorDriverPairsExportOwnersBeforeHandoff MarkingStacksProduct.MajorSerialEntersFromDoGarbageCollection; do
  echo "arm=$arm test=$test rc=$(cat "$root/$arm/$test.rc") wall=$(cat "$root/$arm/$test.wall")"
  /usr/bin/grep -E 'EXPORT_OWNER_TARGET|EXPORT_OWNER_DRIVER|FAIL|tests:' "$root/$arm/$test.log"
 done
done
