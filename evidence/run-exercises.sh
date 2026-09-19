#!/bin/bash
ulimit -c 0
R=${1:?remote build root}
OUT=${2:?evidence directory}
export CANGJIE_HOME=/root/sym_cangjie_runtime_593_implement_r5700751289/cangjie-home
export GC_UNIT_CJC_RUNTIME_LIB_DIR=/root/sym_cjcj_48_implement_r5685150408/host/runtime/lib/linux_x86_64_cjnative
export GCV2_RUNTIME_LIB_DIR=$R/testable/build/runtime-staging/lib/x86_64_Release
export P2_PLAIN_MAIN=1
mkdir -p "$OUT"
uptime > "$OUT/uptime-before.txt"
for entry in p2FieldBarrierExercise p2SlowFieldInputExercise p2FinalizerClosureExercise p2FinalizerRegistrationExercise p2ArrayFieldExercise; do
 (export P2_TEST_ENTRY=$entry P2_FIELD_OUT=$OUT/$entry
  mkdir -p "$P2_FIELD_OUT"
  start=$SECONDS
  taskset -c 0-31 bash "$R/testable/runtime/tests/gc_unit/run_p2_field_barrier.sh" > "$P2_FIELD_OUT/run.log" 2>&1
  echo $? > "$P2_FIELD_OUT/run.rc"
  echo "$((SECONDS-start))" > "$P2_FIELD_OUT/wall.txt"
  echo "$entry rc=$(cat "$P2_FIELD_OUT/run.rc")"
  grep -E 'P2_.*(FAIL|RESULT|CONTROL)|error:|CHECK|FATAL' "$P2_FIELD_OUT/run.log" | tail -12
 ) &
done
wait
uptime > "$OUT/uptime-after.txt"
