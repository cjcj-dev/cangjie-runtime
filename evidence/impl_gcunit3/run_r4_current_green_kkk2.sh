#!/usr/bin/env bash
set -uo pipefail

readonly REPO=/root/impl_gcunit3-aa48-run
readonly PRODUCT=/root/sodepot/impl_gcunit3/r4-main-merged
readonly DEPOT=/root/sodepot/impl_gcunit3/r4-current-green
readonly TEST_OUT="$REPO/runtime/tests/gc_unit/build_impl_gcunit3_r4"
readonly OUT="$REPO/evidence/impl_gcunit3/r4-current-green"
readonly CORES=16-31

tests=(
    M0Exit.RootFixFailsClosedOnUnmappedForwardedRoot
    M0Exit.RootFixFailsClosedOnUnusableActiveWitness
    M0Exit.RootFixFailsClosedOnUnusableRetiredWitness
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
    sha256sum \
        "$REPO/runtime/src/Heap/Allocator/ForwardingTable.cpp" \
        "$REPO/runtime/src/Heap/Allocator/ForwardingTable.h" \
        "$REPO/runtime/src/Heap/Collector/Collector.h" \
        "$REPO/runtime/src/Heap/WCollector/WCollector.h" \
        "$REPO/runtime/tests/gc_unit/clear_entries_product_unit.cpp" \
        "$REPO/runtime/tests/gc_unit/test_forwarding_no_geometry.cpp" \
        "$REPO/runtime/src/Heap/Collector/Mark.cpp" \
        "$REPO/runtime/src/ObjectModel/MArray.cpp" \
        "$REPO/runtime/src/ObjectModel/MArray.h" \
        "$REPO/runtime/tests/gc_unit/test_m0_exit.cpp" \
        "$REPO/runtime/tests/gc_unit/test_segmented_array_init.cpp" \
        "$PRODUCT/libcangjie-runtime.so" "$PRODUCT/libboundscheck.so"
} >"$OUT/preflight.log" 2>&1

set +e
GC_UNIT_OUT="$TEST_OUT" GCV2_RUNTIME_LIB_DIR="$PRODUCT" MRT_TESTABLE_INTERNALS=1 \
    GC_UNIT_FILTER=M0Exit.RootFixFailsClosedOnUnmappedForwardedRoot \
    taskset -c "$CORES" bash "$REPO/runtime/tests/gc_unit/run_standalone.sh" \
    >"$OUT/standalone-build-and-probe.log" 2>&1
standalone_rc=$?
set -e
printf '%s\n' "$standalone_rc" >"$OUT/standalone-build-and-probe.rc"
if [[ ! -x "$TEST_OUT/cj_gc_unit" || ! -x "$TEST_OUT/cj_gc_forwarding_publication_unit" ]]; then
    exit "$standalone_rc"
fi

cp "$PRODUCT/libcangjie-runtime.so" "$DEPOT/"
cp "$PRODUCT/libboundscheck.so" "$DEPOT/"
cp "$TEST_OUT/cj_gc_unit" "$DEPOT/"
cp "$TEST_OUT/cj_gc_forwarding_publication_unit" "$DEPOT/"
sha256sum "$DEPOT/libcangjie-runtime.so" "$DEPOT/libboundscheck.so" \
    "$DEPOT/cj_gc_unit" "$DEPOT/cj_gc_forwarding_publication_unit" >"$OUT/products.sha256"
stat -c '%y %n' "$DEPOT/libcangjie-runtime.so" "$DEPOT/libboundscheck.so" \
    "$DEPOT/cj_gc_unit" "$DEPOT/cj_gc_forwarding_publication_unit" >"$OUT/products.stat"
LD_LIBRARY_PATH="$DEPOT" ldd "$DEPOT/cj_gc_unit" >"$OUT/test-elf.ldd"

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
LD_LIBRARY_PATH="$DEPOT" taskset -c "$CORES" "$DEPOT/cj_gc_forwarding_publication_unit" \
    >"$OUT/forwarding-publication.log" 2>&1
publication_rc=$?
set -e
printf '%s\n' "$publication_rc" >"$OUT/forwarding-publication.rc"

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
    sha256sum "$DEPOT/libcangjie-runtime.so" "$DEPOT/libboundscheck.so" \
        "$DEPOT/cj_gc_unit" "$DEPOT/cj_gc_forwarding_publication_unit"
} >"$OUT/postflight.log" 2>&1

if /usr/bin/grep -q $'\t[1-9][0-9]*$' "$OUT/results.tsv" || \
    [[ $publication_rc -ne 0 || $managed_rc -ne 0 ]]; then
    exit 1
fi
touch "$OUT/R4_CURRENT_GREEN_DONE"
