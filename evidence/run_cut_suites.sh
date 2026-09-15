#!/bin/bash
ulimit -c 0
root=/root/sym_cangjie_runtime_596_implement_r5673746875
for arm in thread-cut export-cut restored; do
(
 start=$SECONDS
 out="$root/suite-$arm"
 mkdir -p "$out"
 uptime > "$out/uptime-before.txt"
 export GC_UNIT_JOBS=192
 bash /root/sym_cangjie_runtime_596_implement_r5673746875-restored/testable/runtime/tests/gc_unit/run_parallel_tests.sh "$root/units/testable/cj_gc_unit" "$root/units/testable/cj_gc_forwarding_publication_unit" "$out" "/root/sym_cangjie_runtime_596_implement_r5673746875-$arm/testable/build/runtime-staging/lib/x86_64_Release" > "$out/run.log" 2>&1
 echo $? > "$out/rc"
 echo "wall=$((SECONDS-start))" > "$out/wall"
 uptime > "$out/uptime-after.txt"
) &
done
wait
