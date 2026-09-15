#!/bin/bash
set -euo pipefail
ulimit -c 0
R=/root/sym_cangjie_runtime_607_implement_r5684610492-build3
P=/root/sym_cangjie_runtime_608_implement_r5683164869/abi-final-green
mkdir -p "$R/managed"
[ -d "$R/sdk" ] || cp -a --reflink=auto "$P/target" "$R/sdk"
cp -a "$P/std-install/." "$R/sdk/"
cp "$R/testable/build/runtime-staging/lib/x86_64_Release/"{libcangjie-runtime.so,libboundscheck.so} "$R/sdk/runtime/lib/linux_x86_64_cjnative/"
export CANGJIE_HOME="$R/sdk" GC_UNIT_CJC_RUNTIME_LIB_DIR="$P/host/runtime/lib/linux_x86_64_cjnative"
export GCV2_RUNTIME_LIB_DIR="$R/testable/build/runtime-staging/lib/x86_64_Release" P2_FIELD_OUT="$R/managed"
export PATH="$R/sdk/bin:$R/sdk/third_party/llvm/bin:$PATH"
uptime > "$R/managed/before.txt"
start=$SECONDS
set +e
taskset -c 112-119 bash "$R/testable/runtime/tests/gc_unit/run_p2_field_barrier.sh" > "$R/managed/run.log" 2>&1
rc=$?
set -e
echo "$rc" > "$R/managed/run.rc"
echo "wall=$((SECONDS-start))" > "$R/managed/wall.txt"
uptime > "$R/managed/after.txt"
echo "P2_MANAGED_RC=$rc"
/usr/bin/grep -E 'P2_|error:|CHECK|Error|undefined' "$R/managed/run.log" | head -30 || true
