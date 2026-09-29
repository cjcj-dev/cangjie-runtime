#!/bin/bash
# Phase 2: cut arms + suite runs, head 6955edada9a8babe8b7eb8dcf8557654542385d7.
# SO-only cuts (page, phase) reuse the green test ELF; header cuts (consumer,
# count, allocation) rebuild the test ELF from the cut header (protocol 2.5(b)).
ulimit -c 0
r=/root/sym_cangjie_runtime_1331_implement_r5895529148
cd "$r" || exit 2
STAMP=6955edada9a8babe8b7eb8dcf8557654542385d7
uptime > final-uptime-before2.txt

build_cut() { # $1 arm, $2 tree, $3 patch-cmd (applied to $2) or "none"
  rm -rf "$1" "$2"
  mkdir -p "$2" && tar -xzf source.tar.gz -C "$2"
  if [ "$3" != none ]; then
    eval "$3" "$2"
    diff -ru cutref/runtime/src "$2/runtime/src" > "$r/$1-product-cut.diff" 2>&1 || true
  fi
  ( cd "$r/$2/runtime" || exit 2
    export GC_UNIT_GATE_SKIP=1
    export CCACHE_DIR=/root/.ccache CCACHE_BASEDIR="$r/$2" PATH=/usr/lib/ccache:$PATH
    m="-ffile-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fdebug-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime -fmacro-prefix-map=$CCACHE_BASEDIR=/usr/src/cangjie-runtime"
    export CFLAGS="$m" CXXFLAGS="$m" ASMFLAGS="$m"
    cmake -S "$r/$2/runtime" -B "$r/$1/build" -DCJ_RUNTIME_COMMIT="$STAMP" \
      -DCMAKE_BUILD_TYPE=Release -DCOPYGC_FLAG=1 -DDOPRA_FLAG=1 -DRUNTIME_TRACE_FLAG=1 \
      -DCJ_SDK_VERSION=0.0.1 -DDISABLE_VERSION_CHECK=1 -DCMAKE_C_COMPILER=clang \
      -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_AR_PATH=ar \
      -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
      -DCMAKE_ASM_COMPILER_LAUNCHER=ccache -DMRT_TESTABLE_INTERNALS=OFF \
      -DCMAKE_INSTALL_PREFIX="$r/$1/install" > "$r/$1-configure.log" 2>&1
    echo $? > "$r/$1-configure.rc"
    if [ "$(cat $r/$1-configure.rc)" = 0 ]; then
      cmake --build "$r/$1/build" -j"${KKK2_JOBS:-48}" > "$r/$1-build.log" 2>&1
      echo "$?" > "$r/$1-build.rc"
    else echo NOT_RUN > "$r/$1-build.rc"; fi
    find "$r/$1/build" -name '*.so' -type f -exec sha256sum {} + > "$r/$1-so.sha256" 2>/dev/null
  )
  echo "== build $1 configure=$(cat $1-configure.rc) build=$(cat $1-build.rc)"
}

# ---- builds, in parallel
( build_cut cut-page      tree-page      'bash patches/patch-page.sh' ) &
( build_cut cut-consumer  tree-consumer  'bash patches/patch-consumer.sh' ) &
( build_cut cut-phase     tree-phase     'bash patches/patch-phase.sh' ) &
( build_cut cut-count     tree-count     'python3 patches/patch-count.py' ) &
( build_cut cut-allocation tree-allocation 'python3 patches/patch-allocation.py' ) &
wait

run_suite() { # $1 arm, $2 lib dir, $3 tree used for run_standalone.sh
  out=$r/arm-$1; rm -rf "$out"; mkdir -p "$out"
  start=$SECONDS
  uptime > "$out/uptime-before.txt"
  ( cd "$3" && env GC_UNIT_OUT="$out" GCV2_RUNTIME_LIB_DIR="$2" \
      GCV2_RUNTIME_OUTPUT_ROOT="$2/../.." \
      bash "$3/runtime/tests/gc_unit/run_standalone.sh" ) > "$out/run.log" 2>&1
  rc=$?; echo "$rc" > "$out/run.rc"
  echo "$((SECONDS-start))" > "$out/wall.txt"
  uptime > "$out/uptime-after.txt"
  sha256sum "$out"/cj_gc_unit "$2"/libcangjie-runtime.so > "$out/hashes.txt" 2>/dev/null
  echo "== $1 rc=$rc wall=$(cat $out/wall.txt)s $(/usr/bin/grep -oE 'GC_UNIT_PARALLEL jobs=[0-9]+ tests=[0-9]+ wall=[0-9.]+' "$out/run.log" | tail -1) $(/usr/bin/grep -oE 'GC_UNIT_INCOMPLETE tests=[0-9]+' "$out/run.log" | tail -1)"
  /usr/bin/grep -E '^\[  FAILED  \]' "$out/run.log" | sort -u | head -10
  return 0
}

GL=$r/default/build/runtime-staging/lib/x86_64_Release
( run_suite green       "$GL" "$r/default" ) &
( run_suite filler     "$GL" "$r/default" ) &
( run_suite testable "$r/testable/build/runtime-staging/lib/x86_64_Release" "$r/testable" ) &
( run_suite cut-page      "$r/cut-page/build/runtime-staging/lib/x86_64_Release"      "$r/default" ) &
( run_suite cut-consumer  "$r/cut-consumer/build/runtime-staging/lib/x86_64_Release"  "$r/tree-consumer" ) &
( run_suite cut-phase     "$r/cut-phase/build/runtime-staging/lib/x86_64_Release"     "$r/default" ) &
( run_suite cut-count     "$r/cut-count/build/runtime-staging/lib/x86_64_Release"     "$r/tree-count" ) &
( run_suite cut-allocation "$r/cut-allocation/build/runtime-staging/lib/x86_64_Release" "$r/tree-allocation" ) &
wait
uptime > final-uptime-after2.txt
echo "== PHASE2 DONE"
