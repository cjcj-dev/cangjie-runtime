#!/usr/bin/env bash
set -uo pipefail

readonly REPO=/root/impl_gcunit3-aa48-run
readonly BUILD="$REPO/runtime/CMakebuild_gcunit3"
readonly LIVE_LIB="$REPO/runtime/output/temp/lib/x86_64_Release"
readonly PRIOR=/root/sodepot/impl_gcunit3/r2-green-final
readonly DEPOT=/root/sodepot/impl_gcunit3/r3-green
readonly TEST_OUT="$REPO/runtime/tests/gc_unit/build_impl_gcunit3_r3"
readonly OUT="$REPO/evidence/impl_gcunit3/r3-green"
readonly CORES=16-31

tests=(
    SegmentedArrayInit.EpochFlipRestartsAndRewritesPublishedBlock
    SegmentedArrayInit.EpochFlipRestartsAndRewritesPublishedBlockParallel
    SegmentedArrayInit.YoungGcRepairsIncompleteArrayRoot
    SegmentedArrayInit.YoungGcRepairsIncompleteArrayRootParallel
    SegmentedArrayInit.YoungGcResidualWatermarkUsesManagedFallback
)

mkdir -p "$DEPOT" "$TEST_OUT" "$OUT" "$OUT/managed"
{
    date -Ins
    uptime
    taskset -pc $$
    git -C "$REPO" status --short --untracked-files=no
    sha256sum \
        "$REPO/runtime/src/Heap/Collector/Mark.cpp" \
        "$REPO/runtime/src/ObjectModel/MArray.cpp" \
        "$REPO/runtime/src/ObjectModel/MArray.h" \
        "$REPO/runtime/tests/gc_unit/test_segmented_array_init.cpp"
    stat -c '%y %n' \
        "$REPO/runtime/src/Heap/Collector/Mark.cpp" \
        "$REPO/runtime/src/ObjectModel/MArray.cpp" \
        "$REPO/runtime/src/ObjectModel/MArray.h" \
        "$REPO/runtime/tests/gc_unit/test_segmented_array_init.cpp"
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
sha256sum "$DEPOT/libcangjie-runtime.so" "$DEPOT/libboundscheck.so" >"$OUT/product.sha256"
stat -c '%y %n' "$DEPOT/libcangjie-runtime.so" "$DEPOT/libboundscheck.so" >"$OUT/product.stat"

set +e
GC_UNIT_OUT="$TEST_OUT" GCV2_RUNTIME_LIB_DIR="$DEPOT" MRT_TESTABLE_INTERNALS=1 \
    GC_UNIT_FILTER=SegmentedArrayInit.YoungGcResidualWatermarkUsesManagedFallback \
    taskset -c "$CORES" bash "$REPO/runtime/tests/gc_unit/run_standalone.sh" \
    >"$OUT/standalone-build-and-probe.log" 2>&1
standalone_rc=$?
set -e
printf '%s\n' "$standalone_rc" >"$OUT/standalone-build-and-probe.rc"
if [[ ! -x "$TEST_OUT/cj_gc_unit" ]]; then
    exit "$standalone_rc"
fi
cp "$TEST_OUT/cj_gc_unit" "$DEPOT/"
sha256sum "$DEPOT/cj_gc_unit" >"$OUT/test-elf.sha256"
stat -c '%y %n' "$DEPOT/cj_gc_unit" >"$OUT/test-elf.stat"
LD_LIBRARY_PATH="$DEPOT" ldd "$DEPOT/cj_gc_unit" >"$OUT/test-elf.ldd"

printf 'test\trc\n' >"$OUT/results.tsv"
for test_name in "${tests[@]}"; do
    safe_name=${test_name//./_}
    set +e
    LD_LIBRARY_PATH="$DEPOT" taskset -c "$CORES" \
        "$DEPOT/cj_gc_unit" "--gtest_filter=$test_name" >"$OUT/${safe_name}.log" 2>&1
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
    taskset -pc $$
    sha256sum "$DEPOT/libcangjie-runtime.so" "$DEPOT/libboundscheck.so" "$DEPOT/cj_gc_unit"
} >"$OUT/postflight.log" 2>&1

if /usr/bin/grep -q $'\t[1-9][0-9]*$' "$OUT/results.tsv" || [[ $managed_rc -ne 0 ]]; then
    exit 1
fi
touch "$OUT/R3_GREEN_DONE"
