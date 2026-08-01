#!/usr/bin/env bash
# Stage stop-window measurement for generational GC (MRT_STICKY_MINOR on/off).
# Same pinned binary + runtime; arms differ only by MRT_STICKY_MINOR.
# Dry-run numbers are apparatus checks only — never performance conclusions.
set -euo pipefail

usage()
{
    cat <<'EOF'
Usage: run_perfstage_stopwindow.sh --out DIR [options]

Options:
  --out DIR              evidence directory (required, must be empty)
  --mode MODE            dryrun | stopwindow (default: dryrun)
  --cores RANGE          CPU range (default: dryrun 48-63, stopwindow 0-191)
  --pairs N              interleaved ON/OFF pairs (default: dryrun 1, stopwindow 5)
  --timeout SEC          per-round timeout (default: dryrun 1800, stopwindow 3600)
  --marker LINE          exact MEASURE_ACTIVE line required
  --cjc FILE             pinned compiler binary
  --runtime-so FILE      pinned libcangjie-runtime.so
  --pkg-root DIR         packages root containing <pkg>/src
  --import-path DIR      --import-path for cjc
  --sdk DIR              CANGJIE_HOME / toolchain root
  --corpus LIST          comma packages (default: sema)
  --j N                  compiler -j (default: 16)

Schedule: for each pair p=1..N, for each pkg in corpus:
  ON  (MRT_STICKY_MINOR=1) then OFF (MRT_STICKY_MINOR=0).
Same-window interleave only. Endpoints: wall_s and gc_us (reported separately).
EOF
}

die()
{
    printf 'perfstage: %s\n' "$*" >&2
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

out=''
mode=dryrun
cores=''
pairs=''
round_timeout=''
marker=''
cjc='/root/perfstage-run/pin/cjcj::cjc'
runtime_so='/root/perfstage-run/pin/libcangjie-runtime.so'
pkg_root='/media/kkk2/428602AC8602A111/stickyon3-20260728/src/cjcj/packages'
import_path='/root/stickyon3-target/release'
sdk='/root/.cjv/toolchains/nightly-1.2.0-alpha.20260721165458'
corpus='sema'
parallel_j=16

while (($#)); do
    case "$1" in
        --out) out=${2:-}; shift 2 ;;
        --mode) mode=${2:-}; shift 2 ;;
        --cores) cores=${2:-}; shift 2 ;;
        --pairs) pairs=${2:-}; shift 2 ;;
        --timeout) round_timeout=${2:-}; shift 2 ;;
        --marker) marker=${2:-}; shift 2 ;;
        --cjc) cjc=${2:-}; shift 2 ;;
        --runtime-so) runtime_so=${2:-}; shift 2 ;;
        --pkg-root) pkg_root=${2:-}; shift 2 ;;
        --import-path) import_path=${2:-}; shift 2 ;;
        --sdk) sdk=${2:-}; shift 2 ;;
        --corpus) corpus=${2:-}; shift 2 ;;
        --j) parallel_j=${2:-}; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown argument: $1" ;;
    esac
done

