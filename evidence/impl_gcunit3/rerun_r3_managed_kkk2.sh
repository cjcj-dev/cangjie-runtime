#!/usr/bin/env bash
set -uo pipefail

readonly REPO=/root/impl_gcunit3-aa48-run
readonly BUILD="$REPO/runtime/CMakebuild_gcunit3"
readonly LIVE_LIB="$REPO/runtime/output/temp/lib/x86_64_Release"
readonly DEPOT=/root/sodepot/impl_gcunit3/r3-green
readonly OUT="$REPO/evidence/impl_gcunit3/r3-managed-rerun"
readonly CORES=16-31

tests=(
    SegmentedArrayInit.EpochFlipRestartsAndRewritesPublishedBlock
    SegmentedArrayInit.EpochFlipRestartsAndRewritesPublishedBlockParallel
    SegmentedArrayInit.YoungGcRepairsIncompleteArrayRoot
    SegmentedArrayInit.YoungGcRepairsIncompleteArrayRootParallel
    SegmentedArrayInit.YoungGcResidualWatermarkUsesManagedFallback
)

mkdir -p "$OUT" "$OUT/managed"
{
    date -Ins
    uptime
    sha256sum "$REPO/runtime/src/ObjectModel/MArray.cpp" "$DEPOT/cj_gc_unit"
    stat -c '%y %n' "$REPO/runtime/src/ObjectModel/MArray.cpp" "$DEPOT/cj_gc_unit"
} >"$OUT/preflight.log" 2>&1

cd "$REPO/runtime" || exit $?
GC_UNIT_GATE_SKIP=1 taskset -c "$CORES" cmake --build "$BUILD" \
    --target cangjie-runtime --parallel 16 >"$OUT/build.log" 2>&1
build_rc=$?
printf '%s\n' "$build_rc" >"$OUT/build.rc"
if [[ $build_rc -ne 0 ]]; then
    exit "$build_rc"
fi
cp "$LIVE_LIB/libcangjie-runtime.so" "$DEPOT/"
cp "$LIVE_LIB/libboundscheck.so" "$DEPOT/"
sha256sum "$DEPOT/libcangjie-runtime.so" "$DEPOT/libboundscheck.so" "$DEPOT/cj_gc_unit" \
    >"$OUT/products.sha256"
stat -c '%y %n' "$DEPOT/libcangjie-runtime.so" "$DEPOT/libboundscheck.so" "$DEPOT/cj_gc_unit" \
    >"$OUT/products.stat"

printf 'test\trc\n' >"$OUT/results.tsv"
for test_name in "${tests[@]}"; do
    safe_name=${test_name//./_}
    set +e
    LD_LIBRARY_PATH="$DEPOT" taskset -c "$CORES" "$DEPOT/cj_gc_unit" \
        "--gtest_filter=$test_name" >"$OUT/${safe_name}.log" 2>&1
    test_rc=$?
    set -e
    printf '%s\n' "$test_rc" >"$OUT/${safe_name}.rc"
    printf '%s\t%s\n' "$test_name" "$test_rc" >>"$OUT/results.tsv"
done

set +e
CANGJIE_HOME=/root/rebuild_b_sdk CJC=/root/rebuild_b_sdk/bin/cjc \
    GCV2_RUNTIME_LIB_DIR="$DEPOT" GC_UNIT_OUT="$OUT/managed" \
    taskset -c "$CORES" bash "$REPO/runtime/tests/gc_unit/run_segmented_array_managed.sh" both \
    >"$OUT/managed-gate.log" 2>&1
managed_rc=$?
set -e
printf '%s\n' "$managed_rc" >"$OUT/managed-gate.rc"

{
    date -Ins
    uptime
    sha256sum "$DEPOT/libcangjie-runtime.so" "$DEPOT/libboundscheck.so" "$DEPOT/cj_gc_unit"
} >"$OUT/postflight.log" 2>&1

if /usr/bin/grep -q $'\t[1-9][0-9]*$' "$OUT/results.tsv" || [[ $managed_rc -ne 0 ]]; then
    exit 1
fi
touch "$OUT/R3_MANAGED_RERUN_DONE"
