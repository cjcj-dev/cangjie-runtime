#!/bin/bash
set -euo pipefail
ulimit -c 0
R=/root/sym_cangjie_runtime_627_implement_r5747848521
S=$R/testable/runtime
LIB=$R/testable/build/runtime-staging/lib/x86_64_Release
TAG=${1:?tag}
OUT=$R/$TAG
mkdir -p "$OUT"
flags=(-std=gnu++17 -O0 -g -Wall -Wextra -pthread -fno-rtti -fvisibility-inlines-hidden -DMRT_TESTABLE_INTERNALS=1 -DMRT_PRODUCT_TESTABLE_INTERNALS=1)
for inc in tests/gc_unit src src/Loader/BinaryFile src/Heap src/Heap/z/os/linux src/CJThread/src/runtime/schedule/include include third_party/third_party_bounds_checking_function/include; do flags+=(-I"$S/$inc"); done
flags+=(-I"$LIB/../../include")
start=$SECONDS
clang++ "${flags[@]}" -c "$S/tests/gc_unit/clear_entries_product_unit.cpp" -o "$OUT/publication.o" > "$OUT/build.log" 2>&1
clang++ "${flags[@]}" "$R"/unit-initial/objects/publication/000{0,1,2}-*.o "$OUT/publication.o" -L"$LIB" -Wl,-rpath,"$LIB" -Wl,--exclude-libs,ALL -lcangjie-runtime -lboundscheck -o "$OUT/cj_gc_forwarding_publication_unit" >> "$OUT/build.log" 2>&1
sha256sum "$OUT/cj_gc_forwarding_publication_unit" "$LIB"/*.so > "$OUT/identity.sha256"
printf 'build_rc=0 wall=%s\n' "$((SECONDS-start))"
python3 - "$OUT" "$LIB" <<'PY'
import sys,pathlib,os,subprocess,concurrent.futures,json,time
out=pathlib.Path(sys.argv[1]);lib=sys.argv[2];elf=out/'cj_gc_forwarding_publication_unit'
env=dict(os.environ,LD_LIBRARY_PATH=lib)
names=[];suite=''
for line in subprocess.check_output([str(elf),'--gtest_list_tests'],env=env,text=True).splitlines():
 if line.endswith('.'): suite=line[:-1]
 elif suite=='RawRemapYoungProduct' and line.strip(): names.append(suite+'.'+line.strip())
def run(name):
 with (out/(name+'.log')).open('w') as f:
  try: rc=subprocess.run(['taskset','-c','80-95',str(elf),'--gtest_filter='+name],env=env,stdout=f,stderr=subprocess.STDOUT,timeout=60).returncode
  except subprocess.TimeoutExpired: rc=124
 return dict(name=name,rc=rc)
with concurrent.futures.ThreadPoolExecutor(max_workers=10) as ex: results=list(ex.map(run,names))
(out/'results.json').write_text(json.dumps(results,indent=2))
print(json.dumps(results))
PY
