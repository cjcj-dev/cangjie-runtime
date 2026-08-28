#!/bin/bash
set -uo pipefail

REPO=${REPO:?need cut repo}
BASE=${BASE:?need v6 repo}
OUT=${OUT:?need evidence directory}
CORES=${CORES:-0-15}
export CANGJIE_HOME=${CANGJIE_HOME:-/root/sdks/cjcj-pin-937877c8}

mkdir -p "$OUT"
uptime > "$OUT/uptime.before"
cd "$REPO/runtime"
python3 build.py clean > "$OUT/clean.log" 2>&1
printf '%s\n' "$?" > "$OUT/clean.rc"
taskset -c "$CORES" python3 build.py build -t release > "$OUT/build.log" 2>&1
printf '%s\n' "$?" > "$OUT/build.rc"

P1="$REPO/runtime/output/temp/lib/x86_64_Release"
T0="$BASE/runtime/tests/gc_unit/build_standalone/cj_gc_unit"
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
