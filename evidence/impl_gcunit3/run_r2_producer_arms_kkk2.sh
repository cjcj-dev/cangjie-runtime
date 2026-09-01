#!/usr/bin/env bash
set -uo pipefail

readonly REPO=/root/impl_gcunit3-aa48-run
readonly BUILD="$REPO/runtime/CMakebuild_gcunit3"
readonly LIVE_LIB="$REPO/runtime/output/temp/lib/x86_64_Release"
readonly GREEN=/root/sodepot/impl_gcunit3/r2-green-final
readonly ROOT_OUT="$REPO/evidence/impl_gcunit3/r2-producer-arms-final"
readonly BACKUP=/root/impl_gcunit3-r2-producer-backup
readonly CORES=16-31

readonly MUTATOR="$REPO/runtime/src/Mutator/Mutator.cpp"
readonly RELOCATE="$REPO/runtime/src/Heap/Collector/Relocate.cpp"
readonly BASE_OBJECT="$REPO/runtime/src/Common/BaseObject.cpp"
readonly MARRAY="$REPO/runtime/src/ObjectModel/MArray.cpp"

tests=(
    M0Exit.RootFixRuntimeEnumerationClassifiesNoCopyAsS0
    M0Exit.RootFixClassifiesActiveOnlyUnusableCopyAsS1
    M0Exit.RootFixClassifiesRetiredOnlyUnusableCopyAsS1
    SegmentedArrayInit.EpochFlipRestartsAndRewritesPublishedBlock
    SegmentedArrayInit.EpochFlipRestartsAndRewritesPublishedBlockParallel
    SegmentedArrayInit.YoungGcRepairsIncompleteArrayRoot
    SegmentedArrayInit.YoungGcRepairsIncompleteArrayRootParallel
)

mkdir -p "$ROOT_OUT" "$BACKUP"
rm -f "$ROOT_OUT/R2_PRODUCER_ARMS_DONE"
cp "$MUTATOR" "$BACKUP/Mutator.cpp"
cp "$RELOCATE" "$BACKUP/Relocate.cpp"
cp "$BASE_OBJECT" "$BACKUP/BaseObject.cpp"
cp "$MARRAY" "$BACKUP/MArray.cpp"

restore_sources()
{
    cp "$BACKUP/Mutator.cpp" "$MUTATOR"
    cp "$BACKUP/Relocate.cpp" "$RELOCATE"
    cp "$BACKUP/BaseObject.cpp" "$BASE_OBJECT"
    cp "$BACKUP/MArray.cpp" "$MARRAY"
}

