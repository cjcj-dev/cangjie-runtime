#!/usr/bin/env bash
set -uo pipefail

readonly REPO=/root/impl_gcunit3-aa48-run
readonly BUILD="$REPO/runtime/CMakebuild_gcunit3"
readonly LIVE_LIB="$REPO/runtime/output/temp/lib/x86_64_Release"
readonly OUT="$REPO/runtime/tests/gc_unit/build_impl_gcunit3_r2"
readonly SODEPOT=/root/sodepot/impl_gcunit3/r2-green
readonly EVIDENCE="$REPO/evidence/impl_gcunit3/r2-green"
readonly CORES=16-31

mkdir -p "$OUT" "$SODEPOT" "$EVIDENCE"
rm -f "$EVIDENCE/R2_GREEN_DONE"
{
    date -Ins
    uptime
    taskset -pc $$
    git -C "$REPO" rev-parse HEAD
    git -C "$REPO" status --short
    git -C "$REPO" diff -- runtime/src/Heap/Collector/Mark.cpp \
        runtime/src/ObjectModel/MArray.cpp runtime/src/ObjectModel/MArray.h \
        runtime/tests/gc_unit/test_m0_exit.cpp \
        runtime/tests/gc_unit/test_segmented_array_init.cpp
} >"$EVIDENCE/preflight-and.diff" 2>&1
sha256sum "$REPO/runtime/src/Heap/Collector/Mark.cpp" \
    "$REPO/runtime/src/ObjectModel/MArray.cpp" \
    "$REPO/runtime/src/ObjectModel/MArray.h" \
    "$REPO/runtime/tests/gc_unit/test_m0_exit.cpp" \
    "$REPO/runtime/tests/gc_unit/test_segmented_array_init.cpp" \
    >"$EVIDENCE/source.sha256"
stat -c '%y %n' "$REPO/runtime/src/Heap/Collector/Mark.cpp" \
    "$REPO/runtime/src/ObjectModel/MArray.cpp" \
    "$REPO/runtime/src/ObjectModel/MArray.h" \
    "$REPO/runtime/tests/gc_unit/test_m0_exit.cpp" \
    "$REPO/runtime/tests/gc_unit/test_segmented_array_init.cpp" \
    >"$EVIDENCE/source.stat"

cd "$REPO/runtime" || exit $?
GC_UNIT_GATE_SKIP=1 taskset -c "$CORES" cmake --build "$BUILD" \
    --target cangjie-runtime --parallel 16 >"$EVIDENCE/build.log" 2>&1
build_rc=$?
printf '%s\n' "$build_rc" >"$EVIDENCE/build.rc"
if [[ $build_rc -ne 0 ]]; then
    exit "$build_rc"
fi
cp "$LIVE_LIB/libcangjie-runtime.so" "$SODEPOT/"
cp "$LIVE_LIB/libboundscheck.so" "$SODEPOT/"
sha256sum "$SODEPOT/libcangjie-runtime.so" "$SODEPOT/libboundscheck.so" \
    >"$EVIDENCE/product.sha256"
stat -c '%y %n' "$SODEPOT/libcangjie-runtime.so" "$SODEPOT/libboundscheck.so" \
    >"$EVIDENCE/product.stat"

GC_UNIT_OUT="$OUT" GCV2_RUNTIME_LIB_DIR="$SODEPOT" MRT_TESTABLE_INTERNALS=1 \
    GC_UNIT_FILTER=M0Exit.RootFixRuntimeEnumerationClassifiesNoCopyAsS0 \
    taskset -c "$CORES" bash "$REPO/runtime/tests/gc_unit/run_standalone.sh" \
    >"$EVIDENCE/standalone-build-and-probe.log" 2>&1
standalone_rc=$?
printf '%s\n' "$standalone_rc" >"$EVIDENCE/standalone-build-and-probe.rc"
if [[ ! -x "$OUT/cj_gc_unit" ]]; then
    exit "$standalone_rc"
fi
cp "$OUT/cj_gc_unit" "$SODEPOT/"
sha256sum "$SODEPOT/cj_gc_unit" >"$EVIDENCE/test-elf.sha256"
stat -c '%y %n' "$SODEPOT/cj_gc_unit" >"$EVIDENCE/test-elf.stat"
LD_LIBRARY_PATH="$SODEPOT" ldd "$SODEPOT/cj_gc_unit" >"$EVIDENCE/test-elf.ldd"

tests=(
    M0Exit.RootFixRuntimeEnumerationClassifiesNoCopyAsS0
    M0Exit.RootFixClassifiesActiveOnlyUnusableCopyAsS1
    M0Exit.RootFixClassifiesRetiredOnlyUnusableCopyAsS1
    SegmentedArrayInit.EpochFlipRestartsAndRewritesPublishedBlock
    SegmentedArrayInit.EpochFlipRestartsAndRewritesPublishedBlockParallel
    SegmentedArrayInit.YoungGcRepairsIncompleteArrayRoot
    SegmentedArrayInit.YoungGcRepairsIncompleteArrayRootParallel
)

printf 'test\trc\n' >"$EVIDENCE/results.tsv"
for test_name in "${tests[@]}"; do
    safe_name=${test_name//./_}
    set +e
    LD_LIBRARY_PATH="$SODEPOT" taskset -c "$CORES" \
        "$SODEPOT/cj_gc_unit" "--gtest_filter=$test_name" \
        >"$EVIDENCE/${safe_name}.log" 2>&1
    test_rc=$?
    set -e
    printf '%s\n' "$test_rc" >"$EVIDENCE/${safe_name}.rc"
    printf '%s\t%s\n' "$test_name" "$test_rc" >>"$EVIDENCE/results.tsv"
done

{
    date -Ins
    uptime
    taskset -pc $$
    sha256sum "$SODEPOT/libcangjie-runtime.so" "$SODEPOT/libboundscheck.so" \
        "$SODEPOT/cj_gc_unit"
} >"$EVIDENCE/postflight.log" 2>&1
touch "$EVIDENCE/R2_GREEN_DONE"
