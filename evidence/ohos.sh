#!/bin/bash
ulimit -c 0
root=/root/sym_cangjie_runtime_919_implement_r5786193802
cd "$root" || exit 2
export CCACHE_DIR=/root/.ccache CCACHE_BASEDIR="$root/testable" CCACHE_NOHASHDIR=1 GC_UNIT_GATE_SKIP=1
export PATH=/usr/lib/ccache:$PATH
maps="-ffile-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fdebug-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fmacro-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime"
export CFLAGS="$maps" CXXFLAGS="$maps" ASMFLAGS="$maps"
uptime > ohos-uptime-before.txt
start=$SECONDS
cmake -S "$root/testable/runtime" -B "$root/ohos-build" -DCJ_RUNTIME_COMMIT=30ca42f85bcf5f6ae34a702c0f1b06c890ee17f3 -DCMAKE_BUILD_TYPE=Release -DCOPYGC_FLAG=1 -DDOPRA_FLAG=1 -DRUNTIME_TRACE_FLAG=1 -DCJ_SDK_VERSION=0.0.1 -DDISABLE_VERSION_CHECK=1 -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_AR_PATH=ar -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_ASM_COMPILER_LAUNCHER=ccache -DMRT_TESTABLE_INTERNALS=ON -DMRT_GC_UNIT_OHOS_HOST=ON > ohos-configure.log 2>&1
rc=$?; echo "$rc" > ohos-configure.rc
if [ "$rc" != 0 ]; then exit "$rc"; fi
cmake --build "$root/ohos-build" -j"$(nproc)" > ohos-build.log 2>&1
rc=$?; echo "$rc" > ohos-build.rc
if [ "$rc" != 0 ]; then exit "$rc"; fi
lib="$root/ohos-build/runtime-staging/lib/x86_64_Release"
sha256sum "$lib/libcangjie-runtime.so" "$lib/libboundscheck.so" > ohos-so.sha256
MRT_GC_UNIT_OHOS_HOST=1 GCV2_RUNTIME_LIB_DIR="$lib" GC_UNIT_OUT="$root/unit-ohos" bash "$root/testable/runtime/tests/gc_unit/run_standalone.sh" > ohos-unit.log 2>&1
rc=$?; echo "$rc" > ohos-unit.rc
find "$root/unit-ohos" -maxdepth 1 -type f -executable -exec sha256sum {} + > ohos-elf.sha256
uptime > ohos-uptime-after.txt
echo "$((SECONDS-start))" > ohos-wall.txt
exit "$rc"
