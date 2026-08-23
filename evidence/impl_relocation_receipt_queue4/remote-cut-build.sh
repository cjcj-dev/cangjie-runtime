#!/usr/bin/env bash
set -uo pipefail

label=${1:?cut label required}
lane_root=/root/cj_build/impl_relocation_receipt_queue4-mutant
lane_runtime=$lane_root/src/runtime
lane_build=$lane_runtime/ImplBuildTest
lane_gate=$lane_runtime/tests/gc_unit/build_standalone/gate_run.log

echo "PHASE=$label"
date -Is
uptime
sha256sum "$lane_runtime/src/Heap/Allocator/RegionManager.cpp" \
    "$lane_runtime/src/Heap/Allocator/RegionManager.h"
cmake --build "$lane_build" --target cangjie-runtime -j8
build_rc=$?
echo "PRODUCT_BUILD_RC=$build_rc"
if [[ -f "$lane_gate" ]]; then
    cp "$lane_gate" "$lane_root/$label-gate.log"
    /usr/bin/grep -E '\[  FAIL  \]|\[========\]|GC_UNIT_RUN_DONE' "$lane_gate" || true
else
    echo "GATE_LOG_MISSING"
fi
date -Is
uptime
echo "FINAL_RC=$build_rc"
exit "$build_rc"
