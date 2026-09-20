#!/bin/bash
ulimit -c 0
root=/root/sym_cangjie_runtime_730_implement_r5746502983-green
cd "$root" || exit 2
uptime > tests-uptime-before.txt
for arm in default testable; do
  (
    start=$SECONDS
    lib="$root/$arm/build/runtime-staging/lib/x86_64_Release"
    out="$root/unit-$arm"
    mkdir -p "$out"
    testable=0; [ "$arm" = testable ] && testable=1
    env GC_UNIT_JOBS=192 MRT_TESTABLE_INTERNALS="$testable" GC_UNIT_OUT="$out" GCV2_RUNTIME_LIB_DIR="$lib" GCV2_RUNTIME_OUTPUT_ROOT="$lib/../.." bash "$root/$arm/runtime/tests/gc_unit/run_standalone.sh" > "$out/run.log" 2>&1
    echo $? > "$out/run.rc"
    echo "wall=$((SECONDS-start))" > "$out/wall.txt"
  ) &
done
wait
uptime > tests-uptime-after.txt
