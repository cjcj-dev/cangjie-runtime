#!/bin/bash
set -u
ulimit -c 0
root=/root/sym_cangjie_runtime_608_implement_r5676392826
mkdir -p "$root/units"
uptime > "$root/units/before.txt"
for arm in default testable; do
  (
    start=$SECONDS
    if [[ "$arm" == testable ]]; then export MRT_TESTABLE_INTERNALS=1; fi
    export GCV2_RUNTIME_LIB_DIR="$root/$arm/build/runtime-staging/lib/x86_64_Release"
    export GC_UNIT_OUT="$root/units/$arm"
    bash "$root/$arm/runtime/tests/gc_unit/run_standalone.sh" > "$root/units/$arm.log" 2>&1
    rc=$?
    echo "$rc" > "$root/units/$arm.rc"
    echo "$((SECONDS-start))" > "$root/units/$arm.wall"
    if [[ "$arm" == default && -x "$GC_UNIT_OUT/cj_gc_unit" && -x "$GC_UNIT_OUT/cj_gc_forwarding_publication_unit" ]]; then
      mkdir -p "$root/units/filler"
      CJRT_HEAP_FILLER=0 bash "$root/default/runtime/tests/gc_unit/run_parallel_tests.sh" "$GC_UNIT_OUT/cj_gc_unit" "$GC_UNIT_OUT/cj_gc_forwarding_publication_unit" "$root/units/filler" "$GCV2_RUNTIME_LIB_DIR" > "$root/units/filler.log" 2>&1
      echo "$?" > "$root/units/filler.rc"
    fi
  ) &
done
wait
uptime > "$root/units/after.txt"
