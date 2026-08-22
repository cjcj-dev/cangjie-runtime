#!/usr/bin/env bash
# gaterig: one runtime, one workload recipe, four comparable gates.
set -uo pipefail

readonly GATERIG_VERSION="1"
readonly SD_GOLDEN="368685940367600"
readonly NW_GOLDEN="635925223159200"
readonly SD_ELF_DEFAULT="/root/fysoff2-run/bin-paint/survival_dense"
readonly NW_ELF_DEFAULT="/root/fysoff2-run/bin-paint/natural_wave_notime"

usage() {
    cat >&2 <<'EOF'
usage: GATERIG_CORES=<cjops-window> tools/gate_rig.sh RUNTIME_SO_DIR

RUNTIME_SO_DIR must contain libcangjie-runtime.so and libboundscheck.so.
Reserve GATERIG_CORES with `cjops windows` before running.  Optional variables:
  GATERIG_OUT          evidence directory (default: /tmp/gaterig-<UTC>-<pid>)
  GATERIG_SD_ELF       survival_dense ELF (default: bin-paint provenance)
  GATERIG_NW_ELF       natural_wave_notime ELF (default: bin-paint provenance)
  GATERIG_SOURCE_ROOT  matching runtime source tree (default: script repository)
  GATERIG_SOURCE_COMMIT source commit when SOURCE_ROOT is not a git worktree
EOF
    exit 2
}

die() {
    printf 'GATERIG_ERROR=%s\n' "$*" >&2
    exit 2
}

