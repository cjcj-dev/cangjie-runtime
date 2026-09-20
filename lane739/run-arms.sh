#!/bin/bash
ulimit -c 0
set -u
LANE=sym_cangjie_runtime_739_implement_r5747765432
E=/root/$LANE-evidence
R=/root/$LANE-final
mkdir -p "$E/arms"
uptime > "$E/arms/uptime-before.txt"
run_case() {
  local arm=$1 test=$2 sample=$3 config=$4
  local src=/root/$LANE-cut-$arm/$config/build/runtime-staging/lib/x86_64_Release
  if [ "$arm" = green ] || [ "$arm" = restored ]; then src=$R/$config/build/runtime-staging/lib/x86_64_Release; fi
  local elf=$E/final-locked-testable/cj_gc_unit
  local cores=16-31
  if [ "$config" = default ]; then elf=$E/final-unit-default/cj_gc_unit; cores=0-15; fi
  local dest=$E/arms/$arm-$config
  mkdir -p "$dest"
  local sha depot start rc
  sha=$(sha256sum "$src/libcangjie-runtime.so" | cut -d' ' -f1)
  depot=/root/sodepot/$sha
  mkdir -p "$depot"
  cp -n "$src/libcangjie-runtime.so" "$src/libboundscheck.so" "$depot/"
  sha256sum "$elf" "$depot/libcangjie-runtime.so" "$depot/libboundscheck.so" > "$dest/identity.sha256"
  strings "$depot/libcangjie-runtime.so" | /usr/bin/grep 'CJRT-COMMIT:' > "$dest/lineage.txt"
  start=$SECONDS
  taskset -c "$cores" timeout 100s env MRT_LOG_LEVEL=e LD_LIBRARY_PATH="$depot" "$elf" --gtest_filter="$test" > "$dest/$test.$sample.log" 2>&1
  rc=$?
  printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$arm" "$config" "$test" "$sample" "$rc" "$((SECONDS-start))" > "$dest/$test.$sample.rc"
}
for arm in green consumer notify restart shutdown young reset restored; do
  (
    for test in AllocationStall.OneFreeTreeUnitClaimsOnlyOneOfTwoWaiters AllocationStall.ProductLateWaiterRequiresNextCollection AllocationStall.ProductShutdownAnswersPendingWaiters AllocationStall.ProductReturnedCapacityServesOnlyOneWaiter ObjectAllocatorPaths.MediumNonBlockingAllocatesAfterCacheMiss P05Heuristics.ZPageAllocationIsStackRequest; do
      (for sample in 1 2 3; do run_case "$arm" "$test" "$sample" testable; done) &
    done
    # Young lock restoration is observable at the existing YoungType invariant.
    if [ "$arm" = green ] || [ "$arm" = young ] || [ "$arm" = restored ]; then
      for config in default testable; do
        (for sample in 1 2 3; do run_case "$arm" ObjectAllocatorPaths.MediumBlockingFailureAttemptsCollection "$sample" "$config"; done) &
      done
    fi
    wait
  ) &
done
wait
cat "$E"/arms/*/*.rc > "$E/arms/results.tsv"
uptime > "$E/arms/uptime-after.txt"
