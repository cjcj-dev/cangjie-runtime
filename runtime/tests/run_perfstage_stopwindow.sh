#!/usr/bin/env bash
# Interleaved sticky-minor measurement (same pinned binary; arms = env only).
# Absolute wall under schedutil is not publishable; ratios survive same-window interleave
# when per-arm mean CPU frequency delta <=5%. Dryrun numbers are apparatus-only unless
# mode=measure and FREQ balance passes.
set -euo pipefail

usage()
{
    cat <<'EOF'
Usage: run_perfstage_stopwindow.sh --out DIR [options]

Options:
  --out DIR              evidence directory (required, must be empty)
  --mode MODE            dryrun | measure | stopwindow (default: dryrun)
  --cores RANGE          CPU range (default: dryrun/measure 48-63, stopwindow 0-191)
  --pairs N              interleaved OFF/ON pairs (default: dryrun 1, measure/stopwindow 5)
  --timeout SEC          per-round timeout seconds
  --marker PREFIX        MEASURE_ACTIVE line must start with this (or exact)
  --cjc FILE             pinned compiler binary
  --runtime-so FILE      pinned libcangjie-runtime.so
  --pkg-root DIR         packages root containing <pkg>/src
  --import-path DIR      --import-path for cjc
  --sdk DIR              CANGJIE_HOME / toolchain root
  --corpus LIST          comma packages (default: sema)
  --j N                  compiler -j (default: 16)

Schedule (hard): for each pair p=1..N, for each pkg:
  OFF (MRT_STICKY_MINOR=0) then ON (MRT_STICKY_MINOR=1).
Never segment all-OFF then all-ON.

Endpoints (separate): wall_s ; gc_us (sum of total gc time lines).
Each round records loadavg_1m and mean MHz of cores in --cores (start+end).
FREQ arm-mean delta >5% => batch INVALID_FREQ_IMBALANCE (ratios still reported).
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

# Mean MHz for logical CPUs in RANGE start-end from /proc/cpuinfo.
sample_freq_mean_mhz()
{
    local range=$1
    local start=${range%-*} end=${range#*-}
    python3 - "$start" "$end" <<'PY'
import sys
start, end = map(int, sys.argv[1:3])
cpus = set(range(start, end + 1))
mhz = []
cur = None
with open("/proc/cpuinfo") as f:
    for line in f:
        if line.startswith("processor"):
            cur = int(line.split(":")[1])
        elif line.startswith("cpu MHz") and cur in cpus:
            mhz.append(float(line.split(":")[1]))
            cur = None
if not mhz:
    print("nan")
else:
    print(f"{sum(mhz)/len(mhz):.3f}")
PY
}

sample_loadavg_1m()
{
    awk '{print $1}' /proc/loadavg
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
    measure)
        [[ -n "$cores" ]] || cores=48-63
        [[ -n "$pairs" ]] || pairs=5
        [[ -n "$round_timeout" ]] || round_timeout=2400
        [[ -n "$marker" ]] || marker='perfstage cores=48-63 KIND=build'
        ;;
    stopwindow)
        [[ -n "$cores" ]] || cores=0-191
        [[ -n "$pairs" ]] || pairs=5
        [[ -n "$round_timeout" ]] || round_timeout=3600
        [[ -n "$marker" ]] || marker='perfstage cores=0-191 KIND=measure STOP_WINDOW=1'
        ;;
    *) die "--mode must be dryrun|measure|stopwindow" ;;
esac

[[ "$cores" =~ ^[0-9]+-[0-9]+$ ]] || die '--cores must be START-END'
[[ "$pairs" =~ ^[0-9]+$ && "$pairs" -ge 1 ]] || die '--pairs must be >=1'
[[ "$round_timeout" =~ ^[0-9]+$ && "$round_timeout" -gt 0 ]] || die '--timeout must be positive'
[[ "$parallel_j" =~ ^[0-9]+$ && "$parallel_j" -ge 1 ]] || die '--j must be positive'

# marker: exact line OR line begins with marker (allows trailing since=)
if ! grep -q -x -F "$marker" /dev/shm/MEASURE_ACTIVE 2>/dev/null \
    && ! grep -q -F "$marker" /dev/shm/MEASURE_ACTIVE 2>/dev/null; then
    die "missing marker containing: $marker"
