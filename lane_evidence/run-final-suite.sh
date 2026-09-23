#!/bin/bash
ulimit -c 0
set -u
cd /root/sym917_r2_green || exit 2
uptime > unit-uptime-before.txt
for config in default testable; do
 (
 export GCV2_RUNTIME_LIB_DIR=$PWD/$config/build/runtime-staging/lib/x86_64_Release GC_UNIT_OUT=$PWD/unit-final-$config
 export MRT_TESTABLE_INTERNALS=0
 if [ "$config" = testable ]; then export MRT_TESTABLE_INTERNALS=1; fi
 bash "$config/runtime/tests/gc_unit/run_standalone.sh" > "unit-final-$config.log" 2>&1
 echo "$?" > "unit-final-$config.rc"
 ) &
done
wait
export CJRT_HEAP_FILLER=0
bash default/runtime/tests/gc_unit/run_parallel_tests.sh "$PWD/unit-final-default/cj_gc_unit" "$PWD/unit-final-default/cj_gc_forwarding_publication_unit" "$PWD/unit-final-filler" "$PWD/default/build/runtime-staging/lib/x86_64_Release" > unit-final-filler.log 2>&1
echo "$?" > unit-final-filler.rc
uptime > unit-uptime-after.txt
