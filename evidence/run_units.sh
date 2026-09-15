#!/bin/bash
ulimit -c 0
root=/root/sym_cangjie_runtime_596_implement_r5673746875-base
cd "$root" || exit 2
mkdir -p evidence
uptime > evidence/units-before.txt
for arm in default testable; do
(
  start=$SECONDS
  export GCV2_RUNTIME_LIB_DIR="$root/$arm/build/runtime-staging/lib/x86_64_Release"
  export GCV2_RUNTIME_OUTPUT_ROOT="$root/$arm/build/runtime-staging"
  export GC_UNIT_OUT="$root/$arm/unit" GC_UNIT_JOBS=192
  export MRT_TESTABLE_INTERNALS=0
  [ "$arm" = testable ] && export MRT_TESTABLE_INTERNALS=1
  bash "$root/$arm/runtime/tests/gc_unit/run_standalone.sh" > "evidence/unit-$arm.log" 2>&1
  echo $? > "evidence/unit-$arm.rc"
  echo "wall=$((SECONDS-start))" > "evidence/unit-$arm.wall"
) &
done
wait
uptime > evidence/units-after.txt
