#!/bin/bash
# youngflip: paired A/B large-sample + pause accounting + 12-wave + rec=stw.
# A = MARK on (econ SO) / B = product default off. FOLLOW stays off.
# Cores 160-189 (190-191 blacklisted). 24 single-core slots use 160-183.
set -u
ROOT=/root/youngflip-run
A=$ROOT/so-econ
B=$ROOT/so-product
TOOLS=$ROOT/tools
NW_TRAIN=/root/promowalk-run/bin/natural_wave_notime
NW_12=/root/youngconcfollow-run/bin/natural_wave_notime
SD=/root/youngconcfollow-run/bin/survival_dense
GOLD_NW_TRAIN=1327163996318400
GOLD_NW_12=635925223159200
GOLD_SD=368685940367600
CORES_LO=160
CORES_HI=189
NSLOT=24
SOAK_PID_FILE=/root/cjpmck-run/soak_paint/soak.pid
export PATH=/usr/bin:/bin:/usr/local/bin
mkdir -p "$ROOT/logs" "$ROOT/mega" "$ROOT/econ" "$ROOT/wave12" "$ROOT/recstw"

classify_file() {
  local rc=$1 f=$2
  local t=""
  [ -f "$f" ] && t=$(cat "$f" 2>/dev/null || true)
  if [ "$rc" = 134 ]; then
    if echo "$t" | grep -q 'IsLockedState'; then echo A_locked; return; fi
    if echo "$t" | grep -q 'PREFORWARD'; then echo A2_phase; return; fi
    if echo "$t" | grep -q 'toRegion2Idx'; then echo A3_toreg; return; fi
    echo A4_other134; return
  fi
  if [ "$rc" = 139 ]; then echo B_segv; return; fi
  if [ "$rc" = 1 ]; then echo D_rc1; return; fi
  if [ "$rc" = 124 ]; then echo TO; return; fi
  if [ "$rc" = 0 ]; then echo C_drift; return; fi
  echo other_rc$rc
}

run_one_pair() {
  local spec=$1
  IFS=, read -r arm so load heap i nme bin gold timeout_s slot_id <<< "$spec"
  local slice=$(( (slot_id - 1) % NSLOT ))
  local c0=$(( CORES_LO + slice ))
  local tag="${arm}_${load}_${heap}_$i"
  local f=$OUT/o_${tag}.txt
  local t0 t1 wall rc
  t0=$(date +%s.%N)
  LD_LIBRARY_PATH=$so cjHeapSize=$heap CANGJIE_CJHEAP_SIZE=$heap \
    MRT_GC_LOG=1 MRT_LOG_LEVEL=i \
    taskset -c "$c0" timeout "$timeout_s" "$bin" >"$f" 2>&1
  rc=$?
  t1=$(date +%s.%N)
  wall=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.3f", b-a}')
  local out
  out=$(grep -oE "_OK checksum=[0-9]+" "$f" | tail -1)
  local ok=n
  [ "$out" = "_OK checksum=$gold" ] && ok=y
  python3 "$TOOLS/youngflip_metrics.py" "$f" "$OUT/met_${tag}.tsv" "$wall" "$rc"
  if [ "$ok" = y ] && [ "$rc" = 0 ]; then
    rm -f "$f"
    echo -e "$arm\t$load\t$heap\t$i\t$rc\t${ok}\tGOLD"
  else
    mv "$f" "$OUT/FAIL_${tag}.txt" 2>/dev/null || true
    local cls
    cls=$(classify_file "$rc" "$OUT/FAIL_${tag}.txt")
    echo -e "$arm\t$load\t$heap\t$i\t$rc\t${ok}\t$cls"
  fi
}

run_paired() {
  local load=$1 heap=$2 n=$3 bin=$4 gold=$5 timeout_s=$6
  local name="pair_${load}_${heap}"
  OUT=$ROOT/mega/$name
  mkdir -p "$OUT"
  if [ -f "$OUT/DONE" ]; then
    echo "RUN_SKIP $name"
    cat "$OUT"/summary_*.txt 2>/dev/null || true
    return 0
  fi
  echo "PAIR_START $name N=$n $(date -Is)" | tee -a "$ROOT/logs/driver.log"
  rm -f "$OUT"/o_*.txt "$OUT"/FAIL_*.txt "$OUT"/met_*.tsv "$OUT"/mega.tsv
  export -f run_one_pair classify_file
  export OUT NSLOT CORES_LO CORES_HI TOOLS
  local specs=()
  local i slot_id=0
  for i in $(seq 1 "$n"); do
    if [ $((i % 2)) -eq 1 ]; then
      slot_id=$((slot_id + 1))
      specs+=("A,$A,$load,$heap,$i,$name,$bin,$gold,$timeout_s,$slot_id")
      slot_id=$((slot_id + 1))
      specs+=("B,$B,$load,$heap,$i,$name,$bin,$gold,$timeout_s,$slot_id")
    else
      slot_id=$((slot_id + 1))
      specs+=("B,$B,$load,$heap,$i,$name,$bin,$gold,$timeout_s,$slot_id")
      slot_id=$((slot_id + 1))
      specs+=("A,$A,$load,$heap,$i,$name,$bin,$gold,$timeout_s,$slot_id")
    fi
  done
  printf '%s\n' "${specs[@]}" | xargs -P "$NSLOT" -I{} bash -c 'run_one_pair "$@"' _ {} > "$OUT/mega.tsv"
  python3 "$TOOLS/youngflip_sum.py" "$OUT" "$name"
  touch "$OUT/DONE"
  echo "PAIR_OK $name $(date -Is)" | tee -a "$ROOT/logs/driver.log"
}