fi

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
printf 'mode\tpkg\tarm\tpair\tsticky\trc\tvalidity\twall_s\tgc_us\tgc_pct\tminor\tmajor\tmaxrss_kb\tproduct_sha256\tcjc_sha256\truntime_sha256\tmode_line\tactual_exe\tcpus_allowed\tload_start\tload_end\tfreq_start_mhz\tfreq_end_mhz\tfreq_mean_mhz\trun_dir\n' > "$results"

{
    printf 'role\tpath\tsha256\tsticky_consumer_lines\n'
    printf 'cjc\t%s\t%s\t%s\n' "$cjc" "$(sha256sum "$cjc" | awk '{print $1}')" "$consumers"
    printf 'runtime\t%s\t%s\t-\n' "$runtime_so" "$(sha256sum "$runtime_so" | awk '{print $1}')"
    printf 'SAME_BINARY\tyes\tenv-only MRT_STICKY_MINOR 0 vs 1\n'
} > "$out/inventory.tsv"

{
    cat <<EOF
# PREREGISTERED protocol (frozen before seeing arm ratios)
MODE_DEFAULT=$mode
CORPUS=${pkgs[*]}
SCHEDULE=interleaved_same_window HARD: OFF then ON per pair; pairs sequential; never segment arms
PAIRS=$pairs
CORES=$cores
ENDPOINTS=wall_s (compile total wall); gc_us (sum total gc time, separate column)
SECONDARY=minor_count,major_count,maxrss_kb,product_sha256,freq_mean_mhz,loadavg_1m
PREREG_CRITERION_CLEAR_IMPROVEMENT (ratios only; absolute wall not publishable outside stopwindow):
  primary_ratio: median(wall_OFF)/median(wall_ON) - 1 >= 0.05
                 AND all rounds VALID (rc=0)
                 AND product_sha256 identical across ON/OFF when both complete
                 AND ON minor>0 and OFF minor=0
                 AND FREQ_MEAN_OFF vs ON relative |delta| <= 5%
  secondary_gc: report median(gc_us) separately; never mix into wall criterion
  absolute wall: only stopwindow mode may publish absolute seconds for release notes
POWER baseline seed (harnessfix sticky=0 sema N=8): mean=1295.52s sd=30.08s cv=2.322%
  POWER_N two-sample independent alpha=0.05 power=0.80:
  POWER_N_for_5pct=4 POWER_N_for_10pct=1 POWER_N_for_20pct=1
  planned pairs=5 (margin over 5% power)
FREQ_INVALIDATE_IF_ARM_MEAN_DELTA_GT=5%
DISCLAIMER: dryrun numbers are NOT performance conclusions.
ABSOLUTE_WALL_DISCLAIMER: non-stopwindow absolute wall is under schedutil (core freq can differ ~2.76x); ratios only when FREQ balance holds.
EOF
} > "$out/protocol.txt"

env LD_LIBRARY_PATH="$ld_path" ldd "$cjc" > "$out/cjc.ldd"
grep -q -F "$runtime_so" "$out/cjc.ldd" || die 'cjc does not resolve pinned runtime'
grep '^MemAvailable:' /proc/meminfo > "$out/mem-before.txt"
df -h /root > "$out/disk-before.txt"
date -Iseconds > "$out/started_at.txt"
# one-shot governor snapshot (structural context)
{
    echo "governor=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo missing)"
    echo "scaling_min=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq 2>/dev/null || echo missing)"
    echo "scaling_max=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq 2>/dev/null || echo missing)"
    sample_freq_mean_mhz "$cores" | awk '{print "batch_start_freq_mean_mhz="$1}'
    sample_loadavg_1m | awk '{print "batch_start_loadavg_1m="$1}'
} > "$out/machine_context.txt"

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

    local load_start freq_start
    load_start=$(sample_loadavg_1m)
    freq_start=$(sample_freq_mean_mhz "$cores")
    printf '%s\n' "$load_start" > "$run_dir/load_start.txt"
    printf '%s\n' "$freq_start" > "$run_dir/freq_start_mhz.txt"

    printf 'nice -n 12 taskset -c %q timeout %q env MRT_STICKY_MINOR=%q cjc package=%q -j %q\n' \
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

    local load_end freq_end freq_mean
    load_end=$(sample_loadavg_1m)
    freq_end=$(sample_freq_mean_mhz "$cores")
    freq_mean=$(awk -v a="$freq_start" -v b="$freq_end" 'BEGIN {
        if (a == "nan" || b == "nan") print "nan";
        else printf "%.3f", (a+b)/2
    }')
    printf '%s\n' "$load_end" > "$run_dir/load_end.txt"
    printf '%s\n' "$freq_end" > "$run_dir/freq_end_mhz.txt"
    printf '%s\n' "$freq_mean" > "$run_dir/freq_mean_mhz.txt"

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

    if [[ -f "$product" ]]; then
        rm -f "$product"
    fi

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$mode" "$pkg" "$arm" "$pair" "$sticky" "$rc" "$validity" "${wall_s:-MISSING}" \
        "$gc_us" "$gc_pct" "$minor" "$major" "${maxrss_kb:-MISSING}" "$product_sha" \
        "$cjc_sha" "$rt_sha" "${mode_line:-MISSING}" "$actual_exe" \
        "${cpus_allowed:-MISSING}" "$load_start" "$load_end" "$freq_start" "$freq_end" \
        "$freq_mean" "$run_dir" | tee -a "$results"
}