[[ -n "$out" && "$out" == /* ]] || die '--out must be an absolute path'
case "$mode" in
    dryrun)
        [[ -n "$cores" ]] || cores=48-63
        [[ -n "$pairs" ]] || pairs=1
        [[ -n "$round_timeout" ]] || round_timeout=1800
        [[ -n "$marker" ]] || marker='perfstage cores=48-63 KIND=build'
        ;;
    stopwindow)
        [[ -n "$cores" ]] || cores=0-191
        [[ -n "$pairs" ]] || pairs=5
        [[ -n "$round_timeout" ]] || round_timeout=3600
        [[ -n "$marker" ]] || marker='perfstage cores=0-191 KIND=measure STOP_WINDOW=1'
        ;;
    *) die "--mode must be dryrun or stopwindow" ;;
esac

[[ "$cores" =~ ^[0-9]+-[0-9]+$ ]] || die '--cores must be START-END'
[[ "$pairs" =~ ^[0-9]+$ && "$pairs" -ge 1 ]] || die '--pairs must be >=1'
[[ "$round_timeout" =~ ^[0-9]+$ && "$round_timeout" -gt 0 ]] || die '--timeout must be positive'
[[ "$parallel_j" =~ ^[0-9]+$ && "$parallel_j" -ge 1 ]] || die '--j must be positive'
grep -q -x -F "$marker" /dev/shm/MEASURE_ACTIVE 2>/dev/null || die "missing marker line: $marker"

for file in "$cjc" "$runtime_so"; do
    [[ -f "$file" && -x "$file" ]] || die "missing executable: $file"
done
[[ -d "$pkg_root" ]] || die "missing pkg-root: $pkg_root"
[[ -d "$import_path" ]] || die "missing import-path: $import_path"
[[ -d "$sdk" ]] || die "missing sdk: $sdk"
[[ -x "$sdk/third_party/llvm/bin/opt" && -x "$sdk/third_party/llvm/bin/llc" ]] || \
    die "sdk missing third_party/llvm/bin/{opt,llc}"

IFS=',' read -r -a pkgs <<< "$corpus"
((${#pkgs[@]} > 0)) || die 'empty --corpus'
for pkg in "${pkgs[@]}"; do
    [[ -d "$pkg_root/$pkg/src" ]] || die "missing package src: $pkg_root/$pkg/src"
done

consumers=$(readelf -Ws "$cjc" 2>/dev/null | grep -c '__cj_sticky_logged_base' || true)
[[ "$consumers" -gt 0 ]] || die "compiler has no sticky consumer (__cj_sticky_logged_base)"

runtime_dir=$(dirname "$runtime_so")
ld_path="$runtime_dir:$sdk/runtime/lib/linux_x86_64_cjnative:$sdk/third_party/llvm/lib:$sdk/tools/lib"

mkdir -p "$out"
[[ -z $(find "$out" -mindepth 1 -maxdepth 1 -print -quit 2>/dev/null) ]] || \
    die "output directory is not empty: $out"

results="$out/results.tsv"
printf 'mode\tpkg\tarm\tpair\tsticky\trc\tvalidity\twall_s\tgc_us\tgc_pct\tminor\tmajor\tmaxrss_kb\tproduct_sha256\tcjc_sha256\truntime_sha256\tmode_line\tactual_exe\tcpus_allowed\trun_dir\n' > "$results"

{
    printf 'role\tpath\tsha256\tsticky_consumer_lines\n'
    printf 'cjc\t%s\t%s\t%s\n' "$cjc" "$(sha256sum "$cjc" | awk '{print $1}')" "$consumers"
    printf 'runtime\t%s\t%s\t-\n' "$runtime_so" "$(sha256sum "$runtime_so" | awk '{print $1}')"
    printf 'SAME_BINARY\tyes\tenv-only MRT_STICKY_MINOR 0 vs 1\n'
} > "$out/inventory.tsv"

{
    cat <<EOF
# PREREGISTERED protocol (frozen before stop window)
MODE_DEFAULT=$mode
CORPUS=${pkgs[*]}
SCHEDULE=interleaved_same_window (ON then OFF per pair; pairs sequential)
PAIRS=$pairs
ENDPOINTS=wall_s (compile total wall); gc_us (sum of total gc time lines, separate)
SECONDARY=minor_count,major_count,maxrss_kb,product_sha256
PREREG_CRITERION_CLEAR_IMPROVEMENT:
  primary: median(wall_OFF)/median(wall_ON)-1 >= 0.05
           AND all rounds VALID (rc=0)
           AND product_sha256 identical across ON/OFF when both complete
           AND ON minor>0 and OFF minor=0 (mode actually switched)
  secondary_gc: report median(gc_us) separately; do not mix with wall criterion
  noise: harnessfix sticky=0 sema N=8 mean=1295.52s sample_sd=30.08s cv=2.322%
POWER_N (two-sample independent, alpha=0.05 two-sided, power=0.80, cv=2.322%):
  POWER_N_for_5pct=4
  POWER_N_for_10pct=1
  POWER_N_for_20pct=1
  planned_stopwindow_pairs=5 (covers 5% with margin; floor N>=4 for any 5% claim)
DISCLAIMER: dryrun numbers are NOT performance conclusions.
EOF
} > "$out/protocol.txt"

env LD_LIBRARY_PATH="$ld_path" ldd "$cjc" > "$out/cjc.ldd"
grep -q -F "$runtime_so" "$out/cjc.ldd" || die 'cjc does not resolve pinned runtime'
grep '^MemAvailable:' /proc/meminfo > "$out/mem-before.txt"
df -h /root > "$out/disk-before.txt"
date -Iseconds > "$out/started_at.txt"

run_one()
{
    local pkg=$1 arm=$2 pair=$3 sticky=$4
    local expected_mode
    if [[ "$sticky" == 1 ]]; then expected_mode='on(default)'; else expected_mode='off(env)'; fi

    local run_dir="$out/${pkg}-${arm}-p$(printf '%02d' "$pair")"
    local time_file="$run_dir/time.txt"
    local runtime_log="$run_dir/runtime.log"
    local report_log="$run_dir/report.log"
    local product="$run_dir/out.a"
    mkdir -p "$run_dir"

    printf 'nice -n 12 taskset -c %q timeout %q env MRT_STICKY_MINOR=%q cjc --package %q ... -j %q\n' \
        "$cores" "$round_timeout" "$sticky" "$pkg_root/$pkg/src" "$parallel_j" > "$run_dir/command.txt"

    local launcher_pid rc=0 observed_pid='' actual_exe='UNOBSERVED' actual_sha='UNOBSERVED'
    (
        exec nice -n 12 taskset -c "$cores" /usr/bin/time \
            -f 'wall_s=%e user_s=%U sys_s=%S maxrss_kb=%M exit=%x' -o "$time_file" \
            timeout "$round_timeout" env \
            -u MRT_GC_STRESS -u MRT_GC_STRESS_MINOR -u MRT_STICKY_MINOR_FORCE_SLOW_PATH -u MRT_STICKY_FORCE_SLOW \
            PATH="$sdk/bin:$sdk/tools/bin:$sdk/third_party/llvm/bin:$PATH" \
            CANGJIE_HOME="$sdk" \
            LD_LIBRARY_PATH="$ld_path" \
            cjHeapSize=24GB \
            MRT_STICKY_MINOR="$sticky" \
            MRT_REPORT="$report_log" \
            MRT_LOG_PATH="$runtime_log" \
            MRT_LOG_LEVEL=i \
            "$cjc" \
            --package "$pkg_root/$pkg/src" \
            --module-name cjcj \
            --import-path "$import_path" \
            --output-type=staticlib \
            -O2 -j "$parallel_j" \
            -o "$product" \
            > "$run_dir/stdout.log" 2> "$run_dir/stderr.log"
    ) &
    launcher_pid=$!

    local attempt frontier child exe
    for attempt in $(seq 1 200); do
        frontier=$launcher_pid
        for _ in 1 2 3 4 5; do
            [[ -r "/proc/$frontier/task/$frontier/children" ]] || break
            child=$(awk '{print $1}' "/proc/$frontier/task/$frontier/children" 2>/dev/null || true)
            [[ -n "$child" ]] || break
            frontier=$child
            exe=$(readlink "/proc/$frontier/exe" 2>/dev/null || true)
            if [[ "$exe" == "$cjc" ]]; then
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

    local wall_s maxrss_kb minor major total_gc gc_us gc_pct
    local product_sha cjc_sha rt_sha mode_line cpus_allowed validity
    wall_s=$(sed -nE 's/.*wall_s=([^ ]+).*/\1/p' "$time_file" 2>/dev/null || true)
    maxrss_kb=$(sed -nE 's/.*maxrss_kb=([^ ]+).*/\1/p' "$time_file" 2>/dev/null || true)
    minor=$(count_matches '\[StickyMinor\] run=' "$report_log")
    total_gc=$(count_matches 'total gc time: [0-9,]+ us, collection rate' "$report_log")
    major=$((total_gc - minor))
    ((major >= 0)) || major=0
    gc_us=$(sed -nE 's/.*total gc time: ([0-9,]+) us, collection rate.*/\1/p' "$report_log" \
        | tr -d ',' | awk '{ total += $1 } END { printf "%.0f\n", total + 0 }')
    gc_pct=$(awk -v gc="$gc_us" -v wall="${wall_s:-0}" \
        'BEGIN { if (wall > 0) printf "%.6f", gc / (wall * 10000); else print "nan" }')
    if [[ -f "$product" ]]; then
        product_sha=$(sha256sum "$product" | awk '{print $1}')
    else
        product_sha='NONE'
    fi
    cjc_sha=$(sha256sum "$cjc" | awk '{print $1}')
    rt_sha=$(sha256sum "$runtime_so" | awk '{print $1}')
    mode_line=$(sed -nE 's/.*sticky minor: (.*)$/\1/p' "$runtime_log" 2>/dev/null | tail -n1)
    cpus_allowed=$(sed -nE 's/^Cpus_allowed_list:[[:space:]]*//p' "$run_dir/cpus-allowed.txt" 2>/dev/null || true)

    validity=VALID
    [[ "$rc" == 0 ]] || validity="INVALID_RC_$rc"
    [[ "$actual_sha" == "$cjc_sha" || "$actual_sha" == UNOBSERVED ]] || validity='INVALID_EXE_SHA'
    if [[ -f "$run_dir/environ.txt" ]] && ! grep -q -x -F 'UNOBSERVED' "$run_dir/environ.txt" 2>/dev/null; then
        grep -q -x -F "MRT_STICKY_MINOR=$sticky" "$run_dir/environ.txt" 2>/dev/null || validity='INVALID_ENV'
        grep -q -x -F 'MRT_STICKY_MINOR_FORCE_SLOW_PATH=' "$run_dir/environ.txt" 2>/dev/null && validity='INVALID_FORCE_SLOW'
    fi
    if [[ -f "$run_dir/maps.txt" ]] && ! grep -q -x -F 'UNOBSERVED' "$run_dir/maps.txt" 2>/dev/null; then
        grep -q -F "$runtime_so" "$run_dir/maps.txt" 2>/dev/null || validity='INVALID_RUNTIME_MAP'
    fi
    if [[ -n "$cpus_allowed" && "$cpus_allowed" != UNOBSERVED && "$cpus_allowed" != MISSING ]]; then
        [[ "$cpus_allowed" == "$cores" ]] || validity='INVALID_CPUSET'
    fi
    if [[ -n "$mode_line" ]]; then
        [[ "$mode_line" == "$expected_mode" ]] || validity='INVALID_MODE'
    fi
    if [[ "$arm" == ON && "$rc" == 0 && "$minor" == 0 ]]; then
        validity='INVALID_ZERO_YOUNG'
    elif [[ "$arm" == OFF && "$minor" != 0 ]]; then
        validity='INVALID_OFF_YOUNG'
    fi

    # drop large product after hashing to save disk
    if [[ -f "$product" ]]; then
        rm -f "$product"
    fi

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$mode" "$pkg" "$arm" "$pair" "$sticky" "$rc" "$validity" "${wall_s:-MISSING}" \
        "$gc_us" "$gc_pct" "$minor" "$major" "${maxrss_kb:-MISSING}" "$product_sha" \
        "$cjc_sha" "$rt_sha" "${mode_line:-MISSING}" "$actual_exe" \
        "${cpus_allowed:-MISSING}" "$run_dir" | tee -a "$results"
}

for ((pair=1; pair<=pairs; ++pair)); do
    for pkg in "${pkgs[@]}"; do
        run_one "$pkg" ON "$pair" 1
        run_one "$pkg" OFF "$pair" 0
    done
done

grep '^MemAvailable:' /proc/meminfo > "$out/mem-after.txt"
df -h /root > "$out/disk-after.txt"
date -Iseconds > "$out/finished_at.txt"

# summary: apparatus only; never claim performance from dryrun
invalid=$(awk -F '\t' 'NR > 1 && $7 != "VALID" { n++ } END { print n + 0 }' "$results")
on_young=$(awk -F '\t' 'NR > 1 && $3 == "ON" { s += $11 } END { print s + 0 }' "$results")
off_young=$(awk -F '\t' 'NR > 1 && $3 == "OFF" { s += $11 } END { print s + 0 }' "$results")
cjc_unique=$(awk -F '\t' 'NR > 1 { print $15 }' "$results" | sort -u | wc -l)
rt_unique=$(awk -F '\t' 'NR > 1 { print $16 }' "$results" | sort -u | wc -l)
on_ok=$(awk -F '\t' 'NR > 1 && $3 == "ON" && $7 == "VALID" { n++ } END { print n + 0 }' "$results")
off_ok=$(awk -F '\t' 'NR > 1 && $3 == "OFF" && $7 == "VALID" { n++ } END { print n + 0 }' "$results")
flow=ok
[[ "$invalid" == 0 && "$on_ok" -ge 1 && "$off_ok" -ge 1 && "$cjc_unique" == 1 && "$rt_unique" == 1 ]] || flow=fail
if [[ "$on_young" -le 0 || "$off_young" != 0 ]]; then
    flow=fail
fi

{
    printf 'MODE=%s\n' "$mode"
    printf 'SAME_BINARY=yes\n'
    printf 'CJC_SHA=%s\n' "$(sha256sum "$cjc" | awk '{print $1}')"
    printf 'RT_SHA=%s\n' "$(sha256sum "$runtime_so" | awk '{print $1}')"
    printf 'CORPUS=%s\n' "${pkgs[*]}"
    printf 'PAIRS=%s\n' "$pairs"
    printf 'ON_YOUNG_TOTAL=%s OFF_YOUNG_TOTAL=%s\n' "$on_young" "$off_young"
    printf 'ON_OK=%s OFF_OK=%s INVALID=%s\n' "$on_ok" "$off_ok" "$invalid"
    printf 'CJC_UNIQUE=%s RT_UNIQUE=%s\n' "$cjc_unique" "$rt_unique"
    printf 'FLOW=%s\n' "$flow"
    printf 'DISCLAIMER=dryrun_numbers_are_NOT_performance_conclusions\n'
    if [[ "$mode" == dryrun ]]; then
        printf 'DRYRUN_OK=%s\n' "$([[ "$flow" == ok ]] && echo yes || echo no)"
    fi
} | tee "$out/summary.txt"

[[ "$flow" == ok ]]
