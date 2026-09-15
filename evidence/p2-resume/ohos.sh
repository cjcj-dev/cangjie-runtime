#!/bin/bash
set -u
ulimit -c 0
root=/root/sym_cangjie_runtime_607_implement_r5684610492-build3/ohos
mkdir -p "$root/evidence"
mkdir -p "$root/source"
cp -a --reflink=auto /root/sym_cangjie_runtime_607_implement_r5684610492-build3/default/runtime "$root/source/runtime"
export CCACHE_DIR=/root/.ccache CCACHE_BASEDIR="$root/source" CCACHE_NOHASHDIR=1
export GC_UNIT_GATE_SKIP=1 GC_UNIT_OUT="$root/unit" PATH=/usr/lib/ccache:$PATH
maps="-ffile-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fdebug-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fmacro-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime"
export CFLAGS="$maps" CXXFLAGS="$maps" ASMFLAGS="$maps"
uptime > "$root/evidence/before.txt"
start=$SECONDS
cmake -S "$root/source/runtime" -B "$root/build" -DCJ_RUNTIME_COMMIT=07856fbad358ee08f669e932cf19fabf9f8dcd72 -DCMAKE_BUILD_TYPE=Release -DCOPYGC_FLAG=1 -DDOPRA_FLAG=1 -DRUNTIME_TRACE_FLAG=1 -DCJ_SDK_VERSION=0.0.1 -DDISABLE_VERSION_CHECK=1 -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_AR_PATH=ar -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_ASM_COMPILER_LAUNCHER=ccache -DMRT_GC_UNIT_OHOS_HOST=ON -DMRT_TESTABLE_INTERNALS=ON > "$root/evidence/configure.log" 2>&1
rc=$?; echo "$rc" > "$root/evidence/configure.rc"
if [[ $rc == 0 ]]; then
 cmake --build "$root/build" -j"$(nproc)" --target cangjie-runtime > "$root/evidence/build.log" 2>&1
 rc=$?; echo "$rc" > "$root/evidence/build.rc"
fi
if [[ $rc == 0 ]]; then
 export GCV2_RUNTIME_LIB_DIR="$root/build/runtime-staging/lib/x86_64_Release" MRT_GC_UNIT_OHOS_HOST=1
 sha256sum "$GCV2_RUNTIME_LIB_DIR/libcangjie-runtime.so" "$GCV2_RUNTIME_LIB_DIR/libboundscheck.so" > "$root/evidence/so.sha256"
 bash "$root/source/runtime/tests/gc_unit/run_standalone.sh" > "$root/evidence/run.log" 2>&1
 rc=$?; echo "$rc" > "$root/evidence/run.rc"
fi
echo "$rc" > "$root/evidence/ohos.rc"
echo "$((SECONDS-start))" > "$root/evidence/wall.txt"
uptime > "$root/evidence/after.txt"

echo "UNIT_OHOS_RC=$rc"
/usr/bin/grep -n -E "error:|Error|FAIL|schedule.h" "$root/evidence/configure.log" "$root/evidence/build.log" 2>/dev/null | head -10