[[ $# -eq 1 ]] || usage

so_dir=$(realpath "$1" 2>/dev/null) || die "cannot resolve runtime SO directory: $1"
so="$so_dir/libcangjie-runtime.so"
bounds="$so_dir/libboundscheck.so"
[[ -f "$so" ]] || die "missing $so"
[[ -f "$bounds" ]] || die "missing $bounds"

cores=${GATERIG_CORES:-}
[[ -n "$cores" ]] || die "GATERIG_CORES is required; reserve it with cjops windows"
[[ "$cores" =~ ^[0-9]+(-[0-9]+)?(,[0-9]+(-[0-9]+)?)*$ ]] || die "invalid GATERIG_CORES: $cores"

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/.." && pwd)
source_root=$(realpath "${GATERIG_SOURCE_ROOT:-$repo_root}" 2>/dev/null) || die "cannot resolve source root"
gc_gate="$source_root/runtime/tests/gc_unit/gate_gc_unit.sh"
[[ -f "$gc_gate" ]] || die "missing gc_unit gate: $gc_gate"

sd_elf=$(realpath "${GATERIG_SD_ELF:-$SD_ELF_DEFAULT}" 2>/dev/null) || die "cannot resolve survival_dense ELF"
nw_elf=$(realpath "${GATERIG_NW_ELF:-$NW_ELF_DEFAULT}" 2>/dev/null) || die "cannot resolve natural_wave_notime ELF"
[[ -x "$sd_elf" ]] || die "survival_dense is not executable: $sd_elf"
[[ -x "$nw_elf" ]] || die "natural_wave_notime is not executable: $nw_elf"

for command_name in awk ldd objdump sha256sum strings taskset timeout uptime; do
    command -v "$command_name" >/dev/null 2>&1 || die "missing command: $command_name"
done

timestamp=$(date -u +%Y%m%dT%H%M%SZ)
out=${GATERIG_OUT:-/tmp/gaterig-$timestamp-$$}
[[ ! -e "$out" ]] || die "refusing to overwrite evidence directory: $out"
mkdir -p "$out/runs" "$out/gc_unit" || die "cannot create evidence directory: $out"
out=$(realpath "$out")

sha_of() {
    sha256sum "$1" | awk '{print $1}'
}

trust_count() {
    local elf=$1
    # Canonical campaign-wide trust counter.  Keep this copy singular.
    objdump -d "$elf" | awk '/shr *\$0x30,/{hot=3;next}
      hot>0{ if($0~/\tje |\tjne /){j++;hot=0} else hot-- } END{print j+0}'
}

strip48_count() {
    local elf=$1
    objdump -d "$elf" | awk '/\tmovabs .*\$0xffffffffffff,/{n++} END{print n+0}'
}

runtime_stamp=$(strings "$so" | awk '/CJRT-COMMIT:/{
    for (i=1; i<=NF; i++) if ($i ~ /^CJRT-COMMIT:[0-9a-f-]+$/) print $i
  }' | sort -u | paste -sd, -)
[[ -n "$runtime_stamp" ]] || runtime_stamp="NO_PROVENANCE_STAMP"
runtime_commit=${runtime_stamp#CJRT-COMMIT:}

source_commit=$(git -C "$source_root" rev-parse HEAD 2>/dev/null || true)
source_commit=${source_commit:-${GATERIG_SOURCE_COMMIT:-NO_SOURCE_COMMIT}}
if [[ "$runtime_stamp" == CJRT-COMMIT:* && "$source_commit" != "NO_SOURCE_COMMIT" &&
      "$runtime_commit" != "$source_commit" ]]; then
    die "gc_unit source/SO mismatch: source=$source_commit runtime=$runtime_commit"
fi

sd_sha=$(sha_of "$sd_elf")
nw_sha=$(sha_of "$nw_elf")
sd_trust=$(trust_count "$sd_elf")
nw_trust=$(trust_count "$nw_elf")
sd_strip=$(strip48_count "$sd_elf")
nw_strip=$(strip48_count "$nw_elf")
official_count=0
(( sd_trust > 0 )) && official_count=$((official_count + 1))
(( nw_trust > 0 )) && official_count=$((official_count + 1))

loader_preflight() {
    local elf=$1 workload=$2 loaded
    local log="$out/loader-$workload.log"
    LD_LIBRARY_PATH="$so_dir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" ldd "$elf" >"$log" 2>&1 || true
    if grep -q 'not found' "$log"; then
        die "dynamic-loader preflight failed for $workload; see $log"
    fi
    loaded=$(awk '$1=="libcangjie-runtime.so" && $2=="=>" {print $3; exit}' "$log")
    [[ -n "$loaded" ]] || die "cannot prove runtime binding for $workload; see $log"
    loaded=$(realpath "$loaded" 2>/dev/null || true)
    [[ "$loaded" == "$so" ]] || die "$workload binds $loaded instead of $so"
}

loader_preflight "$sd_elf" sd256
loader_preflight "$nw_elf" nw256

readback_line=$(taskset -c "$cores" bash -c 'taskset -pc $$' 2>&1) || die "taskset cannot realize window $cores: $readback_line"
readback=$(printf '%s\n' "$readback_line" | grep -oE '[0-9]+(-[0-9]+)?(,[0-9]+(-[0-9]+)?)*$' | tail -1)
[[ -n "$readback" ]] || die "cannot parse taskset readback: $readback_line"

load_start=$(uptime | sed 's/^[[:space:]]*//')
started_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)

{
    printf 'GATERIG_VERSION\t%s\n' "$GATERIG_VERSION"
    printf 'SO\tpath\tsha256\tCJRT-COMMIT\tsource_commit\n'
    printf 'SO\t%s\t%s\t%s\t%s\n' "$so" "$(sha_of "$so")" "$runtime_stamp" "$source_commit"
    printf 'ELF\tworkload\tpath\tsha256\ttrust\tstrip48\tgolden\tprovenance\n'
    printf 'ELF\tsd256\t%s\t%s\t%s\t%s\t%s\t%s\n' "$sd_elf" "$sd_sha" "$sd_trust" "$sd_strip" "$SD_GOLDEN" "$([[ $sd_trust -eq 0 ]] && printf SELF_COMPILED || printf 'OFFICIAL_ARTIFACT!')"
    printf 'ELF\tnw256\t%s\t%s\t%s\t%s\t%s\t%s\n' "$nw_elf" "$nw_sha" "$nw_trust" "$nw_strip" "$NW_GOLDEN" "$([[ $nw_trust -eq 0 ]] && printf SELF_COMPILED || printf 'OFFICIAL_ARTIFACT!')"
    printf 'CORES\tcjops_window\ttaskset_readback\n'
    printf 'CORES\t%s\t%s\n' "$cores" "$readback"
    printf 'LOAD\tpoint\ttimestamp_utc\tuptime\n'
    printf 'LOAD\tSTART\t%s\t%s\n' "$started_at" "$load_start"
} | tee "$out/identity.tsv"

run_gc_state() {
    local state=$1
    local log="$out/runs/g1-$state.log"
    local stamp="$out/gc_unit/.gate_stamp"
    local rc total passed failed tally
    # Required anti-cache proof: remove the gate stamp before every state.
    rm -f -- "$stamp"
    if [[ "$state" == "default" ]]; then
        taskset -c "$cores" env -u CJRT_HEAP_FILLER \
            GCV2_RUNTIME_LIB_DIR="$so_dir" GC_UNIT_OUT="$out/gc_unit" \
            bash "$gc_gate" >"$log" 2>&1
        rc=$?
    else
        taskset -c "$cores" env CJRT_HEAP_FILLER=0 \
            GCV2_RUNTIME_LIB_DIR="$so_dir" GC_UNIT_OUT="$out/gc_unit" \
            bash "$gc_gate" >"$log" 2>&1
        rc=$?
    fi
    tally=$(grep -E '^\[========\] [0-9]+ tests: [0-9]+ passed, [0-9]+ failed' "$log" | tail -1 || true)
    total=$(printf '%s\n' "$tally" | awk '{print $2}')
    passed=$(printf '%s\n' "$tally" | awk '{print $4}')
    failed=$(printf '%s\n' "$tally" | awk '{print $6}')
    total=${total:-0}; passed=${passed:-0}; failed=${failed:-0}
    printf '%s\t%s\t%s\t%s\n' "$rc" "$total" "$passed" "$failed"
}

classify_run() {
    local elf=$1 workload=$2 index=$3
    shift 3
    local stdout="$out/runs/${workload}-${index}.out"
    local stderr="$out/runs/${workload}-${index}.err"
    local meta="$out/runs/${workload}-${index}.meta"
    local begin end wall_ms rc checksum class
    begin=$(date +%s%N)
    taskset -c "$cores" timeout 300s env \
        -u CJRT_HEAP_FILLER -u cjGCInterval \
        LD_LIBRARY_PATH="$so_dir${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        "$@" "$elf" >"$stdout" 2>"$stderr"
    rc=$?
    end=$(date +%s%N)
    wall_ms=$(( (end - begin) / 1000000 ))
    if [[ "$workload" == g2-* || "$workload" == g3-* ]]; then
        checksum=$(awk '/SURVIVAL_DENSE_OK/{for(i=1;i<=NF;i++) if($i~/^checksum=/){sub(/^checksum=/,"",$i);v=$i}} END{print v}' "$stdout")
        [[ $rc -eq 0 && "$checksum" == "$SD_GOLDEN" ]] && class=GOLD || class=""
    else
        checksum=$(awk '/NATURAL_WAVE_OK/{for(i=1;i<=NF;i++) if($i~/^checksum=/){sub(/^checksum=/,"",$i);v=$i}} END{print v}' "$stdout")
        [[ $rc -eq 0 && "$checksum" == "$NW_GOLDEN" ]] && class=GOLD || class=""
    fi
    if [[ -z "$class" ]]; then
        case "$rc" in
            124) class=TIMEOUT ;;
            139) class=SEGV ;;
            134) class=ABRT ;;
            *) class=OTHER ;;
        esac
    fi
    printf 'workload=%s\nindex=%s\nrc=%s\nwall_ms=%s\nchecksum=%s\nclass=%s\n' \
        "$workload" "$index" "$rc" "$wall_ms" "${checksum:-NONE}" "$class" >"$meta"
    printf '%s\t%s\t%s\t%s\n' "$class" "$rc" "$wall_ms" "${checksum:-NONE}"
}

