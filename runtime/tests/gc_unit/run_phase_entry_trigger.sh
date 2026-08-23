#!/usr/bin/env bash
# End-to-end major/minor phase-entry half of the forwarding-carrier contract.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
MINOR_SRC="$ROOT/runtime/tests/gc_unit/phase_entry_trigger.cj"
MAJOR_SRC="$ROOT/runtime/tests/gc_unit/phase_entry_major.cj"
OUT="${GC_UNIT_OUT:-$ROOT/runtime/tests/gc_unit/build_standalone}"
RUNTIME_LIB_DIR="${GCV2_RUNTIME_LIB_DIR:?set GCV2_RUNTIME_LIB_DIR}"
CJC_BIN="${CJC:-${CANGJIE_HOME:-}/bin/cjc}"

if [[ ! -x "$CJC_BIN" ]]; then
  echo "PHASE_ENTRY_TRIGGER_FAIL: no matching cjc (set CJC or CANGJIE_HOME)" >&2
  exit 2
fi
if [[ ! -f "$RUNTIME_LIB_DIR/libcangjie-runtime.so" ]]; then
  echo "PHASE_ENTRY_TRIGGER_FAIL: missing product runtime in $RUNTIME_LIB_DIR" >&2
  exit 2
fi

mkdir -p "$OUT"
MINOR_BIN="$OUT/phase_entry_minor"
MAJOR_BIN="$OUT/phase_entry_major"
BUILD_LOG="$OUT/phase_entry_trigger.build.log"
MINOR_RUN_LOG="$OUT/phase_entry_minor.run.log"
MAJOR_RUN_LOG="$OUT/phase_entry_major.run.log"
SDK_RUNTIME="${CANGJIE_HOME:-}/runtime/lib/linux_x86_64_cjnative"
SDK_TOOLS="${CANGJIE_HOME:-}/tools/lib"
SDK_LLVM="${CANGJIE_HOME:-}/third_party/llvm/lib"

LD_LIBRARY_PATH="$RUNTIME_LIB_DIR:$SDK_RUNTIME:$SDK_TOOLS:$SDK_LLVM${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  "$CJC_BIN" "$MINOR_SRC" -O0 --static-std -o "$MINOR_BIN" >"$BUILD_LOG" 2>&1
LD_LIBRARY_PATH="$RUNTIME_LIB_DIR:$SDK_RUNTIME:$SDK_TOOLS:$SDK_LLVM${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  "$CJC_BIN" "$MAJOR_SRC" -O0 --static-std -o "$MAJOR_BIN" >>"$BUILD_LOG" 2>&1

set +e
LD_LIBRARY_PATH="$RUNTIME_LIB_DIR:$SDK_RUNTIME${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  MRT_GC_LOG=1 MRT_LOG_LEVEL=e cjGCInterval=3600s cjHeapSize=1GB \
  timeout 60s "$MINOR_BIN" >"$MINOR_RUN_LOG" 2>&1
minor_rc=$?
LD_LIBRARY_PATH="$RUNTIME_LIB_DIR:$SDK_RUNTIME${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  MRT_GC_LOG=1 MRT_GCV2_DISABLE_MINOR=1 MRT_LOG_LEVEL=e cjGCInterval=3600s cjHeapSize=1GB \
  timeout 60s "$MAJOR_BIN" >"$MAJOR_RUN_LOG" 2>&1
major_rc=$?
set -e

minor=$(grep -c 'rec=cycle .* kind=minor reason=young ' "$MINOR_RUN_LOG" || true)
major=$(grep -c 'rec=cycle .* kind=major ' "$MAJOR_RUN_LOG" || true)
minor_done=$(grep -c '^PHASE_ENTRY_MINOR_OK checksum=' "$MINOR_RUN_LOG" || true)
major_done=$(grep -c '^PHASE_ENTRY_MAJOR_OK checksum=' "$MAJOR_RUN_LOG" || true)
if [[ $minor_rc -ne 0 || $minor_done -ne 1 || $minor -lt 1 ]]; then
  echo "PHASE_ENTRY_MINOR_FAIL rc=$minor_rc done=$minor_done minor=$minor" >&2
  tail -30 "$MINOR_RUN_LOG" >&2
  exit 1
fi
if [[ $major_rc -ne 0 || $major_done -ne 1 || $major -lt 1 ]]; then
  echo "PHASE_ENTRY_MAJOR_FAIL rc=$major_rc done=$major_done major=$major" >&2
  tail -30 "$MAJOR_RUN_LOG" >&2
  exit 1
fi

echo "PHASE_ENTRY_TRIGGER_OK minor_rc=0 minor=$minor major_rc=0 major=$major"
