#!/usr/bin/env bash
set -uo pipefail

readonly REPO=/root/impl_gcunit3-aa48-run
readonly BUILD="$REPO/runtime/CMakebuild_gcunit3"
readonly OUT="$REPO/evidence/impl_gcunit3/r2-cmake-matrix"
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

arms=(
    green:/root/sodepot/impl_gcunit3/r2-green
    cut-mutator-native:/root/sodepot/impl_gcunit3/r2-cut-mutator-native
    cut-watermark-native:/root/sodepot/impl_gcunit3/r2-cut-watermark-native
    cut-minor-relocate:/root/sodepot/impl_gcunit3/r2-cut-minor-relocate
    cut-iterator-skip:/root/sodepot/impl_gcunit3/r2-cut-iterator-skip
    producer-restored:/root/sodepot/impl_gcunit3/r2-restored
    t11-cut:/root/sodepot/impl_gcunit3/r2-t11-cut
    t11-restored:/root/sodepot/impl_gcunit3/r2-t11-restored
)

mkdir -p "$OUT"
{
    date -Ins
    uptime
    sha256sum "$REPO/runtime/tests/gc_unit/test_m0_exit.cpp" \
        "$REPO/runtime/tests/gc_unit/test_segmented_array_init.cpp"
} >"$OUT/preflight.log" 2>&1

taskset -c "$CORES" cmake --build "$BUILD" --target cj_gc_unit --parallel 16 \
    >"$OUT/build.log" 2>&1
build_rc=$?
printf '%s\n' "$build_rc" >"$OUT/build.rc"
if [[ $build_rc -ne 0 ]]; then
    exit "$build_rc"
fi

test_elf="$REPO/runtime/output/temp/bin/x86_64_Release/cj_gc_unit"
if [[ ! -x "$test_elf" ]]; then
    printf 'CMAKE_TEST_ELF_NOT_FOUND\n' >"$OUT/error.log"
    exit 2
fi
printf '%s\n' "$test_elf" >"$OUT/test-elf.path"
sha256sum "$test_elf" >"$OUT/test-elf.sha256"
stat -c '%y %n' "$test_elf" >"$OUT/test-elf.stat"

for entry in "${arms[@]}"; do
    name=${entry%%:*}
    libdir=${entry#*:}
    arm_out="$OUT/$name"
    mkdir -p "$arm_out"
    sha256sum "$libdir/libcangjie-runtime.so" "$libdir/libboundscheck.so" "$test_elf" \
        >"$arm_out/products.sha256"
    LD_LIBRARY_PATH="$libdir" ldd "$test_elf" >"$arm_out/loader.txt"
    printf 'test\trc\n' >"$arm_out/results.tsv"
    for test_name in "${tests[@]}"; do
        safe_name=${test_name//./_}
        set +e
        LD_LIBRARY_PATH="$libdir" taskset -c "$CORES" \
            "$test_elf" "--gtest_filter=$test_name" >"$arm_out/${safe_name}.log" 2>&1
        test_rc=$?
        printf '%s\n' "$test_rc" >"$arm_out/${safe_name}.rc"
        printf '%s\t%s\n' "$test_name" "$test_rc" >>"$arm_out/results.tsv"
    done
done

{
    date -Ins
    uptime
} >"$OUT/postflight.log" 2>&1
touch "$OUT/R2_CMAKE_MATRIX_DONE"
