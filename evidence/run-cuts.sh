#!/bin/bash
ulimit -c 0
BASE=/root/sym_cangjie_runtime_607_implement_r5738304930
R=${1:-$BASE-final3}
OUT=${2:-$BASE/final-cuts}
GREEN=${3:-$BASE/final-green}
export CANGJIE_HOME=/root/sym_cangjie_runtime_593_implement_r5700751289/cangjie-home
export GC_UNIT_CJC_RUNTIME_LIB_DIR=/root/sym_cjcj_48_implement_r5685150408/host/runtime/lib/linux_x86_64_cjnative
export P2_REUSE_ARTIFACTS=1 P2_PLAIN_MAIN=1
mkdir -p "$OUT"
uptime > "$OUT/uptime-before.txt"
for arm in producer consumer promotion final strong nonmajor; do
 for entry in p2FieldBarrierExercise p2SlowFieldInputExercise p2MinorDuringOldMarkExercise p2FinalizerClosureExercise p2FinalizerRegistrationExercise p2ArrayFieldExercise; do
 (
  O=$OUT/$arm/$entry
  mkdir -p "$O"
  LIB=$BASE-finalcut-$arm/testable/build/runtime-staging/lib/x86_64_Release
  # Field cut consumer is exercised through two real minor cycles before the
  # old-field scenario: it must not be hidden by store-buffer marking on cycle 1.
  export GCV2_RUNTIME_LIB_DIR=$LIB P2_FIELD_OUT=$GREEN/$entry
  if [[ "$entry" = p2FieldBarrierExercise ]]; then export P2_MINOR_ONLY=1; fi
  start=$SECONDS
  taskset -c 0-31 bash "$R/testable/runtime/tests/gc_unit/run_p2_field_barrier.sh" > "$O/run.log" 2>&1
  rc=$?; echo "$rc" > "$O/run.rc"; echo "$((SECONDS-start))" > "$O/wall.txt"
  echo "$arm $entry rc=$rc"
  grep -E 'P2_.*FAIL|CHECK|FATAL' "$O/run.log" | tail -6
 ) &
 done
done
wait
uptime > "$OUT/uptime-after.txt"
