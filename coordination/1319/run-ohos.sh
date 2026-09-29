#!/usr/bin/env bash
ulimit -c 0
root=/root/sym_cangjie_runtime_1319_implement_r5899659797
mkdir -p "$root/ohos-host-libs"
cp "$(clang++ -print-file-name=libc.so.6)" "$root/ohos-host-libs/libc.so"
export LD_LIBRARY_PATH="$root/ohos-host-libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export CCACHE_DIR=/root/.ccache CCACHE_BASEDIR="$root/testable" CCACHE_NOHASHDIR=1 GC_UNIT_GATE_SKIP=1
ccache -M 50G
maps="-ffile-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fdebug-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fmacro-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime"
export CFLAGS="$maps" CXXFLAGS="$maps" ASMFLAGS="$maps"
uptime > "$root/ohos-uptime-before.txt"
start=$SECONDS
cmake -S "$root/testable/runtime" -B "$root/ohos-build" -DCJ_RUNTIME_COMMIT=b645992f7cbf11e985529f722cff62e72444f3c0 -DCMAKE_BUILD_TYPE=Release -DCOPYGC_FLAG=1 -DDOPRA_FLAG=1 -DRUNTIME_TRACE_FLAG=1 -DCJ_SDK_VERSION=0.0.1 -DDISABLE_VERSION_CHECK=1 -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_AR_PATH=ar -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_ASM_COMPILER_LAUNCHER=ccache -DMRT_TESTABLE_INTERNALS=ON -DMRT_GC_UNIT_OHOS_HOST=ON > "$root/ohos-configure.log" 2>&1
rc=$?; echo "$rc" > "$root/ohos-configure.rc"
if [ "$rc" = 0 ]; then cmake --build "$root/ohos-build" -j"$(nproc)" > "$root/ohos-build.log" 2>&1; rc=$?; fi
echo "$rc" > "$root/ohos-build.rc"
if [ "$rc" = 0 ]; then
 export GCV2_RUNTIME_LIB_DIR="$root/ohos-build/runtime-staging/lib/x86_64_Release" GCV2_RUNTIME_OUTPUT_ROOT="$root/ohos-build/runtime-staging" GC_UNIT_OUT="$root/unit-ohos" MRT_GC_UNIT_OHOS_HOST=1
 sha256sum "$GCV2_RUNTIME_LIB_DIR"/{libcangjie-runtime,libboundscheck}.so > "$root/ohos-so.sha256"
 bash "$root/testable/runtime/tests/gc_unit/run_standalone.sh" > "$root/unit-ohos.log" 2>&1; rc=$?
fi
echo "$rc" > "$root/unit-ohos.rc"
echo "OHOS_RC=$rc wall=$((SECONDS-start))"
uptime > "$root/ohos-uptime-after.txt"
/usr/bin/grep -m 15 -E 'error:|undefined reference|GC_UNIT_OHOS_HOST_FILTER|SUMMARY' "$root/ohos-build.log" "$root/unit-ohos.log" 2>/dev/null | cut -c1-250
