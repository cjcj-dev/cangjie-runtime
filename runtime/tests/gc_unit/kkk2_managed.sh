#!/usr/bin/env bash
# Build ELF once, run finalizer/segmented/phase_entry N times, emit JSON.
# Sync analyzer tools into SRCROOT (kkk2 extract is runtime/-only).
# Usage: kkk2_managed.sh <runtime-sha>
set -euo pipefail
ulimit -c 0

SHA=${1:?runtime-sha}
LANE=${LANE:-/root/sym_cangjie_runtime_593_implement_r5700751289}
N=${N:-3}
SRCROOT=${SRCROOT:-$LANE/default}
OUT=${OUT:-$LANE/managed-runs}
HOST_RT=${GC_UNIT_CJC_RUNTIME_LIB_DIR:-${HOST_RT:-/root/sym_cjcj_48_implement_r5685150408/host/runtime/lib/linux_x86_64_cjnative}}
export CANGJIE_HOME=${CANGJIE_HOME:-$LANE/cangjie-home}
export GCV2_RUNTIME_LIB_DIR=${GCV2_RUNTIME_LIB_DIR:-$SRCROOT/build/runtime-staging/lib/x86_64_Release}
export GC_UNIT_CJC_RUNTIME_LIB_DIR="$HOST_RT"

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"

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
JSON="$OUT/kkk2_managed.json"
echo "kkk2_managed sha=$SHA n=$N srcroot=$SRCROOT out=$OUT"

run_one() {
  local name="$1"
  local script="$2"
  local i=1
  while [[ $i -le $N ]]; do
    local rundir="$OUT/${name}_n$i"
    mkdir -p "$rundir"
    export GC_UNIT_OUT="$rundir"
    set +e
    bash "$script" >"$rundir/wrapper.log" 2>&1
    local rc=$?
    set -e
    echo "$rc" >"$rundir/rc"
    echo "${name}_n$i rc=$rc"
    i=$((i + 1))
  done
}

GC_UNIT="$SRCROOT/runtime/tests/gc_unit"
run_one finalizer "$GC_UNIT/run_finalizer_trigger.sh"
run_one segmented "$GC_UNIT/run_segmented_array_managed.sh"
run_one phase "$GC_UNIT/run_phase_entry_trigger.sh"

python3 - <<PY
import json, os, pathlib
out = pathlib.Path("$OUT")
n = int("$N")
names = ["finalizer", "segmented", "phase"]
result = {"runtime_sha": "$SHA", "n": n, "out": str(out), "runs": {}}
for name in names:
    rcs = []
    for i in range(1, n + 1):
        p = out / f"{name}_n{i}" / "rc"
        rcs.append(int(p.read_text().strip()) if p.exists() else -1)
    result["runs"][name] = rcs
    result[f"{name}_all_zero"] = all(rc == 0 for rc in rcs)
path = out / "kkk2_managed.json"
path.write_text(json.dumps(result, indent=2) + "\n")
print(path.read_text())
PY
