#!/usr/bin/env bash
# Validate the sticky-minor measurement apparatus with same-source fixtures.
# This script establishes apparatus validity and a same-arm wall-clock noise
# floor. It deliberately does not report a performance comparison between arms.
set -euo pipefail

usage()
{
    cat <<'EOF'
Usage: run_perfprep_harness.sh --out DIR [options]

Options:
  --out DIR                 evidence directory (required, must be empty)
  --cores RANGE             CPU range (default: 48-63)
  --rounds N                repeated main arm-A rounds (default: 5, minimum: 5)
  --timeout SEC             timeout for one round (default: 180)
  --marker LINE             required exact MEASURE_ACTIVE line
  --main-bin FILE           fixture containing the sticky consumer
  --control-bin FILE        same-source fixture without the consumer
  --main-source FILE        source used for the main fixture
  --control-source FILE     source used for the no-consumer fixture
  --runtime-dir DIR         equipped runtime directory
  --sdk DIR                 dependency root

The schedule is main A1, main B1, no-consumer A1, no-consumer B1, then
main A2..AN. A means MRT_STICKY_MINOR=1; B means MRT_STICKY_MINOR=0.
MRT_GC_STRESS_MINOR=1 supplies a deterministic fixture workload; neither
sticky slow path nor other stress controls are enabled.
EOF
}

die()
{
    printf 'perfprep: %s\n' "$*" >&2
    exit 2
}

count_matches()
{
    local pattern=$1 file=$2
    if [[ -f "$file" ]]; then
        grep -a -E -c "$pattern" "$file" 2>/dev/null || true
    else
        printf '0\n'
    fi
}

sum_matches()
{
    local pattern=$1 file=$2
    if [[ -f "$file" ]]; then
        sed -nE "$pattern" "$file" | awk '{ total += $1 } END { printf "%.0f\n", total + 0 }'
    else
        printf '0\n'
    fi
}

out=''
cores=48-63
rounds=5
round_timeout=180
marker='perfprep cores=48-63 KIND=build'
main_bin='/root/gcstress-run/evidence/stress-fixture/smoke_full'
control_bin='/root/fleet/evidence/stock-llc-control/smoke_stock'
main_source='/root/fleet/evidence/b1-sticky-smoke/main.cj'
control_source='/root/fleet/evidence/stock-llc-control/main.cj'
runtime_dir='/root/fleet/RT-CAND-EQUIPPED'
sdk='/root/fleet/sdk-745b'

while (($#)); do
    case "$1" in
        --out) out=${2:-}; shift 2 ;;
        --cores) cores=${2:-}; shift 2 ;;
        --rounds) rounds=${2:-}; shift 2 ;;
        --timeout) round_timeout=${2:-}; shift 2 ;;
        --marker) marker=${2:-}; shift 2 ;;
        --main-bin) main_bin=${2:-}; shift 2 ;;
        --control-bin) control_bin=${2:-}; shift 2 ;;
        --main-source) main_source=${2:-}; shift 2 ;;
        --control-source) control_source=${2:-}; shift 2 ;;
        --runtime-dir) runtime_dir=${2:-}; shift 2 ;;
        --sdk) sdk=${2:-}; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown argument: $1" ;;
    esac
done

