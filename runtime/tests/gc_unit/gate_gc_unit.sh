#!/usr/bin/env bash
# Fail-closed GC unit gate: suite must exist, build, and pass.
# Intended for kkk2 / CI wrappers. Exit non-zero if anything is missing.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SRC="$ROOT/runtime/tests/gc_unit"
SCRIPT="$SRC/run_standalone.sh"

if [[ ! -d "$SRC" ]]; then
  echo "GC_UNIT_GATE_FAIL: missing $SRC" >&2
  exit 2
fi
if [[ ! -f "$SCRIPT" ]]; then
  echo "GC_UNIT_GATE_FAIL: missing run_standalone.sh" >&2
  exit 2
fi
if [[ ! -f "$SRC/test_defect_regressions.cpp" ]]; then
  echo "GC_UNIT_GATE_FAIL: missing defect regression suite (Phase 2)" >&2
  exit 2
fi

if [[ -z "${GCV2_RUNTIME_LIB_DIR:-}" ]]; then
  for cand in \
    "$ROOT/runtime/output/temp/lib/x86_64_Release" \
    "$ROOT/runtime/output/temp/lib/x86_64_Relwithdebinfo"; do
    if [[ -f "$cand/libcangjie-runtime.so" ]]; then
      export GCV2_RUNTIME_LIB_DIR="$cand"
      break
    fi
  done
fi
if [[ -z "${GCV2_RUNTIME_LIB_DIR:-}" || ! -f "$GCV2_RUNTIME_LIB_DIR/libcangjie-runtime.so" ]]; then
  echo "GC_UNIT_GATE_FAIL: no libcangjie-runtime.so (set GCV2_RUNTIME_LIB_DIR)" >&2
  exit 2
fi

export GC_UNIT_OUT="${GC_UNIT_OUT:-$SRC/build_standalone}"
bash "$SCRIPT"
echo "GC_UNIT_GATE_OK"
