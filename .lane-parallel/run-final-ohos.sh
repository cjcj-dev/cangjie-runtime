#!/bin/bash
ulimit -c 0
set -u
root=/root/sym_cangjie_runtime_700_implement_r5723534433-final-green
cd "$root" || exit 2
export CCACHE_DIR=/root/.ccache CCACHE_BASEDIR="$root/testable" CCACHE_NOHASHDIR=1 GC_UNIT_GATE_SKIP=1
export PATH=/usr/lib/ccache:$PATH
maps="-ffile-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fdebug-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fmacro-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime"
export CFLAGS="$maps" CXXFLAGS="$maps" ASMFLAGS="$maps"
mkdir -p ohos-evidence
uptime > ohos-evidence/uptime-before.txt
start=$SECONDS
cmake -S "$root/testable/runtime" -B "$root/ohos-build" -DCJ_RUNTIME_COMMIT=936c2943d7244faca1bca743ad53b83fad3d90bb -DCMAKE_BUILD_TYPE=Release -DCOPYGC_FLAG=1 -DDOPRA_FLAG=1 -DRUNTIME_TRACE_FLAG=1 -DCJ_SDK_VERSION=0.0.1 -DDISABLE_VERSION_CHECK=1 -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_AR_PATH=ar -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_ASM_COMPILER_LAUNCHER=ccache -DMRT_TESTABLE_INTERNALS=ON -DMRT_GC_UNIT_OHOS_HOST=ON > ohos-evidence/configure.log 2>&1
rc=$?; echo "$rc" > ohos-evidence/configure.rc
if [ "$rc" = 0 ]; then
 cmake --build "$root/ohos-build" -j"$(nproc)" > ohos-evidence/build.log 2>&1
 rc=$?; echo "$rc" > ohos-evidence/build.rc
fi
if [ "$rc" = 0 ]; then
 lib="$root/ohos-build/runtime-staging/lib/x86_64_Release"
 sha256sum "$lib"/*.so > ohos-evidence/so.sha256
 env MRT_GC_UNIT_OHOS_HOST=1 MRT_TESTABLE_INTERNALS=1 GC_UNIT_OUT="$root/ohos-evidence" GCV2_RUNTIME_LIB_DIR="$lib" GCV2_RUNTIME_OUTPUT_ROOT="$lib/../.." bash "$root/testable/runtime/tests/gc_unit/run_standalone.sh" > ohos-evidence/run.log 2>&1
 rc=$?
fi
echo "$rc" > ohos-evidence/run.rc
echo "$((SECONDS-start))" > ohos-evidence/wall.txt
uptime > ohos-evidence/uptime-after.txt
printf 'OHOS_RC=%s wall=%s\n' "$rc" "$(cat ohos-evidence/wall.txt)"
tail -10 ohos-evidence/run.log 2>/dev/null
