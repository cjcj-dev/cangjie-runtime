#!/usr/bin/env bash
# kkk2 OFF/ON/CTRL recipe for REPORT-trigrate2. CTRL uses existing cjGCInterval
# (CjScheduler.cpp:696) to delay HEU without MRT_GCV2_TRIGGER_RATE.
set -u
LIB=${LIB:-/root/trigrate2-run/lib}
SD=${SD:-/root/gcparity-run/bin/survival_dense}
AD=${AD:-/root/gcparity-run/bin/allocation_dense}
NW=${NW:-/root/fysoff2-run/bin-paint/natural_wave_notime}
CORES=${CORES:-32-39}
OUT=${OUT:-/root/trigrate2-run/meas}
mkdir -p "$OUT"
run_one() {
  local arm=$1 bin=$2 heap=$3 i=$4
  shift 4
  local d="$OUT/${arm}/r$(printf '%02d' "$i")"
  mkdir -p "$d"
  env -i PATH=/usr/bin:/bin HOME=/root LC_ALL=C LD_LIBRARY_PATH="$LIB" \
    cjHeapSize="$heap" MRT_GCV2_MARKPAR_FORCE_SERIAL=1 MRT_GC_LOG=1 \
    "$@" \
    taskset -c "$CORES" /usr/bin/time -f 'wall_s=%e maxrss_kb=%M exit=%x' -o "$d/time.txt" \
    timeout --signal=TERM --kill-after=2s 180s "$bin" >"$d/stdout" 2>"$d/stderr"
  echo $? >"$d/rc"
}
