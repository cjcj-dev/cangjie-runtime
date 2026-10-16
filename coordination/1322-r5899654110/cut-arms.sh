#!/usr/bin/env bash
set -u
ulimit -c 0
r=/root/sym_cangjie_runtime_1322_implement_r5899654110
cd "$r" || exit 2
product_head=${1:?pass source commit}
export CCACHE_DIR=/root/.ccache CCACHE_NOHASHDIR=1 GC_UNIT_GATE_SKIP=1
ccache -M 50G
uptime > keep/cuts-uptime-before.txt
for arm in producer-time producer-freed consumer-time consumer-freed phase-major; do
(
    start=$SECONDS
    mkdir -p "cut-$arm" "keep/$arm"
    tar -xzf source.tar.gz -C "cut-$arm"
    cd "cut-$arm" || exit 2
    patch -p1 < "$r/$arm.diff" > "$r/keep/$arm/patch.log" 2>&1 || exit 3
    export CCACHE_BASEDIR=$PWD
    maps="-ffile-prefix-map=$PWD=/usr/src/cangjie-runtime -fdebug-prefix-map=$PWD=/usr/src/cangjie-runtime -fmacro-prefix-map=$PWD=/usr/src/cangjie-runtime"
    export CFLAGS="$maps" CXXFLAGS="$maps" ASMFLAGS="$maps"
    cmake -S runtime -B build -DCJ_RUNTIME_COMMIT="$product_head" -DCMAKE_BUILD_TYPE=Release -DCOPYGC_FLAG=1 -DDOPRA_FLAG=1 -DRUNTIME_TRACE_FLAG=1 -DCJ_SDK_VERSION=0.0.1 -DDISABLE_VERSION_CHECK=1 -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_AR_PATH=ar -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_ASM_COMPILER_LAUNCHER=ccache -DMRT_TESTABLE_INTERNALS=OFF -DCMAKE_INSTALL_PREFIX="$PWD/install" > "$r/keep/$arm/configure.log" 2>&1
    rc=$?; echo "$rc" > "$r/keep/$arm/configure.rc"
    if [ "$rc" = 0 ]; then
        cmake --build build -j$(nproc) > "$r/keep/$arm/build.log" 2>&1
        rc=$?; echo "$rc" > "$r/keep/$arm/build.rc"
    fi
    if [ "$rc" = 0 ]; then
        lib=$PWD/build/runtime-staging/lib/x86_64_Release
        sha256sum "$lib/libcangjie-runtime.so" "$lib/libboundscheck.so" > "$r/keep/$arm/so.sha256"
        id=$(sha256sum "$lib/libcangjie-runtime.so" | cut -d' ' -f1)
        mkdir -p "/root/sodepot/$id"
        cp "$lib/libcangjie-runtime.so" "$lib/libboundscheck.so" "/root/sodepot/$id/"
        cp "$r/keep/native-candidate/gc_totals_native" "$r/keep/$arm/"
        GC_TOTALS_REUSE_ELF=1 GC_UNIT_OUT="$r/keep/$arm" GCV2_RUNTIME_LIB_DIR="/root/sodepot/$id" bash "$r/default/runtime/tests/gc_unit/run_gc_totals_native.sh" > "$r/keep/$arm/summary.log" 2>&1
        rc=$?
    fi
    echo "arm=$arm rc=$rc wall=$((SECONDS-start)) jobs=$(nproc)" > "$r/keep/$arm/result.txt"
) &
done
wait
uptime > keep/cuts-uptime-after.txt
for arm in producer-time producer-freed consumer-time consumer-freed phase-major; do
    cat "keep/$arm/result.txt"
    cat "keep/$arm/summary.log" 2>/dev/null || true
done
