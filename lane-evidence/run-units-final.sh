#!/bin/bash
ulimit -c 0
root=/root/sym_cangjie_runtime_720_implement_r5746103729
run_arm() {
  local arm=$1 config=$2 filler=$3 testable=$4
  local lib=$root/$config/build/runtime-staging/lib/x86_64_Release
  local out=$root/unit-ed08-$arm
  mkdir -p "$out"
  uptime > "$out/uptime-before.txt"
  sha256sum "$lib"/*.so > "$out/so.sha256"
  local start=$SECONDS
  env GC_UNIT_JOBS=192 CANGJIE_BUILD_JOBS=192 CJRT_HEAP_FILLER=$filler MRT_TESTABLE_INTERNALS=$testable \
    GC_UNIT_OUT="$out" GCV2_RUNTIME_LIB_DIR="$lib" GCV2_RUNTIME_OUTPUT_ROOT="$lib/../.." \
    bash "$root/$config/runtime/tests/gc_unit/run_standalone.sh" > "$out/run.log" 2>&1
  echo $? > "$out/run.rc"
  echo $((SECONDS-start)) > "$out/wall.txt"
  uptime > "$out/uptime-after.txt"
  sha256sum "$out"/cj_gc*unit > "$out/elf.sha256" 2>/dev/null || true
}
run_arm default default 1 0 &
run_arm filler default 0 0 &
run_arm testable testable 1 1 &
wait
for a in default filler testable; do echo ARM=$a rc=$(cat $root/unit-$a/run.rc) wall=$(cat $root/unit-$a/wall.txt); done
