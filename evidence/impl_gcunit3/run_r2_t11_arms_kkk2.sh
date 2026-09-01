#!/usr/bin/env bash
set -uo pipefail

readonly REPO=/root/impl_gcunit3-aa48-run
readonly BUILD="$REPO/runtime/CMakebuild_gcunit3"
readonly LIVE_LIB="$REPO/runtime/output/temp/lib/x86_64_Release"
readonly GREEN=/root/sodepot/impl_gcunit3/r2-green
readonly ROOT_OUT="$REPO/evidence/impl_gcunit3/r2-t11-arms"
readonly SOURCE="$REPO/runtime/src/Heap/Collector/Relocate.cpp"
readonly BACKUP=/root/impl_gcunit3-r2-t11-Relocate.cpp
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

mkdir -p "$ROOT_OUT/cut" "$ROOT_OUT/restored"
rm -f "$ROOT_OUT/R2_T11_ARMS_DONE"
cp "$SOURCE" "$BACKUP"
sha256sum "$SOURCE" "$BACKUP" >"$ROOT_OUT/source-before.sha256"
stat -c '%y %n' "$SOURCE" "$BACKUP" >"$ROOT_OUT/source-before.stat"

git -C "$REPO" apply /root/cut-rootfix-consumer.patch
git -C "$REPO" diff -- runtime/src/Heap/Collector/Relocate.cpp >"$ROOT_OUT/cut/cut.diff"
sha256sum "$SOURCE" >"$ROOT_OUT/cut/source.sha256"
stat -c '%y %n' "$SOURCE" >"$ROOT_OUT/cut/source.stat"
{
    date -Ins
    uptime
    taskset -pc $$
} >"$ROOT_OUT/cut/preflight.log" 2>&1

cd "$REPO/runtime" || exit $?
GC_UNIT_GATE_SKIP=1 taskset -c "$CORES" cmake --build "$BUILD" \
    --target cangjie-runtime --parallel 16 >"$ROOT_OUT/cut/build.log" 2>&1
cut_build_rc=$?
printf '%s\n' "$cut_build_rc" >"$ROOT_OUT/cut/build.rc"
if [[ $cut_build_rc -ne 0 ]]; then
    cp "$BACKUP" "$SOURCE"
    exit "$cut_build_rc"
fi
readonly CUT=/root/sodepot/impl_gcunit3/r2-t11-cut
mkdir -p "$CUT"
cp "$LIVE_LIB/libcangjie-runtime.so" "$CUT/"
cp "$LIVE_LIB/libboundscheck.so" "$CUT/"
cp "$GREEN/cj_gc_unit" "$CUT/"
sha256sum "$CUT/libcangjie-runtime.so" "$CUT/libboundscheck.so" "$CUT/cj_gc_unit" \
    >"$ROOT_OUT/cut/products.sha256"
stat -c '%y %n' "$CUT/libcangjie-runtime.so" "$CUT/libboundscheck.so" "$CUT/cj_gc_unit" \
    >"$ROOT_OUT/cut/products.stat"
LD_LIBRARY_PATH="$CUT" ldd "$CUT/cj_gc_unit" >"$ROOT_OUT/cut/loader.txt"
run_tests "$CUT" "$ROOT_OUT/cut"

cp "$BACKUP" "$SOURCE"
sha256sum "$SOURCE" "$BACKUP" >"$ROOT_OUT/source-after.sha256"
git -C "$REPO" status --short -- runtime/src/Heap/Collector/Relocate.cpp \
    >"$ROOT_OUT/source-after.status"

GC_UNIT_GATE_SKIP=1 taskset -c "$CORES" cmake --build "$BUILD" \
    --target cangjie-runtime --parallel 16 >"$ROOT_OUT/restored/build.log" 2>&1
restored_build_rc=$?
printf '%s\n' "$restored_build_rc" >"$ROOT_OUT/restored/build.rc"
if [[ $restored_build_rc -ne 0 ]]; then
    exit "$restored_build_rc"
fi
readonly RESTORED=/root/sodepot/impl_gcunit3/r2-t11-restored
mkdir -p "$RESTORED"
cp "$LIVE_LIB/libcangjie-runtime.so" "$RESTORED/"
cp "$LIVE_LIB/libboundscheck.so" "$RESTORED/"
cp "$GREEN/cj_gc_unit" "$RESTORED/"
sha256sum "$RESTORED/libcangjie-runtime.so" "$RESTORED/libboundscheck.so" \
    "$RESTORED/cj_gc_unit" >"$ROOT_OUT/restored/products.sha256"
stat -c '%y %n' "$RESTORED/libcangjie-runtime.so" "$RESTORED/libboundscheck.so" \
    "$RESTORED/cj_gc_unit" >"$ROOT_OUT/restored/products.stat"
LD_LIBRARY_PATH="$RESTORED" ldd "$RESTORED/cj_gc_unit" >"$ROOT_OUT/restored/loader.txt"
run_tests "$RESTORED" "$ROOT_OUT/restored"

{
    date -Ins
    uptime
    taskset -pc $$
    sha256sum "$GREEN/libcangjie-runtime.so" "$GREEN/libboundscheck.so" "$GREEN/cj_gc_unit"
    sha256sum "$RESTORED/libcangjie-runtime.so" "$RESTORED/libboundscheck.so" "$RESTORED/cj_gc_unit"
} >"$ROOT_OUT/postflight-and-green-restored.sha256" 2>&1
touch "$ROOT_OUT/R2_T11_ARMS_DONE"
