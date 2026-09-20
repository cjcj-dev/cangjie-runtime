#!/bin/bash
ulimit -c 0
lane=sym_cangjie_runtime_720_implement_r5746103729
elf=/root/$lane/unit-testable/cj_gc_unit
for arm in green producer consumer injection restored; do
 (
  src=$lane
  case $arm in producer|consumer|injection) src=$lane-$arm;; esac
  lib=/root/$src/testable/build/runtime-staging/lib/x86_64_Release
  out=/root/$lane/red-$arm
  mkdir -p "$out"
  uptime > "$out/uptime-before.txt"
  taskset -pc $$ > "$out/affinity.txt"
  sha256sum "$elf" "$lib/libcangjie-runtime.so" "$lib/libboundscheck.so" > "$out/identity.sha256"
  nm --defined-only "$lib/libcangjie-runtime.so" | c++filt | grep -E 'ZGenerationYoung::ZGenerationYoung|ZRemembered::register_found_old|Heap::alloc_page|ZRemsetTableIterator::next' > "$out/product-symbols.txt"
  for test in RememberedLifecycle720.ConstructedGenerationPublishesHighestGranule RememberedLifecycle720.HeapPublicationReachesConstructedRemembered ZGeneration.StaticYoungOldAndId; do
   ( LD_LIBRARY_PATH="$lib" timeout 60 "$elf" --gtest_filter="$test" > "$out/$test.log" 2>&1; echo $? > "$out/$test.rc" ) &
  done
  wait
  uptime > "$out/uptime-after.txt"
 ) &
done
wait