[[ -n "$out" && "$out" == /* ]] || die '--out must be an absolute path'
[[ "$cores" =~ ^[0-9]+-[0-9]+$ ]] || die '--cores must be START-END'
[[ "$rounds" =~ ^[0-9]+$ && "$rounds" -ge 5 ]] || die '--rounds must be at least 5'
[[ "$round_timeout" =~ ^[0-9]+$ && "$round_timeout" -gt 0 ]] || die '--timeout must be positive'
grep -q -x -F "$marker" /dev/shm/MEASURE_ACTIVE 2>/dev/null || die "missing marker line: $marker"

runtime_so="$runtime_dir/libcangjie-runtime.so"
for file in "$main_bin" "$control_bin" "$main_source" "$control_source" "$runtime_so"; do
    [[ -f "$file" ]] || die "missing input file: $file"
done
for file in "$main_bin" "$control_bin"; do
    [[ -x "$file" ]] || die "fixture is not executable: $file"
done
[[ -d "$sdk" ]] || die "missing SDK directory: $sdk"
cmp -s "$main_source" "$control_source" || die 'fixture sources are not byte-identical'
grep -a -q 'MRT_GC_STRESS_MINOR' "$runtime_so" || die 'runtime lacks the deterministic fixture control'

main_consumers=$(readelf -Ws "$main_bin" 2>/dev/null | grep -c '__cj_sticky_logged_base' || true)
control_consumers=$(readelf -Ws "$control_bin" 2>/dev/null | grep -c '__cj_sticky_logged_base' || true)
[[ "$main_consumers" -gt 0 ]] || die 'main fixture has no sticky consumer'
[[ "$control_consumers" == 0 ]] || die 'no-consumer fixture unexpectedly has a sticky consumer'

mkdir -p "$out"
[[ -z $(find "$out" -mindepth 1 -maxdepth 1 -print -quit 2>/dev/null) ]] || \
    die "output directory is not empty: $out"

ld_path="$runtime_dir:$sdk/runtime/lib/linux_x86_64_cjnative:$sdk/third_party/llvm/lib:$sdk/tools/lib"
results="$out/results.tsv"
printf 'kind\tarm\tround\tsticky\trc\tvalidity\twall_s\tgc_us\tgc_pct\tminor\tmajor\tmaxrss_kb\tbinary_sha256\truntime_sha256\tstdout_sha256\tmode\tactual_exe\tcpus_allowed\trun_dir\n' > "$results"
{
    printf 'role\tpath\tsha256\tsticky_consumer_lines\tsource_sha256\n'
    printf 'main\t%s\t%s\t%s\t%s\n' "$main_bin" "$(sha256sum "$main_bin" | awk '{print $1}')" \
        "$main_consumers" "$(sha256sum "$main_source" | awk '{print $1}')"
    printf 'no-consumer\t%s\t%s\t%s\t%s\n' "$control_bin" \
        "$(sha256sum "$control_bin" | awk '{print $1}')" "$control_consumers" \
        "$(sha256sum "$control_source" | awk '{print $1}')"
    printf 'runtime\t%s\t%s\t-\t-\n' "$runtime_so" "$(sha256sum "$runtime_so" | awk '{print $1}')"
} > "$out/inventory.tsv"

env LD_LIBRARY_PATH="$ld_path" ldd "$main_bin" > "$out/main.ldd"
env LD_LIBRARY_PATH="$ld_path" ldd "$control_bin" > "$out/no-consumer.ldd"
grep -q -F "$runtime_so" "$out/main.ldd" || die 'main fixture does not resolve the frozen runtime'
grep -q -F "$runtime_so" "$out/no-consumer.ldd" || die 'no-consumer fixture does not resolve the frozen runtime'
grep '^MemAvailable:' /proc/meminfo > "$out/mem-before.txt"
df -h /root > "$out/disk-before.txt"

run_one()
{
    local kind=$1 arm=$2 round=$3 sticky=$4 fixture expected_mode
    case "$kind" in
        main)
            fixture=$main_bin
            if [[ "$sticky" == 1 ]]; then expected_mode='on(default)'; else expected_mode='off(env)'; fi
            ;;
        no-consumer)
            fixture=$control_bin
            if [[ "$sticky" == 1 ]]; then
                expected_mode='auto-disabled(no consumer)'
            else
                expected_mode='off(env)'
            fi
            ;;
        *) die "unknown kind: $kind" ;;
    esac

    local run_dir="$out/$kind-$arm-r$round"
    local time_file="$run_dir/time.txt"
    local runtime_log="$run_dir/runtime.log"
    local report_log="$run_dir/report.log"
    mkdir -p "$run_dir"
    printf 'nice -n 15 taskset -c %q timeout %q env MRT_STICKY_MINOR=%q MRT_GC_STRESS_MINOR=1 %q\n' \
        "$cores" "$round_timeout" "$sticky" "$fixture" > "$run_dir/command.txt"

    local launcher_pid rc=0 observed_pid='' actual_exe='UNOBSERVED' actual_sha='UNOBSERVED'
    (
        exec nice -n 15 taskset -c "$cores" /usr/bin/time \
            -f 'wall_s=%e user_s=%U sys_s=%S maxrss_kb=%M exit=%x' -o "$time_file" \
            timeout "$round_timeout" env \
            -u MRT_GC_STRESS -u MRT_STICKY_MINOR_FORCE_SLOW_PATH -u MRT_STICKY_FORCE_SLOW \
            LD_LIBRARY_PATH="$ld_path" cjHeapSize=24GB MRT_GC_STRESS_MINOR=1 \
            MRT_STICKY_MINOR="$sticky" MRT_REPORT="$report_log" \
            MRT_LOG_PATH="$runtime_log" MRT_LOG_LEVEL=i "$fixture" \
            > "$run_dir/stdout.log" 2> "$run_dir/stderr.log"
    ) &
    launcher_pid=$!

    local attempt frontier child exe
    for attempt in $(seq 1 100); do
        frontier=$launcher_pid
        for _ in 1 2 3 4; do
            [[ -r "/proc/$frontier/task/$frontier/children" ]] || break
            child=$(awk '{print $1}' "/proc/$frontier/task/$frontier/children" 2>/dev/null || true)
            [[ -n "$child" ]] || break
            frontier=$child
            exe=$(readlink "/proc/$frontier/exe" 2>/dev/null || true)
            if [[ "$exe" == "$fixture" ]]; then
                observed_pid=$frontier
                break 2
            fi
        done
        sleep 0.01
    done
    if [[ -n "$observed_pid" ]]; then
        actual_exe=$(readlink "/proc/$observed_pid/exe" 2>/dev/null || true)
        actual_sha=$(sha256sum "/proc/$observed_pid/exe" 2>/dev/null | awk '{print $1}' || true)
        tr '\0' '\n' < "/proc/$observed_pid/environ" | LC_ALL=C sort > "$run_dir/environ.txt"
        grep '^Cpus_allowed_list:' "/proc/$observed_pid/status" > "$run_dir/cpus-allowed.txt"
        cp "/proc/$observed_pid/maps" "$run_dir/maps.txt"
    else
        printf 'UNOBSERVED\n' > "$run_dir/environ.txt"
        printf 'UNOBSERVED\n' > "$run_dir/cpus-allowed.txt"
        printf 'UNOBSERVED\n' > "$run_dir/maps.txt"
    fi
    wait "$launcher_pid" || rc=$?

    local wall_s maxrss_kb minor major minor_us major_us gc_us gc_pct
    local binary_sha runtime_sha stdout_sha mode cpus_allowed validity
    wall_s=$(sed -nE 's/.*wall_s=([^ ]+).*/\1/p' "$time_file" 2>/dev/null || true)
    maxrss_kb=$(sed -nE 's/.*maxrss_kb=([^ ]+).*/\1/p' "$time_file" 2>/dev/null || true)
    minor=$(count_matches '\[StickyMinor\] run=' "$report_log")
    major=$(count_matches 'total gc time: [0-9,]+ us, collection rate' "$report_log")
    minor_us=$(sum_matches 's/.*\[StickyMinor\] run=.*pause=([0-9]+) us.*/\1/p' "$report_log")
    major_us=$(sed -nE 's/.*total gc time: ([0-9,]+) us, collection rate.*/\1/p' "$report_log" \
        | tr -d ',' | awk '{ total += $1 } END { printf "%.0f\n", total + 0 }')
    gc_us=$((minor_us + major_us))
    gc_pct=$(awk -v gc="$gc_us" -v wall="${wall_s:-0}" \
        'BEGIN { if (wall > 0) printf "%.6f", gc / (wall * 10000); else print "nan" }')
    binary_sha=$(sha256sum "$fixture" | awk '{print $1}')
    runtime_sha=$(sha256sum "$runtime_so" | awk '{print $1}')
    stdout_sha=$(sha256sum "$run_dir/stdout.log" | awk '{print $1}')
    mode=$(sed -nE 's/.*sticky minor: (.*)$/\1/p' "$runtime_log" 2>/dev/null | tail -n1)
    cpus_allowed=$(sed -nE 's/^Cpus_allowed_list:[[:space:]]*//p' "$run_dir/cpus-allowed.txt" 2>/dev/null || true)

    validity=VALID
    [[ "$rc" == 0 ]] || validity="INVALID_RC_$rc"
    [[ "$actual_sha" == "$binary_sha" ]] || validity='INVALID_EXE_SHA'
    grep -q -x -F "MRT_STICKY_MINOR=$sticky" "$run_dir/environ.txt" 2>/dev/null || validity='INVALID_ENV'
    grep -q -x -F 'MRT_GC_STRESS_MINOR=1' "$run_dir/environ.txt" 2>/dev/null || validity='INVALID_STRESS'
    grep -q -x -F 'MRT_STICKY_MINOR_FORCE_SLOW_PATH=' "$run_dir/environ.txt" 2>/dev/null && validity='INVALID_FORCE_SLOW'
    grep -q -x -F 'MRT_STICKY_FORCE_SLOW=' "$run_dir/environ.txt" 2>/dev/null && validity='INVALID_FORCE_SLOW'
    grep -q -F "$runtime_so" "$run_dir/maps.txt" 2>/dev/null || validity='INVALID_RUNTIME_MAP'
    [[ "$cpus_allowed" == "$cores" ]] || validity='INVALID_CPUSET'
    [[ "$mode" == "$expected_mode" ]] || validity='INVALID_MODE'
    if [[ "$kind" == main && "$arm" == A && "$minor" == 0 ]]; then
        validity='INVALID_ZERO_YOUNG'
    elif [[ "$arm" == B && "$minor" != 0 ]]; then
        validity='INVALID_B_YOUNG'
    elif [[ "$kind" == no-consumer && "$minor" != 0 ]]; then
        validity='INVALID_NO_CONSUMER_YOUNG'
    fi

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$kind" "$arm" "$round" "$sticky" "$rc" "$validity" "${wall_s:-MISSING}" "$gc_us" \
        "$gc_pct" "$minor" "$major" "${maxrss_kb:-MISSING}" "$binary_sha" "$runtime_sha" \
        "$stdout_sha" "${mode:-MISSING}" "$actual_exe" "${cpus_allowed:-MISSING}" "$run_dir" | tee -a "$results"
}

run_one main A 1 1
run_one main B 1 0
run_one no-consumer A 1 1
run_one no-consumer B 1 0
for ((round=2; round<=rounds; ++round)); do
    run_one main A "$round" 1
done

grep '^MemAvailable:' /proc/meminfo > "$out/mem-after.txt"
df -h /root > "$out/disk-after.txt"

awk -F '\t' '
    NR == 1 { next }
    $1 == "main" && $2 == "A" && $6 == "VALID" {
        n++; x[n] = $7 + 0; sum += x[n]
        if (n == 1 || x[n] < min) min = x[n]
        if (n == 1 || x[n] > max) max = x[n]
    }
    END {
        if (n == 0) exit 1
        mean = sum / n
        for (i = 1; i <= n; ++i) ss += (x[i] - mean) * (x[i] - mean)
        sd = (n > 1) ? sqrt(ss / (n - 1)) : 0
        printf "N\tmean_s\tmin_s\tmax_s\trange_s\tsample_stddev_s\trange_pct_of_mean\tstddev_pct_of_mean\n"
        printf "%d\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\t%.6f\n", n, mean, min, max,
            max - min, sd, (max - min) * 100 / mean, sd * 100 / mean
    }
' "$results" > "$out/noise-floor.tsv" || true

invalid=$(awk -F '\t' 'NR > 1 && $6 != "VALID" { n++ } END { print n + 0 }' "$results")
main_a_young=$(awk -F '\t' '$1 == "main" && $2 == "A" { s += $10 } END { print s + 0 }' "$results")
main_b_young=$(awk -F '\t' '$1 == "main" && $2 == "B" { s += $10 } END { print s + 0 }' "$results")
control_a_young=$(awk -F '\t' '$1 == "no-consumer" && $2 == "A" { s += $10 } END { print s + 0 }' "$results")
control_b_young=$(awk -F '\t' '$1 == "no-consumer" && $2 == "B" { s += $10 } END { print s + 0 }' "$results")
main_binary_unique=$(awk -F '\t' '$1 == "main" { print $13 }' "$results" | sort -u | wc -l)
control_binary_unique=$(awk -F '\t' '$1 == "no-consumer" { print $13 }' "$results" | sort -u | wc -l)
main_stdout_unique=$(awk -F '\t' '$1 == "main" { print $15 }' "$results" | sort -u | wc -l)
control_stdout_unique=$(awk -F '\t' '$1 == "no-consumer" { print $15 }' "$results" | sort -u | wc -l)
noise_n=$(awk -F '\t' 'NR == 2 { print $1 + 0 }' "$out/noise-floor.tsv" 2>/dev/null || printf '0')

pos_diff=fail
pos_nodiff=fail
[[ "$main_a_young" -gt 0 && "$main_b_young" == 0 ]] && pos_diff=pass
[[ "$control_a_young" == 0 && "$control_b_young" == 0 && "$control_binary_unique" == 1 && \
    "$control_stdout_unique" == 1 ]] && pos_nodiff=pass
verdict=harness-invalid
if [[ "$invalid" == 0 && "$pos_diff" == pass && "$pos_nodiff" == pass && \
    "$main_binary_unique" == 1 && "$main_stdout_unique" == 1 && "$noise_n" -ge 5 ]]; then
    verdict=harness-ready
fi

{
    printf 'POSCTRL_DIFF=%s young_A=%s young_B=%s\n' "$pos_diff" "$main_a_young" "$main_b_young"
    printf 'POSCTRL_NODIFF=%s young_A=%s young_B=%s binary_unique=%s stdout_unique=%s\n' \
        "$pos_nodiff" "$control_a_young" "$control_b_young" "$control_binary_unique" "$control_stdout_unique"
    printf 'MAIN_BINARY_UNIQUE=%s\n' "$main_binary_unique"
    printf 'MAIN_STDOUT_UNIQUE=%s\n' "$main_stdout_unique"
    printf 'INVALID_ROUNDS=%s\n' "$invalid"
    printf 'NOISE_N=%s\n' "$noise_n"
    printf 'VERDICT=%s\n' "$verdict"
} | tee "$out/summary.txt"

[[ "$verdict" == harness-ready ]]
