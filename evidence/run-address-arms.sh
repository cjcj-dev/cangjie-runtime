#!/bin/bash
set -u
ulimit -c 0
lane=/root/sym_cangjie_runtime_608_implement_r5676392826
out="$lane/address-arms"
mkdir -p "$out"
uptime > "$out/before.txt"
for mechanism in finalizable uncolor; do
 for arm in green cut restored; do
  dir="$out/$mechanism/$arm"; mkdir -p "$dir"
  lib="${lane}-restored/default/build/runtime-staging/lib/x86_64_Release"
  elf="${lane}-migrated/units/default/cj_gc_unit"
  if [[ "$arm" == cut ]]; then
   lib="${lane}-cut-$mechanism/default/build/runtime-staging/lib/x86_64_Release"
   if [[ "$mechanism" == uncolor ]]; then elf="${lane}-cut-uncolor/units/default/cj_gc_unit"; fi
  fi
  sha256sum "$elf" "$lib/libcangjie-runtime.so" "$lib/libboundscheck.so" > "$dir/artifacts.sha256"
  nm --defined-only "$lib/libcangjie-runtime.so" | c++filt | grep -E 'ZGlobalsPointers::flip_old_mark_start|HeapSlot.*GetAddress|ZPointer::uncolor' > "$dir/product-symbols.txt"
  for t in ZAddress.FinalizableFlip ZAddress.UncolorRoundTrip ZAddress.AddressValidity ZAddress.IsChecks; do
   LD_LIBRARY_PATH="$lib" "$elf" --gtest_filter="$t" > "$dir/$t.log" 2>&1
   echo "$?" > "$dir/$t.rc"
  done
 done
done
uptime > "$out/after.txt"
