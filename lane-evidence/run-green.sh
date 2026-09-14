#!/bin/bash
ulimit -c 0
root=/root/sym_cangjie_runtime_581_implement_r5668277053-final3
cd "$root" || exit 2
export GC_UNIT_BUILD_JOBS=192
uptime > unit-uptime-before.txt
for arm in default filler; do
 (
   export GCV2_RUNTIME_LIB_DIR="$root/default/build/runtime-staging/lib/x86_64_Release" GC_UNIT_OUT="$root/unit-$arm"
   if [ "$arm" = filler ]; then export CJRT_HEAP_FILLER=0; fi
   start=$SECONDS
   taskset -c 32-63 bash default/runtime/tests/gc_unit/run_standalone.sh > "unit-$arm.log" 2>&1
   echo $? > "unit-$arm.rc"
   echo $((SECONDS-start)) > "unit-$arm.wall"
 ) &
done
wait
uptime > unit-uptime-after.txt
