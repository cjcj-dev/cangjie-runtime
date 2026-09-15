#!/bin/bash
set -u
ulimit -c 0
R=${P2_RUNTIME_ROOT:-/root/sym_cangjie_runtime_607_implement_r5684610492-build3}
SDK=/root/sym_cangjie_runtime_607_implement_r5684610492-build3/sdk
P=/root/sym_cangjie_runtime_608_implement_r5683164869/abi-final-green
export CANGJIE_HOME="$SDK" GC_UNIT_CJC_RUNTIME_LIB_DIR="$P/host/runtime/lib/linux_x86_64_cjnative"
export P2_PLAIN_MAIN=1
export GCV2_RUNTIME_LIB_DIR="$R/testable/build/runtime-staging/lib/x86_64_Release" P2_FIELD_OUT="$R/${P2_RUN_TAG:-managed-v2}"
export PATH="$SDK/bin:$SDK/third_party/llvm/bin:$PATH"
mkdir -p "$P2_FIELD_OUT"
uptime > "$P2_FIELD_OUT/before.txt"
start=$SECONDS
taskset -c "${P2_CORES:-48-55}" bash "$R/testable/runtime/tests/gc_unit/run_p2_field_barrier.sh" > "$P2_FIELD_OUT/run.log" 2>&1
rc=$?
echo "$rc" > "$P2_FIELD_OUT/run.rc"
echo "wall=$((SECONDS-start))" > "$P2_FIELD_OUT/wall.txt"
uptime > "$P2_FIELD_OUT/after.txt"
echo "P2_MANAGED_RC=$rc"
/usr/bin/grep -E 'P2_|error:|Check failed:|has no owning' "$P2_FIELD_OUT/run.log" | head -35
