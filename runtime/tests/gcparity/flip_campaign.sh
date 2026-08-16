#!/usr/bin/env bash
# Mirror-interleaved OFF/ON campaign for MRT_GCV2_MINOR_YOUNG_FLIP.
# Both arms of a cell stay inside the same window (cores 80-95); odd rounds run
# OFF then ON, even rounds run ON then OFF, so arm and drift are not confounded.
set -u

base=/root/minorflip-run
rounds=${1:-20}
diag=${2:-none}
tag=${3:-formal}
runtime_lib="$base/rt-main"
bin_dir="$base/bin"
progress="$base/evidence/$tag-progress.tsv"
runroot="$base/runs/$tag"

install -d "$base/evidence" "$runroot"
if [[ -e "$progress" ]]; then
    echo "refusing to overwrite existing progress file: $progress" >&2
    exit 2
fi
printf 'workload\theap\tmode\tarm\tround\tposition\trc\twall_s\tminor\tmaskequiv_checked\tstdout_sha\n' \
    > "$progress"

printf 'begin=%s\nrounds=%s\ndiag=%s\n' "$(date --iso-8601=ns)" "$rounds" "$diag" \
    > "$base/evidence/$tag-window.txt"

for workload in allocation_dense survival_dense; do
    for heap in 256MB; do
        for mode in serial parallel; do
            for round in $(seq 1 "$rounds"); do
                if (( round % 2 == 1 )); then
                    order=(OFF ON)
                else
                    order=(ON OFF)
                fi
                position=0
                for arm in "${order[@]}"; do
                    position=$((position + 1))
                    out_dir=$(printf '%s/%s/%s/%s/%s/r%02d' \
                        "$runroot" "$workload" "$heap" "$mode" "$arm" "$round")
                    bash "$base/tools/flip_run_one.sh" \
                        "$out_dir" "$arm" "$workload" "$heap" "$mode" \
                        "$runtime_lib" "$bin_dir" 420 "$diag"
                    rc=$(cat "$out_dir/rc")
                    wall=$(sed -n 's/.*wall_s=\([0-9.]*\).*/\1/p' "$out_dir/time.txt" | head -1)
                    minor=$(cat "$out_dir"/report.log.* "$out_dir"/runtime.log.* 2>/dev/null \
                        | grep -c 'young collection stw time')
                    checked=$(cat "$out_dir"/report.log.* "$out_dir"/runtime.log.* 2>/dev/null \
                        | sed -n 's/.*\[GCV2\]\[maskequiv\] checked=\([0-9]*\).*/\1/p' | tail -1)
                    sha=$(cut -d' ' -f1 < "$out_dir/stdout.sha256")
                    printf '%s\t%s\t%s\t%s\tr%02d\t%s\t%s\t%s\t%s\t%s\t%s\n' \
                        "$workload" "$heap" "$mode" "$arm" "$round" "$position" \
                        "$rc" "$wall" "$minor" "${checked:-NA}" "$sha" >> "$progress"
                done
            done
        done
    done
done

printf 'end=%s\n' "$(date --iso-8601=ns)" >> "$base/evidence/$tag-window.txt"
printf 'DONE\n' >> "$progress"
