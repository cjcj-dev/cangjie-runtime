#!/usr/bin/env bash
set -u
ulimit -c 0
r=/root/sym_cangjie_runtime_1305_implement_r5892797534
uptime > "$r/units-uptime-before.txt"
for arm in default testable; do
 (
  start=$SECONDS
  out="$r/unit-final-$arm"
  export GC_UNIT_OUT="$out" GCV2_RUNTIME_LIB_DIR="$r/$arm/build/runtime-staging/lib/x86_64_Release"
  export GCV2_RUNTIME_OUTPUT_ROOT="$r/$arm/build/runtime-staging"
  mkdir -p "$out"
  sha256sum "$GCV2_RUNTIME_LIB_DIR/"*.so > "$out/so.sha256"
  bash "$r/$arm/runtime/tests/gc_unit/run_standalone.sh" > "$out/run.log" 2>&1
  echo "$?" > "$out/run.rc"
  sha256sum "$out/cj_gc_unit" "$out/cj_gc_forwarding_publication_unit" > "$out/elf.sha256"
  echo "$((SECONDS-start))" > "$out/wall.txt"
  if [[ $arm == default && -x "$out/cj_gc_unit" ]]; then
   # Filler is the exact default ELF, not a second compile.
   filler="$r/unit-final-filler"
   mkdir -p "$filler"
   cp "$out/so.sha256" "$out/elf.sha256" "$filler/"
   start=$SECONDS
   CJRT_HEAP_FILLER=0 bash "$r/default/runtime/tests/gc_unit/run_parallel_tests.sh" "$out/cj_gc_unit" "$out/cj_gc_forwarding_publication_unit" "$filler" "$GCV2_RUNTIME_LIB_DIR" > "$filler/run.log" 2>&1
   echo "$?" > "$filler/run.rc"
   echo "$((SECONDS-start))" > "$filler/wall.txt"
  fi
 ) &
done
wait
uptime > "$r/units-uptime-after.txt"
for arm in default filler testable; do
 out="$r/unit-final-$arm"
 echo "UNIT=$arm rc=$(cat "$out/run.rc") wall=$(cat "$out/wall.txt")"
 /usr/bin/grep -E 'GC_UNIT_PARALLEL |GC_UNIT_RUN_DONE|^\[========\] [0-9]{2,} tests|\[  FAILED  \]|\[  INCOMPLETE \]|error:' "$out/run.log" | head -20
done
