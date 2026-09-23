#!/usr/bin/env bash
# Build and run finalizer/segmented/phase_entry N times per host arm, emit JSON.
# Target arm: stained; cjc compiler host: official/H48 runtime.
# cjc itself always uses H48 host (0904); CANGJIE_HOME is sdkdepot colored SDK.
# Usage: kkk2_managed.sh <runtime-sha>
set -euo pipefail
ulimit -c 0

RUNNER_SHA256=$(sha256sum "${BASH_SOURCE[0]}" | awk '{print $1}')
SHA=${1:?runtime-sha}
LANE=${LANE:-/root/sym_cangjie_runtime_708_implement_r5740357995}
N=${N:-3}
SRCROOT=${SRCROOT:-$LANE/default}
OUT=${OUT:-$LANE/managed-runs}
# Compiler/stdlib SDK for the inline TLAB ABI (top@0/end@8).
COLORED_SDK=${COLORED_SDK:-/root/sdkdepot/b99430a618af-1ecb811801ca}
H48_RT=${H48_RT:-/root/sym_cjcj_48_implement_r5685150408/host/runtime/lib/linux_x86_64_cjnative}
STAINED_RT=${STAINED_RT:-$SRCROOT/build/runtime-staging/lib/x86_64_Release}
export CANGJIE_HOME=${CANGJIE_HOME:-$COLORED_SDK}
export GC_UNIT_CJC_RUNTIME_LIB_DIR="$H48_RT"
export HOST_RT="$H48_RT"

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

# Keep compiler status separate from the wrapper's runtime/assertion status.
# The proxy is inherited by all three existing wrappers through their CJC input.
export MANAGED_REAL_CJC=${CJC:-$CANGJIE_HOME/bin/cjc}
export CJC="$OUT/cjc-record-status"
cat > "$CJC" <<'COMPILER'
#!/usr/bin/env bash
set +e
"$MANAGED_REAL_CJC" "$@"
rc=$?
printf '%s\n' "$rc" >> "$GC_UNIT_OUT/compile.rc"
exit "$rc"
COMPILER
chmod +x "$CJC"

run_one() {
  local arm="$1"
  local name="$2"
  local script="$3"
  local i=1
  while [[ $i -le $N ]]; do
    local rundir="$OUT/${arm}_${name}_n$i"
    # Each attempt starts without stale executables or status from an earlier run.
    # Keep old evidence in its own directory when OUT is reused.
    if [[ -e "$rundir" ]]; then
      mv "$rundir" "$(mktemp -d "$OUT/${arm}_${name}_n$i.previous.XXXXXX")/run"
    fi
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
  local target_rt="$2"
  export GCV2_RUNTIME_LIB_DIR="$target_rt"
  echo "kkk2_managed arm=$arm compile_HOST_RT=$H48_RT target=$target_rt CANGJIE_HOME=$CANGJIE_HOME"
  local GC_UNIT="$SRCROOT/runtime/tests/gc_unit"
  if [[ ! -d "$GC_UNIT" ]]; then
    GC_UNIT="$HERE"
  fi
  run_one "$arm" finalizer "$GC_UNIT/run_finalizer_trigger.sh" &
  local finalizer_pid=$!
  run_one "$arm" segmented "$GC_UNIT/run_segmented_array_managed.sh" &
  local segmented_pid=$!
  run_one "$arm" phase "$GC_UNIT/run_phase_entry_trigger.sh" &
  local phase_pid=$!
  wait "$finalizer_pid" "$segmented_pid" "$phase_pid"
}

run_arm stained "$STAINED_RT"

python3 - <<PY
import json, pathlib
out = pathlib.Path("$OUT")
n = int("$N")
names = ["finalizer", "segmented", "phase"]
arms = ["stained"]
result = {
    "runtime_sha": "$SHA",
    "runner_sha256": "$RUNNER_SHA256",
    "n": n,
    "out": str(out),
    "cangjie_home": "$CANGJIE_HOME",
    "h48_rt": "$H48_RT",
    "stained_rt": "$STAINED_RT",
    "sdk_manifest": {},
    "runtime_so_sha256": "",
    "abi_probe": {
        "sdk_selected": "$CANGJIE_HOME",
    },
    "arms": {},
    "failed": [],
    "build_fail": [],
}
import hashlib
manifest_path = pathlib.Path("$CANGJIE_HOME") / "MANIFEST"
if manifest_path.is_file():
    result["sdk_manifest"] = dict(line.split("=", 1) for line in manifest_path.read_text().splitlines() if "=" in line)
    result["sdk_manifest_sha256"] = hashlib.sha256(manifest_path.read_bytes()).hexdigest()
runtime_so = pathlib.Path("$STAINED_RT") / "libcangjie-runtime.so"
if runtime_so.is_file():
    result["runtime_so_sha256"] = hashlib.sha256(runtime_so.read_bytes()).hexdigest()
print("MANAGED_SDK_IDENTITY " + json.dumps({"sdk": result["cangjie_home"], "manifest": result["sdk_manifest"], "runtime_so_sha256": result["runtime_so_sha256"]}, sort_keys=True))
print("MANAGED_ABI_PROBE " + json.dumps(result["abi_probe"], sort_keys=True))
executables = {
    "finalizer": ["finalizer_trigger"],
    "segmented": ["segmented_array_managed"],
    "phase": ["phase_entry_minor", "phase_entry_major"],
}
def is_elf(path):
    if not path.is_file():
        return False
    with path.open("rb") as stream:
        return stream.read(4) == b"\x7fELF"

for arm in arms:
    result["arms"][arm] = {"runs": {}, "build_fail": []}
    for name in names:
        rcs = []
        for i in range(1, n + 1):
            run = out / f"{arm}_{name}_n{i}"
            p = run / "rc"
            compile_status = run / "compile.rc"
            compile_rcs = ([int(line) for line in compile_status.read_text().splitlines()]
                           if compile_status.exists() else [])
            missing = [binary for binary in executables[name] if not is_elf(run / binary)]
            if any(rc != 0 for rc in compile_rcs):
                build_failed = True
            else:
                build_failed = bool(missing)
            if build_failed:
                failure = {"arm": arm, "name": name, "iteration": i,
                           "compile_rc": compile_rcs, "missing_elf": missing,
                           "wrapper_rc": int(p.read_text().strip()) if p.exists() else None,
                           "log": str(run / "wrapper.log")}
                result["build_fail"].append(failure)
                result["arms"][arm]["build_fail"].append(failure)
                continue
            rcs.append(int(p.read_text().strip()) if p.exists() else -1)
        result["arms"][arm]["runs"][name] = rcs
        ok = len(rcs) == n and all(rc == 0 for rc in rcs)
        result["arms"][arm][f"{name}_all_zero"] = ok
        if any(rc != 0 for rc in rcs):
            result["failed"].append(f"{arm}/{name}")
    result["arms"][arm]["all_zero"] = all(
        result["arms"][arm][f"{name}_all_zero"] for name in names
    )
path = out / "kkk2_managed.json"
path.write_text(json.dumps(result, indent=2) + "\n")
print(path.read_text())
PY
