#!/bin/bash
ulimit -c 0
r=/root/sym_cangjie_runtime_1312_implement_r5899117285
mkdir -p "$r/refined-targets"
export r
run_one() {
 name=$1
 env LD_LIBRARY_PATH="$r/testable/build/runtime-staging/lib/x86_64_Release" timeout 30 "$r/unit-refined/cj_gc_unit" --gtest_filter=StringDedup.$name > "$r/refined-targets/$name.log" 2>&1
 echo $? > "$r/refined-targets/$name.rc"
 /usr/bin/grep -E 'DEDUP_|FAIL|CHECK|Check failed' "$r/refined-targets/$name.log" | head -7
 echo "$name rc=$(cat "$r/refined-targets/$name.rc")"
}
export -f run_one
printf '%s\n' YoungTableRootKeepsIdentity YoungStrongRootControl YoungDifferentContentControl RelocationWaitAllowsWorkerAndStop | xargs -n1 -P$(nproc) bash -c 'run_one "$1"' _
