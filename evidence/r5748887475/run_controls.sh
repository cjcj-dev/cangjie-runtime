#!/bin/bash
ulimit -c 0
set -u
arm=$1
lib=$2
elf=$3
out=$4
cores=$5
mkdir -p "$out/so"
cp "$lib"/libcangjie-runtime.so "$lib"/libboundscheck.so "$lib"/libcangjie-trace.so "$out/so/"
uptime > "$out/uptime-before.txt"
date -Iseconds > "$out/run-time.txt"
echo "$cores" > "$out/cores.txt"
sha256sum "$elf" "$out/so"/*.so > "$out/before.sha256"
strings "$out/so/libcangjie-runtime.so" | /usr/bin/grep 'CJRT-DECLARED:' > "$out/lineage.txt"
start=$SECONDS
for name in NativeAnnotationHandle.Parameter NativeAnnotationHandle.Method NativeAnnotationHandle.InstanceField NativeAnnotationHandle.StaticField NativeAnnotationHandle.Type NativeHandle.SlotsStayStableAcrossGrowth ZJNICritical.RawHolderExcludesCollectionRelocation; do
 ( taskset -c "$cores" env LD_LIBRARY_PATH="$out/so" GC_UNIT_FILTER="$name" timeout 35 "$elf" > "$out/$name.log" 2>&1; echo $? > "$out/$name.rc" ) &
done
wait
echo $((SECONDS-start)) > "$out/wall.txt"
sha256sum "$elf" "$out/so"/*.so > "$out/after.sha256"
uptime > "$out/uptime-after.txt"
for f in "$out"/*.rc; do echo "$arm ${f##*/} $(cat "$f")"; done
/usr/bin/grep -h -E 'ANNOTATION_HANDLE_TARGET|HANDLE_SLOT_TARGET|JNI_RELOCATION_TARGET|EXPECT failed' "$out"/*.log