id_block() {
  echo "===== ID $(date -Is) ====="
  echo "HEAD_SRC=$(cat $ROOT/HEAD 2>/dev/null || echo unknown)"
  local arm so
  for arm in so-econ so-product; do
    so=$ROOT/$arm/libcangjie-runtime.so
    echo "$arm SHA=$(sha256sum $so | awk '{print $1}')"
    echo "$arm STAMP=$(strings $so | grep -o 'CJRT-COMMIT:[0-9a-f-]*' | head -1)"
    echo "$arm MASK=$(nm -D $so | grep -c g_cjLoadBadMask || true)"
  done
  echo "NW_TRAIN=$(sha256sum $NW_TRAIN)"
  echo "NW_12=$(sha256sum $NW_12)"
  echo "SD=$(sha256sum $SD)"
}

cmd=${1:-help}
case "$cmd" in
  id) id_block ;;
  pair-nw1g) run_paired nw 1GB "${2:-100}" "$NW_TRAIN" "$GOLD_NW_TRAIN" 300 ;;
  pair-nw256) run_paired nw 256MB "${2:-50}" "$NW_12" "$GOLD_NW_12" 180 ;;
  pair-sd256) run_paired sd 256MB "${2:-50}" "$SD" "$GOLD_SD" 180 ;;
  pair-sd1g) run_paired sd 1GB "${2:-8}" "$SD" "$GOLD_SD" 180 ;;
  wave12)
    n=${2:-8}
    OUT=$ROOT/wave12
    mkdir -p "$OUT"
    echo "WAVE12_START N=$n $(date -Is)"
    for i in $(seq 1 "$n"); do
      f=$OUT/A_$i.txt
      t0=$(date +%s.%N)
      LD_LIBRARY_PATH=$A cjHeapSize=1GB CANGJIE_CJHEAP_SIZE=1GB \
        MRT_GC_LOG=1 MRT_LOG_LEVEL=i \
        taskset -c $CORES_LO-$CORES_HI timeout 180 "$NW_12" >"$f" 2>&1
      rc=$?
      t1=$(date +%s.%N)
      wall=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.3f", b-a}')
      cs=$(grep -oE "_OK checksum=[0-9]+" "$f" | tail -1)
      python3 "$TOOLS/youngflip_metrics.py" "$f" "$OUT/met_A_$i.tsv" "$wall" "$rc"
      echo "wave12 A_$i rc=$rc $cs wall=$wall"
    done
    echo "WAVE12_END $(date -Is)"
    ;;
  recstw)
    n=${2:-5}
    OUT=$ROOT/recstw
    mkdir -p "$OUT"
    echo "RECSTW_START N=$n $(date -Is) (soak already STOP'd by campaign)"
    for i in $(seq 1 "$n"); do
      f=$OUT/A_$i.txt
      t0=$(date +%s.%N)
      LD_LIBRARY_PATH=$A cjHeapSize=1GB CANGJIE_CJHEAP_SIZE=1GB \
        MRT_GC_LOG=1 MRT_LOG_LEVEL=i \
        taskset -c $CORES_LO-$CORES_HI timeout 180 "$SD" >"$f" 2>&1
      rc=$?
      t1=$(date +%s.%N)
      wall=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.3f", b-a}')
      python3 "$TOOLS/youngflip_metrics.py" "$f" "$OUT/met_A_$i.tsv" "$wall" "$rc"
      python3 -c '
import sys
held=[]
for line in open(sys.argv[1], errors="replace"):
    if "[GCLOG]" in line and "rec=stw" in line and "reason=young_collection" in line:
        for tok in line.split():
            if tok.startswith("held_ns="):
                try: held.append(int(tok.split("=",1)[1]))
                except: pass
held.sort()
med=held[len(held)//2] if held else 0
open(sys.argv[2],"w").write(str(med)+"\n")
print("held_n",len(held),"med",med)
' "$f" "$OUT/held_A_$i.txt"
      echo "recstw A_$i rc=$rc wall=$wall held_med=$(cat $OUT/held_A_$i.txt)"
    done
    python3 -c '
import pathlib, statistics, sys
root=pathlib.Path(sys.argv[1])
xs=[]
for p in sorted(root.glob("held_A_*.txt")):
    xs.append(int(p.read_text().strip() or 0))
print("recstw held_meds", xs)
print("recstw cand_med", statistics.median(xs) if xs else "NA")
' "$OUT"
    echo "RECSTW_END $(date -Is)"
    ;;
  *)
    echo "usage: $0 {id|pair-nw1g|pair-nw256|pair-sd256|pair-sd1g|wave12|recstw} [N]"
    exit 2
    ;;
esac