# HARD interleave: OFF then ON per pair
for ((pair=1; pair<=pairs; ++pair)); do
    for pkg in "${pkgs[@]}"; do
        run_one "$pkg" OFF "$pair" 0
        run_one "$pkg" ON "$pair" 1
    done
done

grep '^MemAvailable:' /proc/meminfo > "$out/mem-after.txt"
df -h /root > "$out/disk-after.txt"
date -Iseconds > "$out/finished_at.txt"

python3 - "$results" "$out/summary.txt" "$mode" <<'PY'
import statistics, sys, math
from pathlib import Path
results_path, summary_path, mode = sys.argv[1:4]
rows = []
with open(results_path) as f:
    header = f.readline().rstrip("\n").split("\t")
    for line in f:
        if not line.strip():
            continue
        d = dict(zip(header, line.rstrip("\n").split("\t")))
        rows.append(d)

def fnum(x):
    try:
        return float(x)
    except Exception:
        return None

invalid = sum(1 for r in rows if r.get("validity") != "VALID")
off = [r for r in rows if r.get("arm") == "OFF"]
on = [r for r in rows if r.get("arm") == "ON"]
off_ok = [r for r in off if r.get("validity") == "VALID"]
on_ok = [r for r in on if r.get("validity") == "VALID"]
off_young = sum(int(r.get("minor") or 0) for r in off)
on_young = sum(int(r.get("minor") or 0) for r in on)
cjc_unique = len({r.get("cjc_sha256") for r in rows})
rt_unique = len({r.get("runtime_sha256") for r in rows})

def arm_mean(rs, key):
    xs = [fnum(r.get(key)) for r in rs]
    xs = [x for x in xs if x is not None and not math.isnan(x)]
    return statistics.mean(xs) if xs else float("nan")

freq_off = arm_mean(off, "freq_mean_mhz")
freq_on = arm_mean(on, "freq_mean_mhz")
load_off = arm_mean(off, "load_start")  # start load as proxy
load_on = arm_mean(on, "load_start")
if freq_off and freq_on and not math.isnan(freq_off) and not math.isnan(freq_on) and freq_off > 0:
    freq_delta_pct = abs(freq_off - freq_on) / ((freq_off + freq_on) / 2) * 100
else:
    freq_delta_pct = float("nan")

# observed CV from OFF walls if N>=3 else from all VALID walls
off_walls = [fnum(r["wall_s"]) for r in off_ok if fnum(r.get("wall_s")) is not None]
on_walls = [fnum(r["wall_s"]) for r in on_ok if fnum(r.get("wall_s")) is not None]
all_walls = off_walls + on_walls
cv_source = "OFF"
cv_walls = off_walls
if len(cv_walls) < 3:
    cv_source = "ALL_VALID"
    cv_walls = all_walls
if len(cv_walls) >= 2:
    mean = statistics.mean(cv_walls)
    sd = statistics.stdev(cv_walls)
    cv = sd / mean if mean else float("nan")
else:
    mean = sd = cv = float("nan")

