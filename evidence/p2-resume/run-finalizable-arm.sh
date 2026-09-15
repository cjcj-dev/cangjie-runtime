#!/bin/bash
set -u
ulimit -c 0
arm=$1
R=/root/sym_cangjie_runtime_607_implement_r5684610492-build7
A="$R/closure-artifacts"
case "$arm" in
 green) P="$R/testable"; cores=112-119;;
 producer) P=/root/sym_cangjie_runtime_607_implement_r5684610492-final-producer/testable; cores=120-127;;
 consumer) P=/root/sym_cangjie_runtime_607_implement_r5684610492-final-field/testable; cores=128-135;;
 restored) P=/root/sym_cangjie_runtime_607_implement_r5684610492-final-restored/testable; cores=112-119;;
 *) exit 2;;
esac
SO="$P/build/runtime-staging/lib/x86_64_Release"
SDK=/root/sym_cangjie_runtime_607_implement_r5684610492-build3/sdk
O="$R/qualified-finalizable/$arm"
mkdir -p "$O"
printf 'arm=%s cores=%s\n' "$arm" "$cores" > "$O/config.txt"
uptime > "$O/uptime-before.txt"
sha256sum "$A/p2_field_barrier" "$A/libp2_field_barrier.so" "$SO/libcangjie-runtime.so" "$SO/libboundscheck.so" > "$O/identity.sha256"
strings "$SO/libcangjie-runtime.so" | /usr/bin/grep '^CJRT-' > "$O/lineage.txt"
start=$(date +%s%N)
LD_DEBUG=libs LD_LIBRARY_PATH="$SO:$A:$SDK/runtime/lib/linux_x86_64_cjnative" cjGCInterval=3600s \
 taskset -c "$cores" timeout 60s "$A/p2_field_barrier" > "$O/run.log" 2>&1
rc=$?
echo "$rc" > "$O/run.rc"
echo "wall_ns=$(($(date +%s%N)-start))" > "$O/wall.txt"
uptime > "$O/uptime-after.txt"
python3 - "$O" <<'PY'
from pathlib import Path
import json,sys
p=Path(sys.argv[1]); lines=(p/'run.log').read_text().splitlines(); d={'rc':int((p/'run.rc').read_text()),'assertions':[x for x in lines if x.startswith('P2_ASSERT ')], 'result':[x for x in lines if x.startswith('P2_CLOSURE_RESULT')]}; (p/'result.json').write_text(json.dumps(d,indent=2)+'\n');print(p.name,d['rc'],len(d['assertions']));print('\n'.join(x for x in d['assertions'] if x.endswith('FAIL') or 'root_control' in x));print(d['result'])
PY
