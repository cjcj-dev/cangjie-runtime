#!/bin/bash
ulimit -c 0
R=/root/sym_cangjie_runtime_607_implement_r5738304930-ohos
SRC=/root/sym_cangjie_runtime_607_implement_r5738304930-final3/default/runtime
mkdir -p "$R"
export CCACHE_DIR=/root/.ccache CCACHE_BASEDIR=/root/sym_cangjie_runtime_607_implement_r5738304930-final3/default CCACHE_NOHASHDIR=1 GC_UNIT_GATE_SKIP=1
maps="-ffile-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fdebug-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fmacro-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime"
export CFLAGS="$maps" CXXFLAGS="$maps" ASMFLAGS="$maps"
ccache -M 50G
uptime > "$R/uptime-before.txt"
start=$SECONDS
cmake -S "$SRC" -B "$R/build" -DCJ_RUNTIME_COMMIT=71c23ca31fa6f351c821be17d30c7fed1ccd1326 -DCMAKE_BUILD_TYPE=Release -DCOPYGC_FLAG=1 -DDOPRA_FLAG=1 -DRUNTIME_TRACE_FLAG=1 -DCJ_SDK_VERSION=0.0.1 -DDISABLE_VERSION_CHECK=1 -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_AR_PATH=ar -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_ASM_COMPILER_LAUNCHER=ccache -DMRT_GC_UNIT_OHOS_HOST=ON -DMRT_TESTABLE_INTERNALS=OFF > "$R/configure.log" 2>&1
crc=$?; echo "$crc" > "$R/configure.rc"
if [[ $crc = 0 ]]; then
 cmake --build "$R/build" -j"$(nproc)" > "$R/build.log" 2>&1
 rc=$?; echo "$rc" > "$R/build.rc"
else rc=$crc; fi
if [[ $rc = 0 ]]; then
 LIB=$R/build/runtime-staging/lib/x86_64_Release
 sha256sum "$LIB"/*.so > "$R/so.sha256"
 unset GC_UNIT_GATE_SKIP
 env MRT_GC_UNIT_OHOS_HOST=1 GC_UNIT_OUT="$R/unit" GCV2_RUNTIME_LIB_DIR="$LIB" GCV2_RUNTIME_OUTPUT_ROOT="$R/build/runtime-staging" taskset -c 0-31 bash "$SRC/tests/gc_unit/run_standalone.sh" > "$R/run.log" 2>&1
 rc=$?; echo "$rc" > "$R/run.rc"
fi
echo "$rc" > "$R/arm.rc"
echo "$((SECONDS-start))" > "$R/wall.txt"
uptime > "$R/uptime-after.txt"
echo "OHOS configure=$crc arm=$rc wall=$(cat "$R/wall.txt") jobs=$(nproc)"
grep -E 'error:|GC_UNIT_OHOS|undefined reference|Error [0-9]' "$R/build.log" "$R/run.log" 2>/dev/null | head -12
