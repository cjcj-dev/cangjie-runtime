#!/usr/bin/env bash
# Single-arm harness for MRT_GCV2_MINOR_YOUNG_FLIP (minor relocate-start colour flip).
#
# The only difference between the OFF and ON arm is MRT_GCV2_MINOR_YOUNG_FLIP.
# MRT_GCV2_MINOR_CONC_REF_FIX is never set here: it implies the flip, so setting it
# would destroy the single-arm property this harness exists to establish.
#
# The three opt-in flags (reffix dedup / remset hash / mark-remset intersect) are
# written out explicitly as 0 so the base path does not depend on their current
# defaults, matching arm "A" of the flagrecheck campaign.
set -u

if [[ $# -ne 9 ]]; then
    echo "usage: $0 OUT_DIR ARM WORKLOAD HEAP MODE RUNTIME_LIB BIN_DIR TIMEOUT_SECONDS DIAG" >&2
    exit 2
fi

out_dir=$1
arm=$2
workload=$3
heap=$4
mode=$5
runtime_lib=$6
bin_dir=$7
timeout_seconds=$8
diag=$9
cores=80-95

case "$arm" in
    OFF|ON) ;;
    *) echo "unknown arm: $arm" >&2; exit 2 ;;
esac
case "$workload" in
    allocation_dense|survival_dense) binary="$bin_dir/$workload" ;;
    *) echo "unknown workload: $workload" >&2; exit 2 ;;
esac
case "$mode" in
    serial|parallel) ;;
    *) echo "unknown mark mode: $mode" >&2; exit 2 ;;
esac
case "$diag" in
    none|maskequiv|waitfwd) ;;
    *) echo "unknown diag: $diag" >&2; exit 2 ;;
esac
if [[ -e "$out_dir" ]]; then
    echo "refusing to overwrite existing run directory: $out_dir" >&2
    exit 2
fi
install -d "$out_dir"

flip=0
if [[ "$arm" == ON ]]; then
    flip=1
fi

common_env=(
    PATH=/usr/bin:/bin
    HOME=/root
    LC_ALL=C
    LD_LIBRARY_PATH="$runtime_lib"
    cjHeapSize="$heap"
    MRT_GCV2_FULL_YOUNG_SCAN=1
    MRT_LOG_LEVEL=i
    MRT_LOG_PATH="$out_dir/runtime.log"
    MRT_REPORT="$out_dir/report.log"
    MRT_GC_LOG=1
    MRT_GCV2_REFFIX_COVERED_DEDUP=0
    MRT_GCV2_REMSET_HASH_OPT=0
    MRT_GCV2_MARK_REMSET_INTERSECT=0
)
if [[ "$mode" == serial ]]; then
    common_env+=(MRT_GCV2_MARKPAR_FORCE_SERIAL=1)
else
    common_env+=(MRT_GCV2_MARKPAR_WORKERS=2)
fi
if [[ "$flip" == 1 ]]; then
    common_env+=(MRT_GCV2_MINOR_YOUNG_FLIP=1)
fi
# Diagnostics live in their own runs, never in the formal cost window:
#   maskequiv -> counts every set_good_masks() call, so ON-minus-OFF is exactly the
#                number of extra flips the flag caused (the positive control).
#   waitfwd   -> counts relocate_or_remap_object arms; kept only to record that it
#                cannot discriminate the arms, because major Preforward flips
#                unconditionally and dominates that counter.
case "$diag" in
    maskequiv) common_env+=(MRT_GCV2_MASKEQUIV=1) ;;
    waitfwd) common_env+=(MRT_GCV2_WAITFWD=1) ;;
esac

{
    printf 'arm=%s\nworkload=%s\nheap=%s\nmode=%s\n' "$arm" "$workload" "$heap" "$mode"
    printf 'minor_young_flip=%s\ndiag=%s\n' "$flip" "$diag"
    printf 'minor_conc_ref_fix=unset\nreffix_dedup=0\nremset_hash=0\nmark_intersect=0\n'
    printf 'cores=%s\ntimeout_seconds=%s\ntimeout_signal=ABRT\nkill_after_seconds=30\n' \
        "$cores" "$timeout_seconds"
    printf 'binary=%s\nruntime_lib=%s\n' "$binary" "$runtime_lib"
    printf 'env_names=%s\n' "$(printf '%s\n' "${common_env[@]}" | cut -d= -f1 | tr '\n' ',')"
    sha256sum "$binary" "$runtime_lib/libcangjie-runtime.so" "$runtime_lib/libboundscheck.so"
} > "$out_dir/meta.txt"

begin=$(date --iso-8601=ns)
set +e
env -i "${common_env[@]}" \
    taskset -c "$cores" \
    timeout --signal=ABRT --kill-after=30s "${timeout_seconds}s" \
    /usr/bin/time -f 'wall_s=%e maxrss_kb=%M exit=%x' -o "$out_dir/time.txt" \
    "$binary" > "$out_dir/stdout" 2> "$out_dir/stderr"
rc=$?
set -e
end=$(date --iso-8601=ns)

printf '%s\n' "$rc" > "$out_dir/rc"
printf 'begin=%s\nend=%s\n' "$begin" "$end" >> "$out_dir/meta.txt"
sha256sum "$out_dir/stdout" > "$out_dir/stdout.sha256"
exit 0
