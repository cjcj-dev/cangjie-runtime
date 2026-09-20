#!/bin/bash
set -u
ulimit -c 0
root=/root/sym_cangjie_runtime_730_implement_r5746502983
source_root=$root-validated
out=$root-medium-evidence
mkdir -p "$out"
uptime > "$out/uptime-before.txt"
taskset -pc $$ > "$out/launcher-affinity.txt"
run_arm() (
  arm=$1; config=$2
  arm_source=$root-$arm
  if [ "$arm" = green ] || [ "$arm" = restored ]; then arm_source=$root-medium-baseline; fi
  lib=$arm_source/$config/build/runtime-staging/lib/x86_64_Release
  elf=$source_root/unit-extended-$config/cj_gc_unit
  dest=$out/$arm-$config
  mkdir -p "$dest"
  for input in "$elf" "$lib/libcangjie-runtime.so" "$lib/libboundscheck.so"; do
    if [ ! -f "$input" ]; then echo "MISSING_INPUT=$input" > "$dest/error.txt"; exit 70; fi
  done
  key=$(sha256sum "$lib/libcangjie-runtime.so" | cut -d' ' -f1)
  depot=/root/sodepot/$key
  mkdir -p "$depot"
  cp "$lib/libcangjie-runtime.so" "$lib/libboundscheck.so" "$depot/"
  sha256sum "$elf" "$depot/libcangjie-runtime.so" "$depot/libboundscheck.so" > "$dest/identity.sha256"
  strings "$depot/libcangjie-runtime.so" | /usr/bin/grep 'CJRT-COMMIT:' > "$dest/lineage.txt"
  nm --defined-only "$depot/libcangjie-runtime.so" | c++filt > "$dest/product-defined.txt"
  env LD_LIBRARY_PATH="$depot" ldd "$elf" > "$dest/loaded-libraries.txt"
  tests=(ObjectAllocatorPaths.ManagedSizeRouting ObjectAllocatorPaths.ManagedFastMediumConsumesCachedPage ObjectAllocatorPaths.MediumNonBlockingAllocatesAfterCacheMiss ObjectAllocatorPaths.MediumBlockingFailureAttemptsCollection TLABUsage.BoundsAndDemand SegmentedArrayInit.SmallReferenceArrayKeepsFastPath)
  for test in "${tests[@]}"; do
    (
      # Same test's repetitions are sequential; independent tests and SO arms run concurrently.
      for sample in 1 2 3; do
        start=$SECONDS
        taskset -c 0-15 timeout 65s env MRT_LOG_LEVEL=e LD_LIBRARY_PATH="$depot" "$elf" --gtest_filter="$test" > "$dest/$test.$sample.log" 2>&1
        rc=$?
        printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$arm" "$config" "$test" "$sample" "$rc" "$((SECONDS-start))" > "$dest/$test.$sample.rc"
      done
    ) &
  done
  wait
)
for config in default testable; do
  (
    elf=$source_root/unit-extended-$config/cj_gc_unit
    nm --defined-only "$elf" | c++filt > "$out/test-$config-defined.txt"
    nm -u "$elf" | c++filt > "$out/test-$config-imports.txt"
    count=0
    for arm in green medium-nonblocking medium-blocking restored; do
      run_arm "$arm" "$config" &
      count=$((count+1))
      # Each fixture starts its own runtime worker pool; at most six SO arms
      # overlap across both configurations, with cases parallel inside each arm.
      if (( count % 3 == 0 )); then wait; fi
    done
    wait
  ) &
done
wait
cat "$out"/*/*.rc > "$out/results.tsv"
uptime > "$out/uptime-after.txt"
echo "RESULTS=$out/results.tsv"
