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

PYTHONDONTWRITEBYTECODE=1 python3 -m unittest \
  "$ROOT/runtime/tests/perf_vs_official/test_analyze_youngstw.py" \
  "$ROOT/runtime/tests/perf_vs_official/test_gclog_schema.py" \
  "$ROOT/runtime/tests/perf_vs_official/test_phase_leaf_ledger.py" \
  >"$OUT/schema_ledger.unit.log" 2>&1

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
  MRT_GC_LOG=1 MRT_GCV2_DISABLE_MINOR=1 MRT_LOG_LEVEL=e cjGCInterval=3600s cjHeapSize=1GB \
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
  PYTHONDONTWRITEBYTECODE=1 python3 - "$ROOT" "$mode" "$log" >"$guard_log" 2>&1 <<'PY'
import sys
from pathlib import Path

root, mode, log_path = Path(sys.argv[1]), sys.argv[2], Path(sys.argv[3])
sys.path.insert(0, str(root / "runtime/tests/perf_vs_official"))
from gclog_schema import phase_leaf_ledger, parse_gclog

text = log_path.read_text(encoding="utf-8", errors="replace")
records = parse_gclog(text)
ledger = phase_leaf_ledger(text)
errors = []
if not records.cycles:
    errors.append("cycle=0")
if not records.stw:
    errors.append("stw=0")
if not records.phases:
    errors.append("phase=0")
if not records.phase_leaves:
    errors.append("phase_leaf=0")
if records.phases and not any(0 < record.ns < 1000 for record in records.phases):
    errors.append("sub_microsecond_phase=0")

if mode in ("minor", "major"):
    marker = "PHASE_ENTRY_MINOR_OK checksum=" if mode == "minor" else "PHASE_ENTRY_MAJOR_OK checksum="
    if marker not in text:
        errors.append("completion=0")
    if not any(record.kind == mode for record in records.cycles):
        errors.append(f"{mode}_cycle=0")
    if records.phase_leaves and not any(">" in record.path for record in records.phase_leaves):
        errors.append("nested_leaf_path=0")
else:
    if "TIMER_LEDGER_CONTRACT_OK" not in text:
        errors.append("completion=0")
    leaves = {record.name: record for record in records.phase_leaves}
    if "contract.root" in leaves or "contract.middle" in leaves:
        errors.append("parent_emitted_as_leaf")
    deep = leaves.get("contract.deep")
    if deep is None or deep.path != "contract.deep>contract.middle>contract.root":
        errors.append("deep_leaf_path")
    captured = leaves.get("cycle.captured")
    if captured is None or captured.seq == 0 or captured.seq not in {cycle.seq for cycle in records.cycles}:
        errors.append("cycle_captured_at_construction")
    finalizer = leaves.get("Finalizer")
    if finalizer is None or finalizer.seq != 0:
        errors.append("unowned_finalizer_seq")
    if "Finalizer" not in ledger["unowned_nonpillar_names"]:
        errors.append("unowned_nonpillar_exclusion")
    external = leaves.get("contract.external")
    if external is None or external.seq != 0:
        errors.append("active_cycle_external_seq")
    internal = leaves.get("young.flush_alloc")
    if internal is None or internal.seq == 0 or internal.seq not in {cycle.seq for cycle in records.cycles}:
        errors.append("active_cycle_internal_seq")
    if "contract.external" not in ledger["unowned_nonpillar_names"]:
        errors.append("active_cycle_external_unowned")
    owned_row = next((row for row in ledger["cycles"] if internal is not None and
                      row["seq"] == internal.seq), None)
    if owned_row is None or owned_row["structural_leaf_ns"] != internal.ns:
        errors.append("active_cycle_internal_bound")

print(
    f"SCHEMA_LEDGER_GUARD mode={mode} cycles={len(records.cycles)} stw={len(records.stw)} "
    f"phase={len(records.phases)} phase_leaf={len(records.phase_leaves)} "
    f"ledger_cycles={len(ledger['cycles'])} errors={','.join(errors) if errors else 'none'}"
)
raise SystemExit(1 if errors else 0)
PY
  local rc=$?
  set -e
  echo "$rc" >"$OUT/$mode.guard.rc"
  return "$rc"
}

if guard_log minor "$MINOR_RUN_LOG"; then minor_guard_rc=0; else minor_guard_rc=$?; fi
if guard_log major "$MAJOR_RUN_LOG"; then major_guard_rc=0; else major_guard_rc=$?; fi
if guard_log timer "$TIMER_RUN_LOG"; then timer_guard_rc=0; else timer_guard_rc=$?; fi

if [[ $minor_rc -ne 0 || $major_rc -ne 0 || $timer_rc -ne 0 ||
      $minor_guard_rc -ne 0 || $major_guard_rc -ne 0 || $timer_guard_rc -ne 0 ]]; then
  echo "PHASE_ENTRY_TRIGGER_FAIL minor_rc=$minor_rc major_rc=$major_rc timer_rc=$timer_rc " \
       "minor_guard_rc=$minor_guard_rc major_guard_rc=$major_guard_rc timer_guard_rc=$timer_guard_rc" >&2
  tail -30 "$MINOR_RUN_LOG" >&2
  tail -30 "$MAJOR_RUN_LOG" >&2
  tail -30 "$TIMER_RUN_LOG" >&2
  exit 1
fi

echo "PHASE_ENTRY_TRIGGER_OK minor_rc=0 major_rc=0 timer_rc=0 guards=3"
