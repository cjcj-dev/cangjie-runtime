#!/bin/bash
ulimit -c 0
root=/root/sym_cangjie_runtime_606_implement_r5674249495-final2
cd "$root" || exit 2
export GC_UNIT_JOBS=192 CCACHE_DIR=/root/.ccache PATH=/usr/lib/ccache:$PATH
uptime > test-uptime-before.txt
for arm in default testable; do
  (
    export MRT_TESTABLE_INTERNALS=0
    [ "$arm" = testable ] && export MRT_TESTABLE_INTERNALS=1
    export GCV2_RUNTIME_LIB_DIR="$root/$arm/build/runtime-staging/lib/x86_64_Release"
    export GC_UNIT_OUT="$root/unit-$arm"
    start=$SECONDS
    bash "$root/$arm/runtime/tests/gc_unit/run_standalone.sh" > "$root/unit-$arm.log" 2>&1
    echo "$?" > "$root/unit-$arm.rc"
    echo "$((SECONDS-start))" > "$root/unit-$arm.wall"
    if [ "$arm" = default ] && [ -x "$GC_UNIT_OUT/cj_gc_unit" ]; then
      export CJRT_HEAP_FILLER=0
      mkdir -p "$root/unit-filler"
      bash "$root/default/runtime/tests/gc_unit/run_parallel_tests.sh" "$GC_UNIT_OUT/cj_gc_unit" "$GC_UNIT_OUT/cj_gc_forwarding_publication_unit" "$root/unit-filler" "$GCV2_RUNTIME_LIB_DIR" > "$root/unit-filler.log" 2>&1
      echo "$?" > "$root/unit-filler.rc"
    fi
  ) &
done
wait
uptime > test-uptime-after.txt
for arm in default testable filler; do
  echo "$arm rc=$(cat "$root/unit-$arm.rc" 2>/dev/null)"
  /usr/bin/grep -n -E 'error:|GC_UNIT_COMPILE_FAIL|TOTAL|SUMMARY|FAIL|PASS.*[0-9]' "$root/unit-$arm.log" | tail -12
done
