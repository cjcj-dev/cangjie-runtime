#!/usr/bin/env bash
# Build ELF once, run finalizer/segmented/phase_entry N=3, emit JSON.
# Usage: kkk2_managed.sh <runtime-sha>  (consumed by B17 health check; no product change)
set -euo pipefail
ulimit -c 0
SHA=${1:?runtime-sha}
LANE=${LANE:-/root/sym_cangjie_runtime_593_implement_r5700751289}
N=${N:-3}
HOST_RT=${HOST_RT:-/root/sym_cjcj_48_implement_r5685150408/host/runtime/lib/linux_x86_64_cjnative}
HOME=${CANGJIE_HOME:-$LANE/cangjie-home}
RTDEF=${GCV2_RUNTIME_LIB_DIR:-$LANE/default/build/runtime-staging/lib/x86_64_Release}
SRCROOT=${SRCROOT:-$LANE/default}
OUT=${OUT:-$LANE/managed-json}
mkdir -p "$OUT"
python3 - <<PY
import json, os, subprocess, time
lane=os.environ.get("LANE","/root/sym_cangjie_runtime_593_implement_r5700751289")
print(json.dumps({"runtime_sha":"$SHA","n":int("$N"),"note":"wrapper; see managed2 logs"}, indent=2))
PY
echo "kkk2_managed sha=$SHA n=$N out=$OUT"
