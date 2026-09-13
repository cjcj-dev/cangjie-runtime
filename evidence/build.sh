#!/bin/bash
ulimit -c 0
set -u
export GC_UNIT_GATE_SKIP=1
export CCACHE_DIR=/root/.ccache
export CCACHE_BASEDIR=/root/sym_cangjie_runtime_465_implement_r5651041991/source
export CCACHE_NOHASHDIR=1
export CFLAGS="${CFLAGS:-} -ffile-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fdebug-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fmacro-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime"
export CXXFLAGS="${CXXFLAGS:-} -ffile-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fdebug-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fmacro-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime"
export ASMFLAGS="${ASMFLAGS:-} -ffile-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fdebug-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fmacro-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime"
ccache -M 50G
cd /root/sym_cangjie_runtime_465_implement_r5651041991 || exit 1
run_root=$PWD
uptime > uptime-before.txt
nproc > jobs.txt
build_arm() {
    config=$1
    unit=OFF
    if [ "$config" = testable ]; then unit=ON; fi
    started=$SECONDS
    (cd "$run_root/source/runtime" && cmake -S . -B "$run_root/build-$config" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=/usr/bin/clang -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_ASM_COMPILER_LAUNCHER=ccache \
        -DCOPYGC_FLAG=1 -DDOPRA_FLAG=1 -DOHOS_FLAG=0 -DANDROID_FLAG=0 -DIOS_FLAG=0 \
        -DIOS_SIMULATOR_FLAG=0 -DEULER_FLAG=0 -DMACOS_FLAG=0 -DRUNTIME_TRACE_FLAG=1 \
        -DASAN_FLAG=0 -DHWASAN_FLAG=0 -DSANITIZER_SUPPORT=None -DCOV=0 -DDUMPADDRESS_FLAG=0 \
        -DCJ_SDK_VERSION=0.0.1 -DMRT_GC_UNIT_TESTS="$unit" \
        -DCMAKE_INSTALL_PREFIX="$run_root/install-$config" > "$run_root/configure-$config.log" 2>&1)
    config_rc=$?
    echo "CONFIGURE_RC=$config_rc wall=$((SECONDS-started))" > "$config.rc"
    if [ "$config_rc" -eq 0 ]; then
        started=$SECONDS
        cmake --build "build-$config" --target cangjie-runtime -j$(nproc) > "build-$config.log" 2>&1
        rc=$?
        echo "BUILD_RC=$rc wall=$((SECONDS-started))" >> "$config.rc"
        if [ "$rc" -eq 0 ]; then
            libdir="$run_root/build-$config/runtime-staging/lib/x86_64_Release"
            sha256sum "$libdir/libcangjie-runtime.so" "$libdir/libboundscheck.so" > "sha256-$config.txt"
            strings "$libdir/libcangjie-runtime.so" | /usr/bin/grep -E 'CJRT-COMMIT:|CJRT-TREE:' > "provenance-$config.txt"
            date -u +%FT%TZ > "linked-$config.txt"
            nm --defined-only "$libdir/libcangjie-runtime.so" > "nm-$config.txt"
        fi
    else
        echo "BUILD_RC=NOT_STARTED(configure=$config_rc)" >> "$config.rc"
    fi
}
build_arm default &
pid_default=$!
build_arm testable &
pid_testable=$!
wait "$pid_default"
wait "$pid_testable"
uptime > uptime-after.txt
date -u +%FT%TZ > build.done
