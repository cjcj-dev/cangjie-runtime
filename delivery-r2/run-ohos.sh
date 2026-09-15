#!/bin/bash
ulimit -c 0
root=/root/sym_cangjie_runtime_606_implement_r5674249495-green3
cd "$root" || exit 2
mkdir -p ohos
export CCACHE_DIR=/root/.ccache CCACHE_BASEDIR="$root/default" CCACHE_NOHASHDIR=1 GC_UNIT_GATE_SKIP=1 PATH=/usr/lib/ccache:$PATH
maps="-ffile-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fdebug-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fmacro-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime"
export CFLAGS="$maps" CXXFLAGS="$maps" ASMFLAGS="$maps"
uptime > ohos/uptime-before.txt
start=$SECONDS
cmake -S "$root/default/runtime" -B "$root/ohos/build" -DCJ_RUNTIME_COMMIT=45ab94b360655accd50673f98d6763061eded217 -DCMAKE_BUILD_TYPE=Release -DCOPYGC_FLAG=1 -DDOPRA_FLAG=1 -DRUNTIME_TRACE_FLAG=1 -DCJ_SDK_VERSION=0.0.1 -DDISABLE_VERSION_CHECK=1 -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_AR_PATH=ar -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_ASM_COMPILER_LAUNCHER=ccache -DMRT_TESTABLE_INTERNALS=ON -DMRT_GC_UNIT_OHOS_HOST=ON -DCMAKE_INSTALL_PREFIX="$root/ohos/install" > ohos/configure.log 2>&1
rc=$?; echo "$rc" > ohos/configure.rc
if [ "$rc" = 0 ]; then
 cmake --build "$root/ohos/build" -j"$(nproc)" > ohos/build.log 2>&1
 rc=$?; echo "$rc" > ohos/build.rc
fi
if [ "$rc" = 0 ]; then
 export GCV2_RUNTIME_LIB_DIR="$root/ohos/build/runtime-staging/lib/x86_64_Release"
 sha256sum "$GCV2_RUNTIME_LIB_DIR/libcangjie-runtime.so" "$GCV2_RUNTIME_LIB_DIR/libboundscheck.so" > ohos/product.sha256
 export MRT_GC_UNIT_OHOS_HOST=1 MRT_TESTABLE_INTERNALS=1 GC_UNIT_OUT="$root/ohos/unit"
 bash "$root/default/runtime/tests/gc_unit/run_standalone.sh" > ohos/unit.log 2>&1
 rc=$?; echo "$rc" > ohos/unit.rc
fi
echo "$rc" > ohos/rc
echo "$((SECONDS-start))" > ohos/wall
uptime > ohos/uptime-after.txt
echo "OHOS_RC=$rc wall=$(cat ohos/wall)"
/usr/bin/grep -n -m 10 -E 'error:|fatal:|FAIL|MISSING|OHOS_HOST' ohos/unit.log ohos/build.log 2>/dev/null
