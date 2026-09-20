#!/bin/bash
set -u
ulimit -c 0
root=/root/sym_cangjie_runtime_730_implement_r5746042754
cand=/root/diff_56d880754889
out=$root-red-evidence
mkdir -p "$out"
uptime > "$out/uptime-before.txt"
run_arm() {
  arm=$1
  config=$2
  source_root=$root-$arm
  [ "$arm" != green ] || source_root=$cand
  so=$source_root/$config/build/runtime-staging/lib/x86_64_Release
  elf=$cand/unit-$config/cj_gc_unit
  key=$(sha256sum "$so/libcangjie-runtime.so" | cut -d' ' -f1)
  depot=/root/sodepot/$key
  mkdir -p "$depot" "$out/$arm-$config"
  cp "$so/libcangjie-runtime.so" "$so/libboundscheck.so" "$depot/"
  sha256sum "$elf" "$depot/libcangjie-runtime.so" "$depot/libboundscheck.so" > "$out/$arm-$config/identity.sha256"
  strings "$depot/libcangjie-runtime.so" | /usr/bin/grep 'CJRT-COMMIT:' > "$out/$arm-$config/lineage.txt"
  nm --defined-only "$depot/libcangjie-runtime.so" | c++filt > "$out/$arm-$config/product-defined.txt"
  nm --defined-only "$elf" | c++filt > "$out/$arm-$config/test-defined.txt"
  nm -u "$elf" | c++filt > "$out/$arm-$config/test-imports.txt"
  tests=(ObjectAllocatorPaths.ManagedSizeRouting ObjectAllocatorPaths.TLABsShareSmallPage ObjectAllocatorPaths.FastMediumConsumesCachedActualSize TLABUsage.BoundsAndDemand AllocationStall.WaiterBlocksInSaferegion SegmentedArrayInit.SmallReferenceArrayKeepsFastPath)
  if [ "$config" = testable ]; then
    tests+=(SegmentedArrayInit.EpochFlipRestartsAndRewritesPublishedBlock SegmentedArrayInit.TwoGcReferenceInitializationRestartsOnlyOnce SegmentedArrayInit.TwoGcPrimitiveInitializationDoesNotRestart)
  fi
  for test in "${tests[@]}"; do
    (
      start=$SECONDS
      taskset -c 0-7 timeout 60s env MRT_LOG_LEVEL=e LD_LIBRARY_PATH="$depot" "$elf" --gtest_filter="$test" > "$out/$arm-$config/$test.log" 2>&1
      rc=$?
      printf '%s\t%s\t%s\t%s\t%s\n' "$arm" "$config" "$test" "$rc" "$((SECONDS-start))" > "$out/$arm-$config/$test.rc"
    ) &
  done
  wait
}
for arm in green publish consume tlab fast restored; do run_arm "$arm" default & done
for arm in green segment restored; do run_arm "$arm" testable & done
wait
cat "$out"/*/*.rc > "$out/results.tsv"
uptime > "$out/uptime-after.txt"
awk -F '\t' '$4 != 0 {print}' "$out/results.tsv"
echo "RESULTS=$out/results.tsv"
