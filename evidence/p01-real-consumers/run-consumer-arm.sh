#!/bin/bash
set -u
ulimit -c 0
arm=$1
R=/root/sym_cangjie_runtime_608_implement_r5683164869
A=$R/abi-final-$arm
O=$R/consumer-final-$arm
SO=$R-finalrt/default/build/runtime-staging/lib/x86_64_Release
STD=$A/std-install/runtime/lib/linux_x86_64_cjnative
mkdir -p "$O"
[ "$(cat "$A/evidence/build.rc")" = 0 ] || exit 80
start=$SECONDS
export LLC=$A/llvm-build/bin/llc GCV2_RUNTIME_LIB_DIR=$SO SLOT_DOMAIN_STD_LIB_DIR=$STD GC_UNIT_OUT=$O
# Every arm uses the same runtime source path and link recipe. std is selected
# at load time, so its arm directory does not alter the test ELF's RPATH.
taskset -c 0-7 bash "$R/default/runtime/tests/gc_unit/run_slot_domain_llc.sh" > "$O/run-1.log" 2>&1
echo "$?" > "$O/run-1.rc"
for i in 2 3 hole heap_dyn heap_null global; do
 (
  filter=all
  case "$i" in hole|heap_dyn|heap_null|global) filter=$i;; esac
  uptime > "$O/run-$i.before"
  env -u LD_PRELOAD SLOT_DOMAIN_MAPS="$O/run-$i.maps" LD_LIBRARY_PATH="$SO:$STD" \
    taskset -c 0-7 timeout 30s "$O/slot_domain_driver" "$filter" > "$O/run-$i.log" 2>&1
  echo "$?" > "$O/run-$i.rc"
  uptime > "$O/run-$i.after"
 ) &
done
wait
sha256sum "$A/target/bin/cjcj-stage1" "$A/llvm-build/bin/llc" "$SO/libcangjie-runtime.so" "$SO/libboundscheck.so" "$STD/libcangjie-std-core.so" "$O/slot_domain_driver" > "$O/joint.sha256"
cp "$A/evidence/std.sha256" "$O/std.sha256"
echo "wall=$((SECONDS-start))" > "$O/wall.txt"
for i in 1 2 3 hole heap_dyn heap_null global; do
 printf 'CONSUMER arm=%s sample=%s rc=%s ' "$arm" "$i" "$(cat "$O/run-$i.rc")"
 /usr/bin/grep '^SLOT_DOMAIN_RESULT' "$O/run-$i.log"
done
