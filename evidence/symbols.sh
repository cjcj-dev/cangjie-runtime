#!/bin/bash
ulimit -c 0
for lane in sym_cangjie_runtime_919_implement_r5786193802-baseline sym_cangjie_runtime_919_implement_r5786193802; do
 cd "/root/$lane" || exit 2
 for arm in default testable; do
  so="$arm/build/runtime-staging/lib/x86_64_Release/libcangjie-runtime.so"
  nm --defined-only "$so" > "$arm-full-defined.txt"
  nrc=$?
  c++filt < "$arm-full-defined.txt" > "$arm-full-demangled.txt"
  sha256sum "$so"
  echo "lane=$lane arm=$arm nm_rc=$nrc"
  /usr/bin/grep -c -E 'handoffLock|y2yDirtyLock|y2yDirtyHolders|y2yDirtySlots' "$arm-full-demangled.txt"
  echo "removed_symbol_grep_rc=$?"
  /usr/bin/grep -c -F 'MapleRuntime::StoreBarrierBuffer::Flush()' "$arm-full-demangled.txt"
  echo "positive_control_grep_rc=$?"
 done
 cat uptime-before.txt uptime-after.txt
 done
