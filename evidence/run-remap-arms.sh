#!/bin/bash
set -u
ulimit -c 0
lane=/root/sym_cangjie_runtime_608_implement_r5676392826
for arm in green cut restored; do
 out="$lane/remap-arms/$arm"; mkdir -p "$out"
 lib="${lane}-checkpoint/testable/build/runtime-staging/lib/x86_64_Release"
 if [[ "$arm" == cut ]]; then lib="${lane}-cut-remap/testable/build/runtime-staging/lib/x86_64_Release"; fi
 elf="${lane}-checkpoint/units/testable/cj_gc_forwarding_publication_unit"
 sha256sum "$elf" "$lib/libcangjie-runtime.so" "$lib/libboundscheck.so" > "$out/artifacts.sha256"
 for t in RawRemapYoungProduct.MajorWatermarkConsumesYoungTable RawRemapYoungProduct.MajorWatermarkRemapsDerivedPromotedSource RawRemapYoungProduct.MajorKeepsOldPendingThenRelocatesRawRoot; do
  LD_LIBRARY_PATH="$lib" "$elf" --gtest_filter="$t" > "$out/$t.log" 2>&1
  echo $? > "$out/$t.rc"
 done
done
