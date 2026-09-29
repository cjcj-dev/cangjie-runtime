#!/usr/bin/env bash
# Single configuration arms only. Two configurations use kkk2_build_two.sh.
set -u
ulimit -c 0
lane=sym_cangjie_runtime_1315_implement_r5893010973
root=/root/$lane
arm=$1
task_head=$2
mode=${3:-default}
cd "$root/$arm" || exit 2
export CCACHE_DIR=/root/.ccache GC_UNIT_GATE_SKIP=1 PATH=/usr/lib/ccache:$PATH
export CCACHE_BASEDIR="$root/$arm" CCACHE_NOHASHDIR=1
maps="-ffile-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fdebug-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fmacro-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime"
export CFLAGS="$maps" CXXFLAGS="$maps" ASMFLAGS="$maps"
ccache -M 50G >"$root/$arm/ccache.log" 2>&1
uptime > "$root/$arm/uptime-before.txt"
start=$SECONDS
testable=OFF
extras=()
if [ "$mode" = ohos ]; then
    testable=ON
    extras+=(-DMRT_GC_UNIT_OHOS_HOST=ON)
fi
cmake -S "$root/$arm/runtime" -B "$root/$arm/build" -DCJ_RUNTIME_COMMIT="$task_head" -DCMAKE_BUILD_TYPE=Release -DCOPYGC_FLAG=1 -DDOPRA_FLAG=1 -DRUNTIME_TRACE_FLAG=1 -DCJ_SDK_VERSION=0.0.1 -DDISABLE_VERSION_CHECK=1 -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_AR_PATH=ar -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_ASM_COMPILER_LAUNCHER=ccache -DMRT_TESTABLE_INTERNALS="$testable" -DCMAKE_INSTALL_PREFIX="$root/$arm/install" "${extras[@]}" > "$root/$arm/configure.log" 2>&1
crc=$?
echo "$crc" > "$root/$arm/configure.rc"
if [ "$crc" = 0 ]; then
    cmake --build "$root/$arm/build" -j"$(nproc)" > "$root/$arm/build.log" 2>&1
    brc=$?
else
    brc=$crc
fi
echo "$brc" > "$root/$arm/build.rc"
echo "wall=$((SECONDS-start)) jobs=$(nproc)" > "$root/$arm/wall.txt"
uptime > "$root/$arm/uptime-after.txt"
if [ "$brc" = 0 ]; then
    mkdir -p "$root/keep/$arm/lib"
    cp "$root/$arm/build/runtime-staging/lib/x86_64_Release/"{libcangjie-runtime.so,libboundscheck.so} "$root/keep/$arm/lib/"
    sha256sum "$root/keep/$arm/lib/"*.so > "$root/keep/$arm/so.sha256"
fi
printf '%s configure_rc=%s build_rc=%s wall=%ss jobs=%s\n' "$arm" "$crc" "$brc" "$((SECONDS-start))" "$(nproc)"
if [ "$brc" != 0 ]; then
    /usr/bin/grep -n -m 12 -E 'error:|undefined reference|fatal error|GC_UNIT_GATE_FAIL' "$root/$arm/"{configure,build}.log | cut -c1-250
fi
exit "$brc"
