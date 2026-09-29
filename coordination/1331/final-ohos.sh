#!/bin/bash
ulimit -c 0
r=/root/sym_cangjie_runtime_1331_implement_r5895529148
cd "$r" || exit 2
rm -rf ohos-tree ohos-build unit-ohos
mkdir -p ohos-tree && tar -xzf source.tar.gz -C ohos-tree
export CCACHE_DIR=/root/.ccache CCACHE_BASEDIR=$r/ohos-tree CCACHE_NOHASHDIR=1 GC_UNIT_GATE_SKIP=1
m="-ffile-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fdebug-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fmacro-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime"
export CFLAGS="$m" CXXFLAGS="$m" ASMFLAGS="$m"
mkdir -p unit-ohos
uptime > unit-ohos/uptime-before.txt
start=$SECONDS
cmake -S "$r/ohos-tree/runtime" -B "$r/ohos-build" -DCJ_RUNTIME_COMMIT=6955edada9a8babe8b7eb8dcf8557654542385d7 \
  -DCMAKE_BUILD_TYPE=Release -DCOPYGC_FLAG=1 -DDOPRA_FLAG=1 -DRUNTIME_TRACE_FLAG=1 \
  -DCJ_SDK_VERSION=0.0.1 -DDISABLE_VERSION_CHECK=1 -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_AR_PATH=ar -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_ASM_COMPILER_LAUNCHER=ccache \
  -DMRT_TESTABLE_INTERNALS=ON -DMRT_GC_UNIT_OHOS_HOST=ON \
  -DCMAKE_INSTALL_PREFIX="$r/ohos-install" > unit-ohos/configure.log 2>&1
rc=$?; echo "$rc" > unit-ohos/configure.rc
if [ "$rc" = 0 ]; then
  cmake --build "$r/ohos-build" -j$(nproc) > unit-ohos/build.log 2>&1
  rc=$?; echo "$rc" > unit-ohos/build.rc
fi
if [ "$rc" = 0 ]; then
  lib=$r/ohos-build/runtime-staging/lib/x86_64_Release
  sha256sum "$lib"/*.so > unit-ohos/so.sha256
  env GC_UNIT_OHOS_HOST=1 GC_UNIT_OUT="$r/unit-ohos" GCV2_RUNTIME_LIB_DIR="$lib" \
    GCV2_RUNTIME_OUTPUT_ROOT="$lib/../.." \
    bash "$r/ohos-tree/runtime/tests/gc_unit/run_standalone.sh" > unit-ohos/run.log 2>&1
  rc=$?
fi
echo "$rc" > unit-ohos/run.rc
uptime > unit-ohos/uptime-after.txt
echo "OHOS_RC=$rc wall=$((SECONDS-start))s configure=$(cat unit-ohos/configure.rc)"
/usr/bin/grep -oE 'GC_UNIT_PARALLEL jobs=[0-9]+ tests=[0-9]+ wall=[0-9.]+' unit-ohos/run.log | tail -1
