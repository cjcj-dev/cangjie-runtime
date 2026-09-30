#!/usr/bin/env bash
ulimit -c 0
root=/root/sym_cangjie_runtime_1319_implement_r5899659797
cp "$root/unit-testable/cj_gc_unit" "$root/keep/elf/"
# Same ELF and boundscheck in every arm. Each case is an independent process.
for arm in green head clear ordinary restored; do
 (
  out="$root/cases-$arm"; mkdir -p "$out"
  export LD_LIBRARY_PATH="$root/keep/$arm"
  "$root/keep/elf/cj_gc_unit" --gtest_list_tests > "$out/list.txt" 2>&1
  if [ "$arm" != head ]; then
   /usr/bin/grep '^SegmentedArrayInit\.' "$root/unit-testable/test-lists/main.txt" > "$out/tests.txt"
  else
   : > "$out/tests.txt"
  fi
  if [ "$arm" = head ] || [ "$arm" = green ] || [ "$arm" = restored ]; then
   printf '%s\n' ThreadRootCurrent.SavedColorInvisibleRoot ThreadRootCurrent.TwoEpochInvisibleRoot ThreadRootCurrent.SavedColorNativeFrameRoot ThreadRootCurrent.TwoEpochNativeFrameRoot >> "$out/tests.txt"
  fi
  while IFS= read -r test; do
   (
    MRT_LOG_LEVEL=e timeout 60s "$root/keep/elf/cj_gc_unit" --gtest_filter="$test" > "$out/$test.log" 2>&1
    echo $? > "$out/$test.rc"
   ) &
  done < "$out/tests.txt"
  wait
  sha256sum "$root/keep/elf/cj_gc_unit" "$root/keep/$arm/"*.so > "$out/identity.sha256"
 ) &
done
wait
for arm in green head clear ordinary restored; do
 echo "CASES $arm"
 for file in "$root/cases-$arm/"*.rc; do echo "$(basename "$file" .rc) $(cat "$file")"; done
done
