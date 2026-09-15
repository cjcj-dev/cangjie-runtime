#!/bin/bash
ulimit -c 0
root=/root/sym_cangjie_runtime_606_implement_r5674249495-green5
cd "$root" || exit 2
export CANGJIE_HOME=/root/.cjv/toolchains/gate-colored-23e45a2e
export GCV2_RUNTIME_LIB_DIR="$root/testable/build/runtime-staging/lib/x86_64_Release"
export GC_CYCLE_TESTABLE=1
export GC_UNIT_CJC_RUNTIME_LIB_DIR=/root/.cjv/toolchains/nightly-1.3.0-alpha.20260904010027/runtime/lib/linux_x86_64_cjnative
uptime > managed-retry-uptime-before.txt
for n in 0 1 2; do
  (
    case $n in 0) cores=0-31;; 1) cores=32-63;; 2) cores=96-127;; esac
    export GC_CYCLE_OUT="$root/managed-retry-$n"
    start=$SECONDS
    taskset -c "$cores" bash "$root/testable/runtime/tests/gc_unit/run_generation_cycle_context.sh" > "$root/managed-retry-$n.log" 2>&1
    echo "$?" > "$root/managed-retry-$n.rc"
    echo "$((SECONDS-start))" > "$root/managed-retry-$n.wall"
    echo "$cores" > "$root/managed-retry-$n.cores"
  ) &
done
wait
uptime > managed-retry-uptime-after.txt
for n in 0 1 2; do
 echo "managed-retry-$n rc=$(cat "$root/managed-retry-$n.rc")"
 /usr/bin/grep -n -E 'error:|FAIL|CYCLE_RC|P1_|ASSERT p1_' "$root/managed-retry-$n.log" | tail -20
done
