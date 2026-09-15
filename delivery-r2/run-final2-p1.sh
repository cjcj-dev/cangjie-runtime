#!/bin/bash
ulimit -c 0
root=/root/sym_cangjie_runtime_606_implement_r5674249495-final2
cd "$root" || exit 2
export CANGJIE_HOME=/root/.cjv/toolchains/gate-colored-23e45a2e
export GC_UNIT_CJC_RUNTIME_LIB_DIR=/root/.cjv/toolchains/nightly-1.3.0-alpha.20260904010027/runtime/lib/linux_x86_64_cjnative
export GCV2_RUNTIME_LIB_DIR="$root/testable/build/runtime-staging/lib/x86_64_Release"
export P1_MARK_START_OUT="$root/p1-managed"
uptime > p1-uptime-before.txt
start=$SECONDS
taskset -c 0-31 bash "$root/testable/runtime/tests/gc_unit/run_p1_mark_start.sh" > p1-managed-build-run.log 2>&1
rc=$?
echo "$rc" > p1-managed-build-run.rc
echo "$((SECONDS-start))" > p1-managed-build-run.wall
uptime > p1-uptime-after.txt
echo "P1_BUILD_RUN_RC=$rc"
/usr/bin/grep -n -E 'error:|FAIL|P1_MARK_START_RESULT' p1-managed-build-run.log | tail -15
