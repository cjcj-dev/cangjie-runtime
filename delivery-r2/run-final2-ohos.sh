#!/bin/bash
ulimit -c 0
root=/root/sym_cangjie_runtime_606_implement_r5674249495-final2
cd "$root" || exit 2
mkdir -p ohos-cwdfix ohos-src
tar -xzf "$root/source.tar.gz" -C "$root/ohos-src"
cd "$root/ohos-src/runtime" || exit 2
export CCACHE_DIR=/root/.ccache CCACHE_BASEDIR="$root/ohos-src" CCACHE_NOHASHDIR=1 GC_UNIT_GATE_SKIP=1 PATH=/usr/lib/ccache:$PATH
maps="-ffile-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fdebug-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fmacro-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime"
export CFLAGS="$maps" CXXFLAGS="$maps" ASMFLAGS="$maps"
uptime > $root/ohos-cwdfix/uptime-before.txt
start=$SECONDS
cmake -S "$root/ohos-src/runtime" -B "$root/ohos-cwdfix/build" -DCJ_RUNTIME_COMMIT=9de9226b8d58ed655534b195df2f6938be789062 -DCMAKE_BUILD_TYPE=Release -DCOPYGC_FLAG=1 -DDOPRA_FLAG=1 -DRUNTIME_TRACE_FLAG=1 -DCJ_SDK_VERSION=0.0.1 -DDISABLE_VERSION_CHECK=1 -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_AR_PATH=ar -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_ASM_COMPILER_LAUNCHER=ccache -DMRT_TESTABLE_INTERNALS=ON -DMRT_GC_UNIT_OHOS_HOST=ON -DCMAKE_INSTALL_PREFIX="$root/ohos-cwdfix/install" > $root/ohos-cwdfix/configure.log 2>&1
rc=$?; echo "$rc" > $root/ohos-cwdfix/configure.rc
if [ "$rc" = 0 ]; then
 cmake --build "$root/ohos-cwdfix/build" -j"$(nproc)" > $root/ohos-cwdfix/build.log 2>&1
 rc=$?; echo "$rc" > $root/ohos-cwdfix/build.rc
fi
if [ "$rc" = 0 ]; then
 export GCV2_RUNTIME_LIB_DIR="$root/ohos-cwdfix/build/runtime-staging/lib/x86_64_Release"
 sha256sum "$GCV2_RUNTIME_LIB_DIR/libcangjie-runtime.so" "$GCV2_RUNTIME_LIB_DIR/libboundscheck.so" > $root/ohos-cwdfix/product.sha256
 export MRT_GC_UNIT_OHOS_HOST=1 MRT_TESTABLE_INTERNALS=1 GC_UNIT_OUT="$root/ohos-cwdfix/unit"
 bash "$root/ohos-src/runtime/tests/gc_unit/run_standalone.sh" > $root/ohos-cwdfix/unit.log 2>&1
 rc=$?; echo "$rc" > $root/ohos-cwdfix/unit.rc
fi
echo "$rc" > $root/ohos-cwdfix/rc
echo "$((SECONDS-start))" > $root/ohos-cwdfix/wall
uptime > $root/ohos-cwdfix/uptime-after.txt
echo "OHOS_RC=$rc wall=$(cat $root/ohos-cwdfix/wall)"
/usr/bin/grep -n -m 10 -E 'error:|fatal:|FAIL|MISSING|OHOS_HOST' $root/ohos-cwdfix/unit.log $root/ohos-cwdfix/build.log 2>/dev/null
