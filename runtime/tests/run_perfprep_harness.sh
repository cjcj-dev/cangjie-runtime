#!/usr/bin/env bash
# Build and validate the sticky-minor performance measurement harness.
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
  --jobs N                  compiler jobs and apc (default: 8)
  --timeout SEC             timeout for one round (default: 600)
  --marker LINE             required exact MEASURE_ACTIVE line
  --main-bin FILE           sticky-consumer compiler
  --control-bin FILE        compiler without a sticky consumer
  --runtime-dir DIR         directory containing libcangjie-runtime.so
  --sdk DIR                 CANGJIE_HOME and dependency root
  --main-root DIR           main corpus source root
  --main-import DIR         main corpus import root
  --control-root DIR        no-consumer corpus source root
  --control-import DIR      no-consumer corpus import root

The fixed corpus is packages/basic/src compiled as a static library. The
schedule is main A1, main B1, no-consumer A1, no-consumer B1, then main
A2..AN. A means MRT_STICKY_MINOR=1; B means MRT_STICKY_MINOR=0.
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
jobs=8
round_timeout=600
marker='perfprep cores=48-63 KIND=build'
main_bin='/root/fleet/CJCJ/cjcj::cjc'
control_bin='/root/onsoak-20260728-4d909/cjcj/target/release/bin/cjcj::cjc'
runtime_dir='/root/fleet/RT-CAND'
sdk='/root/fleet/sdk-745b'
main_root='/root/fleet/work/cjcj-b566-src/cjcj'
main_import='/root/fleet/work/cjcj-b566-target/release'
control_root='/root/onsoak-20260728-4d909/cjcj'
control_import='/root/onsoak-20260728-4d909/cjcj/target/release'

while (($#)); do
    case "$1" in
        --out) out=${2:-}; shift 2 ;;
        --cores) cores=${2:-}; shift 2 ;;
        --rounds) rounds=${2:-}; shift 2 ;;
        --jobs) jobs=${2:-}; shift 2 ;;
        --timeout) round_timeout=${2:-}; shift 2 ;;
        --marker) marker=${2:-}; shift 2 ;;
        --main-bin) main_bin=${2:-}; shift 2 ;;
        --control-bin) control_bin=${2:-}; shift 2 ;;
        --runtime-dir) runtime_dir=${2:-}; shift 2 ;;
        --sdk) sdk=${2:-}; shift 2 ;;
        --main-root) main_root=${2:-}; shift 2 ;;
        --main-import) main_import=${2:-}; shift 2 ;;
        --control-root) control_root=${2:-}; shift 2 ;;
        --control-import) control_import=${2:-}; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown argument: $1" ;;
    esac
done

