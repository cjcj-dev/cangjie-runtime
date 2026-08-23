#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 10 ]]; then
    echo "usage: $0 OUT ARM HEAP BIN MARKER WORK_UNITS UNIT_NAME RTLIB CORES TIMEOUT_SECONDS" >&2
    exit 2
fi

out=$1
arm=$2
heap=$3
binary=$(realpath "$4")
marker=$5
work_units=$6
unit_name=$7
runtime_lib=$(realpath "$8")
cores=$9
timeout_seconds=${10}

case "$arm" in
    subject|official) ;;
    *) echo "unknown arm: $arm" >&2; exit 2 ;;
esac
[[ ! -e "$out" ]] || { echo "refusing to overwrite: $out" >&2; exit 2; }
[[ -x "$binary" ]] || { echo "workload is not executable: $binary" >&2; exit 2; }
for library in libcangjie-runtime.so libboundscheck.so; do
    [[ -f "$runtime_lib/$library" ]] || { echo "missing $runtime_lib/$library" >&2; exit 2; }
done
install -d "$out"

LD_LIBRARY_PATH="$runtime_lib" ldd "$binary" >"$out/ldd.txt" 2>&1
resolved_runtime=$(awk '$1 == "libcangjie-runtime.so" && $2 == "=>" {print $3}' "$out/ldd.txt")
resolved_bounds=$(awk '$1 == "libboundscheck.so" && $2 == "=>" {print $3}' "$out/ldd.txt")
[[ -n "$resolved_runtime" && -n "$resolved_bounds" ]] || {
    echo "cannot resolve runtime closure; see $out/ldd.txt" >&2
    exit 2
}
[[ $(realpath "$resolved_runtime") == $(realpath "$runtime_lib/libcangjie-runtime.so") ]] || {
    echo "runtime resolution escaped arm closure: $resolved_runtime" >&2
    exit 2
}
[[ $(realpath "$resolved_bounds") == $(realpath "$runtime_lib/libboundscheck.so") ]] || {
    echo "boundscheck resolution escaped arm closure: $resolved_bounds" >&2
    exit 2
}

runtime_sha=$(sha256sum "$runtime_lib/libcangjie-runtime.so" | awk '{print $1}')
bounds_sha=$(sha256sum "$runtime_lib/libboundscheck.so" | awk '{print $1}')
runtime_stamp=$(strings "$runtime_lib/libcangjie-runtime.so" | grep -oE 'CJRT-COMMIT:[0-9a-f-]+' | sort -u | paste -sd, - || true)
[[ -n "$runtime_stamp" ]] || runtime_stamp=NO_PROVENANCE_STAMP

begin=$(date --iso-8601=ns)
{
    printf 'arm=%s\n' "$arm"
    printf 'heap=%s\n' "$heap"
    printf 'binary=%s\n' "$binary"
    printf 'binary_sha256=%s\n' "$(sha256sum "$binary" | awk '{print $1}')"
    printf 'marker=%s\n' "$marker"
    printf 'work_units=%s\n' "$work_units"
    printf 'work_unit_name=%s\n' "$unit_name"
    printf 'runtime_lib=%s\n' "$runtime_lib"
    printf 'runtime_sha256=%s\n' "$runtime_sha"
    printf 'runtime_stamp=%s\n' "$runtime_stamp"
    printf 'boundscheck_sha256=%s\n' "$bounds_sha"
    printf 'cores=%s\n' "$cores"
    printf 'timeout_seconds=%s\n' "$timeout_seconds"
    printf 'begin=%s\n' "$begin"
    printf 'env.cjHeapSize=%s\n' "$heap"
    printf 'env.MRT_LOG_LEVEL=i\n'
    printf 'env.MRT_GC_LOG=1\n'
    printf 'env.MRT_GCV2_*=UNSET\n'
} >"$out/meta.txt"

set +e
env -i \
    HOME=/root \
    LC_ALL=C \
    PATH=/usr/bin:/bin \
    LD_LIBRARY_PATH="$runtime_lib" \
    cjHeapSize="$heap" \
    MRT_LOG_LEVEL=i \
    MRT_LOG_PATH="$out/runtime.log" \
    MRT_REPORT="$out/report.log" \
    MRT_GC_LOG=1 \
    taskset -c "$cores" \
    /usr/bin/time -f $'wall_s=%e\tmaxrss_kb=%M\ttime_exit=%x' -o "$out/time.tsv" \
    timeout --signal=TERM --kill-after=2s "${timeout_seconds}s" \
    "$binary" >"$out/stdout" 2>"$out/stderr"
rc=$?
set -e

end=$(date --iso-8601=ns)
printf '%s\n' "$rc" >"$out/rc"
printf 'end=%s\n' "$end" >>"$out/meta.txt"
sha256sum "$out/stdout" >"$out/stdout.sha256"
if [[ $rc -eq 0 ]] && grep -Fq "$marker" "$out/stdout"; then
    printf 'COMPLETE\n' >"$out/classification"
elif [[ $rc -eq 124 ]]; then
    printf 'TIMEOUT\n' >"$out/classification"
else
    printf 'FAILED\n' >"$out/classification"
fi

echo "ATTEMPT_DONE arm=$arm heap=$heap rc=$rc class=$(cat "$out/classification") out=$out"
