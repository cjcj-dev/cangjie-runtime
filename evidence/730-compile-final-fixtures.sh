#!/usr/bin/env bash
set -u
ulimit -c 0
root=/root/sym_cangjie_runtime_730_implement_r5746939673-green
for config in default testable; do
  (
    start=$SECONDS
    testable=0; [ "$config" = testable ] && testable=1
    MRT_TESTABLE_INTERNALS=$testable GC_UNIT_OUT=$root/unit-$config GCV2_RUNTIME_LIB_DIR=$root/$config/build/runtime-staging/lib/x86_64_Release \
      bash "$root/$config/runtime/tests/gc_unit/run_standalone.sh" > "$root/unit-$config.log" 2>&1
    echo $? > "$root/unit-$config.rc"
    echo $((SECONDS-start)) > "$root/unit-$config.wall"
  ) &
  (
    GC_UNIT_OUT=$root/pinned-$config GCV2_RUNTIME_LIB_DIR=$root/$config/build/runtime-staging/lib/x86_64_Release \
      bash "$root/$config/runtime/tests/gc_unit/run_pinned_publication_window.sh" > "$root/pinned-$config-build.log" 2>&1
    echo $? > "$root/pinned-$config-build.rc"
  ) &
done
wait