[[ -n "$out" && "$out" == /* ]] || die '--out must be an absolute path'
[[ "$cores" =~ ^[0-9]+-[0-9]+$ ]] || die '--cores must be START-END'
[[ "$rounds" =~ ^[0-9]+$ && "$rounds" -ge 5 ]] || die '--rounds must be at least 5'
[[ "$jobs" =~ ^[0-9]+$ && "$jobs" -gt 0 ]] || die '--jobs must be positive'
[[ "$round_timeout" =~ ^[0-9]+$ && "$round_timeout" -gt 0 ]] || die '--timeout must be positive'
grep -q -x -F "$marker" /dev/shm/MEASURE_ACTIVE 2>/dev/null || die "missing marker line: $marker"

runtime_so="$runtime_dir/libcangjie-runtime.so"
for file in "$main_bin" "$control_bin" "$runtime_so"; do
    [[ -f "$file" ]] || die "missing input file: $file"
done
for file in "$main_bin" "$control_bin"; do
    [[ -x "$file" ]] || die "compiler is not executable: $file"
done
for dir in "$sdk" "$main_root/packages/basic/src" "$main_import" \
    "$control_root/packages/basic/src" "$control_import"; do
    [[ -d "$dir" ]] || die "missing input directory: $dir"
done

main_consumers=$(readelf -Ws "$main_bin" 2>/dev/null | grep -c '__cj_sticky_logged_base' || true)
control_consumers=$(readelf -Ws "$control_bin" 2>/dev/null | grep -c '__cj_sticky_logged_base' || true)
[[ "$main_consumers" -gt 0 ]] || die 'main compiler has no sticky consumer'
[[ "$control_consumers" == 0 ]] || die 'no-consumer compiler unexpectedly has a sticky consumer'

mkdir -p "$out"
[[ -z $(find "$out" -mindepth 1 -maxdepth 1 -print -quit 2>/dev/null) ]] || \
    die "output directory is not empty: $out"

ld_path="$runtime_dir:$sdk/third_party/llvm/lib:$sdk/runtime/lib/linux_x86_64_cjnative:$sdk/tools/lib"
results="$out/results.tsv"
printf 'kind\tarm\tround\tsticky\trc\tvalidity\twall_s\tgc_us\tgc_pct\tminor\tmajor\tmaxrss_kb\tcompiler_sha256\truntime_sha256\tartifact_sha256\tmode\tactual_exe\tcpus_allowed\trun_dir\n' > "$results"
{
    printf 'role\tpath\tsha256\tsticky_consumer_lines\n'
    printf 'main\t%s\t%s\t%s\n' "$main_bin" "$(sha256sum "$main_bin" | awk '{print $1}')" "$main_consumers"
    printf 'no-consumer\t%s\t%s\t%s\n' "$control_bin" "$(sha256sum "$control_bin" | awk '{print $1}')" "$control_consumers"
    printf 'runtime\t%s\t%s\t-\n' "$runtime_so" "$(sha256sum "$runtime_so" | awk '{print $1}')"
} > "$out/inventory.tsv"

env LD_LIBRARY_PATH="$ld_path" ldd "$main_bin" > "$out/main.ldd"
env LD_LIBRARY_PATH="$ld_path" ldd "$control_bin" > "$out/no-consumer.ldd"
grep -q -F "$runtime_so" "$out/main.ldd" || die 'main compiler does not resolve the frozen runtime'
grep -q -F "$runtime_so" "$out/no-consumer.ldd" || die 'no-consumer compiler does not resolve the frozen runtime'
grep '^MemAvailable:' /proc/meminfo > "$out/mem-before.txt"
df -h /root > "$out/disk-before.txt"

run_one()
{
    local kind=$1 arm=$2 round=$3 sticky=$4 compiler root import expected_mode
    case "$kind" in
        main)
            compiler=$main_bin
            root=$main_root
            import=$main_import
            if [[ "$sticky" == 1 ]]; then
                expected_mode='on(default)'
            else
                expected_mode='off(env)'
            fi
            ;;
        no-consumer)
            compiler=$control_bin
            root=$control_root
            import=$control_import
            if [[ "$sticky" == 1 ]]; then
                expected_mode='auto-disabled(no consumer)'
            else
                expected_mode='off(env)'
            fi
            ;;
        *) die "unknown kind: $kind" ;;
    esac

    local run_dir="$out/$kind-$arm-r$round"
    local artifact="$run_dir/out.a"
    local time_file="$run_dir/time.txt"
    local runtime_log="$run_dir/runtime.log"
    local report_log="$run_dir/report.log"
    mkdir -p "$run_dir"
    {
        printf 'cd %q\n' "$root"
        printf 'nice -n 15 taskset -c %q timeout %q env MRT_STICKY_MINOR=%q <frozen compiler> --package packages/basic/src --module-name cjcj --import-path %q --output-type=staticlib -o %q --jobs %q --apc=%q\n' \
            "$cores" "$round_timeout" "$sticky" "$import" "$artifact" "$jobs" "$jobs"
    } > "$run_dir/command.txt"

    local launcher_pid rc=0 observed_pid='' actual_exe='UNOBSERVED' actual_sha='UNOBSERVED'
    (
        cd "$root"
        exec nice -n 15 taskset -c "$cores" /usr/bin/time \
            -f 'wall_s=%e user_s=%U sys_s=%S maxrss_kb=%M exit=%x' -o "$time_file" \
            timeout "$round_timeout" env \
            -u MRT_GC_STRESS_MINOR -u MRT_GC_STRESS \
            -u MRT_STICKY_MINOR_FORCE_SLOW_PATH -u MRT_STICKY_FORCE_SLOW \
            CANGJIE_HOME="$sdk" LD_LIBRARY_PATH="$ld_path" cjHeapSize=24GB \
            MRT_STICKY_MINOR="$sticky" MRT_REPORT="$report_log" \
            MRT_LOG_PATH="$runtime_log" MRT_LOG_LEVEL=i \
            "$compiler" --package packages/basic/src --module-name cjcj \
            --import-path "$import" --output-type=staticlib -o "$artifact" \
            --jobs "$jobs" --apc="$jobs" > "$run_dir/stdout.log" 2> "$run_dir/stderr.log"
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
            if [[ "$exe" == "$compiler" ]]; then
                observed_pid=$frontier
                break 2
            fi
        done
        sleep 0.02
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

    local wall_s maxrss_kb minor major minor_us major_ns gc_us gc_pct
    local compiler_sha runtime_sha artifact_sha mode cpus_allowed validity
    wall_s=$(sed -nE 's/.*wall_s=([^ ]+).*/\1/p' "$time_file" 2>/dev/null || true)
    maxrss_kb=$(sed -nE 's/.*maxrss_kb=([^ ]+).*/\1/p' "$time_file" 2>/dev/null || true)
    minor=$(count_matches '\[StickyMinor\] run=' "$report_log")
    major=$(count_matches 'GC for ' "$runtime_log")
    minor_us=$(sum_matches 's/.*\[StickyMinor\] run=.*pause=([0-9]+) us.*/\1/p' "$report_log")
    major_ns=$(sum_matches 's/.*GC for .*total GC time: ([0-9]+)->.*/\1/p' "$runtime_log")
    gc_us=$((minor_us + major_ns / 1000))
    gc_pct=$(awk -v gc="$gc_us" -v wall="${wall_s:-0}" \
        'BEGIN { if (wall > 0) printf "%.6f", gc / (wall * 10000); else print "nan" }')
    compiler_sha=$(sha256sum "$compiler" | awk '{print $1}')
    runtime_sha=$(sha256sum "$runtime_so" | awk '{print $1}')
    if [[ -f "$artifact" ]]; then
        artifact_sha=$(sha256sum "$artifact" | awk '{print $1}')
    else
        artifact_sha='MISSING'
    fi
    mode=$(sed -nE 's/.*sticky minor: (.*)$/\1/p' "$runtime_log" 2>/dev/null | tail -n1)
    cpus_allowed=$(sed -nE 's/^Cpus_allowed_list:[[:space:]]*//p' "$run_dir/cpus-allowed.txt" 2>/dev/null || true)

    validity=VALID
    [[ "$rc" == 0 ]] || validity="INVALID_RC_$rc"
    [[ "$actual_sha" == "$compiler_sha" ]] || validity='INVALID_EXE_SHA'
    grep -q -x -F "MRT_STICKY_MINOR=$sticky" "$run_dir/environ.txt" 2>/dev/null || validity='INVALID_ENV'
    grep -q -x -F 'MRT_GC_STRESS_MINOR=' "$run_dir/environ.txt" 2>/dev/null && validity='INVALID_STRESS'
    grep -q -x -F 'MRT_GC_STRESS=' "$run_dir/environ.txt" 2>/dev/null && validity='INVALID_STRESS'
    grep -q -x -F "MRT_STICKY_MINOR_FORCE_SLOW_PATH=" "$run_dir/environ.txt" 2>/dev/null && validity='INVALID_FORCE_SLOW'
    grep -q -x -F "MRT_STICKY_FORCE_SLOW=" "$run_dir/environ.txt" 2>/dev/null && validity='INVALID_FORCE_SLOW'
    grep -q -F "$runtime_so" "$run_dir/maps.txt" 2>/dev/null || validity='INVALID_RUNTIME_MAP'
    [[ "$cpus_allowed" == "$cores" ]] || validity='INVALID_CPUSET'
    [[ "$artifact_sha" != MISSING ]] || validity='INVALID_ARTIFACT'
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
        "$gc_pct" "$minor" "$major" "${maxrss_kb:-MISSING}" "$compiler_sha" "$runtime_sha" \
        "$artifact_sha" "${mode:-MISSING}" "$actual_exe" "${cpus_allowed:-MISSING}" "$run_dir" | tee -a "$results"

    rm -f "$artifact" "$run_dir/out.cjo" "$run_dir/basic@cjcj.cjo"
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
        n++
        x[n] = $7 + 0
        sum += x[n]
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
main_artifacts=$(awk -F '\t' '$1 == "main" { print $15 }' "$results" | sort -u | wc -l)
control_artifacts=$(awk -F '\t' '$1 == "no-consumer" { print $15 }' "$results" | sort -u | wc -l)
noise_n=$(awk -F '\t' 'NR == 2 { print $1 + 0 }' "$out/noise-floor.tsv" 2>/dev/null || printf '0')

pos_diff=fail
pos_nodiff=fail
[[ "$main_a_young" -gt 0 && "$main_b_young" == 0 ]] && pos_diff=pass
[[ "$control_a_young" == 0 && "$control_b_young" == 0 && "$control_artifacts" == 1 ]] && pos_nodiff=pass
verdict=harness-invalid
if [[ "$invalid" == 0 && "$pos_diff" == pass && "$pos_nodiff" == pass && \
    "$main_artifacts" == 1 && "$noise_n" -ge 5 ]]; then
    verdict=harness-ready
fi

{
    printf 'POSCTRL_DIFF=%s young_A=%s young_B=%s\n' "$pos_diff" "$main_a_young" "$main_b_young"
    printf 'POSCTRL_NODIFF=%s young_A=%s young_B=%s artifact_unique=%s\n' \
        "$pos_nodiff" "$control_a_young" "$control_b_young" "$control_artifacts"
    printf 'MAIN_ARTIFACT_UNIQUE=%s\n' "$main_artifacts"
    printf 'INVALID_ROUNDS=%s\n' "$invalid"
    printf 'NOISE_N=%s\n' "$noise_n"
    printf 'VERDICT=%s\n' "$verdict"
} | tee "$out/summary.txt"

[[ "$verdict" == harness-ready ]]
