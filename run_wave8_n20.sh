#!/bin/bash
set -uo pipefail

BIN=${BIN:?need BIN}
LIB=${LIB:?need LIB directory}
ANALYZER=${ANALYZER:?need ANALYZER}
OUT=${OUT:?need OUT directory}
CORES=${CORES:-0-15}
N=${N:-20}

mkdir -p "$OUT/runs"
field()
{
    awk -v key="$1" '{for (i = 1; i <= NF; ++i) {split($i, part, "="); if (part[1] == key) {print part[2]; exit}}}'
}

{
    echo "BIN=$BIN"
    echo "LIB=$LIB"
    echo "ANALYZER=$ANALYZER"
    echo "CORES=$CORES"
    echo "N=$N"
    sha256sum "$BIN" "$LIB/libcangjie-runtime.so" "$LIB/libboundscheck.so" "$ANALYZER"
    strings "$LIB/libcangjie-runtime.so" | /usr/bin/grep -o 'CJRT-COMMIT:[0-9a-f]*' | head -1
    env LD_LIBRARY_PATH="$LIB" ldd "$BIN" | /usr/bin/grep -E 'libcangjie-runtime|libboundscheck' || true
    uptime
} > "$OUT/identity.before"

printf 'i\trc\tanalyzer_rc\tfirst_div\twave8\tfinal\tdelta_wave8\tdelta_final\tload_before\tload_after\n' \
    > "$OUT/runs.tsv"

for i in $(seq 1 "$N"); do
    prefix="$OUT/runs/$i"
    cut -d' ' -f1 /proc/loadavg > "$prefix.load.before"
    env -i PATH=/usr/bin:/bin LANG=C cjHeapSize=256MB LD_LIBRARY_PATH="$LIB" \
        taskset -c "$CORES" timeout 300 "$BIN" > "$prefix.out" 2> "$prefix.err"
    rc=$?
    printf '%s\n' "$rc" > "$prefix.rc"
    cut -d' ' -f1 /proc/loadavg > "$prefix.load.after"
    "$ANALYZER" "$prefix.out" --rc "$rc" > "$prefix.analysis" 2>&1
    analyzerRc=$?
    firstDiv=$(field first_div < "$prefix.analysis")
    wave8=$(field wave8 < "$prefix.analysis")
    final=$(field final < "$prefix.analysis")
    deltaWave8=$(field delta_wave8 < "$prefix.analysis")
    deltaFinal=$(field delta_final < "$prefix.analysis")
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$i" "$rc" "$analyzerRc" "${firstDiv:-NA}" "${wave8:-NA}" "${final:-NA}" \
        "${deltaWave8:-NA}" "${deltaFinal:-NA}" \
        "$(cat "$prefix.load.before")" "$(cat "$prefix.load.after")" >> "$OUT/runs.tsv"
done

uptime > "$OUT/uptime.after"
sha256sum "$OUT/runs.tsv" > "$OUT/SHA256SUMS"
