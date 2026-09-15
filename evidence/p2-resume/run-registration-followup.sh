#!/bin/bash
set -u
ulimit -c 0
scenario=$1; arm=$2
R=/root/sym_cangjie_runtime_607_implement_r5684610492-build9
L=sym_cangjie_runtime_607_implement_r5684610492
case "$arm" in
 green) P="$R/testable";;
 restored) P=/root/$L-restored9/testable;;
 *) P=/root/$L-$arm/testable;;
esac
case "$scenario" in
 slow) A="$R/slow2"; INPUT=();;
 full) A="$R/full2"; INPUT=(P2_FINALIZABLE=1);;
 minor) A="$R/full2"; INPUT=(P2_MINOR_ONLY=1);;
 array) A="$R/array2"; INPUT=();;
 array-final) A="$R/array2"; INPUT=(P2_ARRAY_FINALIZABLE=1);;
 struct) A="$R/array2"; INPUT=(P2_STRUCT_ARRAY=1);;
 struct-final) A="$R/array2"; INPUT=(P2_STRUCT_ARRAY=1 P2_ARRAY_FINALIZABLE=1);;
 closure) A="$R/closure2"; INPUT=();;
 registration) A="$R/registration4"; INPUT=();;
 *) exit 2;;
esac
SO="$P/build/runtime-staging/lib/x86_64_Release"
SDK=/root/$L-build3/sdk
O="$R/qualified-${scenario}4/$arm"
mkdir -p "$O"
cores=${P2_CORES:-48-55}
printf 'scenario=%s arm=%s cores=%s\n' "$scenario" "$arm" "$cores" > "$O/config.txt"
uptime > "$O/before.txt"
sha256sum "$A/p2_field_barrier" "$A/libp2_field_barrier.so" "$SO/libcangjie-runtime.so" "$SO/libboundscheck.so" > "$O/identity.sha256"
strings "$SO/libcangjie-runtime.so" | /usr/bin/grep '^CJRT-' > "$O/lineage.txt"
start=$(date +%s%N)
env "${INPUT[@]}" LD_DEBUG=libs LD_LIBRARY_PATH="$SO:$A:$SDK/runtime/lib/linux_x86_64_cjnative" cjGCInterval=3600s \
 taskset -c "$cores" timeout 60s "$A/p2_field_barrier" > "$O/run.log" 2>&1
rc=$?
echo "$rc" > "$O/run.rc"
echo "wall_ns=$(($(date +%s%N)-start))" > "$O/wall.txt"
uptime > "$O/after.txt"
python3 - "$O" <<'PY'
from pathlib import Path
import json,sys
p=Path(sys.argv[1]); lines=(p/'run.log').read_text().splitlines(); d={'rc':int((p/'run.rc').read_text()),'assertions':[x for x in lines if x.startswith('P2_ASSERT ')], 'result':[x for x in lines if x.startswith('P2_') and 'RESULT' in x]}; (p/'result.json').write_text(json.dumps(d,indent=2)+'\n');print(p.parent.name,p.name,d['rc'],len(d['assertions']));print('\n'.join(x for x in d['assertions'] if x.endswith('FAIL') or 'control' in x));print(d['result'])
PY
