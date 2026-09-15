#!/bin/bash
ulimit -c 0
root=/root/sym_cangjie_runtime_596_implement_r5673746875-restored
out=/root/sym_cangjie_runtime_596_implement_r5673746875/units
mkdir -p "$out"
for arm in default testable ohos; do
(
  start=$SECONDS
  config=$arm; [ "$arm" = ohos ] && config=default
  export GCV2_RUNTIME_LIB_DIR="$root/$config/build/runtime-staging/lib/x86_64_Release"
  export GCV2_RUNTIME_OUTPUT_ROOT="$root/$config/build/runtime-staging"
  export GC_UNIT_OUT="$out/$arm" GC_UNIT_JOBS=192
  export MRT_TESTABLE_INTERNALS=0 MRT_GC_UNIT_OHOS_HOST=0
  [ "$arm" = testable ] && export MRT_TESTABLE_INTERNALS=1
  [ "$arm" = ohos ] && export MRT_GC_UNIT_OHOS_HOST=1
  uptime > "$out/$arm-before.txt"
  bash "$root/$config/runtime/tests/gc_unit/run_standalone.sh" > "$out/$arm.log" 2>&1
  echo $? > "$out/$arm.rc"
  echo "wall=$((SECONDS-start))" > "$out/$arm.wall"
  uptime > "$out/$arm-after.txt"
  if [ "$arm" = default ]; then
    export CJRT_HEAP_FILLER=0
    start=$SECONDS
    uptime > "$out/filler-before.txt"
    bash "$root/$config/runtime/tests/gc_unit/run_parallel_tests.sh" "$out/default/cj_gc_unit" "$out/default/cj_gc_forwarding_publication_unit" "$out/filler" "$GCV2_RUNTIME_LIB_DIR" > "$out/filler.log" 2>&1
    echo $? > "$out/filler.rc"
    echo "wall=$((SECONDS-start))" > "$out/filler.wall"
    uptime > "$out/filler-after.txt"
  fi
) &
done
wait
