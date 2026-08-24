#!/usr/bin/env bash
# End-to-end phase-kind check through a product runtime built with MRT_ZSTAT=ON.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SRC="$ROOT/runtime/tests/gc_unit/phase_entry_trigger.cj"
CHECKER="$ROOT/runtime/tests/gc_unit/check_phase_kind_accounting.py"
OUT="${GC_UNIT_OUT:-$ROOT/runtime/tests/gc_unit/build_phase_kind}"
RUNTIME_LIB_DIR="${GCV2_RUNTIME_LIB_DIR:?set GCV2_RUNTIME_LIB_DIR}"
CJC_BIN="${CJC:-${CANGJIE_HOME:-}/bin/cjc}"
CORES="${GCV2_PHASE_KIND_CORES:-96-127}"

if [[ ! -x "$CJC_BIN" ]]; then
  echo "PHASE_KIND_ACCOUNTING_FAIL: no matching cjc (set CJC or CANGJIE_HOME)" >&2
  exit 2
fi
for library in libcangjie-runtime.so libboundscheck.so; do
  if [[ ! -f "$RUNTIME_LIB_DIR/$library" ]]; then
    echo "PHASE_KIND_ACCOUNTING_FAIL: missing $RUNTIME_LIB_DIR/$library" >&2
    exit 2
  fi
done
# Versioned dynamic symbols are accepted; the same predicate is exercised against the
# default-ZStat-off product in the integration evidence as its false-side control.
if ! nm -D "$RUNTIME_LIB_DIR/libcangjie-runtime.so" | awk '$0 ~ /ZStat/ { found=1 } END { exit !found }'; then
  echo "PHASE_KIND_ACCOUNTING_FAIL: product runtime was not built with MRT_ZSTAT=ON" >&2
  exit 2
fi

mkdir -p "$OUT"
BIN="$OUT/phase_kind_accounting"
BUILD_LOG="$OUT/build.log"
RUN_LOG="$OUT/run.log"
META="$OUT/meta.txt"
SDK_RUNTIME="${CANGJIE_HOME:-}/runtime/lib/linux_x86_64_cjnative"
SDK_TOOLS="${CANGJIE_HOME:-}/tools/lib"
SDK_LLVM="${CANGJIE_HOME:-}/third_party/llvm/lib"

{
  echo "begin_uptime=$(uptime)"
  echo "cores=$CORES"
  echo "source_head=$(git -C "$ROOT" rev-parse HEAD)"
  echo "runtime_sha256=$(sha256sum "$RUNTIME_LIB_DIR/libcangjie-runtime.so" | awk '{print $1}')"
  echo "boundscheck_sha256=$(sha256sum "$RUNTIME_LIB_DIR/libboundscheck.so" | awk '{print $1}')"
  echo "runtime_lineage=$(strings "$RUNTIME_LIB_DIR/libcangjie-runtime.so" | grep -oE 'CJRT-COMMIT:[0-9a-f-]+' | sort -u | paste -sd, -)"
} >"$META"

LD_LIBRARY_PATH="$RUNTIME_LIB_DIR:$SDK_RUNTIME:$SDK_TOOLS:$SDK_LLVM${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  "$CJC_BIN" "$SRC" -O0 --static-std -o "$BIN" >"$BUILD_LOG" 2>&1
echo "elf_sha256=$(sha256sum "$BIN" | awk '{print $1}')" >>"$META"
ldd "$BIN" >"$OUT/ldd.txt"

set +e
LD_LIBRARY_PATH="$RUNTIME_LIB_DIR:$SDK_RUNTIME${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  MRT_GC_LOG=1 MRT_ZSTAT=1 MRT_LOG_LEVEL=e cjGCInterval=3600s cjHeapSize=1GB \
  /usr/bin/time -f $'wall_s=%e\tmaxrss_kb=%M\ttime_exit=%x' -o "$OUT/time.tsv" \
  taskset -c "$CORES" timeout 60s "$BIN" >"$OUT/stdout" 2>"$RUN_LOG"
run_rc=$?
set -e
echo "$run_rc" >"$OUT/run.rc"
echo "end_uptime=$(uptime)" >>"$META"
if [[ $run_rc -ne 0 ]] || ! grep -q '^PHASE_ENTRY_MINOR_OK checksum=' "$OUT/stdout"; then
  echo "PHASE_KIND_PRODUCT_FAIL rc=$run_rc" >&2
  tail -40 "$RUN_LOG" >&2
  exit 1
fi

python3 "$CHECKER" "$RUN_LOG" | tee "$OUT/check.log"
