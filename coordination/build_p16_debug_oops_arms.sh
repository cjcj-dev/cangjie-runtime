#!/usr/bin/env bash
set -u
ulimit -c 0
source_root=${P16_DEBUG_SOURCE_ROOT:-/root/sym_cangjie_runtime_627_implement_r5744767112-verify-green/default/runtime}
prefix=${P16_DEBUG_PREFIX:-/root/sym_cangjie_runtime_627_implement_r5744767112-debug-oops}
for arm in green cut restored; do
  root=$prefix-$arm
  mkdir -p "$root/runtime"
  tar -C "$source_root" --exclude=tests/gc_unit/build_standalone --exclude=output -cf - . | tar -C "$root/runtime" -xf -
done
P16_DEBUG_CUT_ROOT=$prefix-cut python3 - <<'PY'
from pathlib import Path
import difflib, os
root=Path(os.environ['P16_DEBUG_CUT_ROOT']); p=root/'runtime/src/Heap/z/zAddress.inline.hpp'
a=p.read_text();line='    if (ZVerifyOops && a != zaddress::null) { VerifyAccessedOop(a); }'
assert a.count(line)==1
b=a.replace(line,'    (void)a;',1);p.write_text(b)
(root/'cut.diff').write_text(''.join(difflib.unified_diff(a.splitlines(True),b.splitlines(True),fromfile='a/runtime/src/Heap/z/zAddress.inline.hpp',tofile='b/runtime/src/Heap/z/zAddress.inline.hpp')))
PY
for arm in green cut restored; do (
  root=$prefix-$arm
  export CCACHE_DIR=/root/.ccache CCACHE_BASEDIR="$root" CCACHE_NOHASHDIR=1
  export CFLAGS="-ffile-prefix-map=$root=/usr/src/cangjie-runtime -fdebug-prefix-map=$root=/usr/src/cangjie-runtime -fmacro-prefix-map=$root=/usr/src/cangjie-runtime"
  export CXXFLAGS="$CFLAGS" ASMFLAGS="$CFLAGS"
  uptime > "$root/uptime-before.txt"
  start=$SECONDS
  cmake -S "$root/runtime" -B "$root/build" -DCMAKE_BUILD_TYPE=Debug -DCJ_RUNTIME_COMMIT=${P16_DEBUG_COMMIT:-ce8ad111bba9a7534c4b0c64dc8dd218f23e4a05} -DCOPYGC_FLAG=1 -DDOPRA_FLAG=1 -DRUNTIME_TRACE_FLAG=1 -DCJ_SDK_VERSION=0.0.1 -DDISABLE_VERSION_CHECK=1 -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_AR_PATH=ar -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_ASM_COMPILER_LAUNCHER=ccache > "$root/configure.log" 2>&1
  rc=$?; echo $rc > "$root/configure.rc"
  if [ "$rc" = 0 ]; then GC_UNIT_GATE_SKIP=1 cmake --build "$root/build" -j$(nproc) > "$root/build.log" 2>&1; rc=$?; fi
  echo "$rc" > "$root/build.rc"
  echo wall=$((SECONDS-start)) > "$root/wall.txt"
  sha256sum "$root/build/runtime-staging/lib/x86_64_Debug/"*.so > "$root/so.sha256" 2>/dev/null
  uptime > "$root/uptime-after.txt"
  echo "$arm build_rc=$rc"
  /usr/bin/grep 'error:' "$root/build.log" | head -5
) & done
wait
