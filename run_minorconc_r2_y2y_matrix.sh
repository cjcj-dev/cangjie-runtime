#!/usr/bin/env bash
set -uo pipefail

REPO=${REPO:?need r2 candidate repo}
TEST_T0=${TEST_T0:?need baseline cj_gc_unit}
EVIDENCE=${EVIDENCE:?need evidence dir}
Y2Y_CUT=${Y2Y_CUT:?need y2y discard cut}
TEST_CUT=${TEST_CUT:?need benign test cut}
CORES=${CORES:-0-15}
TEST_NAME=YoungConc.Y2yAfterReleaseBatchForcesContinueAndReachesClosure
PRODUCT_OUT="$REPO/runtime/output/temp/lib/x86_64_Release"
BUILD_DIR="$REPO/runtime/CMakebuild_r2_clang"
MATRIX="$EVIDENCE/y2y_matrix"
mkdir -p "$MATRIX/P0" "$MATRIX/P1" "$MATRIX/Pr"
uptime > "$MATRIX/uptime.before"

run_cell()
{
  label=$1
  test_elf=$2
  product_dir=$3
  start=$(date -Ins)
  env LD_LIBRARY_PATH="$product_dir" taskset -c "$CORES" timeout 120 \
    "$test_elf" "--gtest_filter=$TEST_NAME" > "$MATRIX/$label.out" 2> "$MATRIX/$label.err"
  rc=$?
  end=$(date -Ins)
  {
    printf 'cell=%s\nstart=%s\nend=%s\nrc=%s\n' "$label" "$start" "$end" "$rc"
    sha256sum "$test_elf" "$product_dir/libcangjie-runtime.so" "$product_dir/libboundscheck.so"
  } > "$MATRIX/$label.meta"
  env LD_LIBRARY_PATH="$product_dir" ldd "$test_elf" > "$MATRIX/$label.ldd"
}

restore_product()
{
  git -C "$REPO" apply -R "$Y2Y_CUT" 2>/dev/null || true
  cmake --build "$BUILD_DIR" --target WCollector -j16 > "$MATRIX/Pr/build.log" 2>&1
  (cd "$BUILD_DIR/src" && /usr/bin/cmake -E cmake_link_script \
    CMakeFiles/cangjie-runtime.dir/link.txt --verbose=1) >> "$MATRIX/Pr/build.log" 2>&1
}
trap restore_product EXIT

cp "$PRODUCT_OUT/libcangjie-runtime.so" "$PRODUCT_OUT/libboundscheck.so" "$MATRIX/P0/"
run_cell T0P0 "$TEST_T0" "$MATRIX/P0"

git -C "$REPO" apply "$Y2Y_CUT"
cmake --build "$BUILD_DIR" --target WCollector -j16 > "$MATRIX/P1/build.log" 2>&1
(cd "$BUILD_DIR/src" && /usr/bin/cmake -E cmake_link_script \
  CMakeFiles/cangjie-runtime.dir/link.txt --verbose=1) >> "$MATRIX/P1/build.log" 2>&1
cp "$PRODUCT_OUT/libcangjie-runtime.so" "$PRODUCT_OUT/libboundscheck.so" "$MATRIX/P1/"
run_cell T0P1 "$TEST_T0" "$MATRIX/P1"

git -C "$REPO" apply -R "$Y2Y_CUT"
cmake --build "$BUILD_DIR" --target WCollector -j16 > "$MATRIX/Pr/build.log" 2>&1
(cd "$BUILD_DIR/src" && /usr/bin/cmake -E cmake_link_script \
  CMakeFiles/cangjie-runtime.dir/link.txt --verbose=1) >> "$MATRIX/Pr/build.log" 2>&1
cp "$PRODUCT_OUT/libcangjie-runtime.so" "$PRODUCT_OUT/libboundscheck.so" "$MATRIX/Pr/"
run_cell TrPr "$TEST_T0" "$MATRIX/Pr"
trap - EXIT

# T1 is a benign test-side rebuild. Its source-only cut changes a DETAIL token
# outside TEST_NAME; fixed P0 must therefore stay green.
T1_REPO="${T1_REPO:-$EVIDENCE/y2y_t1_repo}"
cp -a --reflink=auto "$REPO" "$T1_REPO"
git -C "$T1_REPO" apply "$TEST_CUT"
env MRT_TESTABLE_INTERNALS=1 GCV2_RUNTIME_LIB_DIR="$MATRIX/P0" \
  GC_UNIT_OUT="$MATRIX/T1_build" bash "$T1_REPO/runtime/tests/gc_unit/run_standalone.sh" \
  > "$MATRIX/T1_build.log" 2>&1 || true
run_cell T1P0 "$MATRIX/T1_build/cj_gc_unit" "$MATRIX/P0"

{
  printf 'cell\trc\ttest_sha\tproduct_sha\tevidence\n'
  for label in T0P0 T0P1 T1P0 TrPr; do
    meta="$MATRIX/$label.meta"
    rc=$(sed -n 's/^rc=//p' "$meta")
    test_sha=$(tail -3 "$meta" | sed -n '1s/ .*//p')
    product_sha=$(tail -3 "$meta" | sed -n '2s/ .*//p')
    printf '%s\t%s\t%s\t%s\t%s\n' "$label" "$rc" "$test_sha" "$product_sha" "$meta"
  done
} > "$MATRIX/results.tsv"
uptime > "$MATRIX/uptime.after"
sha256sum "$MATRIX"/*.meta "$MATRIX"/*.ldd "$MATRIX"/*.out "$MATRIX"/*.err > "$MATRIX/SHA256SUMS"

[[ $(sed -n '2s/^[^[:space:]]*[[:space:]]\([^[:space:]]*\).*/\1/p' "$MATRIX/results.tsv") == 0 ]]
[[ $(sed -n '3s/^[^[:space:]]*[[:space:]]\([^[:space:]]*\).*/\1/p' "$MATRIX/results.tsv") != 0 ]]
[[ $(sed -n '4s/^[^[:space:]]*[[:space:]]\([^[:space:]]*\).*/\1/p' "$MATRIX/results.tsv") == 0 ]]
[[ $(sed -n '5s/^[^[:space:]]*[[:space:]]\([^[:space:]]*\).*/\1/p' "$MATRIX/results.tsv") == 0 ]]
