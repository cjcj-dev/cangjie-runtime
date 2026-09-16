#!/bin/bash
set -u
ulimit -c 0
root=$1
config=$2
cores=$3
mode=$4
source_root=${5:-$root}
tag=${6:-unit-$config}
out=$root/$tag
lib=$root/$config/build/runtime-staging/lib/x86_64_Release
mkdir -p "$out"
export GC_UNIT_JOBS=192 GC_UNIT_BUILD_JOBS=192 GC_UNIT_OUT="$out"
export GCV2_RUNTIME_LIB_DIR="$lib" GCV2_RUNTIME_OUTPUT_ROOT="$root/$config/build/runtime-staging"
export MRT_TESTABLE_INTERNALS=0
[ "$config" = testable ] && export MRT_TESTABLE_INTERNALS=1
case "$tag" in *filler*) export CJRT_HEAP_FILLER=0;; esac
df -h /root > "$out/df-before.txt"
uptime > "$out/uptime-before.txt"
printf '%s\n' "$cores" > "$out/cores.txt"
start=$SECONDS
if [ "$mode" = build ]; then
  taskset -c "$cores" bash "$root/$config/runtime/tests/gc_unit/run_standalone.sh" > "$out/run.log" 2>&1
  rc=$?
  main=$out/cj_gc_unit
  pub=$out/cj_gc_forwarding_publication_unit
else
  main=$source_root/unit-$config/cj_gc_unit
  pub=$source_root/unit-$config/cj_gc_forwarding_publication_unit
  taskset -c "$cores" bash "$root/$config/runtime/tests/gc_unit/run_parallel_tests.sh" "$main" "$pub" "$out" "$lib" > "$out/run.log" 2>&1
  rc=$?
fi
echo "$rc" > "$out/run.rc"
echo "$((SECONDS-start))" > "$out/wall.txt"
uptime > "$out/uptime-after.txt"
sha256sum "$main" "$pub" > "$out/elf.sha256" 2>/dev/null
sha256sum "$lib/libcangjie-runtime.so" "$lib/libboundscheck.so" > "$out/so.sha256"
if [ -x "$main" ] && [[ "$tag" != sample-* ]]; then
  nm --defined-only "$lib/libcangjie-runtime.so" > "$out/product-defined.txt"
  nm --defined-only "$main" > "$out/test-defined.txt"
  for test in ZVirtualMemoryManagerTest.test_insert_merges_neighbours ZVirtualMemoryManagerTest.test_remove_from_low ZPhysicalMemoryManager.BackingIndicesSurviveVirtualShuffle MappedCache.SmallPagesUseLowestAddress MappedCache.ProductHarvestRemapsToLowestFreeVirtual Uncommitter.UncommitIdleUnitsReleasesPhysical Uncommitter.TestUncommitIndependentPartitionThread ZVirtualMemoryManagerTest.InitialSegmentsPreserveNineReservations ZVirtualMemoryManagerTest.CompilerTablePublishesEveryRange ZVirtualMemoryManagerTest.CompilerTableRejectsOversizedPublication; do
    env LD_LIBRARY_PATH="$lib" taskset -c "$cores" timeout 30s "$main" --gtest_filter="$test" > "$out/filter-$test.log" 2>&1
    echo "$?" > "$out/filter-$test.rc"
  done
fi
printf 'UNIT arm=%s config=%s rc=%s wall=%s\n' "$root" "$config" "$rc" "$(cat "$out/wall.txt")"
grep -E '^\[==========\]|^\[  PASSED|^\[  FAILED|GC_UNIT_PARALLEL|error:' "$out/run.log" | tail -14
# Retain all linked programs/logs/receipts; these objects are reproducible.
if [ -d "$out/objects" ]; then rm -rf "$out/objects"; fi
exit "$rc"
