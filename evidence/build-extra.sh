#!/bin/bash
ulimit -c 0
root=/root/sym_cangjie_runtime_959_implement_r5790205235
cd "$root" || exit 2
export CCACHE_DIR=/root/.ccache CCACHE_BASEDIR="$root/default" CCACHE_NOHASHDIR=1 GC_UNIT_GATE_SKIP=1
maps="-ffile-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fdebug-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fmacro-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime"
export CFLAGS="$maps" CXXFLAGS="$maps" ASMFLAGS="$maps"
uptime > extra-uptime-before.txt
for arm in debug ohos; do
(
 start=$SECONDS
 kind=Release; host=ON
 if [ "$arm" = debug ]; then kind=Debug; host=OFF; fi
 cmake -S "$root/default/runtime" -B "$root/$arm-build" -DCJ_RUNTIME_COMMIT=c6728382c68634b067f6253242b05af5e6a67579 -DCMAKE_BUILD_TYPE="$kind" -DCOPYGC_FLAG=1 -DDOPRA_FLAG=1 -DRUNTIME_TRACE_FLAG=1 -DCJ_SDK_VERSION=0.0.1 -DDISABLE_VERSION_CHECK=1 -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_AR_PATH=ar -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_ASM_COMPILER_LAUNCHER=ccache -DMRT_TESTABLE_INTERNALS=ON -DMRT_GC_UNIT_OHOS_HOST="$host" -DCMAKE_INSTALL_PREFIX="$root/$arm-install" > "$arm-configure.log" 2>&1
 rc=$?; echo "$rc" > "$arm-configure.rc"
 if [ "$rc" = 0 ]; then cmake --build "$root/$arm-build" -j"$(nproc)" > "$arm-build.log" 2>&1; rc=$?; fi
 echo "$rc" > "$arm-build.rc"
 echo "$((SECONDS-start))" > "$arm-wall.txt"
 if [ "$rc" = 0 ]; then sha256sum "$root/$arm-build/runtime-staging/lib/x86_64_$kind/"*.so > "$arm-so.sha256"; fi
) &
done
wait
uptime > extra-uptime-after.txt
