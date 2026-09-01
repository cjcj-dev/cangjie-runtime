#!/usr/bin/env bash
set -uo pipefail

readonly REPO=/root/impl_gcunit3-aa48-run
readonly ROOT_OUT="$REPO/evidence/impl_gcunit3/r2-dual-config"
readonly GREEN_ELF=/root/sodepot/impl_gcunit3/r2-green/cj_gc_unit
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

mkdir -p "$ROOT_OUT"
rm -f "$ROOT_OUT/R2_DUAL_CONFIG_DONE"
{
    date -Ins
    uptime
    taskset -pc $$
    git -C "$REPO" status --short -- runtime/src runtime/tests/gc_unit
    sha256sum "$REPO/runtime/src/Heap/Collector/Mark.cpp" \
        "$REPO/runtime/src/ObjectModel/MArray.cpp" \
        "$REPO/runtime/src/ObjectModel/MArray.h" \
        "$REPO/runtime/tests/gc_unit/test_m0_exit.cpp" \
        "$REPO/runtime/tests/gc_unit/test_segmented_array_init.cpp"
    echo 'RECIPE=cmake -S runtime -B <arm> then cmake --build <arm> --target cangjie-runtime --parallel 16'
} >"$ROOT_OUT/preflight.log" 2>&1

build_arm()
{
    local name=$1
    local gc_units=$2
    local testable=$3
    local build="$REPO/runtime/CMakebuild_gcunit3_dual_$name"
    local out="$ROOT_OUT/$name"
    local depot="/root/sodepot/impl_gcunit3/r2-dual-$name"
    mkdir -p "$out" "$depot"

    taskset -c "$CORES" cmake -S "$REPO/runtime" -B "$build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCJ_SDK_VERSION=0.0.1 \
        -DDISABLE_VERSION_CHECK=ON \
        -DMRT_GC_UNIT_TESTS="$gc_units" \
        -DMRT_TESTABLE_INTERNALS="$testable" \
        >"$out/configure.log" 2>&1
    local configure_rc=$?
    printf '%s\n' "$configure_rc" >"$out/configure.rc"
    if [[ $configure_rc -ne 0 ]]; then
        return "$configure_rc"
    fi

    GC_UNIT_GATE_SKIP=1 taskset -c "$CORES" cmake --build "$build" \
        --target cangjie-runtime --parallel 16 >"$out/build.log" 2>&1
    local build_rc=$?
    printf '%s\n' "$build_rc" >"$out/build.rc"
    if [[ $build_rc -ne 0 ]]; then
        return "$build_rc"
    fi

    local libdir="$REPO/runtime/output/temp/lib/x86_64_Release"
    cp "$libdir/libcangjie-runtime.so" "$depot/"
    cp "$libdir/libboundscheck.so" "$depot/"
    sha256sum "$depot/libcangjie-runtime.so" "$depot/libboundscheck.so" \
        >"$out/products.sha256"
    stat -c '%y %n' "$depot/libcangjie-runtime.so" "$depot/libboundscheck.so" \
        >"$out/products.stat"
    sed -n -E '/^(CJ_SDK_VERSION|DISABLE_VERSION_CHECK|MRT_GC_UNIT_TESTS|MRT_TESTABLE_INTERNALS|CMAKE_BUILD_TYPE):/p' \
        "$build/CMakeCache.txt" >"$out/configuration.cache"
    nm --defined-only "$depot/libcangjie-runtime.so" \
        | /usr/bin/grep 'CJ_MRT_SetLargeArrayInitTestHooks' >"$out/hook-symbol.txt"
    printf '%s\n' "${PIPESTATUS[1]}" >"$out/hook-symbol.rc"
}

build_arm on ON ON || exit $?
cp "$GREEN_ELF" /root/sodepot/impl_gcunit3/r2-dual-on/cj_gc_unit
sha256sum /root/sodepot/impl_gcunit3/r2-dual-on/cj_gc_unit >"$ROOT_OUT/on/test-elf.sha256"
LD_LIBRARY_PATH=/root/sodepot/impl_gcunit3/r2-dual-on \
    ldd /root/sodepot/impl_gcunit3/r2-dual-on/cj_gc_unit >"$ROOT_OUT/on/loader.txt"
printf 'test\trc\n' >"$ROOT_OUT/on/results.tsv"
for test_name in "${tests[@]}"; do
    safe_name=${test_name//./_}
    set +e
    LD_LIBRARY_PATH=/root/sodepot/impl_gcunit3/r2-dual-on taskset -c "$CORES" \
        /root/sodepot/impl_gcunit3/r2-dual-on/cj_gc_unit "--gtest_filter=$test_name" \
        >"$ROOT_OUT/on/${safe_name}.log" 2>&1
    test_rc=$?
    set -e
    printf '%s\n' "$test_rc" >"$ROOT_OUT/on/${safe_name}.rc"
    printf '%s\t%s\n' "$test_name" "$test_rc" >>"$ROOT_OUT/on/results.tsv"
done

build_arm off OFF OFF || exit $?
{
    date -Ins
    uptime
    taskset -pc $$
} >"$ROOT_OUT/postflight.log" 2>&1
touch "$ROOT_OUT/R2_DUAL_CONFIG_DONE"
