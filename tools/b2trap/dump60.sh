#!/bin/bash
# b2trap2 T1 pure-dump arm N=60, no watchpoints, lean recipe, family-class
set -u
export PATH=/usr/bin:/bin:/usr/local/bin:$PATH
CORES=48-79
ROOT=/root/b2trap-run
SDK=/root/gate006-run/sdk
N=${1:-60}
START=${2:-1}
mkdir -p "$ROOT/evidence" "$ROOT/runs" "$ROOT/logs" "$ROOT/dumps"
LOG="$ROOT/logs/dump60_$(date +%Y%m%d_%H%M%S).log"
exec > >(tee -a "$LOG") 2>&1
echo "=== DUMP60 START $(date -Is) N=$N START=$START ==="
MATRIX="$ROOT/evidence/dump60_matrix.tsv"
if [ ! -f "$MATRIX" ]; then
  echo -e "phase\tround\trc\tclass\tfamily\tobjsz\tobj\tholder\tb2geo\tdump_mode\tdump_bytes\twall_s\tnote" > "$MATRIX"
fi

export LD_LIBRARY_PATH="$ROOT/so:$SDK/runtime/lib/linux_x86_64_cjnative:$SDK/third_party/llvm/lib"
export cjHeapSize=64MB
export MRT_GCV2_FULL_YOUNG_SCAN=0
export MRT_GCV2_SKIP_PINNED=1
export MRT_GC_LOG=1
unset MRT_GCV2_MINOR_GC_ALOT 2>/dev/null || true

classify() {
  local dir=$1 rc=$2
  local class=OTHER family=0 objsz=- obj=- holder=- note=-
  if [ "$rc" -eq 0 ] && grep -q NATURAL_WAVE_OK "$dir/stdout.txt" 2>/dev/null; then
    class=OK; family=0
  elif grep -qE 'INVALID_OBJECT_SIZE|sizeguard' "$dir/stderr.txt" 2>/dev/null; then
    class=SIZEGUARD; family=1
    objsz=$(grep -aoE 'objSize=[0-9]+' "$dir/stderr.txt" | head -1 | grep -oE '[0-9]+$' || echo -)
    obj=$(grep -aoE 'obj=0x[0-9a-f]+' "$dir/stderr.txt" | head -1 | grep -oE '0x[0-9a-f]+' || echo -)
  elif grep -q 'toRegion2Idx != INVALID_VALUE' "$dir/stderr.txt" 2>/dev/null; then
    class=TOREGION2; family=1
  elif grep -q 'IsMarkedObject' "$dir/stderr.txt" 2>/dev/null; then
    class=ISMARKED; family=1
  elif grep -qE 'IsValidObject|Invalid object' "$dir/stderr.txt" 2>/dev/null; then
    class=INVALID_OBJ; family=1
    obj=$(grep -aoE 'Invalid object 0x[0-9a-f]+' "$dir/stderr.txt" | head -1 | grep -oE '0x[0-9a-f]+' || echo -)
    holder=$(grep -aoE 'strong object 0x[0-9a-f]+' "$dir/stderr.txt" | head -1 | grep -oE '0x[0-9a-f]+' || echo -)
    note=$(grep -aoE 'offset [0-9]+' "$dir/stderr.txt" | head -1 | tr ' ' _ || echo -)
  elif [ "$rc" -eq 139 ] || grep -q 'SIGSEGV' "$dir/stderr.txt" 2>/dev/null; then
    class=SEGV; family=1
  elif [ "$rc" -eq 134 ] || grep -q 'SIGABRT' "$dir/stderr.txt" 2>/dev/null; then
    if grep -qE 'Check failed' "$dir/stderr.txt" 2>/dev/null; then
      class=ABRT_CHECK; family=1
      note=$(grep -aoE 'Check failed: .*' "$dir/stderr.txt" | head -1 | tr '\t' ' ' | cut -c1-80 || echo -)
    else
      class=ABRT_OTHER; family=0
    fi
  elif [ "$rc" -eq 124 ]; then
    class=TIMEOUT; family=0
  fi
  # if natural_wave OK but supervisor saw fatal (should not), still family
  if [ "$class" = "OK" ] && grep -q FATAL_SIGNAL "$dir/supervisor.log" 2>/dev/null; then
    class=OK_BUT_FATAL; family=1
  fi
  printf '%s %s %s %s %s %s\n' "$class" "$family" "$objsz" "$obj" "$holder" "$note"
}

