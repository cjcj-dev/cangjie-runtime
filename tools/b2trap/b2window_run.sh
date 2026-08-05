#!/bin/bash
# b2window: same lean late-arm recipe as b2bulk + all-thread stack dump at fatal
set -u
export PATH=/usr/bin:/bin:/usr/local/bin:$PATH
CORES=48-79
ROOT=/root/b2trap-run
SDK=/root/gate006-run/sdk
N=${1:-24}
START=${2:-1}
SKIP=${3:-80000}
EVERY=${4:-400}
mkdir -p "$ROOT/evidence" "$ROOT/runs" "$ROOT/logs" "$ROOT/dumps"
LOG="$ROOT/logs/b2window_$(date +%Y%m%d_%H%M%S).log"
exec > >(tee -a "$LOG") 2>&1
echo "=== B2WINDOW START $(date -Is) N=$N SKIP=$SKIP EVERY=$EVERY sup=$(sha256sum $ROOT/b2trap_supervisor|awk '{print $1}') ==="
MATRIX="$ROOT/evidence/b2window_matrix.tsv"
echo -e "phase\tround\trc\tclass\tfamily\tnode_seen\tarmed\twp_hit\twp_interior\tsettle_t\toverlap\tthr_n\tmemmove_n\twall_s" > "$MATRIX"
export LD_LIBRARY_PATH="$ROOT/so:$SDK/runtime/lib/linux_x86_64_cjnative:$SDK/third_party/llvm/lib"
export cjHeapSize=64MB MRT_GCV2_FULL_YOUNG_SCAN=0 MRT_GCV2_SKIP_PINNED=1 MRT_GC_LOG=1
unset MRT_GCV2_MINOR_GC_ALOT 2>/dev/null || true
count0() { local n; n=$(grep -cE "$1" "$2" 2>/dev/null) || n=0; echo "$n"; }
classify() {
  local dir=$1 rc=$2 class=OTHER family=0
  if [ "$rc" -eq 0 ] && grep -q NATURAL_WAVE_OK "$dir/stdout.txt" 2>/dev/null; then class=OK; family=0
  elif grep -qE "INVALID_OBJECT_SIZE|sizeguard" "$dir/stderr.txt" 2>/dev/null; then class=SIZEGUARD; family=1
  elif grep -q "IsMarkedObject" "$dir/stderr.txt" 2>/dev/null; then class=ISMARKED; family=1
  elif grep -qE "IsValidObject|Invalid object" "$dir/stderr.txt" 2>/dev/null; then class=INVALID_OBJ; family=1
  elif [ "$rc" -eq 139 ] || grep -q SIGSEGV "$dir/stderr.txt" 2>/dev/null; then class=SEGV; family=1
  elif [ "$rc" -eq 134 ] || grep -q SIGABRT "$dir/stderr.txt" 2>/dev/null; then
    if grep -qE "Check failed" "$dir/stderr.txt" 2>/dev/null; then class=ABRT_CHECK; family=1; else class=ABRT_OTHER; family=0; fi
  fi
  echo "$class $family"
}
for r in $(seq $START $((START+N-1))); do
  avail=$(awk '/MemAvailable/{print int($2/1048576)}' /proc/meminfo)
  if [ "$avail" -lt 30 ]; then echo "DEFERRED_LOW_MEM avail=${avail}G round=$r"; sleep 15; fi
  dir="$ROOT/runs/win_r${r}"; rm -rf "$dir"; mkdir -p "$dir"
  t0=$(date +%s)
  set +e
  timeout 150s taskset -c $CORES setarch x86_64 -R \
    "$ROOT/b2trap_supervisor" -o "$dir" -t 120 -F 1 -A node -K $SKIP -S $EVERY -- \
    "$ROOT/bin/natural_wave" >"$dir/stdout.txt" 2>"$dir/stderr.txt"
  rc=$?; set -e
  wall=$(($(date +%s)-t0))
  read -r class family < <(classify "$dir" "$rc")
  node_seen=$(grep -aoE "node_seen=[0-9]+" "$dir/supervisor.log" 2>/dev/null | tail -1 | grep -oE "[0-9]+$" || true); node_seen=${node_seen:-0}
  armed=$(count0 "MIDRUN_ARM" "$dir/supervisor.log")
  wp_hit=$(count0 "WP_HIT" "$dir/supervisor.log")
  wp_int=$(count0 "WP_INTERIOR_WRITER" "$dir/supervisor.log")
  settle_t=$(grep -c "=> TRANSIENT" "$dir/supervisor.log" 2>/dev/null || true); settle_t=${settle_t:-0}
  overlap=$(count0 "B2WIN_OVERLAP " "$dir/supervisor.log")
  # ALL_THREADS_END n=.. got_regs=.. memmove_like=.. overlap=..
  thr_n=$(grep -aoE "got_regs=[0-9]+" "$dir/supervisor.log" 2>/dev/null | tail -1 | grep -oE "[0-9]+$" || true); thr_n=${thr_n:-0}
  memmove_n=$(grep -aoE "memmove_like=[0-9]+" "$dir/supervisor.log" 2>/dev/null | tail -1 | grep -oE "[0-9]+$" || true); memmove_n=${memmove_n:-0}
  conf=$(grep -c "B2WIN_OVERLAP_CONFIRMED" "$dir/supervisor.log" 2>/dev/null || true); conf=${conf:-0}
  noov=$(grep -c "B2WIN_NO_OVERLAP" "$dir/supervisor.log" 2>/dev/null || true); noov=${noov:-0}
  end=$(grep SUPERVISOR_END "$dir/supervisor.log" 2>/dev/null | tail -1 || true)
  echo "END_LINE $end"
  if [ "$family" -ne 0 ] || [ "$wp_int" -ne 0 ] || [ "$overlap" -ne 0 ]; then
    mkdir -p "$ROOT/dumps/win_r${r}"
    grep -E "FATAL|REGS|THREAD_|ALL_THREADS|B2WIN|WP_INTERIOR|B2BULK|MIDRUN|SUPERVISOR_|SWBP_UNPLANT|LIB_MAPS" \
      "$dir/supervisor.log" > "$ROOT/dumps/win_r${r}/sum.txt" 2>/dev/null || true
    grep -E "B2WIN|THREAD_STACK|ALL_THREADS|WP_INTERIOR|B2BULK" \
      "$dir/supervisor.log" > "$ROOT/dumps/win_r${r}/window.txt" 2>/dev/null || true
  fi
  echo -e "win\t${r}\t${rc}\t${class}\t${family}\t${node_seen}\t${armed}\t${wp_hit}\t${wp_int}\t${settle_t}\t${overlap}\t${thr_n}\t${memmove_n}\t${wall}" | tee -a "$MATRIX"
done
echo "=== HIST ==="; awk -F"\t" 'NR>1{c[$4]++} END{for(k in c) print c[k],k}' "$MATRIX" | sort -rn
echo "=== FAMILY ==="; awk -F"\t" 'NR>1{n++; if($5==1)f++} END{print f+0"/"n+0}' "$MATRIX"
echo "=== OVERLAP_ROUNDS ==="; awk -F"\t" 'NR>1{if($11+0>0)o++} END{print o+0}' "$MATRIX"
echo "=== WP_INT ==="; awk -F"\t" 'NR>1{s+=$9} END{print s+0}' "$MATRIX"
echo "=== END $(date -Is) ==="
