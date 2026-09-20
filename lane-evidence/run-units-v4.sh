#!/bin/bash
ulimit -c 0
root=/root/sym_cangjie_runtime_720_implement_r5746103729-v4
for arm in default filler testable; do
 (
  config=$arm; testable=0; filler=1
  [ "$arm" = filler ] && config=default && filler=0
  [ "$arm" = testable ] && testable=1
  lib=$root/$config/build/runtime-staging/lib/x86_64_Release
  out=$root/unit-$arm
  mkdir -p "$out"
  uptime > "$out/uptime-before.txt"
  taskset -pc $$ > "$out/affinity.txt"
  sha256sum "$lib"/*.so > "$out/so.sha256"
  start=$SECONDS
  env GC_UNIT_JOBS=192 CANGJIE_BUILD_JOBS=192 CJRT_HEAP_FILLER=$filler MRT_TESTABLE_INTERNALS=$testable GC_UNIT_OUT="$out" GCV2_RUNTIME_LIB_DIR="$lib" GCV2_RUNTIME_OUTPUT_ROOT="$lib/../.." bash "$root/$config/runtime/tests/gc_unit/run_standalone.sh" > "$out/run.log" 2>&1
  echo $? > "$out/run.rc"
  echo $((SECONDS-start)) > "$out/wall.txt"
  uptime > "$out/uptime-after.txt"
  sha256sum "$out"/cj_gc*unit > "$out/elf.sha256" 2>/dev/null
 ) &
done
wait
