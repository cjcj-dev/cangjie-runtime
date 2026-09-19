#!/bin/bash
ulimit -c 0
BASE=/root/sym_cangjie_runtime_607_implement_r5739597397
TESTROOT=$BASE-final
ARTIFACTS=$TESTROOT/green
OUT=$BASE/final9-cuts
export CANGJIE_HOME=/root/sym_cangjie_runtime_593_implement_r5700751289/cangjie-home
export GC_UNIT_CJC_RUNTIME_LIB_DIR=/root/sym_cjcj_48_implement_r5685150408/host/runtime/lib/linux_x86_64_cjnative
export P2_REUSE_ARTIFACTS=1 P2_PLAIN_MAIN=1
mkdir -p "$OUT"
uptime > "$OUT/uptime-before.txt"
run() {
 local arm=$1 entry=$2
 local lib=$BASE-finalcut-$arm/testable/build/runtime-staging/lib/x86_64_Release
 [[ $arm = green || $arm = restored ]] && lib=$TESTROOT/testable/build/runtime-staging/lib/x86_64_Release
 local o=$OUT/$arm/$entry
 mkdir -p "$o"
 local start=$SECONDS
 export GCV2_RUNTIME_LIB_DIR=$lib P2_FIELD_OUT=$ARTIFACTS/$entry
 unset P2_MINOR_ONLY P2_ARRAY_FINALIZABLE
 [[ $entry = p2FieldBarrierExercise ]] && export P2_MINOR_ONLY=1
 [[ $entry = p2ArrayFieldExercise ]] && export P2_ARRAY_FINALIZABLE=1
 taskset -c 0-31 bash "$TESTROOT/testable/runtime/tests/gc_unit/run_p2_field_barrier.sh" > "$o/run.log" 2>&1
 local rc=$?
 echo $rc > "$o/run.rc"
 echo $((SECONDS-start)) > "$o/wall.txt"
 echo "$arm $entry rc=$rc"
 /usr/bin/grep -E 'P2_.*(FAIL|RESULT)|Check failed' "$o/run.log" | tail -8
}
for arm in green entry consumer partial final strong nonmajor index binding restored; do
 for entry in p2FieldBarrierExercise p2SlowFieldInputExercise p2MinorDuringOldMarkExercise p2ArrayFieldExercise p2RemsetBindingExercise; do
  (run "$arm" "$entry") &
 done
done
wait
uptime > "$OUT/uptime-after.txt"
