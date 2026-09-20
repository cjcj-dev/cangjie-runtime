#!/bin/bash
ulimit -c 0
R=/root/sym_cangjie_runtime_739_implement_r5747765432-final
E=/root/sym_cangjie_runtime_739_implement_r5747765432-evidence
uptime > "$E/final-compile-before.txt"
for arm in default testable; do
  (t=0; [ "$arm" = testable ] && t=1
   src="$R/$arm/runtime/tests/gc_unit"
   sed '/^GC_UNIT_MAIN_ENV=/,$d' "$src/run_standalone.sh" > "$src/compile_only_739.sh"
   start=$SECONDS
   env MRT_TESTABLE_INTERNALS=$t GCV2_RUNTIME_LIB_DIR="$R/$arm/build/runtime-staging/lib/x86_64_Release" GC_UNIT_OUT="$E/final-unit-$arm" bash "$src/compile_only_739.sh" > "$E/final-unit-$arm-compile.log" 2>&1
   echo $? > "$E/final-unit-$arm-compile.rc"
   echo $((SECONDS-start)) > "$E/final-unit-$arm-compile.wall") &
done
wait
uptime > "$E/final-compile-after.txt"
start=$SECONDS
env MRT_GC_UNIT_OHOS_HOST=1 GCV2_RUNTIME_LIB_DIR="$R/default/build/runtime-staging/lib/x86_64_Release" GC_UNIT_OUT="$E/final-unit-ohos" bash "$R/default/runtime/tests/gc_unit/run_standalone.sh" > "$E/final-unit-ohos.log" 2>&1
echo $? > "$E/final-unit-ohos.rc"
echo $((SECONDS-start)) > "$E/final-unit-ohos.wall"
