#!/bin/bash
# youngflip campaign: smoke → paired cells → STOP soak → wave12 + rec=stw → CONT soak.
# File markers only. No pid capture, no process-group signal, no pkill.
set -u
ROOT=/root/youngflip-run
LOG=$ROOT/logs/campaign.log
SOAK_PID_FILE=/root/cjpmck-run/soak_paint/soak.pid
mkdir -p "$ROOT/logs"
exec >>"$LOG" 2>&1
echo "===== CAMPAIGN_START $(date -Is) ====="
echo "MemAvailable=$(awk '/MemAvailable/{print $2}' /proc/meminfo) kB"
echo "load=$(cat /proc/loadavg)"

soak_stopped=0
resume_soak() {
  if [ "$soak_stopped" = 1 ] && [ -s "$SOAK_PID_FILE" ]; then
    echo "RECSTW_CONT soak.pid=$(cat "$SOAK_PID_FILE")"
    kill -CONT "$(cat "$SOAK_PID_FILE")" || true
    soak_stopped=0
  fi
}
cleanup() {
  resume_soak
  if [ -f /dev/shm/MEASURE_ACTIVE ]; then
    grep -v 'youngflip-1 cores=160-189' /dev/shm/MEASURE_ACTIVE > /dev/shm/MEASURE_ACTIVE.tmp || true
    mv /dev/shm/MEASURE_ACTIVE.tmp /dev/shm/MEASURE_ACTIVE
  fi
}
trap cleanup EXIT

until=$(( $(date +%s) + 43200 ))
if ! grep -q 'youngflip-1 cores=160-189' /dev/shm/MEASURE_ACTIVE 2>/dev/null; then
  echo "youngflip-1 cores=160-189 host=kkk2 since=$(date -u +%Y-%m-%dT%H:%M:%SZ) until=$until" >> /dev/shm/MEASURE_ACTIVE
fi
echo "MEASURE_LINE=$(grep youngflip /dev/shm/MEASURE_ACTIVE)"

mem_ok() {
  local avail
  avail=$(awk '/MemAvailable/{print $2}' /proc/meminfo)
  echo "MemAvailable=${avail}kB"
  if [ "$avail" -lt 41943040 ]; then
    echo "DEFERRED_LOW_MEM avail=$avail"
    return 1
  fi
  return 0
}

cd "$ROOT"
bash tools/youngflip_driver.sh id

smoke_cell() {
  local tag=$1 n=$2
  local smoke_dir=$ROOT/mega/smoke_$tag
  if [ -f "$smoke_dir.ok" ]; then
    echo "SMOKE_SKIP $tag"
    return 0
  fi
  echo "SMOKE_START $tag N=$n $(date -Is)"
  case "$tag" in
    sd1g) bash tools/youngflip_driver.sh pair-sd1g "$n" ;;
    nw256) bash tools/youngflip_driver.sh pair-nw256 "$n" ;;
    sd256) bash tools/youngflip_driver.sh pair-sd256 "$n" ;;
    nw1g) bash tools/youngflip_driver.sh pair-nw1g "$n" ;;
  esac
  # driver writes DONE; smoke N=1 must not block full N. Move aside.
  local src
  case "$tag" in
    sd1g) src=$ROOT/mega/pair_sd_1GB ;;
    nw256) src=$ROOT/mega/pair_nw_256MB ;;
    sd256) src=$ROOT/mega/pair_sd_256MB ;;
    nw1g) src=$ROOT/mega/pair_nw_1GB ;;
  esac
  mkdir -p "$smoke_dir"
  if [ -d "$src" ]; then
    cp -a "$src/summary_A.txt" "$src/summary_B.txt" "$src/mega.tsv" "$smoke_dir/" 2>/dev/null || true
    rm -f "$src/DONE"
    # keep FAIL/met from smoke; full run uses new i=1..N overlapping names — move smoke artifacts
    mkdir -p "$smoke_dir/art"
    mv "$src"/FAIL_* "$src"/met_* "$src"/mega.tsv "$src"/summary_*.txt "$smoke_dir/art/" 2>/dev/null || true
  fi
  echo "SMOKE_SUM $tag"
  cat "$smoke_dir/art/summary_A.txt" "$smoke_dir/art/summary_B.txt" 2>/dev/null || cat "$smoke_dir"/summary_*.txt 2>/dev/null || true
  touch "$smoke_dir.ok"
  echo "SMOKE_OK $tag $(date -Is)"
}

# cheap smoke first
mem_ok || { echo CAMPAIGN_DEFER; echo 2 > "$ROOT/campaign.rc"; touch "$ROOT/campaign.done"; exit 0; }
smoke_cell sd1g 1

# full cells. smoke already used N=1 on sd1g so that cell starts from empty DONE.
echo "FULL_START $(date -Is)"
mem_ok && bash tools/youngflip_driver.sh pair-sd1g 8
mem_ok && bash tools/youngflip_driver.sh pair-nw256 50
mem_ok && bash tools/youngflip_driver.sh pair-sd256 50
mem_ok && bash tools/youngflip_driver.sh pair-nw1g 100

if [ -s "$SOAK_PID_FILE" ]; then
  echo "RECSTW_STOP soak.pid=$(cat "$SOAK_PID_FILE")"
  if kill -STOP "$(cat "$SOAK_PID_FILE")"; then
    soak_stopped=1
  fi
else
  echo "SOAK_ABSENT"
fi
sleep 3
echo "STAT_160_189_after_STOP"
ps -eo pid,psr,stat,comm --no-headers | awk '$2>=160 && $2<=189 && $3 ~ /T|R/' | grep -E 'paint|wave|dense|timeout' | head -20 || true

echo "WAVE12_START $(date -Is)"
mem_ok && bash tools/youngflip_driver.sh wave12 8

echo "RECSTW_START $(date -Is)"
mem_ok && bash tools/youngflip_driver.sh recstw 5
resume_soak

echo "===== CAMPAIGN_END $(date -Is) ====="
echo 0 > "$ROOT/campaign.rc"
touch "$ROOT/campaign.done"
