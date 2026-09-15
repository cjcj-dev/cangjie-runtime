#!/bin/bash
set -u
ulimit -c 0
arm=$1
R=/root/sym_cangjie_runtime_607_implement_r5684610492-build7
case "$arm" in
 green) P="$R/testable"; cores=112-119;;
 cut) P=/root/sym_cangjie_runtime_607_implement_r5684610492-final-duplicate/testable; cores=120-127;;
 restored) P=/root/sym_cangjie_runtime_607_implement_r5684610492-final-restored/testable; cores=128-135;;
 *) exit 2;;
esac
SO="$P/build/runtime-staging/lib/x86_64_Release"
ELF="$R/unit-testable/cj_gc_unit"
O="$R/qualified-duplicate/$arm"
mkdir -p "$O"
uptime > "$O/before.txt"
sha256sum "$ELF" "$SO/libcangjie-runtime.so" "$SO/libboundscheck.so" > "$O/identity.sha256"
strings "$SO/libcangjie-runtime.so" | /usr/bin/grep '^CJRT-' > "$O/lineage.txt"
for test in FinalDiscoveryIsClaimedOnce FinalDiscoveryProcessEnqueue StrongUpgradeDropsFinalReference; do
 (start=$(date +%s%N); LD_LIBRARY_PATH="$SO" taskset -c "$cores" timeout 60s "$ELF" "--gtest_filter=ReferenceProcessor.$test" > "$O/$test.log" 2>&1; echo "$?" > "$O/$test.rc"; echo "wall_ns=$(($(date +%s%N)-start))" > "$O/$test.wall") &
done
wait
uptime > "$O/after.txt"
python3 - "$O" <<'PY'
import json,sys
from pathlib import Path
p=Path(sys.argv[1]); result={}
for q in p.glob('*.rc'):
 result[q.stem]={'rc':int(q.read_text()),'log':q.with_suffix('.log').read_text()}
(p/'result.json').write_text(json.dumps(result,indent=2)+'\n'); print(p.name,{k:v['rc'] for k,v in result.items()})
PY
