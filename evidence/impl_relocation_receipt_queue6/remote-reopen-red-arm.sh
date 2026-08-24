#!/usr/bin/env bash
set -uo pipefail

lane_root=/root/cj_build/impl_relocation_receipt_queue6-full/src
lane_runtime=$lane_root/runtime
lane_build=$lane_runtime/ImplBuildTest
lane_evidence=/root/cj_build/impl_relocation_receipt_queue6-red-arm
fault_diff=$lane_evidence/reopen-after-prepare-fault.diff
product_file=$lane_runtime/src/Heap/Allocator/RegionManager.h
gate_log=$lane_runtime/tests/gc_unit/build_standalone/gate_run.log

mkdir -p "$lane_evidence"
before_sha=$(sha256sum "$product_file" | awk '{print $1}')
echo "SOURCE_SHA_BEFORE=$before_sha"
cd "$lane_root" || exit 125
patch -p1 --forward < "$fault_diff"
patch_rc=$?
echo "FAULT_APPLY_RC=$patch_rc"

cmake --build "$lane_build" --target cangjie-runtime -j8 > "$lane_evidence/product-fault-build.log" 2>&1
fault_build_rc=$?
echo "FAULT_PRODUCT_BUILD_RC=$fault_build_rc"
if [[ -f "$gate_log" ]]; then
    cp "$gate_log" "$lane_evidence/fault-gate-run.log"
    /usr/bin/grep '^\[  FAIL  \]' "$gate_log" || true
    /usr/bin/grep '^\[========\]' "$gate_log" | tail -n 1 || true
fi

patch -p1 --reverse < "$fault_diff"
restore_patch_rc=$?
after_sha=$(sha256sum "$product_file" | awk '{print $1}')
echo "FAULT_REVERSE_RC=$restore_patch_rc"
echo "SOURCE_SHA_AFTER=$after_sha"
if [[ "$before_sha" == "$after_sha" ]]; then
    echo "SOURCE_RESTORE_MATCH=1"
else
    echo "SOURCE_RESTORE_MATCH=0"
fi

bash /root/cj_build/impl_relocation_receipt_queue6-build-testable.sh \
    > "$lane_evidence/restored-green.log" 2>&1
restored_rc=$?
echo "RESTORED_GREEN_RC=$restored_rc"
tail -n 18 "$lane_evidence/restored-green.log"

fail_count=125
target_count=0
tally_ok=0
if [[ -f "$lane_evidence/fault-gate-run.log" ]]; then
    fail_count=$(/usr/bin/grep -c '^\[  FAIL  \]' "$lane_evidence/fault-gate-run.log" || true)
    target_count=$(/usr/bin/grep -c \
        '^\[  FAIL  \] RelocationRequestQueue.ProductPreparationCannotReopenClosedWorkerGeneration$' \
        "$lane_evidence/fault-gate-run.log" || true)
    if /usr/bin/grep -Eq '^\[========\] [0-9]+ tests: [0-9]+ passed, 1 failed$' \
        "$lane_evidence/fault-gate-run.log"; then
        tally_ok=1
    fi
fi
echo "FAULT_FAIL_COUNT=$fail_count"
echo "FAULT_TARGET_FAIL_COUNT=$target_count"
echo "FAULT_TALLY_OK=$tally_ok"

if [[ $patch_rc -ne 0 || $fault_build_rc -eq 0 || $restore_patch_rc -ne 0 ||
      "$before_sha" != "$after_sha" || $restored_rc -ne 0 || $fail_count -ne 1 ||
      $target_count -ne 1 || $tally_ok -ne 1 ]]; then
    echo "RED_ARM_FINAL_RC=1"
    exit 1
fi
echo "RED_ARM_FINAL_RC=0"
