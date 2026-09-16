#!/bin/bash
# Invoke inside wf_kkk2.sh bsh (shared build slot).
set -u
ulimit -c 0
root=$1; commit=$2
out=$root/ohos
mkdir -p "$out"
export CCACHE_DIR=/root/.ccache CCACHE_BASEDIR=$root/default CCACHE_NOHASHDIR=1 GC_UNIT_GATE_SKIP=1
maps="-ffile-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fdebug-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fmacro-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime"
export CFLAGS="$maps" CXXFLAGS="$maps" ASMFLAGS="$maps"
uptime > "$out/uptime-before.txt"
df -h /root > "$out/df-before.txt"
start=$SECONDS
cmake -S "$root/default/runtime" -B "$out/build" -DCJ_RUNTIME_COMMIT="$commit" \
    -DCMAKE_BUILD_TYPE=Release -DCOPYGC_FLAG=1 -DDOPRA_FLAG=1 -DRUNTIME_TRACE_FLAG=1 \
    -DCJ_SDK_VERSION=0.0.1 -DDISABLE_VERSION_CHECK=1 -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_AR_PATH=ar -DCMAKE_C_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_ASM_COMPILER_LAUNCHER=ccache \
    -DMRT_TESTABLE_INTERNALS=ON -DMRT_GC_UNIT_OHOS_HOST=ON > "$out/configure.log" 2>&1
rc=$?; echo "$rc" > "$out/configure.rc"
if [ "$rc" = 0 ]; then
    cmake --build "$out/build" -j"$(nproc)" > "$out/build.log" 2>&1
    rc=$?; echo "$rc" > "$out/build.rc"
fi
echo "$rc" > "$out/attempt.rc"
echo "wall=$((SECONDS-start))" > "$out/wall.txt"
uptime > "$out/uptime-after.txt"
echo "P07_OHOS_ATTEMPT_RC=$rc"
/usr/bin/grep -n -m 12 -E 'error:|CMake Error' "$out/configure.log" "$out/build.log" 2>/dev/null
exit "$rc"
