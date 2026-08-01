#!/usr/bin/env bash
# Three-arm interleaved sticky-minor measurement (same pinned binary).
# A=OFF minor, B=MINOR no-evac (ship default), C=EVAC (threshold>0).
# Absolute wall under schedutil is not publishable; ratios need FREQ delta<=5%.
# Dryrun numbers are apparatus-only — never performance conclusions.
set -euo pipefail

usage()
{
    cat <<'EOF'
Usage: run_perfstage_stopwindow.sh --out DIR [options]

Arms (same binary; env only):
  A OFF   MRT_STICKY_MINOR=0
  B MINOR MRT_STICKY_MINOR=1 MRT_STICKY_EVAC_THRESHOLD=0   (ship default)
  C EVAC  MRT_STICKY_MINOR=1 MRT_STICKY_EVAC_THRESHOLD=<n>

Schedule (hard): for each pair, for each pkg: A then B then C.
Never segment arms. Endpoints: wall_s and gc_us separate; STW pause from
"sticky minor stw time" lines. Ratios of interest: B/A and C/B.

Options:
  --out DIR --mode dryrun|measure|stopwindow
  --cores RANGE --pairs N --timeout SEC --marker TEXT
  --cjc FILE --runtime-so FILE --pkg-root DIR --import-path DIR --sdk DIR
  --corpus LIST --j N
  --evac-threshold N   (default 25; also run twin with --evac-threshold2)
  --evac-threshold2 N  (optional second C level, default 50; 0 disables twin)
  --evac-max-regions N (default 8)
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
print("nan" if not mhz else f"{sum(mhz)/len(mhz):.3f}")
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
evac_threshold=25
evac_threshold2=50
evac_max_regions=8

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
        --evac-threshold) evac_threshold=${2:-}; shift 2 ;;
        --evac-threshold2) evac_threshold2=${2:-}; shift 2 ;;
        --evac-max-regions) evac_max_regions=${2:-}; shift 2 ;;
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
[[ "$evac_threshold" =~ ^[0-9]+$ && "$evac_threshold" -ge 1 && "$evac_threshold" -le 100 ]] || \
    die '--evac-threshold must be 1..100'
[[ "$evac_threshold2" =~ ^[0-9]+$ && "$evac_threshold2" -le 100 ]] || die '--evac-threshold2 must be 0..100'
[[ "$evac_max_regions" =~ ^[0-9]+$ && "$evac_max_regions" -ge 1 ]] || die '--evac-max-regions must be >=1'

if ! grep -q -x -F "$marker" /dev/shm/MEASURE_ACTIVE 2>/dev/null \
    && ! grep -q -F "$marker" /dev/shm/MEASURE_ACTIVE 2>/dev/null; then
    die "missing marker containing: $marker"
fi

for file in "$cjc" "$runtime_so"; do
    [[ -f "$file" && -x "$file" ]] || die "missing executable: $file"
done
[[ -d "$pkg_root" && -d "$import_path" && -d "$sdk" ]] || die 'missing pkg-root/import-path/sdk'
[[ -x "$sdk/third_party/llvm/bin/opt" && -x "$sdk/third_party/llvm/bin/llc" ]] || \
    die "sdk missing third_party/llvm/bin/{opt,llc}"

