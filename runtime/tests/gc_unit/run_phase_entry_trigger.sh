#!/usr/bin/env bash
# End-to-end minor/major/Timer entry contract through one product runtime SO.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$ROOT"
MINOR_SRC="$ROOT/runtime/tests/gc_unit/phase_entry_trigger.cj"
MAJOR_SRC="$ROOT/runtime/tests/gc_unit/phase_entry_major.cj"
TIMER_SRC="$ROOT/runtime/tests/gc_unit/timer_ledger_contract.cpp"
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
TIMER_BIN="$OUT/timer_ledger_contract"
BUILD_LOG="$OUT/phase_entry_trigger.build.log"
MINOR_RUN_LOG="$OUT/phase_entry_minor.run.log"
MAJOR_RUN_LOG="$OUT/phase_entry_major.run.log"
TIMER_RUN_LOG="$OUT/timer_ledger_contract.run.log"
SDK_RUNTIME="${CANGJIE_HOME:-}/runtime/lib/linux_x86_64_cjnative"
SDK_TOOLS="${CANGJIE_HOME:-}/tools/lib"
SDK_LLVM="${CANGJIE_HOME:-}/third_party/llvm/lib"

set +e
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest \
  "$ROOT/runtime/tests/perf_vs_official/test_analyze_youngstw.py" \
  "$ROOT/runtime/tests/perf_vs_official/test_gclog_schema.py" \
  "$ROOT/runtime/tests/perf_vs_official/test_phase_leaf_ledger.py" \
  "$ROOT/runtime/tests/perf_vs_official/test_phase_entry_guard.py" \
  >"$OUT/schema_ledger.unit.log" 2>&1
analyzer_unit_rc=$?
set -e
echo "$analyzer_unit_rc" >"$OUT/schema_ledger.unit.rc"

if [[ "${PHASE_ENTRY_REUSE_ELFS:-0}" != 1 ]]; then
  LD_LIBRARY_PATH="$RUNTIME_LIB_DIR:$SDK_RUNTIME:$SDK_TOOLS:$SDK_LLVM${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    "$CJC_BIN" "$MINOR_SRC" -O0 --static-std -o "$MINOR_BIN" >"$BUILD_LOG" 2>&1
  LD_LIBRARY_PATH="$RUNTIME_LIB_DIR:$SDK_RUNTIME:$SDK_TOOLS:$SDK_LLVM${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    "$CJC_BIN" "$MAJOR_SRC" -O0 --static-std -o "$MAJOR_BIN" >>"$BUILD_LOG" 2>&1
  "$CXX_BIN" -std=gnu++17 -O0 -I"$ROOT/runtime/src" -I"$ROOT/runtime/include" \
    -I"$ROOT/runtime/third_party/third_party_bounds_checking_function/include" "$TIMER_SRC" \
    -L"$RUNTIME_LIB_DIR" -Wl,-rpath,"$RUNTIME_LIB_DIR" -Wl,--no-as-needed \
    -lcangjie-runtime -lboundscheck -ldl -lpthread -o "$TIMER_BIN" >>"$BUILD_LOG" 2>&1
else
  for binary in "$MINOR_BIN" "$MAJOR_BIN" "$TIMER_BIN"; do
    if [[ ! -x "$binary" ]]; then
      echo "PHASE_ENTRY_TRIGGER_FAIL: PHASE_ENTRY_REUSE_ELFS=1 but missing $binary" >&2
      exit 2
    fi
  done
  echo "reused existing three ELF files" >"$BUILD_LOG"
fi

sha256sum "$RUNTIME_LIB_DIR/libcangjie-runtime.so" "$RUNTIME_LIB_DIR/libboundscheck.so" \
  "$MINOR_BIN" "$MAJOR_BIN" "$TIMER_BIN" >"$OUT/product-and-three-elf.sha256"
for binary in "$MINOR_BIN" "$MAJOR_BIN" "$TIMER_BIN"; do
  LD_LIBRARY_PATH="$RUNTIME_LIB_DIR:$SDK_RUNTIME${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
    ldd "$binary" >"$OUT/$(basename "$binary").ldd.txt"
done

set +e
LD_LIBRARY_PATH="$RUNTIME_LIB_DIR:$SDK_RUNTIME${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  MRT_GC_LOG=1 MRT_LOG_LEVEL=e cjGCInterval=3600s cjHeapSize=1GB \
  timeout 60s "$MINOR_BIN" >"$MINOR_RUN_LOG" 2>&1
minor_rc=$?
LD_LIBRARY_PATH="$RUNTIME_LIB_DIR:$SDK_RUNTIME${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  MRT_GC_LOG=1 MRT_LOG_LEVEL=e cjGCInterval=3600s cjHeapSize=1GB \
  timeout 60s "$MAJOR_BIN" >"$MAJOR_RUN_LOG" 2>&1
major_rc=$?
LD_LIBRARY_PATH="$RUNTIME_LIB_DIR:$SDK_RUNTIME${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  MRT_GC_LOG=1 MRT_LOG_LEVEL=e timeout 60s "$TIMER_BIN" >"$TIMER_RUN_LOG" 2>&1
timer_rc=$?
set -e
printf 'minor_rc=%s\nmajor_rc=%s\ntimer_rc=%s\n' "$minor_rc" "$major_rc" "$timer_rc" >"$OUT/program.rc"

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
if guard_log timer "$TIMER_RUN_LOG"; then timer_guard_rc=0; else timer_guard_rc=$?; fi

if [[ $analyzer_unit_rc -ne 0 ||
      $minor_rc -ne 0 || $major_rc -ne 0 || $timer_rc -ne 0 ||
      $minor_guard_rc -ne 0 || $major_guard_rc -ne 0 || $timer_guard_rc -ne 0 ]]; then
  echo "PHASE_ENTRY_TRIGGER_FAIL analyzer_unit_rc=$analyzer_unit_rc " \
       "minor_rc=$minor_rc major_rc=$major_rc timer_rc=$timer_rc " \
       "minor_guard_rc=$minor_guard_rc major_guard_rc=$major_guard_rc timer_guard_rc=$timer_guard_rc" >&2
  tail -30 "$OUT/schema_ledger.unit.log" >&2
  tail -30 "$MINOR_RUN_LOG" >&2
  tail -30 "$MAJOR_RUN_LOG" >&2
  tail -30 "$TIMER_RUN_LOG" >&2
  exit 1
fi

echo "PHASE_ENTRY_TRIGGER_OK analyzer_unit_rc=0 minor_rc=0 major_rc=0 timer_rc=0 guards=3"
