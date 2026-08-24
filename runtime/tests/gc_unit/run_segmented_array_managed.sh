#!/usr/bin/env bash
# End-to-end managed/full and managed/young segmented-array root branches.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SRC="$ROOT/runtime/tests/gc_unit/segmented_array_managed.cj"
OUT="${GC_UNIT_OUT:-$ROOT/runtime/tests/gc_unit/build_standalone}"
RUNTIME_LIB_DIR="${GCV2_RUNTIME_LIB_DIR:?set GCV2_RUNTIME_LIB_DIR}"
CJC_BIN="${CJC:-${CANGJIE_HOME:-}/bin/cjc}"
MODE="${1:-both}"

case "$MODE" in
  both|full|young) ;;
  *)
    echo "usage: $0 [both|full|young]" >&2
    exit 2
    ;;
esac

if [[ ! -x "$CJC_BIN" ]]; then
  echo "SEGMENTED_ARRAY_MANAGED_FAIL: no matching cjc (set CJC or CANGJIE_HOME)" >&2
  exit 2
fi
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

LD_LIBRARY_PATH="$RUNTIME_LIB_DIR:$SDK_RUNTIME:$SDK_TOOLS:$SDK_LLVM${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  "$CJC_BIN" "$SRC" -O0 --static-std -o "$BIN" >"$BUILD_LOG" 2>&1

if [[ "$MODE" == both || "$MODE" == full ]]; then
  set +e
  LD_LIBRARY_PATH="$RUNTIME_LIB_DIR:$SDK_RUNTIME${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    MRT_GC_UNIT_MANAGED_SEGMENTED=full MRT_GCV2_CONCURRENT_STACK_SCAN=1 \
    MRT_LOG_LEVEL=e cjGCInterval=3600s cjHeapSize=64MB \
    timeout 60s "$BIN" >"$FULL_LOG" 2>&1
  full_rc=$?
  set -e
  full_branch=$(/usr/bin/grep -c '\[SEGMENTED_MANAGED_OK\] mode=full ' "$FULL_LOG" || true)
  full_done=$(/usr/bin/grep -c '^SEGMENTED_ARRAY_MANAGED_FIXTURE_OK checksum=37$' "$FULL_LOG" || true)
  if [[ $full_rc -ne 0 || $full_branch -ne 1 || $full_done -ne 1 ]]; then
    echo "SEGMENTED_ARRAY_MANAGED_FAIL mode=full rc=$full_rc branch=$full_branch done=$full_done" >&2
    tail -30 "$FULL_LOG" >&2
    exit 1
  fi
  echo "SEGMENTED_ARRAY_MANAGED_OK mode=full rc=0 branch=1 done=1"
fi

if [[ "$MODE" == both || "$MODE" == young ]]; then
  set +e
  LD_LIBRARY_PATH="$RUNTIME_LIB_DIR:$SDK_RUNTIME${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    MRT_GC_UNIT_MANAGED_SEGMENTED=young MRT_LOG_LEVEL=e cjGCInterval=3600s cjHeapSize=64MB \
    timeout 60s "$BIN" >"$YOUNG_LOG" 2>&1
  young_rc=$?
  set -e
  young_branch=$(/usr/bin/grep -c '\[SEGMENTED_MANAGED_OK\] mode=young ' "$YOUNG_LOG" || true)
  young_done=$(/usr/bin/grep -c '^SEGMENTED_ARRAY_MANAGED_FIXTURE_OK checksum=37$' "$YOUNG_LOG" || true)
  if [[ $young_rc -ne 0 || $young_branch -ne 1 || $young_done -ne 1 ]]; then
    echo "SEGMENTED_ARRAY_MANAGED_FAIL mode=young rc=$young_rc branch=$young_branch done=$young_done" >&2
    tail -30 "$YOUNG_LOG" >&2
    exit 1
  fi
  echo "SEGMENTED_ARRAY_MANAGED_OK mode=young rc=0 branch=1 done=1"
fi
