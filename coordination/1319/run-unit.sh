#!/usr/bin/env bash
ulimit -c 0
root=/root/sym_cangjie_runtime_1319_implement_r5899659797
uptime > "$root/unit-uptime-before.txt"
for arm in default testable; do
 (
  start=$SECONDS
  export GCV2_RUNTIME_LIB_DIR="$root/$arm/build/runtime-staging/lib/x86_64_Release"
  export GCV2_RUNTIME_OUTPUT_ROOT="$root/$arm/build/runtime-staging"
  export GC_UNIT_OUT="$root/unit-$arm"
  bash "$root/$arm/runtime/tests/gc_unit/run_standalone.sh" > "$root/unit-$arm.log" 2>&1
  echo $? > "$root/unit-$arm.rc"
  echo "wall=$((SECONDS-start))" > "$root/unit-$arm.wall"
 ) &
done
wait
uptime > "$root/unit-uptime-after.txt"
for arm in default testable; do
 echo "$arm rc=$(cat "$root/unit-$arm.rc") $(cat "$root/unit-$arm.wall")"
 /usr/bin/grep -m 15 -E 'error:|undefined reference|SUMMARY|GC_UNIT_COMPILE_PARALLEL|failed=|GC_UNIT_LINK_FAIL' "$root/unit-$arm.log" | cut -c1-250
done
