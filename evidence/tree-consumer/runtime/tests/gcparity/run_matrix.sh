#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 6 ]]; then
    echo "usage: $0 OUT_ROOT N BIN_DIR RTLIB CORES TIMEOUT_SECONDS" >&2
    exit 2
fi

out_root=$1
n=$2
bin_dir=$3
runtime_lib=$4
cores=$5
timeout_seconds=$6
runner=$(cd "$(dirname "$0")" && pwd)/run_one.sh

for workload in allocation_dense survival_dense; do
    for heap in 256MB 1GB; do
        for fys in 1 0; do
            cell="$out_root/$workload/$heap/fys$fys"
            install -d "$cell"
            for ((round = 1; round <= n; round++)); do
                if ((round % 2 == 1)); then
                    order=(young minoroff)
                else
                    order=(minoroff young)
                fi
                for arm in "${order[@]}"; do
                    printf '%s workload=%s heap=%s fys=%s round=%02d arm=%s\n' \
                        "$(date --iso-8601=seconds)" "$workload" "$heap" "$fys" "$round" "$arm"
                    "$runner" "$cell/r$(printf '%02d' "$round")-$arm" "$arm" "$heap" "$fys" \
                        "$bin_dir/$workload" "$runtime_lib" "$cores" "$timeout_seconds"
                done
            done
        done
    done
done
