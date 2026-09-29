#!/usr/bin/env bash
set -u
ulimit -c 0
r=/root/sym_cangjie_runtime_1305_implement_r5892797534
mkdir -p "$r/keep/tests" "$r/matrix"
cp "$r/unit-final-testable/cj_gc_unit" "$r/keep/tests/cj_gc_unit"
elf="$r/keep/tests/cj_gc_unit"
sha256sum "$elf" > "$r/matrix/elf.sha256"
nm --defined-only "$elf" | c++filt > "$r/matrix/elf.defined.txt"
nm -u "$elf" | c++filt > "$r/matrix/elf.undefined.txt"
# A positive control (the test functions) distinguishes a working nm from an
# empty command result. Product entry definitions must be absent from the ELF.
/usr/bin/grep P82HeapIterator "$r/matrix/elf.defined.txt" > "$r/matrix/test-symbols.txt"
/usr/bin/grep 'HeapIterator::' "$r/matrix/elf.undefined.txt" > "$r/matrix/product-imports.txt"
/usr/bin/grep 'HeapIterator::' "$r/matrix/elf.defined.txt" > "$r/matrix/product-copies.txt"
echo "$?" > "$r/matrix/product-copies.grep-rc"
uptime > "$r/matrix/uptime-before.txt"
for arm in candidate restored termination objects arrays bitmap entry; do
 [[ $(cat "$r/arms/$arm/build.rc") == 0 ]] || exit 2
 sha=$(sha256sum "$r/keep/$arm/libcangjie-runtime.so" | cut -d' ' -f1)
 depot="/root/sodepot/$sha"
 mkdir -p "$depot"
 for name in libcangjie-runtime.so libboundscheck.so libcangjie-trace.so; do
   if [[ -e "$depot/$name" ]]; then cmp "$r/keep/$arm/$name" "$depot/$name" || exit 3
   else cp "$r/keep/$arm/$name" "$depot/$name"; fi
 done
 mkdir -p "$r/matrix/$arm"
 sha256sum "$depot/"*.so > "$r/matrix/$arm/so.sha256"
 echo "$depot" > "$r/matrix/$arm/library-path.txt"
 cases='TerminationAgreement ReachableSet VisitsOnce ArrayChunks OverflowRoots'
 # The phase-entry cut intentionally removes the initial roots. Its witness
 # enters Heap::object_iterate directly and checks the returned object set.
 [[ $arm == entry ]] && cases='OverflowRoots'
 for test in $cases; do
   (
     start=$SECONDS
     timeout 40 taskset -c 48-63 env LD_LIBRARY_PATH="$depot" "$elf" --gtest_filter="P82HeapIterator.$test" > "$r/matrix/$arm/$test.log" 2>&1
     echo "$?" > "$r/matrix/$arm/$test.rc"
     echo "$((SECONDS-start))" > "$r/matrix/$arm/$test.wall"
   ) &
 done
done
wait
uptime > "$r/matrix/uptime-after.txt"
python3 - "$r" <<'PY'
import json,sys,pathlib
r=pathlib.Path(sys.argv[1]); matrix={}
for d in sorted((r/'matrix').iterdir()):
 if not d.is_dir(): continue
 matrix[d.name]={}
 for p in sorted(d.glob('*.rc')):
  name=p.stem; rc=int(p.read_text()); log=(d/(name+'.log')).read_text()
  markers=[x for x in log.splitlines() if 'P82_' in x and ('ASSERT' in x or 'TERMINATION' in x)]
  matrix[d.name][name]={'rc':rc,'assertions':markers,'log':str(d/(name+'.log'))}
  print(d.name,name,'rc='+str(rc),'; '.join(markers))
(r/'matrix/results.json').write_text(json.dumps(matrix,indent=2)+'\n')
PY
