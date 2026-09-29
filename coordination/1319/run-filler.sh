#!/usr/bin/env bash
ulimit -c 0
root=/root/sym_cangjie_runtime_1319_implement_r5899659797
start=$SECONDS
uptime > "$root/filler-uptime-before.txt"
CJRT_HEAP_FILLER=0 bash "$root/default/runtime/tests/gc_unit/run_parallel_tests.sh" "$root/unit-default/cj_gc_unit" "$root/unit-default/cj_gc_forwarding_publication_unit" "$root/unit-filler" "$root/default/build/runtime-staging/lib/x86_64_Release" > "$root/unit-filler.log" 2>&1
rc=$?; echo "$rc" > "$root/unit-filler.rc"
sha256sum "$root/unit-default/cj_gc_unit" "$root/unit-default/cj_gc_forwarding_publication_unit" "$root/default/build/runtime-staging/lib/x86_64_Release/"*.so > "$root/unit-filler/identity.sha256"
uptime > "$root/filler-uptime-after.txt"
echo "UNIT_FILLER_RC=$rc wall=$((SECONDS-start))"
cat "$root/unit-filler/parallel_tally.txt"
