#!/usr/bin/env bash
# Build ELF once per host arm, run finalizer/segmented/phase_entry N times, emit JSON.
# Two HOST_RT arms: H48 (compiler host) and stained (candidate staging runtime).
# CANGJIE_HOME is the pinned colored SDK in sdkdepot (not /root/sdks or .cjv).
# Usage: kkk2_managed.sh <runtime-sha>
set -euo pipefail
ulimit -c 0

SHA=${1:?runtime-sha}
LANE=${LANE:-/root/sym_cangjie_runtime_708_implement_r5740357995}
N=${N:-3}
SRCROOT=${SRCROOT:-$LANE/default}
OUT=${OUT:-$LANE/managed-runs}
COLORED_SDK=${COLORED_SDK:-/root/sdkdepot/945fe3e8f023-fa13e8d5c17b}
H48_RT=${H48_RT:-/root/sym_cjcj_48_implement_r5685150408/host/runtime/lib/linux_x86_64_cjnative}
STAINED_RT=${STAINED_RT:-$SRCROOT/build/runtime-staging/lib/x86_64_Release}
export CANGJIE_HOME=${CANGJIE_HOME:-$COLORED_SDK}
export GCV2_RUNTIME_LIB_DIR=${GCV2_RUNTIME_LIB_DIR:-$STAINED_RT}

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

if [[ ! -x "$CANGJIE_HOME/bin/cjc" ]]; then
  echo "kkk2_managed FAIL: CANGJIE_HOME=$CANGJIE_HOME missing bin/cjc (pin sdkdepot first)" >&2
  exit 2
fi

mkdir -p "$OUT"
JSON="$OUT/kkk2_managed.json"
echo "kkk2_managed sha=$SHA n=$N srcroot=$SRCROOT out=$OUT home=$CANGJIE_HOME"

run_one() {
  local arm="$1"
  local name="$2"
  local script="$3"
  local i=1
  while [[ $i -le $N ]]; do
    local rundir="$OUT/${arm}_${name}_n$i"
    mkdir -p "$rundir"
    export GC_UNIT_OUT="$rundir"
    set +e
    bash "$script" >"$rundir/wrapper.log" 2>&1
    local rc=$?
    set -e
    echo "$rc" >"$rundir/rc"
    echo "${arm}_${name}_n$i rc=$rc"
    i=$((i + 1))
  done
}

run_arm() {
  local arm="$1"
  local host_rt="$2"
  export GC_UNIT_CJC_RUNTIME_LIB_DIR="$host_rt"
  export HOST_RT="$host_rt"
  echo "kkk2_managed arm=$arm HOST_RT=$host_rt CANGJIE_HOME=$CANGJIE_HOME GCV2=$GCV2_RUNTIME_LIB_DIR"
  local GC_UNIT="$SRCROOT/runtime/tests/gc_unit"
  if [[ ! -d "$GC_UNIT" ]]; then
    GC_UNIT="$HERE"
  fi
  run_one "$arm" finalizer "$GC_UNIT/run_finalizer_trigger.sh"
  run_one "$arm" segmented "$GC_UNIT/run_segmented_array_managed.sh"
  run_one "$arm" phase "$GC_UNIT/run_phase_entry_trigger.sh"
}

run_arm h48 "$H48_RT"
run_arm stained "$STAINED_RT"

python3 - <<PY
import json, pathlib
out = pathlib.Path("$OUT")
n = int("$N")
names = ["finalizer", "segmented", "phase"]
arms = ["h48", "stained"]
result = {
    "runtime_sha": "$SHA",
    "n": n,
    "out": str(out),
    "cangjie_home": "$CANGJIE_HOME",
    "h48_rt": "$H48_RT",
    "stained_rt": "$STAINED_RT",
    "arms": {},
    "failed": [],
}
for arm in arms:
    result["arms"][arm] = {"runs": {}}
    for name in names:
        rcs = []
        for i in range(1, n + 1):
            p = out / f"{arm}_{name}_n{i}" / "rc"
            rcs.append(int(p.read_text().strip()) if p.exists() else -1)
        result["arms"][arm]["runs"][name] = rcs
        ok = all(rc == 0 for rc in rcs)
        result["arms"][arm][f"{name}_all_zero"] = ok
        if not ok:
            result["failed"].append(f"{arm}/{name}")
    result["arms"][arm]["all_zero"] = all(
        result["arms"][arm][f"{name}_all_zero"] for name in names
    )
path = out / "kkk2_managed.json"
path.write_text(json.dumps(result, indent=2) + "\n")
print(path.read_text())
PY
