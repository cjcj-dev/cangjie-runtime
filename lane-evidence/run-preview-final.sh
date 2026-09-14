#!/bin/bash
ulimit -c 0
set -u
root=/root/sym_cangjie_runtime_571_implement_r5668109509-preview-final
D=/root/sym_cangjie_runtime_560b_sym_cangjie_runtime_564_implement_r5664061027
RT=/root/sym_cangjie_runtime_560_implement_r5662982700/clean-build/testable/build/runtime-staging/lib/x86_64_Release
SDK=$D/gate-sdk/runtime/lib/linux_x86_64_cjnative
mkdir -p "$root"
uptime > "$root/uptime-before.txt"
for input in finalizer_trigger segmented_array_managed phase_entry_trigger; do
 name=$input; heap=256MB; extra=()
 if [ "$input" = segmented_array_managed ]; then heap=64MB; extra=(MRT_GC_UNIT_MANAGED_SEGMENTED=full cjGCInterval=3600s); fi
 if [ "$input" = phase_entry_trigger ]; then name=phase_entry_major; heap=1GB; extra=(MRT_GC_LOG=1 cjGCInterval=3600s); fi
 bin=$D/managed/$input-1/$name
 for n in 1 2 3; do
  (out=$root/$input-$n; mkdir -p "$out"; start=$SECONDS
   sha256sum "$bin" "$RT/libcangjie-runtime.so" "$RT/libboundscheck.so" > "$out/identity.sha256"
   printf 'cores=0-15 heap=%s\n' "$heap" > "$out/environment.txt"
   taskset -c 0-15 env LD_LIBRARY_PATH="$RT:$SDK" MRT_LOG_LEVEL=e cjHeapSize="$heap" "${extra[@]}" timeout 180s gdb -batch -x "$root/native-root-preview.py" --args "$bin" > "$out/gdb.out" 2> "$out/gdb.err"
   echo $? > "$out/rc"; echo $((SECONDS-start)) > "$out/wall") &
 done
done
wait
uptime > "$root/uptime-after.txt"
for dir in "$root"/*-?; do
 echo "${dir##*/} rc=$(cat "$dir/rc") wall=$(cat "$dir/wall")"
 /usr/bin/grep -E 'PREVIEW_COUNT|PREVIEW_UNIQUE' "$dir/gdb.out"
done
