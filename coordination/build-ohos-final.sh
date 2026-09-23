#!/usr/bin/env bash
set -uo pipefail
ulimit -c 0
root=/root/sym_cangjie_runtime_906_final_restored
cd "$root" || exit 2
export CCACHE_DIR=/root/.ccache CCACHE_BASEDIR="$root/default" CCACHE_NOHASHDIR=1 GC_UNIT_GATE_SKIP=1
maps="-ffile-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fdebug-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fmacro-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime"
export CFLAGS="$maps" CXXFLAGS="$maps" ASMFLAGS="$maps"
mkdir -p ohos-product-unit
uptime > ohos-product-unit/uptime-before.txt
start=$SECONDS
cmake -S "$root/default/runtime" -B "$root/ohos-build" -DCJ_RUNTIME_COMMIT=59d1c122388e335cd7295d5b3a05a9a42e0dcf10 -DCMAKE_BUILD_TYPE=Release -DCOPYGC_FLAG=1 -DDOPRA_FLAG=1 -DRUNTIME_TRACE_FLAG=1 -DCJ_SDK_VERSION=0.0.1 -DDISABLE_VERSION_CHECK=1 -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_AR_PATH=ar -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_ASM_COMPILER_LAUNCHER=ccache -DMRT_TESTABLE_INTERNALS=ON -DMRT_GC_UNIT_OHOS_HOST=ON -DCMAKE_INSTALL_PREFIX="$root/ohos-install" > ohos-configure.log 2>&1
rc=$?; echo "$rc" > ohos-configure.rc
if [[ "$rc" == 0 ]]; then
    cmake --build "$root/ohos-build" -j"$(nproc)" > ohos-build.log 2>&1
    rc=$?; echo "$rc" > ohos-build.rc
fi
if [[ "$rc" == 0 ]]; then
    lib="$root/ohos-build/runtime-staging/lib/x86_64_Release"
    sha256sum "$lib/"*.so > ohos-product-unit/so.sha256
    MRT_GC_UNIT_OHOS_HOST=1 MRT_TESTABLE_INTERNALS=1 GC_UNIT_OUT="$root/ohos-product-unit" GCV2_RUNTIME_LIB_DIR="$lib" GCV2_RUNTIME_OUTPUT_ROOT="$lib/../.." taskset -c 96-111 bash "$root/default/runtime/tests/gc_unit/run_standalone.sh" > ohos-product-unit/run.log 2>&1
    rc=$?
fi
echo "$rc" > ohos-product-unit/run.rc
printf 'wall=%s jobs=%s\n' "$((SECONDS-start))" "$(nproc)" > ohos-product-unit/wall.txt
uptime > ohos-product-unit/uptime-after.txt
printf 'OHOS_RC=%s\n' "$rc"
exit "$rc"
