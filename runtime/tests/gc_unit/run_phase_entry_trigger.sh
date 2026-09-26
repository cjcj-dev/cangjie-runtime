#!/usr/bin/env bash
# End-to-end minor/major/Timer entry contract through one product runtime SO.
# Explicit fixture requests: this runner does not test automatic allocation policy.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$ROOT"
MINOR_SRC="$ROOT/runtime/tests/gc_unit/phase_entry_trigger.cj"
MAJOR_SRC="$ROOT/runtime/tests/gc_unit/phase_entry_major.cj"
OUT="${GC_UNIT_OUT:-$ROOT/runtime/tests/gc_unit/build_standalone}"
RUNTIME_LIB_DIR="${GCV2_RUNTIME_LIB_DIR:?set GCV2_RUNTIME_LIB_DIR}"
CJC_BIN="${CJC:-${CANGJIE_HOME:-}/bin/cjc}"
CXX_BIN="${CXX:-c++}"

if [[ ! -x "$CJC_BIN" ]]; then
  echo "PHASE_ENTRY_TRIGGER_FAIL: no matching cjc (set CJC or CANGJIE_HOME)" >&2
  exit 2
fi
for library in libcangjie-runtime.so libboundscheck.so; do
  if [[ ! -f "$RUNTIME_LIB_DIR/$library" ]]; then
    echo "PHASE_ENTRY_TRIGGER_FAIL: missing $RUNTIME_LIB_DIR/$library" >&2
    exit 2
  fi
done

mkdir -p "$OUT"
MINOR_BIN="$OUT/phase_entry_minor"
MAJOR_BIN="$OUT/phase_entry_major"
REQUEST_LIB="$OUT/libphase_entry_request.so"
BUILD_LOG="$OUT/phase_entry_trigger.build.log"
MINOR_RUN_LOG="$OUT/phase_entry_minor.run.log"
MAJOR_RUN_LOG="$OUT/phase_entry_major.run.log"
SDK_RUNTIME="${CANGJIE_HOME:-}/runtime/lib/linux_x86_64_cjnative"
SDK_TOOLS="${CANGJIE_HOME:-}/tools/lib"
SDK_LLVM="${CANGJIE_HOME:-}/third_party/llvm/lib"

set +e
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest \
  "$ROOT/runtime/tests/perf_vs_official/test_analyze_youngstw.py" \
  "$ROOT/runtime/tests/perf_vs_official/test_gclog_schema.py" \
  "$ROOT/runtime/tests/perf_vs_official/test_phase_entry_guard.py" \
  "$ROOT/runtime/tests/perf_vs_official/test_wait_phase_entry_cycle.py" \
  >"$OUT/schema_ledger.unit.log" 2>&1
analyzer_unit_rc=$?
set -e
echo "$analyzer_unit_rc" >"$OUT/schema_ledger.unit.rc"

if [[ "${PHASE_ENTRY_REUSE_ELFS:-0}" != 1 ]]; then
  "$CXX_BIN" -std=gnu++17 -O0 -fPIC -shared -I"$ROOT/runtime/src" -I"$ROOT/runtime/include" \
    -I"$ROOT/runtime/src/Heap/z/os/linux" \
    -I"$ROOT/runtime/src/CJThread/src/runtime/schedule/include" \
    -I"$RUNTIME_LIB_DIR/../../include" \
    -I"$ROOT/runtime/third_party/third_party_bounds_checking_function/include" \
    "$ROOT/runtime/tests/gc_unit/phase_entry_request.cpp" \
    -L"$RUNTIME_LIB_DIR" -Wl,-rpath,"$RUNTIME_LIB_DIR" -lcangjie-runtime -lboundscheck \
    -o "$REQUEST_LIB" >"$BUILD_LOG" 2>&1
  LD_LIBRARY_PATH="${GC_UNIT_CJC_RUNTIME_LIB_DIR:?set GC_UNIT_CJC_RUNTIME_LIB_DIR to the compiler host runtime}:$SDK_TOOLS:$SDK_LLVM${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    "$CJC_BIN" "$MINOR_SRC" -O0 --static-std -L"$RUNTIME_LIB_DIR" -L"$OUT" -lphase_entry_request \
    -o "$MINOR_BIN" >>"$BUILD_LOG" 2>&1
  LD_LIBRARY_PATH="${GC_UNIT_CJC_RUNTIME_LIB_DIR:?set GC_UNIT_CJC_RUNTIME_LIB_DIR to the compiler host runtime}:$SDK_TOOLS:$SDK_LLVM${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    "$CJC_BIN" "$MAJOR_SRC" -O0 --static-std -L"$RUNTIME_LIB_DIR" -o "$MAJOR_BIN" >>"$BUILD_LOG" 2>&1
