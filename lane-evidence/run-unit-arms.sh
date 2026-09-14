#!/bin/bash
ulimit -c 0
set -u
root=/root/sym_cangjie_runtime_571_implement_r5668109509-final
uptime > "$root/unit-uptime-before.txt"
for arm in default testable ohos; do
  (start=$SECONDS; shape=$arm; [ "$arm" = ohos ] && shape=testable
   cd "$root/$shape"
   export GCV2_RUNTIME_LIB_DIR=$PWD/build/runtime-staging/lib/x86_64_Release GC_UNIT_OUT=$root/unit-$arm GC_UNIT_JOBS=192 GC_UNIT_BUILD_JOBS=192
   export MRT_TESTABLE_INTERNALS=0; [ "$shape" = testable ] && export MRT_TESTABLE_INTERNALS=1
   [ "$arm" = ohos ] && export MRT_GC_UNIT_OHOS_HOST=1
   bash runtime/tests/gc_unit/run_standalone.sh > "$root/unit-$arm.log" 2>&1
   echo $? > "$root/unit-$arm.rc"; echo $((SECONDS-start)) > "$root/unit-$arm.wall") &
done
wait
uptime > "$root/unit-uptime-after.txt"
for arm in default testable ohos; do
 echo "$arm rc=$(cat "$root/unit-$arm.rc") wall=$(cat "$root/unit-$arm.wall")"
 /usr/bin/grep -E 'GC_UNIT_PARALLEL|tests:|FAILED|error:|PRODUCT_SYMBOL_MISSING' "$root/unit-$arm.log" | tail -8
done
