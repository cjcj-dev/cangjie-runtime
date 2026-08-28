#!/bin/bash
set -uo pipefail

REPO=${REPO:?need cut repo}
BASE=${BASE:?need v6 repo}
OUT=${OUT:?need evidence directory}
CORES=${CORES:-0-15}
BUILD="$REPO/runtime/CMakebuild"
P1="$REPO/runtime/output/temp/lib/x86_64_Release"
T0="$BASE/runtime/tests/gc_unit/build_standalone/cj_gc_unit"

mkdir -p "$OUT" "$P1"
uptime > "$OUT/uptime.before"
cd "$BUILD/src"
taskset -c "$CORES" /usr/bin/cmake -E cmake_link_script \
    CMakeFiles/cangjie-runtime.dir/link.txt --verbose=1 > "$OUT/link.log" 2>&1
printf '%s\n' "$?" > "$OUT/link.rc"
sha256sum "$T0" "$P1/libcangjie-runtime.so" "$P1/libboundscheck.so" > "$OUT/identity"
strings "$P1/libcangjie-runtime.so" | /usr/bin/grep -o 'CJRT-COMMIT:[0-9a-f]*' | head -1 > "$OUT/lineage"

for test_name in \
    YoungConc.LateEdgeFollowReceiptReachesYoungMarkConsumer \
    YoungConc.LateEdgeFollowReceiptReachesYoungRuntimeDispatch; do
    label=${test_name##*.}
    env LD_LIBRARY_PATH="$P1" taskset -c "$CORES" timeout 120 \
        "$T0" "--gtest_filter=$test_name" > "$OUT/$label.out" 2> "$OUT/$label.err"
    printf '%s\n' "$?" > "$OUT/$label.rc"
    env LD_LIBRARY_PATH="$P1" ldd "$T0" > "$OUT/$label.ldd"
done
uptime > "$OUT/uptime.after"
sha256sum "$OUT"/* > "$OUT/SHA256SUMS"
