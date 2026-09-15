#!/bin/bash
set -u
ulimit -c 0
lane=/root/sym_cangjie_runtime_608_implement_r5676392826
for arm in green cut restored; do
 out="$lane/nested-arms/$arm"; mkdir -p "$out"
 lib="${lane}-nested-tests/testable/build/runtime-staging/lib/x86_64_Release"
 if [[ "$arm" == cut ]]; then lib="${lane}-cut-nested/testable/build/runtime-staging/lib/x86_64_Release"; fi
 for family in main publication; do
  elf="${lane}-nested-tests/units/testable/cj_gc_unit"
  tests='ThreadRootCurrent.C1StackFieldHistoricalColor ThreadRootCurrent.C4HeaderlessHistoricalColor ThreadRootCurrent.C2ObjectRefHistoricalColor'
  if [[ "$family" == publication ]]; then
   elf="${lane}-nested-tests/units/testable/cj_gc_forwarding_publication_unit"
   tests='RawRemapYoungProduct.MajorRemapsStackObjectField RawRemapYoungProduct.MajorRemapsHeaderlessRecordField RawRemapYoungProduct.MajorWatermarkConsumesYoungTable'
  fi
  sha256sum "$elf" "$lib/libcangjie-runtime.so" "$lib/libboundscheck.so" > "$out/$family-artifacts.sha256"
  for t in $tests; do
   LD_LIBRARY_PATH="$lib" "$elf" --gtest_filter="$t" > "$out/$t.log" 2>&1
   echo $? > "$out/$t.rc"
  done
 done
done
