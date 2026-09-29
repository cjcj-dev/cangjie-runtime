#!/bin/bash
ulimit -c 0
r=/root/sym_cangjie_runtime_1331_implement_r5895529148
export CCACHE_DIR=/root/.ccache CCACHE_BASEDIR=$r/default CCACHE_NOHASHDIR=1 GC_UNIT_GATE_SKIP=1
maps="-ffile-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fdebug-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fmacro-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime"
export CFLAGS="$maps" CXXFLAGS="$maps" ASMFLAGS="$maps"
mkdir -p "$r/unit-ohos"
uptime > "$r/unit-ohos/uptime-before.txt"
start=$SECONDS
cmake -S "$r/default/runtime" -B "$r/ohos-build" -DCMAKE_BUILD_TYPE=Release -DCOPYGC_FLAG=1 -DDOPRA_FLAG=1 -DRUNTIME_TRACE_FLAG=1 -DCJ_SDK_VERSION=0.0.1 -DDISABLE_VERSION_CHECK=1 -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_AR_PATH=ar -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_ASM_COMPILER_LAUNCHER=ccache -DMRT_TESTABLE_INTERNALS=ON -DMRT_GC_UNIT_OHOS_HOST=ON -DCMAKE_INSTALL_PREFIX="$r/ohos-install" > "$r/unit-ohos/configure.log" 2>&1
rc=$?; echo "$rc" > "$r/unit-ohos/configure.rc"
if [ "$rc" = 0 ]; then
 cmake --build "$r/ohos-build" -j$(nproc) > "$r/unit-ohos/build.log" 2>&1
 rc=$?; echo "$rc" > "$r/unit-ohos/build.rc"
fi
if [ "$rc" = 0 ]; then
 lib=$r/ohos-build/runtime-staging/lib/x86_64_Release
 sha256sum "$lib"/*.so > "$r/unit-ohos/so.sha256"
 env GC_UNIT_OHOS_HOST=1 GC_UNIT_OUT="$r/unit-ohos" GCV2_RUNTIME_LIB_DIR="$lib" GCV2_RUNTIME_OUTPUT_ROOT="$lib/../.." bash "$r/default/runtime/tests/gc_unit/run_standalone.sh" > "$r/unit-ohos/run.log" 2>&1
 rc=$?
fi
echo "$rc" > "$r/unit-ohos/run.rc"
uptime > "$r/unit-ohos/uptime-after.txt"
echo "OHOS_RC=$rc wall=$((SECONDS-start))"
