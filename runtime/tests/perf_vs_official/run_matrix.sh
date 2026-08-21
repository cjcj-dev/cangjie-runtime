#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 7 ]]; then
    echo "usage: $0 OUTPUT_DIR WORKLOADS_TSV OFFICIAL_RT SUBJECT_RT ROUNDS CORES TIMEOUT_SECONDS" >&2
    exit 2
fi

out=$1
workloads=$(realpath "$2")
official_rt=$(realpath "$3")
subject_rt=$(realpath "$4")
rounds=$5
cores=$6
timeout_seconds=$7
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
soak_pid_file=/root/cjpmck-run/soak_paint/soak.pid

[[ "$rounds" =~ ^[0-9]+$ && "$rounds" -ge 1 ]] || { echo "ROUNDS must be positive" >&2; exit 2; }
[[ ! -e "$out" ]] || { echo "refusing to overwrite: $out" >&2; exit 2; }
[[ -r "$workloads" ]] || { echo "cannot read workload manifest: $workloads" >&2; exit 2; }
[[ -r "$soak_pid_file" ]] || { echo "cannot read soak pid file: $soak_pid_file" >&2; exit 2; }

soak=$(tr -dc '0-9' <"$soak_pid_file")
[[ -n "$soak" && -r "/proc/$soak/status" ]] || { echo "soak process is not observable" >&2; exit 2; }
soak_state=$(awk '/^State:/ {print $2}' "/proc/$soak/status")
[[ "$soak_state" == T || "$soak_state" == t ]] || {
    echo "soak is not stopped (state=$soak_state); STOP it in a separate command before measurement" >&2
    exit 2
}
grep -Eq "^perfbar(-[0-9]+)? cores=${cores}([[:space:]]|$)" /dev/shm/MEASURE_ACTIVE || {
    echo "perfbar does not own cores=$cores in /dev/shm/MEASURE_ACTIVE" >&2
    exit 2
}

install -d "$out/runs" "$out/evidence"
cp "$workloads" "$out/manifest.tsv"
cat /dev/shm/MEASURE_ACTIVE >"$out/evidence/MEASURE_ACTIVE.before"
cat /proc/meminfo >"$out/evidence/meminfo.before"
cat /proc/loadavg >"$out/evidence/loadavg.before"
lscpu >"$out/evidence/lscpu.txt"
uname -a >"$out/evidence/uname.txt"
{
    printf 'begin=%s\n' "$(date --iso-8601=ns)"
    printf 'hostname=%s\n' "$(hostname)"
    printf 'rounds=%s\ncores=%s\ntimeout_seconds=%s\n' "$rounds" "$cores" "$timeout_seconds"
    printf 'official_rt=%s\nsubject_rt=%s\n' "$official_rt" "$subject_rt"
    printf 'official_runtime_sha256=%s\n' "$(sha256sum "$official_rt/libcangjie-runtime.so" | awk '{print $1}')"
    printf 'subject_runtime_sha256=%s\n' "$(sha256sum "$subject_rt/libcangjie-runtime.so" | awk '{print $1}')"
    printf 'driver_sha256=%s\n' "$(sha256sum "$0" | awk '{print $1}')"
    printf 'run_one_sha256=%s\n' "$(sha256sum "$script_dir/run_one.sh" | awk '{print $1}')"
    printf 'gc_tuning=UNSET\n'
} >"$out/campaign.meta"
printf '%s\n' $'workload\theap\tround\tstatus\tdetail' >"$out/deferred.tsv"

run_attempt() {
    local id=$1 heap=$2 round=$3 arm=$4 binary=$5 marker=$6 work_units=$7 unit_name=$8
    local run_dir
    run_dir="$out/runs/$id/$heap/r$(printf '%02d' "$round")-$arm"
    "$script_dir/run_one.sh" "$run_dir" "$arm" "$heap" "$binary" "$marker" \
        "$work_units" "$unit_name" \
        "$([[ "$arm" == subject ]] && printf '%s' "$subject_rt" || printf '%s' "$official_rt")" \
        "$cores" "$timeout_seconds" | tee -a "$out/campaign.log"
}

while IFS=$'\t' read -r id _source _source_sha _compiler _compiler_sha _compiler_stamp binary binary_sha marker work_units unit_name; do
    [[ "$id" == workload ]] && continue
    [[ -n "$id" ]] || continue
    [[ $(sha256sum "$binary" | awk '{print $1}') == "$binary_sha" ]] || {
        echo "binary SHA drift for $id" >&2
        exit 2
    }
    for heap in 256MB 1GB; do
        for ((round = 1; round <= rounds; round++)); do
            mem_available_kb=$(awk '/^MemAvailable:/ {print $2}' /proc/meminfo)
            if ((mem_available_kb < 41943040)); then
                printf '%s\t%s\t%s\tDEFERRED_LOW_MEM\tMemAvailable_kB=%s\n' \
                    "$id" "$heap" "$round" "$mem_available_kb" >>"$out/deferred.tsv"
                continue
            fi
            printf '%s %s %s\n' "$(date --iso-8601=ns)" "$id/$heap/r$round" "$(cat /proc/loadavg)" \
                >>"$out/evidence/pair_loadavg.tsv"
            if ((round % 2 == 1)); then
                order=(subject official)
            else
                order=(official subject)
            fi
            for arm in "${order[@]}"; do
                run_attempt "$id" "$heap" "$round" "$arm" "$binary" "$marker" "$work_units" "$unit_name"
            done
        done
    done
done <"$workloads"

cat /proc/meminfo >"$out/evidence/meminfo.after"
cat /proc/loadavg >"$out/evidence/loadavg.after"
printf 'end=%s\n' "$(date --iso-8601=ns)" >>"$out/campaign.meta"
echo "MATRIX_RUN_DONE out=$out"
