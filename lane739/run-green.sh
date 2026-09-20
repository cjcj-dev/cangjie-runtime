#!/bin/bash
ulimit -c 0
R=/root/sym_cangjie_runtime_739_implement_r5747765432-green
E=/root/sym_cangjie_runtime_739_implement_r5747765432-evidence/green
mkdir -p "$E"
uptime > "$E/uptime-before.txt"
for arm in default testable; do
  (t=0; [ "$arm" = testable ] && t=1
   start=$SECONDS
   env MRT_TESTABLE_INTERNALS=$t GCV2_RUNTIME_LIB_DIR="$R/$arm/build/runtime-staging/lib/x86_64_Release" GC_UNIT_OUT="$E/unit-$arm" bash "$R/$arm/runtime/tests/gc_unit/run_standalone.sh" > "$E/unit-$arm.log" 2>&1
   echo $? > "$E/unit-$arm.rc"
   echo $((SECONDS-start)) > "$E/unit-$arm.wall") &
done
wait
uptime > "$E/uptime-after.txt"
