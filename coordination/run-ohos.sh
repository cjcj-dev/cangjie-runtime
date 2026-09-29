#!/usr/bin/env bash
set -u
ulimit -c 0
r=/root/sym_cangjie_runtime_1305_implement_r5892797534
root="$r/ohos"
mkdir -p "$root"
if [[ ! -d "$root/runtime" ]]; then cp -a "$r/arms/candidate/runtime" "$root/runtime"; fi
if [[ -f "$root/configure.log" && ! -f "$root/configure-first.log" ]]; then cp "$root/configure.log" "$root/configure-first.log"; fi
mkdir -p "$root/shim"
cp "$(clang -print-file-name=libc.so.6)" "$root/shim/libc.so"
export LD_LIBRARY_PATH="$root/shim${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
sha256sum "$root/shim/libc.so" > "$root/shim.sha256"
export CCACHE_DIR=/root/.ccache CCACHE_NOHASHDIR=1 CCACHE_BASEDIR="$root" GC_UNIT_GATE_SKIP=1
export PATH=/usr/lib/ccache:$PATH
maps="-ffile-prefix-map=$root=/usr/src/cangjie-runtime -fdebug-prefix-map=$root=/usr/src/cangjie-runtime -fmacro-prefix-map=$root=/usr/src/cangjie-runtime"
export CFLAGS="$maps" CXXFLAGS="$maps" ASMFLAGS="$maps"
uptime > "$root/uptime-before.txt"
start=$SECONDS
cmake -S "$root/runtime" -B "$root/build" -DCJ_RUNTIME_COMMIT=ffe0aba27261a647a57fcfce38f5bcab95b20853 -DCMAKE_BUILD_TYPE=Release -DCOPYGC_FLAG=1 -DDOPRA_FLAG=1 -DRUNTIME_TRACE_FLAG=1 -DCJ_SDK_VERSION=0.0.1 -DDISABLE_VERSION_CHECK=1 -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_AR_PATH=ar -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_ASM_COMPILER_LAUNCHER=ccache -DMRT_TESTABLE_INTERNALS=ON -DMRT_GC_UNIT_OHOS_HOST=ON -DCMAKE_INSTALL_PREFIX="$root/install" > "$root/configure.log" 2>&1
rc=$?; echo "$rc" > "$root/configure.rc"
if [[ $rc == 0 ]]; then cmake --build "$root/build" -j"$(nproc)" > "$root/build.log" 2>&1; rc=$?; fi
echo "$rc" > "$root/build.rc"
if [[ $rc == 0 ]]; then
 export GC_UNIT_OUT="$root/unit" GCV2_RUNTIME_LIB_DIR="$root/build/runtime-staging/lib/x86_64_Release" GCV2_RUNTIME_OUTPUT_ROOT="$root/build/runtime-staging"
 mkdir -p "$root/unit" "$r/keep/ohos"
 cp -a "$GCV2_RUNTIME_LIB_DIR/"*.so "$r/keep/ohos/"
 sha256sum "$r/keep/ohos/"*.so > "$root/so.sha256"
 bash "$root/runtime/tests/gc_unit/run_standalone.sh" > "$root/run.log" 2>&1; rc=$?
fi
echo "$rc" > "$root/run.rc"
echo "$((SECONDS-start))" > "$root/wall.txt"
uptime > "$root/uptime-after.txt"
echo "UNIT_OHOS_RC=$rc build_rc=$(cat "$root/build.rc") wall=$(cat "$root/wall.txt")"
/usr/bin/grep -E 'GC_UNIT_OHOS_HOST_(FILTER|OK|FAIL)|error:|RESULT=' "$root/run.log" "$root/build.log" | head -12
