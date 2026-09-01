#!/usr/bin/env bash
set -uo pipefail

readonly REPO=/root/impl_gcunit3-aa48-run
readonly BUILD="$REPO/runtime/CMakebuild_gcunit3"
readonly LIVE_LIB="$REPO/runtime/output/temp/lib/x86_64_Release"
readonly GREEN=/root/sodepot/impl_gcunit3/t11-t12-candidate
readonly CUT=/root/sodepot/impl_gcunit3/t11-rootfix-cut
readonly RESTORED=/root/sodepot/impl_gcunit3/t11-rootfix-restored
readonly EVIDENCE="$REPO/evidence/impl_gcunit3/t11-cut-restore"
readonly SOURCE="$REPO/runtime/src/Heap/Collector/Relocate.cpp"
readonly RESTORE_SOURCE=/root/Relocate.impl_gcunit3.restored.cpp
readonly CORES=16-31

mkdir -p "$CUT" "$RESTORED" "$EVIDENCE"
git -C "$REPO" diff -- runtime/src/Heap/Collector/Relocate.cpp >"$EVIDENCE/rootfix-product-cut.diff"
sha256sum "$SOURCE" "$RESTORE_SOURCE" >"$EVIDENCE/source-before-restore.sha256"

cd "$REPO/runtime" || exit $?
GC_UNIT_GATE_SKIP=1 taskset -c "$CORES" cmake --build "$BUILD" \
    --target cangjie-runtime --parallel 16 >"$EVIDENCE/cut-build.log" 2>&1
cut_build_rc=$?
printf '%s\n' "$cut_build_rc" >"$EVIDENCE/cut-build.rc"
if [[ $cut_build_rc -ne 0 ]]; then
    cp "$RESTORE_SOURCE" "$SOURCE"
    exit "$cut_build_rc"
fi
cp "$LIVE_LIB/libcangjie-runtime.so" "$CUT/"
cp "$LIVE_LIB/libboundscheck.so" "$CUT/"
sha256sum "$CUT/libcangjie-runtime.so" "$CUT/libboundscheck.so" "$GREEN/cj_gc_unit" \
    >"$EVIDENCE/cut-products.sha256"
LD_LIBRARY_PATH="$CUT" ldd "$GREEN/cj_gc_unit" >"$EVIDENCE/cut-loader.txt"

tests=(
    M0Exit.RootFixRuntimeEnumerationClassifiesNoCopyAsS0
    M0Exit.RootFixClassifiesActiveOnlyUnusableCopyAsS1
    M0Exit.RootFixClassifiesRetiredOnlyUnusableCopyAsS1
    M0Exit.ReadRuntimeEntryFailsClosedOnForwardedWithoutMapping
)
printf 'arm\ttest\trc\n' >"$EVIDENCE/results.tsv"
for test_name in "${tests[@]}"; do
    safe_name=${test_name//./_}
    set +e
    LD_LIBRARY_PATH="$CUT" taskset -c "$CORES" \
        "$GREEN/cj_gc_unit" "--gtest_filter=$test_name" \
        >"$EVIDENCE/cut-${safe_name}.log" 2>&1
    test_rc=$?
    set -e
    printf 'cut\t%s\t%s\n' "$test_name" "$test_rc" >>"$EVIDENCE/results.tsv"
done

cp "$RESTORE_SOURCE" "$SOURCE"
sha256sum "$SOURCE" "$RESTORE_SOURCE" >"$EVIDENCE/source-after-restore.sha256"
git -C "$REPO" status --short -- runtime/src/Heap/Collector/Relocate.cpp \
    >"$EVIDENCE/source-after-restore.status"

GC_UNIT_GATE_SKIP=1 taskset -c "$CORES" cmake --build "$BUILD" \
    --target cangjie-runtime --parallel 16 >"$EVIDENCE/restored-build.log" 2>&1
restored_build_rc=$?
printf '%s\n' "$restored_build_rc" >"$EVIDENCE/restored-build.rc"
if [[ $restored_build_rc -ne 0 ]]; then
    exit "$restored_build_rc"
fi
cp "$LIVE_LIB/libcangjie-runtime.so" "$RESTORED/"
cp "$LIVE_LIB/libboundscheck.so" "$RESTORED/"
sha256sum "$RESTORED/libcangjie-runtime.so" "$RESTORED/libboundscheck.so" "$GREEN/cj_gc_unit" \
    >"$EVIDENCE/restored-products.sha256"
LD_LIBRARY_PATH="$RESTORED" ldd "$GREEN/cj_gc_unit" >"$EVIDENCE/restored-loader.txt"

for test_name in "${tests[@]}"; do
    safe_name=${test_name//./_}
    set +e
    LD_LIBRARY_PATH="$RESTORED" taskset -c "$CORES" \
        "$GREEN/cj_gc_unit" "--gtest_filter=$test_name" \
        >"$EVIDENCE/restored-${safe_name}.log" 2>&1
    test_rc=$?
    set -e
    printf 'restored\t%s\t%s\n' "$test_name" "$test_rc" >>"$EVIDENCE/results.tsv"
done

{
    date -Ins
    uptime
    taskset -pc $$
    git -C "$REPO" status --short -- runtime/src/Heap/Collector/Relocate.cpp
} >"$EVIDENCE/postflight.log" 2>&1
touch "$EVIDENCE/T11_CUT_RESTORE_DONE"
