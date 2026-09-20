#!/usr/bin/env bash
set -u
ulimit -c 0
root=/root/sym_cangjie_runtime_730_implement_r5746939673
out=$root-evidence
mkdir -p "$out"
uptime > "$out/uptime-before.txt"
run_one_arm() (
  arm=$1; config=$2
  source=$root-$arm
  lib=$source/$config/build/runtime-staging/lib/x86_64_Release
  elf=$root-green/unit-$config/cj_gc_unit
  dest=$out/$arm-$config
  mkdir -p "$dest"
  key=$(sha256sum "$lib/libcangjie-runtime.so" | cut -d' ' -f1)
  depot=/root/sodepot/$key
  mkdir -p "$depot"
  # Depots were filled before any arm started; never rewrite a loaded SO.
  cmp "$lib/libcangjie-runtime.so" "$depot/libcangjie-runtime.so" || exit 71
  cmp "$lib/libboundscheck.so" "$depot/libboundscheck.so" || exit 71
  sha256sum "$elf" "$depot/libcangjie-runtime.so" "$depot/libboundscheck.so" > "$dest/identity.sha256"
  strings "$depot/libcangjie-runtime.so" | grep 'CJRT-COMMIT:' > "$dest/lineage.txt"
  env LD_LIBRARY_PATH="$depot" ldd "$elf" > "$dest/loaded-libraries.txt"
  nm --defined-only "$depot/libcangjie-runtime.so" | c++filt > "$dest/product-defined.txt"
  uptime > "$dest/uptime-before.txt"
  taskset -pc $$ > "$dest/launcher-affinity.txt"
  tests=(SharedSmallPage.MigrationUsesCurrentCPU SharedSmallPage.SmallHeapUsesSharedSlotZero SharedSmallPage.AtomicBoundsPreserveTop)
  if [ "$config" = testable ]; then tests+=(AllocationStall.ProductLateWaiterRequiresNextCollection AllocationStall.CompletedWaveDoesNotFailLateWaiter); fi
  for test in "${tests[@]}"; do
    (
      for sample in 1 2 3; do
        start=$SECONDS
        taskset -c 16-31 timeout 65s env MRT_LOG_LEVEL=e LD_LIBRARY_PATH="$depot" "$elf" --gtest_filter="$test" > "$dest/$test.$sample.log" 2>&1
        rc=$?
        printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$arm" "$config" "$test" "$sample" "$rc" "$((SECONDS-start))" > "$dest/$test.$sample.rc"
      done
    ) &
  done
  # Identical fixture ELF in every arm. GDB injection is deterministic N=1.
  fixture=$root-green/pinned-$config/pinned_publication_window
  if [ "$config" = testable ]; then fixture=$root-green/pinned-testable-matched/pinned_publication_window; fi
  sha256sum "$fixture" >> "$dest/identity.sha256"
  (
    start=$SECONDS
    taskset -c 16-31 timeout 90s env MRT_LOG_LEVEL=e LD_LIBRARY_PATH="$depot" gdb -q -batch \
      -x "$root-green/$config/runtime/tests/gc_unit/pinned_publication_window.gdb" "$fixture" > "$dest/pinned.gdb.log" 2>&1
    rc=$?
    printf '%s\t%s\tPinnedPublication.GdbWindow\t1\t%s\t%s\n' "$arm" "$config" "$rc" "$((SECONDS-start))" > "$dest/pinned.gdb.rc"
  ) &
  wait
  uptime > "$dest/uptime-after.txt"
)
# Prepare shared immutable depots sequentially before starting parallel tests.
for config in default testable; do
  for arm in green restored cpu pinned late; do
    lib=$root-$arm/$config/build/runtime-staging/lib/x86_64_Release
    key=$(sha256sum "$lib/libcangjie-runtime.so" | cut -d' ' -f1)
    depot=/root/sodepot/$key
    mkdir -p "$depot"
    for name in libcangjie-runtime.so libboundscheck.so; do
      if [ ! -e "$depot/$name" ]; then cp "$lib/$name" "$depot/$name"; fi
      cmp "$lib/$name" "$depot/$name" || exit 71
    done
  done
done
for config in default testable; do
  for arm in green restored cpu pinned late; do
    run_one_arm "$arm" "$config" &
  done
done
wait
cat "$out"/*/*.rc > "$out/results.tsv"
uptime > "$out/uptime-after.txt"
