#!/usr/bin/env bash
set -eu
ulimit -c 0
prefix=${P16_DEBUG_PREFIX:-/root/sym_cangjie_runtime_627_implement_r5744767112-debug-oops}
root=$prefix-green
out=$prefix-retained
lib=$root/build/runtime-staging/lib/x86_64_Debug
mkdir "$out"
cp -a "$lib" "$out/green"
header=$root/runtime/src/Heap/z/zAddress.inline.hpp
cp "$header" "$out/zAddress.original.hpp"
python3 - "$header" <<'PY'
from pathlib import Path
import sys
p=Path(sys.argv[1]);s=p.read_text();line='    if (ZVerifyOops && a != zaddress::null) { VerifyAccessedOop(a); }';assert s.count(line)==1;p.write_text(s.replace(line,'    (void)a;',1))
PY
uptime > "$out/uptime-before.txt"
for arm in cut restored; do
  if [[ "$arm" = restored ]]; then cp "$out/zAddress.original.hpp" "$header"; fi
  start=$SECONDS
  GC_UNIT_GATE_SKIP=1 cmake --build "$root/build" -j$(nproc) > "$out/$arm-build.log" 2>&1
  echo $? > "$out/$arm-build.rc"
  echo wall=$((SECONDS-start)) > "$out/$arm-wall.txt"
  cp -a "$lib" "$out/$arm"
  sha256sum "$out/$arm/"*.so > "$out/$arm-so.sha256"
done
sha256sum "$out/green/"*.so > "$out/green-so.sha256"
cmp "$out/green/libcangjie-runtime.so" "$out/restored/libcangjie-runtime.so"
echo restored_identity_rc=$?
uptime > "$out/uptime-after.txt"
