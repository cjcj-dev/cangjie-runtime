#!/usr/bin/env bash
# Managed std.runtime -> product serviceability regression (#1322).
set -euo pipefail
ulimit -c 0
root=$(cd "$(dirname "$0")/../../.." && pwd)
out=${GC_UNIT_OUT:?set GC_UNIT_OUT}
rt=${GCV2_RUNTIME_LIB_DIR:?set GCV2_RUNTIME_LIB_DIR}
sdk=${CANGJIE_HOME:?set CANGJIE_HOME to a qualified coloured SDK}
host=${GC_UNIT_CJC_RUNTIME_LIB_DIR:?set compiler host runtime}
cjc=${CJC:-$sdk/bin/cjc}
mkdir -p "$out"
uptime > "$out/uptime-before.txt"
start=$SECONDS
if [[ ${GC_TOTALS_REUSE_ELF:-0} != 1 ]]; then
    set +e
    LD_LIBRARY_PATH="$host:$sdk/tools/lib:$sdk/third_party/llvm/lib" \
        "$cjc" "$root/runtime/tests/gc_unit/gc_totals.cj" -O0 --static-std \
        -L"$rt" -o "$out/gc_totals" > "$out/build.log" 2>&1
    rc=$?
    set -e
    echo "$rc" > "$out/build.rc"
    [[ $rc == 0 ]] || exit "$rc"
fi
sha256sum "$out/gc_totals" "$rt/libcangjie-runtime.so" "$rt/libboundscheck.so" > "$out/artifacts.sha256"
LD_LIBRARY_PATH="$rt:$sdk/runtime/lib/linux_x86_64_cjnative" ldd "$out/gc_totals" > "$out/ldd.txt"
set +e
LD_LIBRARY_PATH="$rt:$sdk/runtime/lib/linux_x86_64_cjnative" \
    cjHeapSize=256MB cjGCInterval=3600s timeout 60s "$out/gc_totals" > "$out/run.log" 2>&1
rc=$?
set -e
echo "$rc" > "$out/run.rc"
printf 'wall=%s\n' "$((SECONDS-start))" > "$out/wall.txt"
uptime > "$out/uptime-after.txt"
cat "$out/run.log"
exit "$rc"