run_series() {
    local id=$1 n=$2 elf=$3
    shift 3
    local i class rc wall checksum
    : >"$out/$id.attempts.tsv"
    printf 'attempt\tclass\trc\twall_ms\tchecksum\n' >>"$out/$id.attempts.tsv"
    for ((i=1; i<=n; i++)); do
        IFS=$'\t' read -r class rc wall checksum < <(classify_run "$elf" "$id" "$i" "$@")
        printf '%s\t%s\t%s\t%s\t%s\n' "$i" "$class" "$rc" "$wall" "$checksum" >>"$out/$id.attempts.tsv"
        printf 'GATERIG_PROGRESS gate=%s attempt=%s/%s class=%s rc=%s wall_ms=%s checksum=%s\n' \
            "$id" "$i" "$n" "$class" "$rc" "$wall" "$checksum" >&2
    done
}

distribution() {
    local attempts=$1 class
    for class in GOLD TIMEOUT SEGV ABRT OTHER; do
        awk -F'\t' -v want="$class" 'NR>1 && $2==want{n++} END{print n+0}' "$attempts"
    done | paste -sd$'\t' -
}

IFS=$'\t' read -r g1_default_rc g1_default_total g1_default_pass g1_default_fail < <(run_gc_state default)
IFS=$'\t' read -r g1_filler_rc g1_filler_total g1_filler_pass g1_filler_fail < <(run_gc_state filler0)
if [[ $g1_default_rc -eq 0 && $g1_default_total -gt 0 && $g1_default_total -eq $g1_default_pass &&
      $g1_filler_rc -eq 0 && $g1_filler_total -gt 0 && $g1_filler_total -eq $g1_filler_pass ]]; then
    g1_result=PASS
