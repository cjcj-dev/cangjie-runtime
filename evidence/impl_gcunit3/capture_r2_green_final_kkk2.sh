#!/usr/bin/env bash
set -uo pipefail

readonly REPO=/root/impl_gcunit3-aa48-run
readonly BUILD="$REPO/runtime/CMakebuild_gcunit3"
readonly LIVE_LIB="$REPO/runtime/output/temp/lib/x86_64_Release"
readonly PRIOR_GREEN=/root/sodepot/impl_gcunit3/r2-green
readonly DEPOT=/root/sodepot/impl_gcunit3/r2-green-final
readonly OUT="$REPO/evidence/impl_gcunit3/r2-green-final"
readonly CORES=16-31

tests=(
    M0Exit.RootFixRuntimeEnumerationClassifiesNoCopyAsS0
    M0Exit.RootFixClassifiesActiveOnlyUnusableCopyAsS1
    M0Exit.RootFixClassifiesRetiredOnlyUnusableCopyAsS1
    SegmentedArrayInit.EpochFlipRestartsAndRewritesPublishedBlock
    SegmentedArrayInit.EpochFlipRestartsAndRewritesPublishedBlockParallel
    SegmentedArrayInit.YoungGcRepairsIncompleteArrayRoot
    SegmentedArrayInit.YoungGcRepairsIncompleteArrayRootParallel
)

mkdir -p "$DEPOT" "$OUT"
rm -f "$OUT/R2_GREEN_FINAL_DONE"
{
    date -Ins
    uptime
    taskset -pc $$
    git -C "$REPO" status --short --untracked-files=no
} >"$OUT/preflight.log" 2>&1

GC_UNIT_GATE_SKIP=1 taskset -c "$CORES" cmake --build "$BUILD" \
    --target cangjie-runtime --parallel 16 >"$OUT/build.log" 2>&1
build_rc=$?
printf '%s\n' "$build_rc" >"$OUT/build.rc"
if [[ $build_rc -ne 0 ]]; then
    exit "$build_rc"
fi

cp "$LIVE_LIB/libcangjie-runtime.so" "$DEPOT/"
cp "$PRIOR_GREEN/libboundscheck.so" "$DEPOT/"
cp "$PRIOR_GREEN/cj_gc_unit" "$DEPOT/"
sha256sum "$DEPOT/libcangjie-runtime.so" "$DEPOT/libboundscheck.so" "$DEPOT/cj_gc_unit" \
    >"$OUT/products.sha256"
stat -c '%y %n' "$DEPOT/libcangjie-runtime.so" "$DEPOT/libboundscheck.so" "$DEPOT/cj_gc_unit" \
    >"$OUT/products.stat"
LD_LIBRARY_PATH="$DEPOT" ldd "$DEPOT/cj_gc_unit" >"$OUT/loader.txt"

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

{
    date -Ins
    uptime
    taskset -pc $$
    sha256sum "$DEPOT/libcangjie-runtime.so" "$DEPOT/libboundscheck.so" "$DEPOT/cj_gc_unit"
} >"$OUT/postflight.log" 2>&1
touch "$OUT/R2_GREEN_FINAL_DONE"
