#!/bin/bash
ulimit -c 0
set -u
prefix=/root/sym_cangjie_runtime_571_implement_r5668109509-post
elf=$prefix-final/unit-testable/cj_gc_unit
out=$prefix-evidence/gdb
lib=$prefix-final/testable/build/runtime-staging/lib/x86_64_Release
mkdir -p "$out"
for name in MinorPublication MajorSeed; do
 for n in 1 2 3; do
  (start=$SECONDS
   taskset -c 0-15 env LD_LIBRARY_PATH="$lib" timeout 90s gdb -batch -x "$prefix-final/marker-capture.py" --args "$elf" --gtest_filter=NativeRootCurrent.$name > "$out/$name-$n.log" 2>&1
   echo $? > "$out/$name-$n.rc"; echo $((SECONDS-start)) > "$out/$name-$n.wall") &
 done
done
wait
for file in "$out"/*.log; do
 echo "${file##*/} rc=$(cat "${file%.log}.rc")"
 /usr/bin/grep -E 'NATIVE_ROOT_ORACLE|NATIVE_READ_RESULT|MARKER_ARGUMENT|CAPTURE_ERROR|native_root_marker_current' "$file"
done