else
    g1_result=FAIL
fi

run_series g2-sd256-default 8 "$sd_elf" cjHeapSize=256MB
run_series g3-sd256-interval5s 8 "$sd_elf" cjHeapSize=256MB cjGCInterval=5s
run_series g4-nw256-default 20 "$nw_elf" cjHeapSize=256MB

IFS=$'\t' read -r g2_gold g2_timeout g2_segv g2_abrt g2_other < <(distribution "$out/g2-sd256-default.attempts.tsv")
IFS=$'\t' read -r g3_gold g3_timeout g3_segv g3_abrt g3_other < <(distribution "$out/g3-sd256-interval5s.attempts.tsv")
IFS=$'\t' read -r g4_gold g4_timeout g4_segv g4_abrt g4_other < <(distribution "$out/g4-nw256-default.attempts.tsv")
[[ $g2_gold -eq 8 ]] && g2_result=PASS || g2_result=FAIL

load_end=$(uptime | sed 's/^[[:space:]]*//')
ended_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
printf 'LOAD\tEND\t%s\t%s\n' "$ended_at" "$load_end" | tee -a "$out/identity.tsv"

table="$out/gates.tsv"
{
    printf 'gate\tmode\tN\ttimeout_s\tcriterion\tGOLD\tTIMEOUT\tSEGV\tABRT\tOTHER\tresult\tdetail\n'
    printf 'G1\tgc_unit dual-state\t2\t600\tboth N/N passed\tNA\tNA\tNA\tNA\tNA\t%s\tdefault %s/%s passed (%s failed); CJRT_HEAP_FILLER=0 %s/%s passed (%s failed)\n' \
        "$g1_result" "$g1_default_pass" "$g1_default_total" "$g1_default_fail" \
        "$g1_filler_pass" "$g1_filler_total" "$g1_filler_fail"
    printf 'G2\tsd256 default cadence\t8\t300\t8/8 checksum=%s\t%s\t%s\t%s\t%s\t%s\t%s\tcjHeapSize=256MB\n' \
        "$SD_GOLDEN" "$g2_gold" "$g2_timeout" "$g2_segv" "$g2_abrt" "$g2_other" "$g2_result"
    printf 'G3\tsd256 interval=5s\t8\t300\tRECORD_ONLY\t%s\t%s\t%s\t%s\t%s\tRECORDED\tcjHeapSize=256MB cjGCInterval=5s\n' \
        "$g3_gold" "$g3_timeout" "$g3_segv" "$g3_abrt" "$g3_other"
    printf 'G4\tnw256 terminal distribution\t20\t300\tTERMINAL_DISTRIBUTION\t%s\t%s\t%s\t%s\t%s\tRECORDED\tcjHeapSize=256MB\n' \
        "$g4_gold" "$g4_timeout" "$g4_segv" "$g4_abrt" "$g4_other"
} >"$table"

printf '%s\n' 'GATES_TSV_BEGIN'
cat "$table"
printf '%s\n' 'GATES_TSV_END' 'GATES_MARKDOWN_BEGIN'
awk -F'\t' '
  NR==1 {printf "| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |\n",$1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12;
         print "|---|---|---:|---:|---|---:|---:|---:|---:|---:|---|---|"; next}
  {printf "| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |\n",$1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12}
' "$table"
printf '%s\n' 'GATES_MARKDOWN_END'

if [[ $g1_result == PASS && $g2_result == PASS ]]; then
    blocking=PASS
    exit_rc=0
else
    blocking=FAIL
    exit_rc=1
fi
verdict="GATERIG_VERDICT=blocking:${blocking};G1:${g1_result};G2:${g2_result};G3:RECORDED(GOLD=${g3_gold},TIMEOUT=${g3_timeout},SEGV=${g3_segv},ABRT=${g3_abrt},OTHER=${g3_other});G4:RECORDED(GOLD=${g4_gold},TIMEOUT=${g4_timeout},SEGV=${g4_segv},ABRT=${g4_abrt},OTHER=${g4_other});evidence:${out}"
if (( official_count > 0 )); then
    verdict+=";⚠ 本表的终态分布含配方因素，⛔ 不能单独用作 runtime 正确性证据"
fi
printf '%s\n' "$verdict" | tee "$out/verdict.txt"
exit "$exit_rc"
