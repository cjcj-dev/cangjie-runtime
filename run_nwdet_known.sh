#!/bin/bash
set -uo pipefail

BIN=${BIN:?need BIN}
LIB=${LIB:?need LIB directory}
OUT=${OUT:?need OUT directory}
CORES=${CORES:-0-15}

mkdir -p "$OUT"
{
    echo "BIN=$BIN"
    echo "LIB=$LIB"
    echo "CORES=$CORES"
    sha256sum "$BIN" "$LIB/libcangjie-runtime.so" "$LIB/libboundscheck.so"
    strings "$LIB/libcangjie-runtime.so" | /usr/bin/grep -o 'CJRT-COMMIT:[0-9a-f]*' | head -1
    env LD_LIBRARY_PATH="$LIB" ldd "$BIN" | /usr/bin/grep -E 'libcangjie-runtime|libboundscheck' || true
    uptime
} > "$OUT/identity.before"

printf 'heap_mb\trc\tsignature\n' > "$OUT/results.tsv"
for heap in 320 352; do
    env -i PATH=/usr/bin:/bin LANG=C cjHeapSize="${heap}MB" LD_LIBRARY_PATH="$LIB" \
        taskset -c "$CORES" timeout 300 "$BIN" > "$OUT/${heap}.out" 2> "$OUT/${heap}.err"
    rc=$?
    signature=$(sed -n '/\[FINDTO\]\[fail-closed\]/p;/Runtime panic:/p;/SIGABRT/p' "$OUT/${heap}.err" | head -1)
    printf '%s\t%s\t%s\n' "$heap" "$rc" "${signature:-none}" >> "$OUT/results.tsv"
done

uptime > "$OUT/uptime.after"
sha256sum "$OUT"/*.out "$OUT"/*.err "$OUT/results.tsv" > "$OUT/SHA256SUMS"
