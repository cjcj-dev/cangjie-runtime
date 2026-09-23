#!/bin/bash
ulimit -c 0
set -u
# Arguments: candidate lane, arm lane, result label. All paths are kkk2-local.
candidate=$1
arm=$2
label=$3
config=${4:-default}
root=/root/$candidate
cd "$root" || exit 2
elf=$root/unit-final-$config/cj_gc_unit
lib=/root/$arm/$config/build/runtime-staging/lib/x86_64_Release
out=$root/causal-$label
mkdir -p "$out"
if [ ! -x "$elf" ] || [ ! -f "$lib/libcangjie-runtime.so" ]; then echo "missing causal input"; exit 2; fi
uptime > "$out/uptime-before.txt"
sha256sum "$elf" "$lib/libcangjie-runtime.so" "$lib/libboundscheck.so" > "$out/identity.sha256"
nm --defined-only "$lib/libcangjie-runtime.so" | c++filt > "$out/product-symbols.txt"
nm --defined-only "$elf" | c++filt > "$out/test-symbols.txt"
export LD_LIBRARY_PATH="$lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
start=$SECONDS
for test in ObjectAllocator917.RetireYoungRequiresSafepoint ObjectAllocator917.RetireOldRequiresSafepoint ObjectAllocator917.YoungPhaseRequiresSafepoint ObjectAllocator917.OldPhaseRequiresSafepoint ObjectAllocator917.FastAvailableRequiresMutator ObjectAllocator917.TLABEntryRequiresMutator ObjectAllocatorPaths.TLABsShareSmallPage SharedSmallPage.AgeRefillAndRetirement ZValue.shared_small_page_is_per_cpu_storage; do
 (LD_DEBUG=libs "$elf" --gtest_filter="$test" > "$out/$test.log" 2>&1; echo "$?" > "$out/$test.rc") &
done
wait
echo "$((SECONDS-start))" > "$out/wall.txt"
uptime > "$out/uptime-after.txt"
for file in "$out"/*.rc; do echo "$(basename "$file")=$(cat "$file")"; done
