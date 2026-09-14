#!/bin/bash
ulimit -c 0
set -u
root=/root/sym_cangjie_runtime_571_implement_r5668109509-preview
D=/root/sym_cangjie_runtime_560b_sym_cangjie_runtime_564_implement_r5664061027
RT=/root/sym_cangjie_runtime_560_implement_r5662982700/clean-build/testable/build/runtime-staging/lib/x86_64_Release
SDK=$D/gate-sdk/runtime/lib/linux_x86_64_cjnative
mkdir -p "$root"
uptime > "$root/uptime-before.txt"
for input in finalizer_trigger segmented_array_managed phase_entry_trigger; do
  name=$input; [ "$input" = phase_entry_trigger ] && name=phase_entry_major
  bin=$D/managed/$input-1/$name
  (out=$root/$input; mkdir -p "$out"; start=$SECONDS
   sha256sum "$bin" "$RT/libcangjie-runtime.so" "$RT/libboundscheck.so" > "$out/identity.sha256"
   env LD_LIBRARY_PATH="$RT:$SDK" MRT_LOG_LEVEL=e cjHeapSize=64MB cjGCInterval=3600s MRT_GC_UNIT_MANAGED_SEGMENTED=full timeout 180s gdb -batch -x "$root/native-root-preview.py" --args "$bin" > "$out/gdb.out" 2> "$out/gdb.err"
   echo $? > "$out/rc"; echo $((SECONDS-start)) > "$out/wall" ) &
done
wait
uptime > "$root/uptime-after.txt"
for input in finalizer_trigger segmented_array_managed phase_entry_trigger; do
 echo "$input rc=$(cat "$root/$input/rc") wall=$(cat "$root/$input/wall")"
 /usr/bin/grep -E 'PREVIEW_COUNT|PREVIEW_UNIQUE|PREVIEW_STOP' "$root/$input/gdb.out"
done