# power N from observed CV (fallback harnessfix cv=0.02322)
use_cv = cv if cv == cv and cv > 0 else 0.02322
za, zb = 1.96, 0.8416
def power_n(pct):
    return math.ceil(2 * (za + zb) ** 2 * (use_cv / pct) ** 2)

# apparatus flow: valid rounds + young direction + single binary
apparatus = "ok"
if invalid or not off_ok or not on_ok or cjc_unique != 1 or rt_unique != 1:
    apparatus = "fail"
if on_young <= 0 or off_young != 0:
    apparatus = "fail"
freq_ok = freq_delta_pct == freq_delta_pct and freq_delta_pct <= 5.0
freq_tag = "FREQ_BALANCED" if freq_ok else "INVALID_FREQ_IMBALANCE"
# dryrun: apparatus only (FREQ reported, not gating). measure/stopwindow: FREQ gates ratio.
if mode == "dryrun":
    flow = apparatus
else:
    flow = "ok" if apparatus == "ok" and freq_ok else "fail"

ratio = float("nan")
if off_walls and on_walls:
    med_off = statistics.median(off_walls)
    med_on = statistics.median(on_walls)
    if med_on > 0:
        ratio = med_off / med_on - 1.0

# seed power from harnessfix if N_walls < 3
seed_cv = 0.02322
power_cv = use_cv if len(cv_walls) >= 3 else seed_cv
def power_n2(pct, c=power_cv):
    return math.ceil(2 * (za + zb) ** 2 * (c / pct) ** 2)

lines = []
lines.append(f"MODE={mode}")
lines.append("SAME_BINARY=yes")
lines.append(f"APPARATUS={apparatus}")
lines.append(f"FLOW={flow}")
lines.append(f"INVALID={invalid}")
lines.append(f"OFF_OK={len(off_ok)} ON_OK={len(on_ok)}")
lines.append(f"OFF_YOUNG_TOTAL={off_young} ON_YOUNG_TOTAL={on_young}")
lines.append(f"CJC_UNIQUE={cjc_unique} RT_UNIQUE={rt_unique}")
lines.append(f"FREQ_MEAN_OFF_{freq_off:.3f}_ON_{freq_on:.3f}_DELTA_{freq_delta_pct:.3f}%")
lines.append(f"LOAD_MEAN_OFF_{load_off:.3f}_ON_{load_on:.3f}")
lines.append(f"FREQ_TAG={freq_tag}")
cv_pct = cv * 100 if cv == cv else float("nan")
mean_s = mean if mean == mean else float("nan")
sd_s = sd if sd == sd else float("nan")
lines.append(f"OBSERVED_CV_{cv_pct:.3f}%_SOURCE_{cv_source}_N_{len(cv_walls)}_MEAN_{mean_s:.3f}_SD_{sd_s:.3f}")
lines.append(f"POWER_N_for_5pct_{power_n2(0.05)}_10pct_{power_n2(0.10)}_20pct_{power_n2(0.20)}_CV_USED_{power_cv:.5f}")
lines.append("DISCLAIMER=dryrun_numbers_are_NOT_performance_conclusions")
lines.append("ABSOLUTE_WALL=non-stopwindow under schedutil; not for release absolute claims")
if mode == "dryrun":
    lines.append(f"DRYRUN_OK={'yes' if apparatus=='ok' else 'no'}")
    lines.append("RATIO_CLAIM=forbidden_in_dryrun")
    if not freq_ok:
        lines.append("NOTE=FREQ_IMBALANCE_observed_would_void_ratio_batch")
elif mode in ("measure", "stopwindow"):
    if apparatus == "ok" and freq_ok and ratio == ratio:
        clear = "yes" if ratio >= 0.05 else "no"
        lines.append(f"RATIO_MEDIAN_OFF_over_ON_minus1={ratio:.6f}")
        lines.append(f"CLEAR_IMPROVEMENT_PREREG={clear}")
    else:
        lines.append("RATIO_CLAIM=blocked_apparatus_or_freq")
# final absolute stopwindow: planned 5 pairs * 2 arms * ~22min + overhead
est_min = 5 * 2 * 22 + 10
lines.append(f"STOP_WINDOW_FOR_FINAL_ONLY_{est_min}")
text = "\n".join(lines) + "\n"
Path(summary_path).write_text(text)
print(text, end="")
sys.exit(0 if flow == "ok" else 1)
PY
