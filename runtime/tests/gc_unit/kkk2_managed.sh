#!/usr/bin/env bash
# Sync analyzer tools into SRCROOT (kkk2 extract is runtime/-only), then run
# phase_entry N times. Usage: kkk2_managed.sh <runtime-sha>
set -euo pipefail
ulimit -c 0

SHA=${1:?runtime-sha}
LANE=${LANE:-/root/sym_cangjie_runtime_693_implement_r5711880533}
N=${N:-3}
SRCROOT=${SRCROOT:-$LANE/default}
OUT=${OUT:-$LANE/managed-runs}
HOST_RT=${GC_UNIT_CJC_RUNTIME_LIB_DIR:-${HOST_RT:-/root/sym_cjcj_48_implement_r5685150408/host/runtime/lib/linux_x86_64_cjnative}}
export CANGJIE_HOME=${CANGJIE_HOME:-${HOME:-$LANE/cangjie-home}}
export GCV2_RUNTIME_LIB_DIR=${GCV2_RUNTIME_LIB_DIR:-$SRCROOT/build/runtime-staging/lib/x86_64_Release}
export GC_UNIT_CJC_RUNTIME_LIB_DIR="$HOST_RT"
export GC_UNIT_OUT="${GC_UNIT_OUT:-$OUT/phase_out}"

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"

# run_phase_entry_trigger.sh:40-50 unittest files live under runtime/; the only
# extra ROOT-relative tool they exec is tools/zstat_pillars.py (test_phase_leaf_ledger.py:184).
ANALYZER_TOOLS=(zstat_pillars.py)

sync_analyzer_tools() {
  mkdir -p "$SRCROOT/tools"
  local copied=0 src dest f
  for f in "${ANALYZER_TOOLS[@]}"; do
    dest="$SRCROOT/tools/$f"
    src=""
    if [[ -f "$REPO/tools/$f" ]]; then
      src="$REPO/tools/$f"
    elif [[ -f "$LANE/tools-bundle/$f" ]]; then
      src="$LANE/tools-bundle/$f"
    elif [[ -f "$dest" ]]; then
      src="$dest"
    fi
    if [[ -z "$src" ]]; then
      echo "kkk2_managed FAIL: missing analyzer tool $f (need REPO/tools, $LANE/tools-bundle, or $dest)" >&2
      return 1
    fi
    if [[ "$src" != "$dest" ]]; then
      cp -a "$src" "$dest"
    fi
    copied=$((copied + 1))
  done
  echo "kkk2_managed synced analyzer tools n=$copied dest=$SRCROOT/tools"
}

sync_analyzer_tools

mkdir -p "$OUT"
echo "kkk2_managed sha=$SHA n=$N srcroot=$SRCROOT"

i=1
while [[ $i -le $N ]]; do
  rundir="$OUT/phase_n$i"
  mkdir -p "$rundir"
  export GC_UNIT_OUT="$rundir"
  set +e
  bash "$SRCROOT/runtime/tests/gc_unit/run_phase_entry_trigger.sh" >"$rundir/wrapper.log" 2>&1
  rc=$?
  set -e
  echo "$rc" >"$rundir/rc"
  if [[ -f "$rundir/schema_ledger.unit.rc" ]]; then
    cp "$rundir/schema_ledger.unit.rc" "$rundir/analyzer_unit.rc"
  else
    echo "missing" >"$rundir/analyzer_unit.rc"
  fi
  echo "phase_n$i rc=$rc analyzer_unit_rc=$(cat "$rundir/analyzer_unit.rc")"
  i=$((i + 1))
done
