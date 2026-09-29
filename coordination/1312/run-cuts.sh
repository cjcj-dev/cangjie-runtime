#!/bin/bash
ulimit -c 0
r=/root/sym_cangjie_runtime_1312_implement_r5899117285
cd "$r" || exit 2
export CCACHE_DIR=/root/.ccache CCACHE_NOHASHDIR=1 GC_UNIT_GATE_SKIP=1
ccache -M 50G
uptime > cuts-uptime-before.txt
run_test() {
 arm=$1; name=$2
 out="$r/cuts/$arm"; lib="$out/keep/lib"
 [ "$arm" = candidate ] && lib="$r/testable/build/runtime-staging/lib/x86_64_Release"
 start=$SECONDS
 env LD_LIBRARY_PATH="$lib" timeout 90 "$r/unit-testable/cj_gc_unit" "--gtest_filter=StringDedup.$name" > "$out/$name.log" 2>&1
 rc=$?
 echo "$rc" > "$out/$name.rc"
 echo "$((SECONDS-start))" > "$out/$name.wall"
}
export r; export -f run_test
build_arm() {
 arm=$1
 out="$r/cuts/$arm"
 mkdir -p "$out/keep/lib"
 start=$SECONDS
 if [ "$arm" != candidate ]; then
  mkdir -p "$out"
  cp -a "$r/testable/runtime" "$out/"
  if [ -s "$r/patches/$arm.diff" ]; then (cd "$out" && patch -p1 < "$r/patches/$arm.diff") > "$out/patch.log" 2>&1 || return 2; fi
  export CCACHE_BASEDIR="$out"
  maps="-ffile-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fdebug-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fmacro-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime"
  export CFLAGS="$maps" CXXFLAGS="$maps" ASMFLAGS="$maps"
  cmake -S "$out/runtime" -B "$out/build" -DCJ_RUNTIME_COMMIT=02c9e385772bf1f53369f8263e2a8e8e66edec04 -DCMAKE_BUILD_TYPE=Release -DCOPYGC_FLAG=1 -DDOPRA_FLAG=1 -DRUNTIME_TRACE_FLAG=1 -DCJ_SDK_VERSION=0.0.1 -DDISABLE_VERSION_CHECK=1 -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_AR_PATH=ar -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_ASM_COMPILER_LAUNCHER=ccache -DMRT_TESTABLE_INTERNALS=ON -DCMAKE_INSTALL_PREFIX="$out/install" > "$out/configure.log" 2>&1
  rc=$?; echo "$rc" > "$out/configure.rc"
  if [ "$rc" = 0 ]; then cmake --build "$out/build" -j$(nproc) > "$out/build.log" 2>&1; rc=$?; fi
  echo "$rc" > "$out/build.rc"
  if [ "$rc" != 0 ]; then echo "$arm BUILD_FAILED=$rc"; /usr/bin/grep -m 4 'error:' "$out/build.log"; return "$rc"; fi
  cp "$out/build/runtime-staging/lib/x86_64_Release/"*.so "$out/keep/lib/"
  find "$out/runtime/output/temp" -name runtime-build-inputs.txt -exec cp {} "$out/keep/" \;
  sha256sum "$out/keep/lib/"*.so > "$out/so.sha256"
 else
  sha256sum "$r/testable/build/runtime-staging/lib/x86_64_Release/"*.so > "$out/so.sha256"
 fi
 sha256sum "$r/unit-testable/cj_gc_unit" > "$out/elf.sha256"
 sed "s/^/$arm /" "$r/dedup-tests.txt" | xargs -n2 -P$(nproc) bash -c 'run_test "$1" "$2"' _
 echo "$((SECONDS-start))" > "$out/wall.txt"
 # Completed build objects are reconstructible; keep SO identity and failure logs.
 rm -rf "$out/build"
 echo "$arm DONE wall=$(cat "$out/wall.txt")"
}
export -f build_arm
# Independent candidate/cut/restored arms. Limit arm batches to four so cache
# misses in a header cut do not multiply compiler memory across nine builds.
for arm in candidate restored keepalive peek; do build_arm "$arm" & done
wait
for arm in young phase report; do build_arm "$arm" & done
wait
for arm in resize_find wait2 blocked; do build_arm "$arm" & done
wait
uptime > cuts-uptime-after.txt
python3 - <<'PY'
from pathlib import Path
import json
r=Path('/root/sym_cangjie_runtime_1312_implement_r5899117285/cuts')
result={}
for arm in sorted(r.iterdir()):
 result[arm.name]={p.stem:int(p.read_text()) for p in arm.glob('*.rc')}
 print(arm.name, {k:v for k,v in result[arm.name].items() if v}, 'tests=',sum(k not in ('configure','build') for k in result[arm.name]))
(r/'results.json').write_text(json.dumps(result,indent=2))
PY
