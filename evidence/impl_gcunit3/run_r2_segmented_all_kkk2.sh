#!/usr/bin/env bash
set -uo pipefail

readonly LIBDIR=/root/sodepot/impl_gcunit3/r2-t11-restored
readonly OUT=/root/impl_gcunit3-aa48-run/evidence/impl_gcunit3/r2-segmented-all
readonly CORES=16-31

tests=(
    SegmentedArrayInit.EpochFlipRestartsAndRewritesPublishedBlock
    SegmentedArrayInit.EpochFlipRestartsAndRewritesPublishedBlockParallel
    SegmentedArrayInit.LargePrimitiveArrayKeepsFastPath
    SegmentedArrayInit.ManagedDirtyExtentUsesSegmentedInitializer
    SegmentedArrayInit.ManagedFirstInactiveExtentUsesSegmentedInitializer
    SegmentedArrayInit.ManagedGarbageExtentUsesSegmentedInitializer
    SegmentedArrayInit.ManagedReleasedExtentUsesSegmentedInitializer
    SegmentedArrayInit.NativeDirtyExtentUsesSegmentedInitializer
    SegmentedArrayInit.NativeFirstInactiveExtentUsesSegmentedInitializer
    SegmentedArrayInit.NativeGarbageExtentUsesSegmentedInitializer
    SegmentedArrayInit.NativeLargePrimitiveArrayKeepsFastPath
    SegmentedArrayInit.NativeReleasedExtentUsesSegmentedInitializer
    SegmentedArrayInit.NativeSmallReferenceArrayKeepsFastPath
    SegmentedArrayInit.SmallReferenceArrayKeepsFastPath
    SegmentedArrayInit.YieldKeepsInvisibleRootAndPublishesBoundary
    SegmentedArrayInit.YoungGcRepairsIncompleteArrayRoot
    SegmentedArrayInit.YoungGcRepairsIncompleteArrayRootParallel
)

mkdir -p "$OUT"
{
    date -Ins
    uptime
    sha256sum "$LIBDIR/libcangjie-runtime.so" "$LIBDIR/libboundscheck.so" "$LIBDIR/cj_gc_unit"
    LD_LIBRARY_PATH="$LIBDIR" ldd "$LIBDIR/cj_gc_unit"
} >"$OUT/preflight.log" 2>&1
printf 'test\trc\n' >"$OUT/results.tsv"
for test_name in "${tests[@]}"; do
    safe_name=${test_name//./_}
    set +e
    LD_LIBRARY_PATH="$LIBDIR" taskset -c "$CORES" \
        "$LIBDIR/cj_gc_unit" "--gtest_filter=$test_name" >"$OUT/${safe_name}.log" 2>&1
    test_rc=$?
    printf '%s\n' "$test_rc" >"$OUT/${safe_name}.rc"
    printf '%s\t%s\n' "$test_name" "$test_rc" >>"$OUT/results.tsv"
done
{
    date -Ins
    uptime
} >"$OUT/postflight.log" 2>&1
