#!/bin/bash
set -u
ulimit -c 0
R=/root/sym_cangjie_runtime_607_implement_r5684610492-build6
P=/root/sym_cangjie_runtime_608_implement_r5683164869/abi-final-green
export CANGJIE_HOME=/root/sym_cangjie_runtime_607_implement_r5684610492-build3/sdk
export GC_UNIT_CJC_RUNTIME_LIB_DIR="$P/host/runtime/lib/linux_x86_64_cjnative"
export GCV2_RUNTIME_LIB_DIR="$R/testable/build/runtime-staging/lib/x86_64_Release"
export GC_UNIT_OUT="$R/finalizer-trigger"
mkdir -p "$GC_UNIT_OUT"
uptime > "$GC_UNIT_OUT/before.txt"
start=$SECONDS
bash "$R/testable/runtime/tests/gc_unit/run_finalizer_trigger.sh" > "$GC_UNIT_OUT/runner.log" 2>&1
rc=$?
echo "$rc" > "$GC_UNIT_OUT/run.rc"
echo "wall=$((SECONDS-start))" > "$GC_UNIT_OUT/wall.txt"
sha256sum "$GC_UNIT_OUT/finalizer_trigger" "$GCV2_RUNTIME_LIB_DIR/libcangjie-runtime.so" "$GCV2_RUNTIME_LIB_DIR/libboundscheck.so" > "$GC_UNIT_OUT/identity.sha256"
uptime > "$GC_UNIT_OUT/after.txt"
echo "FINALIZER_TRIGGER_RC=$rc"
/usr/bin/grep -E 'FINALIZER_TRIGGER|Check failed|has no owning|LOADFC' "$GC_UNIT_OUT/runner.log" | head -8
