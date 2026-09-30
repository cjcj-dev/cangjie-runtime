#!/usr/bin/env bash
set -u
ulimit -c 0
root=${1:?lane directory}
head=${2:?candidate sha}
export CCACHE_DIR=/root/.ccache CCACHE_NOHASHDIR=1 GC_UNIT_GATE_SKIP=1
ccache -M 50G
uptime > "$root/arms-uptime-before.txt"
build_arm() {
    arm=$1
    out="$root/cuts/$arm"
    mkdir -p "$out/keep/lib"
    start=$SECONDS
    cp -a "$root/testable/runtime" "$out/runtime"
    if [ "$arm" != restored ]; then
        (cd "$out" && patch -p1 < "$root/patches/$arm.diff") > "$out/patch.log" 2>&1 || return 2
    fi
    export CCACHE_BASEDIR="$out"
    maps="-ffile-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fdebug-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fmacro-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime"
    export CFLAGS="$maps" CXXFLAGS="$maps" ASMFLAGS="$maps"
    cmake -S "$out/runtime" -B "$out/build" -DCJ_RUNTIME_COMMIT="$head" -DCMAKE_BUILD_TYPE=Release -DCOPYGC_FLAG=1 -DDOPRA_FLAG=1 -DRUNTIME_TRACE_FLAG=1 -DCJ_SDK_VERSION=0.0.1 -DDISABLE_VERSION_CHECK=1 -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_AR_PATH=ar -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_ASM_COMPILER_LAUNCHER=ccache -DMRT_TESTABLE_INTERNALS=ON -DCMAKE_INSTALL_PREFIX="$out/install" > "$out/configure.log" 2>&1
    rc=$?
    echo "$rc" > "$out/configure.rc"
    if [ "$rc" = 0 ]; then
        cmake --build "$out/build" -j$(nproc) > "$out/build.log" 2>&1
        rc=$?
    fi
    echo "$rc" > "$out/build.rc"
    if [ "$rc" != 0 ]; then
        /usr/bin/grep -m 5 'error:' "$out/build.log"
        return "$rc"
    fi
    cp "$out/build/runtime-staging/lib/x86_64_Release/"*.so "$out/keep/lib/"
    cp "$out/build/runtime-staging/runtime-build-inputs.txt" "$out/keep/"
    sha256sum "$out/keep/lib/"*.so "$root/focused/dedup-unit" > "$out/artifacts.sha256"
    names='OldNoDeadControl'
    case "$arm" in
        state_*) names="$names CleanupReportStateTransitions" ;;
        shrink_*) names="$names ShrinkingOldBucketKeepsCanonicalIdentity" ;;
        restored) names="$names CleanupReportStateTransitions ShrinkingOldBucketKeepsCanonicalIdentity" ;;
    esac
    for name in $names; do
        (env LD_LIBRARY_PATH="$out/keep/lib" timeout 100 "$root/focused/dedup-unit" --gtest_filter="StringDedup.$name" > "$out/$name.log" 2>&1; echo $? > "$out/$name.rc") &
    done
    wait
    echo "$((SECONDS-start))" > "$out/wall.txt"
    rm -rf "$out/build" "$out/runtime"
    echo "$arm build_rc=$rc wall=$(cat "$out/wall.txt")"
    for name in $names; do
        echo "$arm $name rc=$(cat "$out/$name.rc")"
        /usr/bin/grep -E 'DEDUP_.*TARGET|EXPECT failed:|MAINTENANCE_RESULT' "$out/$name.log" | head -5
    done
}
for arm in state_end state_report shrink_find shrink_transfer restored; do build_arm "$arm" & done
wait
uptime > "$root/arms-uptime-after.txt"
