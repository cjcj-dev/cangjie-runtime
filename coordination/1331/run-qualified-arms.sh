#!/bin/bash
ulimit -c 0
r=/root/sym_cangjie_runtime_1331_implement_r5895529148
for arm in green restored phase consumer padding; do
 (
  case "$arm" in
    green|restored) lib=$r/keep/candidate-staging/lib/x86_64_Release ;;
    phase) lib=$r/keep/final-phase-lib ;;
    consumer) lib=$r/keep/consumer-lib ;;
    padding) lib=$r/keep/final-padding-lib ;;
  esac
  out=$r/qualified-$arm; mkdir -p "$out"
  uptime > "$out/uptime-before.txt"
  sha256sum "$r/unit-default/cj_gc_unit" "$r/unit-default/cj_gc_forwarding_publication_unit" "$lib/"*.so > "$out/hashes.txt"
  start=$SECONDS
  bash "$r/keep/green-runtime/tests/gc_unit/run_parallel_tests.sh" "$r/unit-default/cj_gc_unit" "$r/unit-default/cj_gc_forwarding_publication_unit" "$out" "$lib" > "$out/run.log" 2>&1
  rc=$?; echo "$rc" > "$out/run.rc"
  uptime > "$out/uptime-after.txt"
  echo "$arm rc=$rc wall=$((SECONDS-start))"
  /usr/bin/grep -E 'FAILED|INCOMPLETE|GC_UNIT_PARALLEL' "$out/run.log" | tail -12
 ) &
done
wait
