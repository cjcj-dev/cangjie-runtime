#!/usr/bin/env bash
set -u

if [[ $# -ne 5 ]]; then
  echo "usage: $0 <runtime-root> <evidence-dir> <cut-name> <cmake-target> <patch>" >&2
  exit 2
fi

ROOT=$1
EVIDENCE=$2
CUT=$3
TARGET=$4
PATCH_FILE=$5
CUT_DIR="$EVIDENCE/$CUT"
PRODUCT_DIR="$CUT_DIR/product"
RESTORED_PRODUCT_DIR="$CUT_DIR/restored_product"
TEST_ELF="$EVIDENCE/T2/cj_gc_unit"
LINK_SCRIPT="$ROOT/CMakebuild/src/CMakeFiles/cangjie-runtime.dir/link.txt"
TESTS=(
  YoungWeakClosure.SerialDiscoversWithoutStrongReferentClosure
  YoungWeakClosure.LegacyParallelDiscoversWithoutStrongReferentClosure
  YoungWeakClosure.StripedDiscoversWithoutStrongReferentClosure
  YoungWeakClosure.WeakRemsetSlotFlowsFromClosureToConsumer
  YoungWeakClosure.CommonMajorRootUsesWeakDiscoveryPolicy
  YoungWeakClosure.ExportMajorRootUsesWeakDiscoveryPolicy
  YoungWeakClosure.ExportOnlyMajorRootOwnsItsClosure
)

mkdir -p "$PRODUCT_DIR" "$RESTORED_PRODUCT_DIR"
applied=0
restore_product() {
  if [[ $applied -eq 0 ]]; then
    return
  fi
  patch -R -p1 -d "$(dirname "$ROOT")" <"$PATCH_FILE" >"$CUT_DIR/restore.patch.log" 2>&1
  patch_rc=$?
  GC_UNIT_GATE_SKIP=1 cmake --build "$ROOT/CMakebuild" --target "$TARGET" -j16 \
    >"$CUT_DIR/restore.build.log" 2>&1
  build_rc=$?
  (cd "$ROOT/CMakebuild/src" && cmake -E cmake_link_script "$LINK_SCRIPT" --verbose=0) \
    >"$CUT_DIR/restore.link.log" 2>&1
  link_rc=$?
  printf 'PATCH_RC=%d BUILD_RC=%d LINK_RC=%d\n' "$patch_rc" "$build_rc" "$link_rc" \
    >"$CUT_DIR/restore.rc"
  applied=0
}
trap restore_product EXIT

patch -p1 -d "$(dirname "$ROOT")" <"$PATCH_FILE" >"$CUT_DIR/cut.patch.log" 2>&1
cut_patch_rc=$?
if [[ $cut_patch_rc -ne 0 ]]; then
  echo "CUT_PATCH_RC=$cut_patch_rc" >"$CUT_DIR/cut.rc"
  exit 3
fi
applied=1

GC_UNIT_GATE_SKIP=1 cmake --build "$ROOT/CMakebuild" --target "$TARGET" -j16 \
  >"$CUT_DIR/cut.build.log" 2>&1
cut_build_rc=$?
if [[ $cut_build_rc -ne 0 ]]; then
  echo "CUT_BUILD_RC=$cut_build_rc" >"$CUT_DIR/cut.rc"
  exit 4
fi
(cd "$ROOT/CMakebuild/src" && cmake -E cmake_link_script "$LINK_SCRIPT" --verbose=0) \
  >"$CUT_DIR/cut.link.log" 2>&1
cut_link_rc=$?
if [[ $cut_link_rc -ne 0 ]]; then
  echo "CUT_LINK_RC=$cut_link_rc" >"$CUT_DIR/cut.rc"
  exit 5
fi

cp -a "$ROOT/output/temp/lib/x86_64_Release/libcangjie-runtime.so" "$PRODUCT_DIR/"
cp -a "$ROOT/output/temp/lib/x86_64_Release/libboundscheck.so" "$PRODUCT_DIR/"
sha256sum "$PRODUCT_DIR/libcangjie-runtime.so" "$PRODUCT_DIR/libboundscheck.so" >"$CUT_DIR/product.sha256"
sha256sum "$TEST_ELF" >"$CUT_DIR/test_elf.sha256"

: >"$CUT_DIR/grid.log"
for test_name in "${TESTS[@]}"; do
  echo "GRID_START $test_name" >>"$CUT_DIR/grid.log"
  LD_LIBRARY_PATH="$PRODUCT_DIR" "$TEST_ELF" --gtest_filter="$test_name" >>"$CUT_DIR/grid.log" 2>&1
  one_rc=$?
  echo "GRID_RC $test_name $one_rc" >>"$CUT_DIR/grid.log"
done

restore_product
trap - EXIT
cp -a "$ROOT/output/temp/lib/x86_64_Release/libcangjie-runtime.so" "$RESTORED_PRODUCT_DIR/"
cp -a "$ROOT/output/temp/lib/x86_64_Release/libboundscheck.so" "$RESTORED_PRODUCT_DIR/"
sha256sum "$RESTORED_PRODUCT_DIR/libcangjie-runtime.so" \
  "$RESTORED_PRODUCT_DIR/libboundscheck.so" >"$CUT_DIR/restored_product.sha256"
: >"$CUT_DIR/restored.grid.log"
for test_name in "${TESTS[@]}"; do
  echo "GRID_START $test_name" >>"$CUT_DIR/restored.grid.log"
  LD_LIBRARY_PATH="$RESTORED_PRODUCT_DIR" "$TEST_ELF" --gtest_filter="$test_name" \
    >>"$CUT_DIR/restored.grid.log" 2>&1
  one_rc=$?
  echo "GRID_RC $test_name $one_rc" >>"$CUT_DIR/restored.grid.log"
done
echo "CUT_DONE name=$CUT" >"$CUT_DIR/cut.rc"