IFS=',' read -r -a pkgs <<< "$corpus"
((${#pkgs[@]} > 0)) || die 'empty --corpus'
for pkg in "${pkgs[@]}"; do
    [[ -d "$pkg_root/$pkg/src" ]] || die "missing package src: $pkg_root/$pkg/src"
done

consumers=$(readelf -Ws "$cjc" 2>/dev/null | grep -c '__cj_sticky_logged_base' || true)
[[ "$consumers" -gt 0 ]] || die "compiler has no sticky consumer"

runtime_dir=$(dirname "$runtime_so")
ld_path="$runtime_dir:$sdk/runtime/lib/linux_x86_64_cjnative:$sdk/third_party/llvm/lib:$sdk/tools/lib"

mkdir -p "$out"
[[ -z $(find "$out" -mindepth 1 -maxdepth 1 -print -quit 2>/dev/null) ]] || \
    die "output directory is not empty: $out"

results="$out/results.tsv"
printf 'mode\tpkg\tarm\tpair\tsticky\tevac_threshold\trc\tvalidity\twall_s\tgc_us\tgc_pct\tminor\tmajor\tevac_events\tpause_us_sum\tpause_n\tmaxrss_kb\tproduct_sha256\tcjc_sha256\truntime_sha256\tmode_line\tactual_exe\tcpus_allowed\tload_start\tload_end\tfreq_start_mhz\tfreq_end_mhz\tfreq_mean_mhz\trun_dir\n' > "$results"

{
    printf 'role\tpath\tsha256\tsticky_consumer_lines\n'
    printf 'cjc\t%s\t%s\t%s\n' "$cjc" "$(sha256sum "$cjc" | awk '{print $1}')" "$consumers"
    printf 'runtime\t%s\t%s\t-\n' "$runtime_so" "$(sha256sum "$runtime_so" | awk '{print $1}')"
    printf 'SAME_BINARY\tyes\tenv-only three arms A/B/C\n'
} > "$out/inventory.tsv"

{
    cat <<EOF
# PREREGISTERED three-arm protocol (frozen before ratios)
MODE=$mode
CORPUS=${pkgs[*]}
SCHEDULE=interleaved_same_window HARD: A then B then C per pair; never segment
PAIRS=$pairs CORES=$cores
ARMS:
  A_OFF   MRT_STICKY_MINOR=0
  B_MINOR MRT_STICKY_MINOR=1 MRT_STICKY_EVAC_THRESHOLD=0   # ship default: minor, no evacuation
  C_EVAC  MRT_STICKY_MINOR=1 MRT_STICKY_EVAC_THRESHOLD={$evac_threshold,$evac_threshold2} MRT_STICKY_EVAC_MAX_REGIONS=$evac_max_regions
EVAC_THRESHOLD_SEMANTICS=percent_live_ceiling_0_to_100
  WCollector.cpp:1339 liveBytes*100 > regionBytes*threshold => skip region
  threshold==0 disables evacuation entirely (evacuationEnabled false)
  clamped to 100 in StickyLog.cpp:151
ENDPOINTS=wall_s; gc_us (separate); pause_us_sum from "sticky minor stw time"
RATIOS_OF_INTEREST=B_over_A (minor alone); C_over_B (evacuation increment)  # C/B is release-critical
PREREG_CRITERION_CLEAR_IMPROVEMENT (ratios; absolute wall only in stopwindow):
  for each ratio R in {B/A, C/B}:
    median(wall_base)/median(wall_cand)-1 >= 0.05
    AND all rounds VALID
    AND product_sha256 equal across completing arms
    AND young: A minor=0; B,C minor>0
    AND C has evacuation events >0 when claiming C/B
    AND FREQ arm-mean relative |delta| across A/B/C pairwise <=5%
  secondary: report PAUSE_p50/p90 per arm (must not hide pause cost)
POWER seed harnessfix sticky=0 sema N=8 cv=2.322%:
  POWER_N_for_5pct_4_10pct_1_20pct_1 planned_pairs=5
FREQ_INVALIDATE_IF_ARM_MEAN_DELTA_GT=5%
DISCLAIMER=dryrun_numbers_are_NOT_performance_conclusions
EOF
} > "$out/protocol.txt"

env LD_LIBRARY_PATH="$ld_path" ldd "$cjc" > "$out/cjc.ldd"
grep -q -F "$runtime_so" "$out/cjc.ldd" || die 'cjc does not resolve pinned runtime'
grep '^MemAvailable:' /proc/meminfo > "$out/mem-before.txt"
df -h /root > "$out/disk-before.txt"
date -Iseconds > "$out/started_at.txt"
{
    echo "governor=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo missing)"
    sample_freq_mean_mhz "$cores" | awk '{print "batch_start_freq_mean_mhz="$1}'
    sample_loadavg_1m | awk '{print "batch_start_loadavg_1m="$1}'
} > "$out/machine_context.txt"

# build arm list: A B C@t1 [C@t2]
arm_specs=()
arm_specs+=("A:0:0")
arm_specs+=("B:1:0")
arm_specs+=("C${evac_threshold}:1:${evac_threshold}")
if [[ "$evac_threshold2" -ge 1 && "$evac_threshold2" != "$evac_threshold" ]]; then
    arm_specs+=("C${evac_threshold2}:1:${evac_threshold2}")
fi

run_one()
{
    local pkg=$1 arm=$2 pair=$3 sticky=$4 evac_th=$5
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
    printf 'arm=%s sticky=%s evac_threshold=%s max_regions=%s\n' \
        "$arm" "$sticky" "$evac_th" "$evac_max_regions" > "$run_dir/arm_env.txt"

    local launcher_pid rc=0 observed_pid='' actual_exe='UNOBSERVED' actual_sha='UNOBSERVED'
    (
        exec nice -n 12 taskset -c "$cores" /usr/bin/time \
            -f 'wall_s=%e user_s=%U sys_s=%S maxrss_kb=%M exit=%x' -o "$time_file" \
            timeout "$round_timeout" env \
            -u MRT_GC_STRESS -u MRT_GC_STRESS_MINOR -u MRT_STICKY_MINOR_FORCE_SLOW_PATH -u MRT_STICKY_FORCE_SLOW \
            -u MRT_STICKY_EVAC_BITMAP_POSCTRL \
            PATH="$sdk/bin:$sdk/tools/bin:$sdk/third_party/llvm/bin:$PATH" \
            CANGJIE_HOME="$sdk" \
            LD_LIBRARY_PATH="$ld_path" \
            cjHeapSize=24GB \
            MRT_STICKY_MINOR="$sticky" \
            MRT_STICKY_EVAC_THRESHOLD="$evac_th" \
            MRT_STICKY_EVAC_MAX_REGIONS="$evac_max_regions" \
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
        if (a=="nan"||b=="nan") print "nan"; else printf "%.3f", (a+b)/2 }')
    printf '%s\n' "$load_end" > "$run_dir/load_end.txt"
    printf '%s\n' "$freq_end" > "$run_dir/freq_end_mhz.txt"
    printf '%s\n' "$freq_mean" > "$run_dir/freq_mean_mhz.txt"

    local wall_s maxrss_kb minor major total_gc gc_us gc_pct
    local product_sha cjc_sha rt_sha mode_line cpus_allowed validity
    local evac_events pause_us_sum pause_n
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
    evac_events=$(count_matches '\[StickyMinor\] evacuation regions=' "$report_log")
    # also count non-zero regions= lines as real work
    pause_us_sum=$(grep -a -E 'sticky minor stw time [0-9]+ us' "$runtime_log" 2>/dev/null \
        | sed -nE 's/.*sticky minor stw time ([0-9]+) us.*/\1/p' \
        | awk '{ total += $1 } END { printf "%.0f\n", total + 0 }')
    pause_n=$(grep -a -E -c 'sticky minor stw time [0-9]+ us' "$runtime_log" 2>/dev/null || true)
    [[ -n "$pause_n" ]] || pause_n=0
    if [[ -f "$product" ]]; then
        product_sha=$(sha256sum "$product" | awk '{print $1}')
        rm -f "$product"
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
        grep -q -x -F "MRT_STICKY_EVAC_THRESHOLD=$evac_th" "$run_dir/environ.txt" 2>/dev/null || validity='INVALID_EVAC_ENV'
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
    if [[ "$arm" == A && "$minor" != 0 ]]; then
        validity='INVALID_A_YOUNG'
    elif [[ "$arm" != A && "$rc" == 0 && "$minor" == 0 ]]; then
        validity='INVALID_ZERO_YOUNG'
    fi

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$mode" "$pkg" "$arm" "$pair" "$sticky" "$evac_th" "$rc" "$validity" "${wall_s:-MISSING}" \
        "$gc_us" "$gc_pct" "$minor" "$major" "$evac_events" "$pause_us_sum" "$pause_n" \
        "${maxrss_kb:-MISSING}" "$product_sha" "$cjc_sha" "$rt_sha" "${mode_line:-MISSING}" \
        "$actual_exe" "${cpus_allowed:-MISSING}" "$load_start" "$load_end" "$freq_start" \
        "$freq_end" "$freq_mean" "$run_dir" | tee -a "$results"
}

for ((pair=1; pair<=pairs; ++pair)); do
    for pkg in "${pkgs[@]}"; do
        for spec in "${arm_specs[@]}"; do
            IFS=':' read -r arm sticky evac_th <<< "$spec"
            run_one "$pkg" "$arm" "$pair" "$sticky" "$evac_th"
        done
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
        rows.append(dict(zip(header, line.rstrip("\n").split("\t"))))

def fnum(x):
    try:
        v = float(x)
        return v
    except Exception:
        return None

def arm_rows(name_prefix=None, exact=None):
    out = []
    for r in rows:
        a = r.get("arm") or ""
        if exact is not None and a == exact:
            out.append(r)
        elif name_prefix is not None and a.startswith(name_prefix):
            out.append(r)
    return out

invalid = sum(1 for r in rows if r.get("validity") != "VALID")
A = [r for r in rows if r.get("arm") == "A"]
B = [r for r in rows if r.get("arm") == "B"]
C = [r for r in rows if (r.get("arm") or "").startswith("C")]
Aok = [r for r in A if r.get("validity") == "VALID"]
Bok = [r for r in B if r.get("validity") == "VALID"]
Cok = [r for r in C if r.get("validity") == "VALID"]
A_young = sum(int(r.get("minor") or 0) for r in A)
B_young = sum(int(r.get("minor") or 0) for r in B)
C_young = sum(int(r.get("minor") or 0) for r in C)
C_evac = sum(int(r.get("evac_events") or 0) for r in C)
cjc_unique = len({r.get("cjc_sha256") for r in rows})
rt_unique = len({r.get("runtime_sha256") for r in rows})

def mean_key(rs, key):
    xs = [fnum(r.get(key)) for r in rs]
    xs = [x for x in xs if x is not None and x == x]
    return statistics.mean(xs) if xs else float("nan")

freq_A, freq_B, freq_C = mean_key(A, "freq_mean_mhz"), mean_key(B, "freq_mean_mhz"), mean_key(C, "freq_mean_mhz")
load_A, load_B, load_C = mean_key(A, "load_start"), mean_key(B, "load_start"), mean_key(C, "load_start")

def rel_delta(x, y):
    if x != x or y != y or (x + y) == 0:
        return float("nan")
    return abs(x - y) / ((x + y) / 2) * 100

deltas = [rel_delta(freq_A, freq_B), rel_delta(freq_B, freq_C), rel_delta(freq_A, freq_C)]
deltas = [d for d in deltas if d == d]
freq_delta_max = max(deltas) if deltas else float("nan")
freq_ok = freq_delta_max == freq_delta_max and freq_delta_max <= 5.0
freq_tag = "FREQ_BALANCED" if freq_ok else "INVALID_FREQ_IMBALANCE"

apparatus = "ok"
if invalid or not Aok or not Bok or not Cok or cjc_unique != 1 or rt_unique != 1:
    apparatus = "fail"
if A_young != 0 or B_young <= 0 or C_young <= 0:
    apparatus = "fail"
# C should show evacuation activity when threshold>0; soft for dryrun if log missing
if mode != "dryrun" and C_evac <= 0:
    apparatus = "fail"

if mode == "dryrun":
    flow = apparatus
else:
    flow = "ok" if apparatus == "ok" and freq_ok else "fail"

# power
seed_cv = 0.02322
A_walls = [fnum(r["wall_s"]) for r in Aok if fnum(r.get("wall_s")) is not None]
cv_walls = A_walls
cv_source = "A"
if len(cv_walls) < 3:
    cv_walls = [fnum(r["wall_s"]) for r in rows if r.get("validity") == "VALID" and fnum(r.get("wall_s")) is not None]
    cv_source = "ALL_VALID"
if len(cv_walls) >= 2:
    mean = statistics.mean(cv_walls)
    sd = statistics.stdev(cv_walls)
    cv = sd / mean if mean else float("nan")
else:
    mean = sd = cv = float("nan")
use_cv = cv if (cv == cv and len(cv_walls) >= 3 and cv > 0) else seed_cv
za, zb = 1.96, 0.8416
def power_n(pct, c=use_cv):
    return math.ceil(2 * (za + zb) ** 2 * (c / pct) ** 2)

def median_walls(rs):
    xs = [fnum(r["wall_s"]) for r in rs if r.get("validity") == "VALID" and fnum(r.get("wall_s")) is not None]
    return statistics.median(xs) if xs else float("nan")

def pause_pct(rs, p):
    xs = []
    for r in rs:
        # approximate per-pause mean if pause_n>0
        s = fnum(r.get("pause_us_sum"))
        n = fnum(r.get("pause_n"))
        if s is not None and n and n > 0:
            xs.append(s / n)
        elif s is not None and s > 0:
            xs.append(s)
    if not xs:
        return float("nan")
    xs = sorted(xs)
    i = int(round((p / 100) * (len(xs) - 1)))
    return xs[i]

med_A, med_B, med_C = median_walls(Aok), median_walls(Bok), median_walls(Cok)
ratio_BA = (med_A / med_B - 1.0) if (med_A == med_A and med_B and med_B > 0) else float("nan")
ratio_CB = (med_B / med_C - 1.0) if (med_B == med_B and med_C and med_C > 0) else float("nan")

lines = []
lines.append(f"MODE={mode}")
lines.append("SAME_BINARY=yes")
lines.append("ARMS=A_OFF,B_MINOR,C_EVAC")
lines.append(f"APPARATUS={apparatus}")
lines.append(f"FLOW={flow}")
lines.append(f"INVALID={invalid}")
lines.append(f"A_OK={len(Aok)} B_OK={len(Bok)} C_OK={len(Cok)}")
lines.append(f"A_YOUNG={A_young} B_YOUNG={B_young} C_YOUNG={C_young} C_EVAC_EVENTS={C_evac}")
lines.append(f"CJC_UNIQUE={cjc_unique} RT_UNIQUE={rt_unique}")
lines.append(f"FREQ_MEAN_A_{freq_A:.3f}_B_{freq_B:.3f}_C_{freq_C:.3f}_DELTA_MAX_{freq_delta_max:.3f}%")
lines.append(f"LOAD_MEAN_A_{load_A:.3f}_B_{load_B:.3f}_C_{load_C:.3f}")
lines.append(f"FREQ_TAG={freq_tag}")
lines.append(f"PAUSE_p50_A_{pause_pct(Aok,50):.0f}_B_{pause_pct(Bok,50):.0f}_C_{pause_pct(Cok,50):.0f}")
lines.append(f"PAUSE_p90_A_{pause_pct(Aok,90):.0f}_B_{pause_pct(Bok,90):.0f}_C_{pause_pct(Cok,90):.0f}")
cv_pct = cv * 100 if cv == cv else float("nan")
mean_s = mean if mean == mean else float("nan")
sd_s = sd if sd == sd else float("nan")
lines.append(f"OBSERVED_CV_{cv_pct:.3f}%_SOURCE_{cv_source}_N_{len(cv_walls)}_MEAN_{mean_s:.3f}_SD_{sd_s:.3f}")
lines.append(f"POWER_N_for_5pct_{power_n(0.05)}_10pct_{power_n(0.10)}_20pct_{power_n(0.20)}_CV_USED_{use_cv:.5f}")
lines.append("DISCLAIMER=dryrun_numbers_are_NOT_performance_conclusions")
lines.append("ABSOLUTE_WALL=non-stopwindow under schedutil; not for release absolute claims")
lines.append("EVAC_THRESHOLD_SEMANTICS=percent_live_ceiling_liveBytes*100_le_regionBytes*threshold")
if mode == "dryrun":
    lines.append(f"DRYRUN_OK={'yes' if apparatus=='ok' else 'no'}")
    lines.append("RATIO_CLAIM=forbidden_in_dryrun")
    if not freq_ok:
        lines.append("NOTE=FREQ_IMBALANCE_observed_would_void_ratio_batch")
else:
    if apparatus == "ok" and freq_ok:
        lines.append(f"RATIO_B_over_A_minus1={ratio_BA:.6f}")
        lines.append(f"RATIO_C_over_B_minus1={ratio_CB:.6f}")
        lines.append(f"CLEAR_IMPROVEMENT_BA={'yes' if ratio_BA>=0.05 else 'no'}")
        lines.append(f"CLEAR_IMPROVEMENT_CB={'yes' if ratio_CB>=0.05 else 'no'}")
    else:
        lines.append("RATIO_CLAIM=blocked_apparatus_or_freq")
# 5 pairs * arms(~4 with two C) * 22min
n_arms = len({r.get("arm") for r in rows}) or 3
est_min = 5 * n_arms * 22 + 15
lines.append(f"STOP_WINDOW_FOR_FINAL_ONLY_{est_min}")
text = "\n".join(lines) + "\n"
Path(summary_path).write_text(text)
print(text, end="")
sys.exit(0 if flow == "ok" else 1)
PY
