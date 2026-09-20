#!/bin/bash
ulimit -c 0
root=/root/sym_cangjie_runtime_720_implement_r5746103729-v4
out=$root/ohos
mkdir -p "$out"
export CCACHE_DIR=/root/.ccache CCACHE_BASEDIR=$root/default CCACHE_NOHASHDIR=1 GC_UNIT_GATE_SKIP=1
maps="-ffile-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fdebug-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fmacro-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime"
export CFLAGS="$maps" CXXFLAGS="$maps" ASMFLAGS="$maps"
uptime > "$out/uptime-before.txt"
start=$SECONDS
cmake -S "$root/default/runtime" -B "$out/build" -DCMAKE_BUILD_TYPE=Release -DCOPYGC_FLAG=1 -DDOPRA_FLAG=1 -DRUNTIME_TRACE_FLAG=1 -DCJ_SDK_VERSION=0.0.1 -DDISABLE_VERSION_CHECK=1 -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_AR_PATH=ar -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_ASM_COMPILER_LAUNCHER=ccache -DMRT_TESTABLE_INTERNALS=ON -DMRT_GC_UNIT_OHOS_HOST=ON -DCJ_RUNTIME_COMMIT=351cad8f0 > "$out/configure.log" 2>&1
rc=$?; echo "$rc" > "$out/configure.rc"
if [ "$rc" = 0 ]; then
 cmake --build "$out/build" -j"$(nproc)" > "$out/build.log" 2>&1
 rc=$?; echo "$rc" > "$out/build.rc"
fi
if [ "$rc" = 0 ]; then
 lib=$out/build/runtime-staging/lib/x86_64_Release
 sha256sum "$lib"/*.so > "$out/so.sha256"
 env GC_UNIT_JOBS=192 MRT_GC_UNIT_OHOS_HOST=1 MRT_TESTABLE_INTERNALS=1 GC_UNIT_OUT="$out/unit" GCV2_RUNTIME_LIB_DIR="$lib" GCV2_RUNTIME_OUTPUT_ROOT="$lib/../.." bash "$root/default/runtime/tests/gc_unit/run_standalone.sh" > "$out/run.log" 2>&1
 rc=$?; echo "$rc" > "$out/run.rc"
fi
echo "$rc" > "$out/overall.rc"
echo $((SECONDS-start)) > "$out/wall.txt"
uptime > "$out/uptime-after.txt"
