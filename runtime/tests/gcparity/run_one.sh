#!/usr/bin/env bash
set -u

if [[ $# -ne 8 ]]; then
    echo "usage: $0 OUT_DIR ARM HEAP FYS BIN RTLIB CORES TIMEOUT_SECONDS" >&2
    exit 2
fi

out_dir=$1
arm=$2
heap=$3
fys=$4
binary=$5
runtime_lib=$6
cores=$7
timeout_seconds=$8

case "$arm" in
    young|minoroff) ;;
    *) echo "unknown arm: $arm" >&2; exit 2 ;;
esac
case "$fys" in
    0|1) ;;
    *) echo "FYS must be 0 or 1" >&2; exit 2 ;;
esac

if [[ -e "$out_dir" ]]; then
    echo "refusing to overwrite existing run directory: $out_dir" >&2
    exit 2
fi
install -d "$out_dir"

{
    printf 'arm=%s\n' "$arm"
    printf 'heap=%s\n' "$heap"
    printf 'fys=%s\n' "$fys"
    printf 'binary=%s\n' "$binary"
    printf 'runtime_lib=%s\n' "$runtime_lib"
    printf 'cores=%s\n' "$cores"
    printf 'timeout_seconds=%s\n' "$timeout_seconds"
    sha256sum "$binary" "$runtime_lib/libcangjie-runtime.so" "$runtime_lib/libboundscheck.so"
} >"$out_dir/meta.txt"

common_env=(
    PATH=/usr/bin:/bin
    HOME=/root
    LC_ALL=C
    LD_LIBRARY_PATH="$runtime_lib"
    cjHeapSize="$heap"
    MRT_GCV2_FULL_YOUNG_SCAN="$fys"
    MRT_GCV2_MARKPAR_FORCE_SERIAL=1
    MRT_LOG_LEVEL=i
    MRT_LOG_PATH="$out_dir/runtime.log"
    MRT_REPORT="$out_dir/report.log"
    MRT_GC_LOG=1
)
if [[ "$arm" == minoroff ]]; then
    common_env+=(MRT_GCV2_DISABLE_MINOR=1)
fi

begin=$(date --iso-8601=ns)
set +e
env -i "${common_env[@]}" \
    taskset -c "$cores" \
    /usr/bin/time -f 'wall_s=%e maxrss_kb=%M exit=%x' -o "$out_dir/time.txt" \
    timeout --signal=TERM --kill-after=2s "${timeout_seconds}s" \
    "$binary" >"$out_dir/stdout" 2>"$out_dir/stderr"
rc=$?
set -e
end=$(date --iso-8601=ns)

printf '%s\n' "$rc" >"$out_dir/rc"
printf 'begin=%s\nend=%s\n' "$begin" "$end" >>"$out_dir/meta.txt"
sha256sum "$out_dir/stdout" >"$out_dir/stdout.sha256"
exit 0
