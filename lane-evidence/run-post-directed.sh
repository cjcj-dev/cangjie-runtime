#!/bin/bash
ulimit -c 0
set -u
prefix=/root/sym_cangjie_runtime_571_implement_r5668109509-post
elf=$prefix-final/unit-testable/cj_gc_unit
out=$prefix-evidence
mkdir -p "$out"
uptime > "$out/uptime-before.txt"
for arm in baseline final entry load-remap major-remap load-heal major-heal restored; do
 lib=$prefix-$arm/testable/build/runtime-staging/lib/x86_64_Release
 [ "$arm" = baseline ] && lib=/root/sym_cangjie_runtime_571_implement_r5668109509-baseline/testable/build/runtime-staging/lib/x86_64_Release
 mkdir -p "$out/$arm"
 sha256sum "$elf" "$lib/libcangjie-runtime.so" "$lib/libboundscheck.so" > "$out/$arm/identity.sha256"
 strings "$lib/libcangjie-runtime.so" | /usr/bin/grep 'CJRT-COMMIT:' > "$out/$arm/stamp.txt"
 for name in MinorPublication MajorSeed PlainMinorRejected PlainMajorRejected ColoredAndNullBoundary; do
  for n in 1 2 3; do
   (start=$SECONDS
    taskset -c 0-15 env LD_LIBRARY_PATH="$lib" timeout 45s "$elf" --gtest_filter=NativeRootCurrent.$name > "$out/$arm/$name-$n.log" 2>&1
    echo $? > "$out/$arm/$name-$n.rc"; echo $((SECONDS-start)) > "$out/$arm/$name-$n.wall") &
  done
 done
done
wait
uptime > "$out/uptime-after.txt"
python3 - "$out" <<'PY'
from pathlib import Path
import sys,json
base=Path(sys.argv[1]); rows=[]
for arm in sorted(p for p in base.iterdir() if p.is_dir()):
 for p in sorted(arm.glob('*.rc')):
  rows.append(dict(arm=arm.name,test=p.stem,rc=int(p.read_text()),log=str(p.with_suffix('.log'))))
(base/'directed.json').write_text(json.dumps(rows,indent=2))
for arm in sorted(set(x['arm'] for x in rows)):
 r=[x for x in rows if x['arm']==arm]
 print(arm,'tests=',len(r),'failures=',[(x['test'],x['rc']) for x in r if x['rc']])
PY