run_one() {
  local round=$1
  local dir="$ROOT/runs/d2_r${round}"
  rm -rf "$dir"
  mkdir -p "$dir"
  local t0=$(date +%s)
  set +e
  # setarch OUTSIDE supervisor so target exe is natural_wave (EXE_RANGE correct)
  timeout 150s taskset -c $CORES setarch x86_64 -R \
    "$ROOT/b2trap_supervisor" -o "$dir" -t 120 -F 2 -- \
    "$ROOT/bin/natural_wave" \
    >"$dir/stdout.txt" 2>"$dir/stderr.txt"
  local rc=$?
  set -e
  local t1=$(date +%s)
  local wall=$((t1-t0))
  echo "$rc" > "$dir/rc.txt"
  local class family objsz obj holder note
  read -r class family objsz obj holder note < <(classify "$dir" "$rc")
  local b2geo=0
  if grep -q B2GEO_CANDIDATE "$dir/supervisor.log" 2>/dev/null; then b2geo=$((b2geo+1)); fi
  if grep -q INTERIOR16_REG "$dir/supervisor.log" 2>/dev/null; then b2geo=$((b2geo+2)); fi
  if grep -q GEO_PTR "$dir/supervisor.log" 2>/dev/null; then b2geo=$((b2geo+4)); fi
  local dump_mode=-
  dump_mode=$(grep -aoE 'mode=(full|bounded)' "$dir/supervisor.log" 2>/dev/null | head -1 | cut -d= -f2 || echo -)
  [ -z "$dump_mode" ] && dump_mode=-
  local dump_bytes=0
  dump_bytes=$(find "$dir" -name 'mem_*.bin' -printf '%s\n' 2>/dev/null | awk '{s+=$1} END{print s+0}')
  if [ "$family" -eq 0 ]; then
    find "$dir" -name 'mem_*.bin' -delete 2>/dev/null || true
  else
    mkdir -p "$ROOT/dumps/d2_r${round}"
    cp -a "$dir/supervisor.log" "$dir"/maps_*.txt "$ROOT/dumps/d2_r${round}/" 2>/dev/null || true
    # extract fatal lines
    grep -E 'FATAL|REGS|B2GEO|INTERIOR|GEO_PTR|DUMP_|EXE_RANGE' "$dir/supervisor.log" \
      > "$ROOT/dumps/d2_r${round}/fatal_summary.txt" 2>/dev/null || true
    ln -sfn "$dir" "$ROOT/dumps/d2_r${round}/run" 2>/dev/null || true
  fi
  echo -e "d2\t${round}\t${rc}\t${class}\t${family}\t${objsz}\t${obj}\t${holder}\t${b2geo}\t${dump_mode}\t${dump_bytes}\t${wall}\t${note}" | tee -a "$MATRIX"
}

END=$((START+N-1))
for r in $(seq $START $END); do
  avail=$(awk '/MemAvailable/{print int($2/1048576)}' /proc/meminfo)
  if [ "$avail" -lt 30 ]; then echo "DEFERRED_LOW_MEM avail=${avail}G round=$r"; sleep 15; fi
  avail_disk=$(df -BG / | awk 'NR==2{gsub(/G/,"",$4); print $4}')
  if [ "$avail_disk" -lt 12 ]; then echo "STOP_LOW_DISK avail=${avail_disk}G round=$r"; break; fi
  echo "--- round $r $(date -Is) mem=${avail}G disk=${avail_disk}G ---"
  run_one $r
done

echo "=== DUMP60 MATRIX ==="
cat "$MATRIX"
echo "=== CLASS HIST ==="
awk -F'\t' 'NR>1{c[$4]++} END{for(k in c) print c[k],k}' "$MATRIX" | sort -rn
echo "=== FAMILY HIT ==="
awk -F'\t' 'NR>1{n++; if($5==1)f++} END{print "family",f+0,"/",n+0}' "$MATRIX"
echo "=== DUMP60 END $(date -Is) ==="