else
  if [[ ! -f "$REQUEST_LIB" ]]; then
    echo "PHASE_ENTRY_TRIGGER_FAIL: missing $REQUEST_LIB" >&2
    exit 2
  fi
  for binary in "$MINOR_BIN" "$MAJOR_BIN"; do
    if [[ ! -x "$binary" ]]; then
      echo "PHASE_ENTRY_TRIGGER_FAIL: PHASE_ENTRY_REUSE_ELFS=1 but missing $binary" >&2
      exit 2
    fi
  done
  echo "reused existing ELF files" >"$BUILD_LOG"
fi

sha256sum "$RUNTIME_LIB_DIR/libcangjie-runtime.so" "$RUNTIME_LIB_DIR/libboundscheck.so" \
  "$MINOR_BIN" "$MAJOR_BIN" "$REQUEST_LIB" >"$OUT/product-and-two-elf.sha256"
for binary in "$MINOR_BIN" "$MAJOR_BIN"; do
  LD_LIBRARY_PATH="$OUT:$RUNTIME_LIB_DIR:$SDK_RUNTIME${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    ldd "$binary" >"$OUT/$(basename "$binary").ldd.txt"
done

set +e
LD_LIBRARY_PATH="$OUT:$RUNTIME_LIB_DIR:$SDK_RUNTIME${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  MRT_GC_LOG=1 MRT_LOG_LEVEL=e cjGCInterval=3600s cjHeapSize=1G \
  python3 "$ROOT/runtime/tests/gc_unit/wait_phase_entry_cycle.py" "$MINOR_BIN" "$MINOR_RUN_LOG"
minor_rc=$?
LD_LIBRARY_PATH="$RUNTIME_LIB_DIR:$SDK_RUNTIME${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  MRT_GC_LOG=1 MRT_LOG_LEVEL=e cjGCInterval=3600s cjHeapSize=1G \
  timeout 60s "$MAJOR_BIN" >"$MAJOR_RUN_LOG" 2>&1
major_rc=$?
set -e
printf 'minor_rc=%s\nmajor_rc=%s\n' "$minor_rc" "$major_rc" >"$OUT/program.rc"

guard_log() {
  local mode="$1"
  local log="$2"
  local guard_log="$OUT/$mode.guard.log"
  set +e
  PYTHONDONTWRITEBYTECODE=1 python3 "$ROOT/runtime/tests/perf_vs_official/phase_entry_guard.py" \
    "$mode" "$log" >"$guard_log" 2>&1
  local rc=$?
  set -e
  echo "$rc" >"$OUT/$mode.guard.rc"
  return "$rc"
}

if guard_log minor "$MINOR_RUN_LOG"; then minor_guard_rc=0; else minor_guard_rc=$?; fi
if guard_log major "$MAJOR_RUN_LOG"; then major_guard_rc=0; else major_guard_rc=$?; fi

if [[ $analyzer_unit_rc -ne 0 ||
      $minor_rc -ne 0 || $major_rc -ne 0 ||
      $minor_guard_rc -ne 0 || $major_guard_rc -ne 0 ]]; then
  echo "PHASE_ENTRY_TRIGGER_FAIL analyzer_unit_rc=$analyzer_unit_rc " \
       "minor_rc=$minor_rc major_rc=$major_rc " \
       "minor_guard_rc=$minor_guard_rc major_guard_rc=$major_guard_rc" >&2
  tail -30 "$OUT/schema_ledger.unit.log" >&2
  tail -30 "$MINOR_RUN_LOG" >&2
  tail -30 "$MAJOR_RUN_LOG" >&2
  exit 1
fi

echo "PHASE_ENTRY_TRIGGER_OK analyzer_unit_rc=0 minor_rc=0 major_rc=0 guards=2"