run_tests()
{
    local libdir=$1
    local out=$2
    printf 'test\trc\n' >"$out/results.tsv"
    for test_name in "${tests[@]}"; do
        local safe_name=${test_name//./_}
        set +e
        LD_LIBRARY_PATH="$libdir" taskset -c "$CORES" \
            "$libdir/cj_gc_unit" "--gtest_filter=$test_name" \
            >"$out/${safe_name}.log" 2>&1
        local test_rc=$?
        printf '%s\n' "$test_rc" >"$out/${safe_name}.rc"
        printf '%s\t%s\n' "$test_name" "$test_rc" >>"$out/results.tsv"
    done
}

run_arm()
{
    local name=$1
    local patch=$2
    local out="$ROOT_OUT/$name"
    local depot="/root/sodepot/impl_gcunit3/r2-final-$name"
    mkdir -p "$out" "$depot"
    restore_sources
    if ! git -C "$REPO" apply "$patch"; then
        printf '1\n' >"$out/apply.rc"
        restore_sources
        return 1
    fi
    printf '0\n' >"$out/apply.rc"
    git -C "$REPO" diff -- runtime/src/Mutator/Mutator.cpp \
        runtime/src/Heap/Collector/Relocate.cpp runtime/src/Common/BaseObject.cpp \
        runtime/src/ObjectModel/MArray.cpp >"$out/cut.diff"
    sha256sum "$MUTATOR" "$RELOCATE" "$BASE_OBJECT" "$MARRAY" >"$out/cut-source.sha256"
    stat -c '%y %n' "$MUTATOR" "$RELOCATE" "$BASE_OBJECT" "$MARRAY" >"$out/cut-source.stat"
    {
        date -Ins
        uptime
        taskset -pc $$
    } >"$out/preflight.log" 2>&1

    cd "$REPO/runtime" || return $?
    GC_UNIT_GATE_SKIP=1 taskset -c "$CORES" cmake --build "$BUILD" \
        --target cangjie-runtime --parallel 16 >"$out/build.log" 2>&1
    local build_rc=$?
    printf '%s\n' "$build_rc" >"$out/build.rc"
    if [[ $build_rc -ne 0 ]]; then
        restore_sources
        return "$build_rc"
    fi
    cp "$LIVE_LIB/libcangjie-runtime.so" "$depot/"
    cp "$GREEN/libboundscheck.so" "$depot/"
    cp "$GREEN/cj_gc_unit" "$depot/"
    sha256sum "$depot/libcangjie-runtime.so" "$depot/libboundscheck.so" \
        "$depot/cj_gc_unit" >"$out/products.sha256"
    stat -c '%y %n' "$depot/libcangjie-runtime.so" "$depot/libboundscheck.so" \
        "$depot/cj_gc_unit" >"$out/products.stat"
    LD_LIBRARY_PATH="$depot" ldd "$depot/cj_gc_unit" >"$out/loader.txt"
    run_tests "$depot" "$out"
    {
        date -Ins
        uptime
        taskset -pc $$
    } >"$out/postflight.log" 2>&1
}

{
    date -Ins
    uptime
    taskset -pc $$
    sha256sum "$MUTATOR" "$RELOCATE" "$BASE_OBJECT" "$MARRAY"
} >"$ROOT_OUT/preflight.log" 2>&1

run_arm cut-mutator-native /root/cut-mutator-native.patch || exit $?
run_arm cut-watermark-native /root/cut-watermark-native.patch || exit $?
run_arm cut-minor-relocate /root/cut-minor-relocate.patch || exit $?
run_arm cut-iterator-skip /root/cut-iterator-skip.patch || exit $?

restore_sources
sha256sum "$MUTATOR" "$RELOCATE" "$BASE_OBJECT" "$MARRAY" >"$ROOT_OUT/restored-source.sha256"
git -C "$REPO" status --short -- runtime/src/Mutator/Mutator.cpp \
    runtime/src/Heap/Collector/Relocate.cpp runtime/src/Common/BaseObject.cpp \
    runtime/src/ObjectModel/MArray.cpp >"$ROOT_OUT/restored-source.status"

readonly RESTORED=/root/sodepot/impl_gcunit3/r2-final-restored
readonly RESTORED_OUT="$ROOT_OUT/restored"
mkdir -p "$RESTORED" "$RESTORED_OUT"
cd "$REPO/runtime" || exit $?
GC_UNIT_GATE_SKIP=1 taskset -c "$CORES" cmake --build "$BUILD" \
    --target cangjie-runtime --parallel 16 >"$RESTORED_OUT/build.log" 2>&1
restored_build_rc=$?
printf '%s\n' "$restored_build_rc" >"$RESTORED_OUT/build.rc"
if [[ $restored_build_rc -ne 0 ]]; then
    exit "$restored_build_rc"
fi
cp "$LIVE_LIB/libcangjie-runtime.so" "$RESTORED/"
cp "$GREEN/libboundscheck.so" "$RESTORED/"
cp "$GREEN/cj_gc_unit" "$RESTORED/"
sha256sum "$RESTORED/libcangjie-runtime.so" "$RESTORED/libboundscheck.so" \
    "$RESTORED/cj_gc_unit" >"$RESTORED_OUT/products.sha256"
stat -c '%y %n' "$RESTORED/libcangjie-runtime.so" "$RESTORED/libboundscheck.so" \
    "$RESTORED/cj_gc_unit" >"$RESTORED_OUT/products.stat"
run_tests "$RESTORED" "$RESTORED_OUT"

{
    date -Ins
    uptime
    taskset -pc $$
    sha256sum "$GREEN/libcangjie-runtime.so" "$GREEN/libboundscheck.so" "$GREEN/cj_gc_unit"
    sha256sum "$RESTORED/libcangjie-runtime.so" "$RESTORED/libboundscheck.so" "$RESTORED/cj_gc_unit"
} >"$ROOT_OUT/postflight-and-green-restored.sha256" 2>&1
touch "$ROOT_OUT/R2_PRODUCER_ARMS_DONE"
