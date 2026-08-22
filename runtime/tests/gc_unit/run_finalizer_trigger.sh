#!/usr/bin/env bash
# End-to-end half of the FnlzRoots contract. The C++ test checks root
# classification; this program proves the language-visible ~init consequence.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SRC="$ROOT/runtime/tests/gc_unit/finalizer_trigger.cj"
OUT="${GC_UNIT_OUT:-$ROOT/runtime/tests/gc_unit/build_standalone}"
RUNTIME_LIB_DIR="${GCV2_RUNTIME_LIB_DIR:?set GCV2_RUNTIME_LIB_DIR}"
CJC_BIN="${CJC:-${CANGJIE_HOME:-}/bin/cjc}"

if [[ ! -x "$CJC_BIN" ]]; then
  echo "FINALIZER_TRIGGER_FAIL: no matching cjc (set CJC or CANGJIE_HOME)" >&2
  exit 2
fi
if [[ ! -f "$RUNTIME_LIB_DIR/libcangjie-runtime.so" ]]; then
  echo "FINALIZER_TRIGGER_FAIL: missing product runtime in $RUNTIME_LIB_DIR" >&2
  exit 2
fi

mkdir -p "$OUT"
BIN="$OUT/finalizer_trigger"
BUILD_LOG="$OUT/finalizer_trigger.build.log"
RUN_LOG="$OUT/finalizer_trigger.run.log"
SDK_RUNTIME="${CANGJIE_HOME:-}/runtime/lib/linux_x86_64_cjnative"
SDK_TOOLS="${CANGJIE_HOME:-}/tools/lib"
SDK_LLVM="${CANGJIE_HOME:-}/third_party/llvm/lib"

LD_LIBRARY_PATH="$RUNTIME_LIB_DIR:$SDK_RUNTIME:$SDK_TOOLS:$SDK_LLVM${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  "$CJC_BIN" "$SRC" -O0 --static-std -o "$BIN" >"$BUILD_LOG" 2>&1

set +e
LD_LIBRARY_PATH="$RUNTIME_LIB_DIR:$SDK_RUNTIME${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  MRT_LOG_LEVEL=e cjHeapSize=256MB timeout 60s "$BIN" >"$RUN_LOG" 2>&1
rc=$?
set -e

finalized=$(grep -c '^FINALIZER_CALLED id=' "$RUN_LOG" || true)
done_count=$(grep -c '^FINALIZER_TRIGGER_DONE allocated=64$' "$RUN_LOG" || true)
if [[ $rc -ne 0 || $done_count -ne 1 || $finalized -ne 64 ]]; then
  echo "FINALIZER_TRIGGER_FAIL rc=$rc done=$done_count finalized=$finalized expected=64" >&2
  tail -20 "$RUN_LOG" >&2
  exit 1
fi

echo "FINALIZER_TRIGGER_OK rc=0 finalized=$finalized expected=64"
