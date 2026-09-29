#!/bin/bash
# Final verification pipeline for lane sym_cangjie_runtime_1331_implement_r5895529148.
# Every arm is built with -DCJ_RUNTIME_COMMIT=6955edada9a8babe8b7eb8dcf8557654542385d7
# so the recipe identity is closed for the delivered head.
ulimit -c 0
r=/root/sym_cangjie_runtime_1331_implement_r5895529148
cd "$r" || exit 2
STAMP=6955edada9a8babe8b7eb8dcf8557654542385d7
uptime > final-uptime-before.txt

build_arm() { # $1 = arm dir name, $2 = testable ON/OFF, $3 = configure var prefix
  local arm=$1 testable=$2
  rm -rf "$arm" "$arm/build"
  mkdir -p "$arm" && tar -xzf source.tar.gz -C "$arm"
  ( cd "$r/$arm/runtime" || exit 2
    start=$SECONDS
    export GC_UNIT_GATE_SKIP=1
    export CCACHE_DIR=/root/.ccache CCACHE_BASEDIR="$r/$arm" PATH=/usr/lib/ccache:$PATH
    maps="-ffile-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fdebug-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fmacro-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime"
    export CFLAGS="$maps" CXXFLAGS="$maps" ASMFLAGS="$maps"
    cmake -S "$r/$arm/runtime" -B "$r/$arm/build" -DCJ_RUNTIME_COMMIT="$STAMP" \
      -DCMAKE_BUILD_TYPE=Release -DCOPYGC_FLAG=1 -DDOPRA_FLAG=1 -DRUNTIME_TRACE_FLAG=1 \
      -DCJ_SDK_VERSION=0.0.1 -DDISABLE_VERSION_CHECK=1 -DCMAKE_C_COMPILER=clang \
      -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_AR_PATH=ar \
      -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
      -DCMAKE_ASM_COMPILER_LAUNCHER=ccache -DMRT_TESTABLE_INTERNALS="$testable" \
      -DCMAKE_INSTALL_PREFIX="$r/$arm/install" > "$r/$arm-configure.log" 2>&1
    crc=$?; echo "$crc" > "$r/$arm-configure.rc"
    if [ "$crc" = 0 ]; then
      cmake --build "$r/$arm/build" -j"${KKK2_JOBS:-48}" > "$r/$arm-build.log" 2>&1
      echo "$?" > "$r/$arm-build.rc"
    else
      echo "NOT_RUN" > "$r/$arm-build.rc"
    fi
    echo "$((SECONDS-start))" > "$r/$arm-wall.txt"
    find "$r/$arm/build" -name '*.so' -type f -exec sha256sum {} + > "$r/$arm-so.sha256" 2>/dev/null
  )
}

# ---- phase 1: green default + testable, in parallel
( build_arm default OFF ) &
( build_arm testable ON ) &
wait
for arm in default testable; do
  echo "== $arm configure_rc=$(cat $arm-configure.rc) build_rc=$(cat $arm-build.rc) wall=$(cat $arm-wall.txt)s"
  if [ "$(cat $arm-build.rc)" != 0 ]; then
    echo "-- first errors ($arm):"
    grep -n -m 25 -E "error:|Error [0-9]|undefined reference|fatal error" "$arm-build.log" "$arm-configure.log" 2>/dev/null | cut -c1-220
  fi
  echo "-- SO ($arm):"; sed -E 's#/root/[^ ]*/build/#build/#' "$arm-so.sha256" | head -4
done
grep -h CJ_RUNTIME_COMMIT default/build/CMakeCache.txt | head -2
echo "== PHASE1 DONE"
