#!/bin/bash
# Invoke inside wf_kkk2.sh sh; SDK is an existing, unchanged private toolchain.
set -u
ulimit -c 0
root=$1; cores=$2; sample=$3
export CANGJIE_HOME=/root/sym_cangjie_runtime_611_implement_r5684604770-b3/managed/sdk
export CJC=$CANGJIE_HOME/bin/cjc
export GC_UNIT_CJC_RUNTIME_LIB_DIR=/root/sym_cangjie_runtime_608_implement_r5683164869/abi-final-green/host/runtime/lib/linux_x86_64_cjnative
export GCV2_RUNTIME_LIB_DIR=$root/testable/build/runtime-staging/lib/x86_64_Release
for test in finalizer_trigger segmented_array_managed; do
    (
        out=$root/managed/$test-$sample
        mkdir -p "$out"
        uptime > "$out/uptime-before.txt"
        sha256sum "$GCV2_RUNTIME_LIB_DIR"/*.so > "$out/so.sha256"
        start=$SECONDS
        GC_UNIT_OUT=$out taskset -c "$cores" bash "$root/testable/runtime/tests/gc_unit/run_$test.sh" > "$out/runner.log" 2>&1
        rc=$?; echo "$rc" > "$out/run.rc"
        echo "wall=$((SECONDS-start))" > "$out/wall.txt"
        uptime > "$out/uptime-after.txt"
        sha256sum "$out/$test" > "$out/elf.sha256" 2>/dev/null
        echo "P07_MANAGED test=$test sample=$sample rc=$rc"
        tail -4 "$out/runner.log"
    ) &
done
wait
