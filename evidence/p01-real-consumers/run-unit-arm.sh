#!/bin/bash
set -u
ulimit -c 0
arm=$1
R=/root/sym_cangjie_runtime_608_implement_r5683164869
cfg=default; flags=(); testable=0
if [ "$arm" = testable ]; then cfg=testable; testable=1; fi
if [ "$arm" = filler ]; then flags=(CJRT_HEAP_FILLER=0); fi
O=$R/unit-$arm
S=$R/$cfg/build/runtime-staging/lib/x86_64_Release
mkdir -p "$O"
uptime > "$O/uptime-before.txt"
sha256sum "$S/libcangjie-runtime.so" "$S/libboundscheck.so" > "$O/so.sha256"
start=$SECONDS
env "${flags[@]}" GC_UNIT_JOBS=192 MRT_TESTABLE_INTERNALS=$testable GC_UNIT_OUT="$O" GCV2_RUNTIME_LIB_DIR="$S" GCV2_RUNTIME_OUTPUT_ROOT="$S/../.." bash "$R/$cfg/runtime/tests/gc_unit/run_standalone.sh" > "$O/run.log" 2>&1
rc=$?
echo "$rc" > "$O/run.rc"
echo "wall=$((SECONDS-start))" > "$O/run.wall"
uptime > "$O/uptime-after.txt"
sha256sum "$O/cj_gc_unit" "$O/cj_gc_forwarding_publication_unit" > "$O/elf.sha256" 2>/dev/null
printf 'UNIT_%s_RC=%s\n' "$arm" "$rc"
/usr/bin/grep -E '^\[========\]|^\[  FAILED  \]|^GC_UNIT_RUN_DONE|^GATE_.*FAIL' "$O/run.log" | tail -12
