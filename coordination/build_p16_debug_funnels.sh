#!/usr/bin/env bash
set -eu
ulimit -c 0
root=/root/sym_cangjie_runtime_627_implement_r5744767112-cont-debug-qualified
mkdir -p "$root/runtime" "$root/arms"
tar -C /root/sym_cangjie_runtime_627_implement_r5744767112-cont-weak-restored/default/runtime --exclude=tests/gc_unit/build_standalone --exclude=output -cf - . | tar -C "$root/runtime" -xf -
export CCACHE_DIR=/root/.ccache CCACHE_BASEDIR="$root" CCACHE_NOHASHDIR=1 CCACHE_DISABLE=1
export CFLAGS="-ffile-prefix-map=$root=/usr/src/cangjie-runtime -fdebug-prefix-map=$root=/usr/src/cangjie-runtime -fmacro-prefix-map=$root=/usr/src/cangjie-runtime"
export CXXFLAGS="$CFLAGS" ASMFLAGS="$CFLAGS"
uptime > "$root/uptime-before.txt"
cmake -S "$root/runtime" -B "$root/build" -DCMAKE_BUILD_TYPE=Debug -DCJ_RUNTIME_COMMIT=f03433bb2c95ed505d0f2140289df7e77f887090 -DCOPYGC_FLAG=1 -DDOPRA_FLAG=1 -DRUNTIME_TRACE_FLAG=1 -DCJ_SDK_VERSION=0.0.1 -DDISABLE_VERSION_CHECK=1 -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_AR_PATH=ar -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_ASM_COMPILER_LAUNCHER=ccache > "$root/configure.log" 2>&1
echo $? > "$root/configure.rc"
cp "$root/runtime/src/Heap/z/zIterator.inline.hpp" "$root/iterator.original"
cp "$root/runtime/src/Mutator/Mutator.cpp" "$root/mutator.original"
for arm in green iterator-cut uncolored-cut restored; do
  cp "$root/iterator.original" "$root/runtime/src/Heap/z/zIterator.inline.hpp"
  cp "$root/mutator.original" "$root/runtime/src/Mutator/Mutator.cpp"
  python3 - "$root" "$arm" <<'PY'
from pathlib import Path
import difflib,sys
r=Path(sys.argv[1]);arm=sys.argv[2]
spec={
 'iterator-cut':('Heap/z/zIterator.inline.hpp','    return referenceArray && is_invisible_object(object);','    (void)referenceArray; return false;'),
 'uncolored-cut':('Mutator/Mutator.cpp','        ZUncoloredRoot::mark(slot, ZPointerLoadGoodMask);','        (void)slot;'),
}
if arm in spec:
 path,old,new=spec[arm];p=r/'runtime/src'/path;a=p.read_text();assert a.count(old)==1
 b=a.replace(old,new);p.write_text(b)
 (r/(arm+'.diff')).write_text(''.join(difflib.unified_diff(a.splitlines(True),b.splitlines(True),fromfile='a/runtime/src/'+path,tofile='b/runtime/src/'+path)))
PY
  start=$SECONDS
  GC_UNIT_GATE_SKIP=1 cmake --build "$root/build" -j$(nproc) > "$root/$arm-build.log" 2>&1
  echo $? > "$root/$arm-build.rc"
  echo wall=$((SECONDS-start)) > "$root/$arm-wall.txt"
  cp -a "$root/build/runtime-staging/lib/x86_64_Debug" "$root/arms/$arm"
  sha256sum "$root/arms/$arm/"*.so > "$root/$arm-so.sha256"
  echo "$arm built wall=$((SECONDS-start))"
done
cmp "$root/arms/green/libcangjie-runtime.so" "$root/arms/restored/libcangjie-runtime.so"
echo $? > "$root/restored-identity.rc"
uptime > "$root/uptime-after.txt"
