#!/bin/bash
set -uo pipefail

BASE=${BASE:?need v6 candidate}
EVIDENCE=${EVIDENCE:?need evidence directory}
RECEIPT_CUT=${RECEIPT_CUT:?need receipt cut patch}
TEST_CUT=${TEST_CUT:?need benign test patch}
CORES=${CORES:-0-15}
P0="$BASE/runtime/output/temp/lib/x86_64_Release"
T0="$BASE/runtime/tests/gc_unit/build_standalone/cj_gc_unit"
CUT_REPO=/root/impl_zgc_minorconc_align_receipt_cut_repo_v6
T1_REPO=/root/impl_zgc_minorconc_align_receipt_t1_repo_v6
MATRIX="$EVIDENCE/receipt_matrix_v6"

mkdir -p "$MATRIX"
uptime > "$MATRIX/uptime.before"

run_cell()
{
    label=$1
    test_elf=$2
    product_dir=$3
    test_name=$4
    start=$(date -Ins)
    env LD_LIBRARY_PATH="$product_dir" taskset -c "$CORES" timeout 120 \
        "$test_elf" "--gtest_filter=$test_name" > "$MATRIX/$label.out" 2> "$MATRIX/$label.err"
    rc=$?
    end=$(date -Ins)
    printf 'label=%s\nstart=%s\nend=%s\nrc=%s\ntest=%s\nproduct=%s\n' \
        "$label" "$start" "$end" "$rc" "$test_elf" "$product_dir" > "$MATRIX/$label.meta"
    sha256sum "$test_elf" "$product_dir/libcangjie-runtime.so" "$product_dir/libboundscheck.so" \
        >> "$MATRIX/$label.meta"
    env LD_LIBRARY_PATH="$product_dir" ldd "$test_elf" > "$MATRIX/$label.ldd"
}

sha256sum "$T0" "$P0/libcangjie-runtime.so" "$P0/libboundscheck.so" > "$MATRIX/T0P0.identity"
nm --defined-only "$T0" > "$MATRIX/T0.full-defined.nm"
nm -u "$T0" > "$MATRIX/T0.undefined.nm"
c++filt < "$MATRIX/T0.full-defined.nm" | /usr/bin/grep -E \
    'WCollector::(DoYoungGarbageCollection|MarkYoungSatbBuffer)' > "$MATRIX/T0.target-defined.txt" || true
c++filt < "$MATRIX/T0.full-defined.nm" | /usr/bin/grep -m 3 -E \
    'MapleRuntime::GcUnit|YoungConc' > "$MATRIX/T0.positive-defined.txt" || true

run_cell T0P0_mark "$T0" "$P0" YoungConc.LateEdgeFollowReceiptReachesYoungMarkConsumer
run_cell T0P0_dispatch "$T0" "$P0" YoungConc.LateEdgeFollowReceiptReachesYoungRuntimeDispatch

cp -a --reflink=auto "$BASE" "$CUT_REPO"
git -C "$CUT_REPO" apply "$RECEIPT_CUT"
git -C "$CUT_REPO" diff --check > "$MATRIX/P1.diff-check.log" 2>&1
git -C "$CUT_REPO" diff > "$MATRIX/P1.diff"
taskset -c "$CORES" cmake --build "$CUT_REPO/runtime/CMakebuild" --target cangjie-runtime -j16 \
    > "$MATRIX/P1.build.log" 2>&1
printf '%s\n' "$?" > "$MATRIX/P1.build.rc"
P1="$CUT_REPO/runtime/output/temp/lib/x86_64_Release"
run_cell T0P1_mark "$T0" "$P1" YoungConc.LateEdgeFollowReceiptReachesYoungMarkConsumer
run_cell T0P1_dispatch "$T0" "$P1" YoungConc.LateEdgeFollowReceiptReachesYoungRuntimeDispatch

cp -a --reflink=auto "$BASE" "$T1_REPO"
git -C "$T1_REPO" apply "$TEST_CUT"
T1_OUT="$T1_REPO/runtime/tests/gc_unit/receipt_matrix_t1"
env GCV2_RUNTIME_LIB_DIR="$P0" GC_UNIT_OUT="$T1_OUT" taskset -c "$CORES" \
    bash "$T1_REPO/runtime/tests/gc_unit/run_standalone.sh" > "$MATRIX/T1.build-run.log" 2>&1
printf '%s\n' "$?" > "$MATRIX/T1.build-run.rc"
T1="$T1_OUT/cj_gc_unit"
run_cell T1P0_mark "$T1" "$P0" YoungConc.LateEdgeFollowReceiptReachesYoungMarkConsumer
run_cell T1P0_dispatch "$T1" "$P0" YoungConc.LateEdgeFollowReceiptReachesYoungRuntimeDispatch

run_cell TrPr_mark "$T0" "$P0" YoungConc.LateEdgeFollowReceiptReachesYoungMarkConsumer
run_cell TrPr_dispatch "$T0" "$P0" YoungConc.LateEdgeFollowReceiptReachesYoungRuntimeDispatch

{
    printf 'cell\trc\ttest_sha\tproduct_sha\n'
    for meta in "$MATRIX"/*.meta; do
        label=$(sed -n 's/^label=//p' "$meta")
        rc=$(sed -n 's/^rc=//p' "$meta")
        test_sha=$(tail -3 "$meta" | sed -n '1s/ .*//p')
        product_sha=$(tail -3 "$meta" | sed -n '2s/ .*//p')
        printf '%s\t%s\t%s\t%s\n' "$label" "$rc" "$test_sha" "$product_sha"
    done
} > "$MATRIX/results.tsv"
uptime > "$MATRIX/uptime.after"
sha256sum "$MATRIX"/* > "$MATRIX/SHA256SUMS"
