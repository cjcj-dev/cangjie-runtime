#!/bin/bash
ulimit -c 0
set -u
cd /root/sym917_r2_green || exit 2
export CCACHE_DIR=/root/.ccache CCACHE_BASEDIR=$PWD/default CCACHE_NOHASHDIR=1 GC_UNIT_GATE_SKIP=1
maps="-ffile-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fdebug-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fmacro-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime"
export CFLAGS="$maps" CXXFLAGS="$maps" ASMFLAGS="$maps"
uptime > ohos-uptime-before.txt
start=$SECONDS
cmake -S "$PWD/default/runtime" -B "$PWD/ohos-build" -DCJ_RUNTIME_COMMIT=3e503f922a2fb9476f5760ac0ecfc7833c4c8c65 -DCMAKE_BUILD_TYPE=Release -DCOPYGC_FLAG=1 -DDOPRA_FLAG=1 -DRUNTIME_TRACE_FLAG=1 -DCJ_SDK_VERSION=0.0.1 -DDISABLE_VERSION_CHECK=1 -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_AR_PATH=ar -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_ASM_COMPILER_LAUNCHER=ccache -DMRT_TESTABLE_INTERNALS=ON -DMRT_GC_UNIT_OHOS_HOST=ON -DCMAKE_INSTALL_PREFIX="$PWD/ohos-install" > ohos-configure.log 2>&1
rc=$?; echo "$rc" > ohos-configure.rc
if [ "$rc" = 0 ]; then
 cmake --build "$PWD/ohos-build" -j"$(nproc)" > ohos-build.log 2>&1
 rc=$?; echo "$rc" > ohos-build.rc
fi
if [ "$rc" = 0 ]; then
 export GCV2_RUNTIME_LIB_DIR=$PWD/ohos-build/runtime-staging/lib/x86_64_Release GC_UNIT_OUT=$PWD/unit-ohos MRT_GC_UNIT_OHOS_HOST=1
 sha256sum "$GCV2_RUNTIME_LIB_DIR"/*.so > ohos-so.sha256
 bash default/runtime/tests/gc_unit/run_standalone.sh > ohos-run.log 2>&1
 rc=$?
fi
echo "$rc" > ohos.rc
echo "$((SECONDS-start))" > ohos-wall.txt
uptime > ohos-uptime-after.txt
