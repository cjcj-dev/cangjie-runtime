#!/usr/bin/env bash
# Managed construction plus externally driven native full/young init windows.
# The native windows do not certify a managed-stack initialization window.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SRC="$ROOT/runtime/tests/gc_unit/segmented_array_managed.cj"
OUT="${GC_UNIT_OUT:-$ROOT/runtime/tests/gc_unit/build_standalone}"
RUNTIME_LIB_DIR="${GCV2_RUNTIME_LIB_DIR:?set GCV2_RUNTIME_LIB_DIR}"
source "$(dirname "$0")/language_toolchain.sh"
gc_unit_language_admit
gc_unit_language_environment
MODE="${1:-construct}"

case "$MODE" in
  both|full|young|construct) ;;
  *)
    echo "usage: $0 [construct|both|full|young]" >&2
    exit 2
    ;;
esac

if [[ ! -f "$RUNTIME_LIB_DIR/libcangjie-runtime.so" ]]; then
  echo "SEGMENTED_ARRAY_MANAGED_FAIL: missing product runtime in $RUNTIME_LIB_DIR" >&2
  exit 2
fi

mkdir -p "$OUT"
BIN="$OUT/segmented_array_managed"
BUILD_LOG="$OUT/segmented_array_managed.build.log"
FULL_LOG="$OUT/segmented_array_managed.full.log"
YOUNG_LOG="$OUT/segmented_array_managed.young.log"
SDK_RUNTIME="${CANGJIE_HOME:-}/runtime/lib/linux_x86_64_cjnative"
SDK_TOOLS="${CANGJIE_HOME:-}/tools/lib"
SDK_LLVM="${CANGJIE_HOME:-}/third_party/llvm/lib"

LD_LIBRARY_PATH="${GC_UNIT_CJC_RUNTIME_LIB_DIR:?set GC_UNIT_CJC_RUNTIME_LIB_DIR to the compiler host runtime}:$SDK_TOOLS:$SDK_LLVM${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  gc_unit_language_compile "$SRC" -O0 --static-std -o "$BIN" >"$BUILD_LOG" 2>&1

# The language fixture retains its real compiler allocation/checksum coverage.
set +e
LD_LIBRARY_PATH="$RUNTIME_LIB_DIR:$SDK_RUNTIME${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  MRT_LOG_LEVEL=e cjGCInterval=3600s cjHeapSize=64MB \
  timeout 60s "$BIN" >"$OUT/segmented_array_managed.construct.log" 2>&1
construct_rc=$?
set -e
test "$construct_rc" = 0
/usr/bin/grep -q '^SEGMENTED_ARRAY_MANAGED_FIXTURE_OK checksum=37$' "$OUT/segmented_array_managed.construct.log"
echo "SEGMENTED_ARRAY_CONSTRUCT_OK rc=$construct_rc"

# The product has no test-mode GC request or fprintf hook. Consume the native
# external fixture built by the same standalone invocation and selected SO.
# Controller authorization: #1318 advisor 20260929T185603Z.
run_external_window() {
  local mode=$1 filter=$2 log=$3
  local elf="$OUT/cj_gc_unit"
  if [[ ! -x "$elf" ]]; then
    echo "SEGMENTED_EXTERNAL_FIXTURE_MISSING elf=$elf" >&2
    return 2
  fi
  local rc
  set +e
  LD_LIBRARY_PATH="$RUNTIME_LIB_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    MRT_LOG_LEVEL=e cjGCInterval=3600s \
    timeout 60s "$elf" --gtest_filter="$filter" >"$log" 2>&1
  rc=$?
  set -e
  test "$rc" = 0
  /usr/bin/grep -q 'SEGMENTED_RESULT_TARGET .*mismatches=0 .*gc=1 header=1' "$log"
  /usr/bin/grep -q 'SEGMENTED_PASSES=2' "$log"
  echo "SEGMENTED_ARRAY_NATIVE_WINDOW_OK mode=$mode rc=$rc"
}
if [[ "$MODE" == both || "$MODE" == full ]]; then
  run_external_window full SegmentedArrayInit.EpochFlipRestartsAndRewritesPublishedBlock "$FULL_LOG"
fi
if [[ "$MODE" == both || "$MODE" == young ]]; then
  run_external_window young SegmentedArrayInit.YoungGcRepairsIncompleteArrayRoot "$YOUNG_LOG"
fi
