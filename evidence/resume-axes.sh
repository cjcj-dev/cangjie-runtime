#!/bin/bash
ulimit -c 0
R=/root/sym_cangjie_runtime_607_implement_r5739597397-resume
O=/root/sym_cangjie_runtime_607_implement_r5739597397-resume/all-axes
export CANGJIE_HOME=/root/sym_cangjie_runtime_593_implement_r5700751289/cangjie-home
export GC_UNIT_CJC_RUNTIME_LIB_DIR=/root/sym_cjcj_48_implement_r5685150408/host/runtime/lib/linux_x86_64_cjnative
export GCV2_RUNTIME_LIB_DIR=$R/testable/build/runtime-staging/lib/x86_64_Release
export P2_PLAIN_MAIN=1
mkdir -p "$O"
uptime > "$O/uptime-before.txt"
run() {
 local entry=$1 tag=$2; shift 2
 mkdir -p "$O/$entry"
 local start=$SECONDS
 env P2_TEST_ENTRY="$entry" P2_FIELD_OUT="$O/$entry" "$@" taskset -c 0-31 bash "$R/testable/runtime/tests/gc_unit/run_p2_field_barrier.sh" > "$O/$entry/$tag.log" 2>&1
 echo $? > "$O/$entry/$tag.rc"
 echo "$((SECONDS-start))" > "$O/$entry/$tag.wall"
 echo "$entry $tag rc=$(cat "$O/$entry/$tag.rc")"
 grep -E 'P2_.*(FAIL|RESULT)|error:|CHECK' "$O/$entry/$tag.log" | tail -8
}
for e in p2FieldBarrierExercise p2SlowFieldInputExercise p2MinorDuringOldMarkExercise p2FinalizerClosureExercise p2FinalizerRegistrationExercise p2ArrayFieldExercise; do run "$e" run & done
wait
export P2_REUSE_ARTIFACTS=1
run p2FieldBarrierExercise final P2_FINALIZABLE=1 &
run p2FieldBarrierExercise minor P2_MINOR_ONLY=1 &
run p2ArrayFieldExercise final P2_ARRAY_FINALIZABLE=1 &
run p2ArrayFieldExercise struct P2_STRUCT_ARRAY=1 &
run p2ArrayFieldExercise struct-final P2_ARRAY_FINALIZABLE=1 P2_STRUCT_ARRAY=1 &
wait
nm --defined-only -C "$GCV2_RUNTIME_LIB_DIR/libcangjie-runtime.so" > "$O/product.full-defined.txt"
nm --defined-only -C "$O/p2FieldBarrierExercise/libp2_field_barrier.so" > "$O/observer.full-defined.txt"
nm -u -C "$O/p2FieldBarrierExercise/libp2_field_barrier.so" > "$O/observer.undefined.txt"
sha256sum "$GCV2_RUNTIME_LIB_DIR/libcangjie-runtime.so" "$GCV2_RUNTIME_LIB_DIR/libboundscheck.so" > "$O/product.sha256"
uptime > "$O/uptime-after.txt"
