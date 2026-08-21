#!/bin/bash
# youngflip: paired A/B large-sample + pause accounting + 12-wave + rec=stw.
# A = MARK on (econ SO) / B = product default off. FOLLOW stays off.
# Cores 160-189 (190-191 blacklisted). 15×2-core slots.
set -u
ROOT=/root/youngflip-run
A=$ROOT/so-econ
B=$ROOT/so-product
NW_TRAIN=/root/promowalk-run/bin/natural_wave_notime
NW_12=/root/youngconcfollow-run/bin/natural_wave_notime
SD=/root/youngconcfollow-run/bin/survival_dense
GOLD_NW_TRAIN=1327163996318400
GOLD_NW_12=635925223159200
GOLD_SD=368685940367600
CORES_LO=160
CORES_HI=189
NSLOT=15
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

parse_metrics() {
  local stderr=$1 out=$2 wall=$3 rc=$4
  python3 - "$stderr" "$out" "$wall" "$rc" <<'PY'
import sys
path, out, wall, rc = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
held=[]
reasons={}
cycles_minor=0
gctrigger=0
stw2=0
si=""
try:
    lines=open(path, errors="replace")
except FileNotFoundError:
    lines=[]
for line in lines:
    if "[GCLOG]" in line and "rec=stw" in line and "held_ns=" in line:
        rec={}
        for tok in line.split():
            if "=" in tok:
                k,v=tok.split("=",1)
                rec[k]=v
        reason=rec.get("reason","")
        reasons[reason]=reasons.get(reason,0)+1
        if reason=="young_collection":
            try:
                held.append(int(rec["held_ns"]))
            except Exception:
                pass
    if "[GCLOG]" in line and "rec=cycle" in line and "kind=minor" in line:
        cycles_minor += 1
    if "[GCV2][gctrigger]" in line:
        gctrigger += 1
    if "stw2_fixpoint" in line:
        stw2 += 1
    if "si_addr=" in line and not si:
        parts=[t for t in line.split() if t.startswith("si_addr=") or t.startswith("si_code=")]
        if parts:
            si=" ".join(parts[:4])
held.sort()
med = held[len(held)//2] if held else 0
if held and len(held)%2==0:
    med = (held[len(held)//2-1]+held[len(held)//2])//2
p99 = held[int(0.99*(len(held)-1))] if held else 0
tot = sum(held) if held else 0
rs = ",".join(f"{k}:{v}" for k,v in sorted(reasons.items()))
open(out,"w").write(
    f"wall={wall}\trc={rc}\tyoung_n={len(held)}\tyoung_med_ns={int(med)}\tyoung_p99_ns={int(p99)}\t"
    f"young_sum_ns={tot}\tminor_cycles={cycles_minor}\tstw2={stw2}\tgctrigger={gctrigger}\t"
    f"si={si or '-'}\treasons={rs}\n")
PY
}

summarize_cell() {
  local OUT=$1 name=$2
  python3 - "$OUT" "$name" <<'PY'
import sys, collections, os, re, statistics
out, name = sys.argv[1], sys.argv[2]
rows=[]
with open(os.path.join(out,"mega.tsv")) as f:
    for line in f:
        p=line.rstrip("\n").split("\t")
        if len(p)>=6:
            rows.append(p)
cls=collections.Counter(); rc=collections.Counter(); si=collections.Counter(); abort=collections.Counter()
ok=0
walls=[]; young_med=[]; young_p99=[]; young_sum=[]; young_n=[]; minor=[]; stw2=[]
for p in rows:
    rci=p[3]; okf=p[4]; c=p[5]
    rc[rci]+=1
    if okf=="y":
        ok+=1; cls["GOLD"]+=1
    else:
        cls[c]+=1
    fn=os.path.join(out, f"FAIL_{p[0]}_{p[1]}_{p[2]}.txt")
    t=""
    if os.path.exists(fn):
        t=open(fn, errors="replace").read()
    if rci=="139":
        m=re.findall(r"si_code=\S+|si_addr=\S+", t)
        si[" ".join(m[:4]) if m else "no_si"]+=1
        if "si_code=" in t:
            abort["si_"+ (re.search(r"si_code=\S+", t).group(0) if re.search(r"si_code=\S+", t) else "?")]+=1
    if rci=="134":
        if "IsLockedState" in t: abort["IsLockedState"]+=1
        elif "PREFORWARD" in t: abort["phase"]+=1
        elif "toRegion2Idx" in t: abort["toRegion2Idx"]+=1
        else: abort["other134"]+=1
    if rci=="1":
        if "Cannot allocate" in t or "Out of memory" in t: abort["oom"]+=1
        else: abort["rc1_other"]+=1
    met=os.path.join(out, f"met_{p[0]}_{p[1]}_{p[2]}.tsv")
    if os.path.exists(met):
        rec={}
        for tok in open(met).read().split():
            if "=" in tok:
                k,v=tok.split("=",1); rec[k]=v
        def f(k):
            try: return float(rec.get(k,0))
            except: return 0.0
        walls.append(f("wall")); young_med.append(f("young_med_ns")); young_p99.append(f("young_p99_ns"))
        young_sum.append(f("young_sum_ns")); young_n.append(f("young_n")); minor.append(f("minor_cycles")); stw2.append(f("stw2"))
n=len(rows)
def med(xs):
    return statistics.median(xs) if xs else float("nan")
line=(
    f"ARM {name} n={n} golden={ok} GOLD={cls['GOLD']} "
    f"A_locked={cls['A_locked']} A2_phase={cls['A2_phase']} A3_toreg={cls['A3_toreg']} "
    f"A4_other134={cls['A4_other134']} B_segv={cls['B_segv']} C_drift={cls['C_drift']} "
    f"D_rc1={cls['D_rc1']} TO={cls['TO']} "
    f"other={sum(v for k,v in cls.items() if k not in ('GOLD','A_locked','A2_phase','A3_toreg','A4_other134','B_segv','C_drift','D_rc1','TO'))} "
    f"rc={dict(rc)} abort={dict(abort)} si_top={si.most_common(5)}"
)
econ=(
    f"ECON {name} n={n} wall_med={med(walls):.3f} young_med_ms={med(young_med)/1e6:.3f} "
    f"young_p99_ms={med(young_p99)/1e6:.3f} young_sum_s={med(young_sum)/1e9:.3f} "
    f"young_n_med={med(young_n)} minor_cyc_med={med(minor)} stw2_med={med(stw2)} "
    f"stw2_pos={sum(1 for x in stw2 if x>0)}/{len(stw2)}"
)
open(os.path.join(out,"summary.txt"),"w").write(line+"\n"+econ+"\n")
print(line)
print(econ)
PY
}

run_cell() {
  local arm=$1 so=$2 load=$3 heap=$4 n=$5 bin=$6 gold=$7 timeout_s=$8
  local name="${arm}_${load}_${heap}"
  local OUT=$ROOT/mega/$name
  mkdir -p "$OUT"
  if [ -f "$OUT/DONE" ]; then
    echo "RUN_SKIP $name"
    cat "$OUT/summary.txt"
    return 0
  fi
  echo "RUN_START $name N=$n $(date -Is)" | tee -a "$ROOT/logs/driver.log"
  rm -f "$OUT"/o_*.txt "$OUT"/FAIL_*.txt "$OUT"/met_*.tsv "$OUT"/mega.tsv
  run_one() {
    local spec=$1
    IFS=, read -r arm so load heap i nme bin gold timeout_s <<< "$spec"
    local slice=$(( (i - 1) % NSLOT ))
    local c0=$(( CORES_LO + slice * 2 ))
    local c1=$(( c0 + 1 ))
    if [ "$c1" -gt "$CORES_HI" ]; then c1=$CORES_LO; fi
    local f=$OUT/o_${load}_${heap}_$i.txt
    local t0 t1 wall rc
    t0=$(date +%s.%N)
    LD_LIBRARY_PATH=$so cjHeapSize=$heap CANGJIE_CJHEAP_SIZE=$heap \
      MRT_GC_LOG=1 MRT_LOG_LEVEL=i \
      taskset -c $c0-$c1 timeout "$timeout_s" "$bin" >"$f" 2>&1
    rc=$?
    t1=$(date +%s.%N)
    wall=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.3f", b-a}')
    local out
    out=$(grep -oE "_OK checksum=[0-9]+" "$f" | tail -1)
    local ok=n
    [ "$out" = "_OK checksum=$gold" ] && ok=y
    parse_metrics "$f" "$OUT/met_${load}_${heap}_$i.tsv" "$wall" "$rc"
    if [ "$ok" = y ] && [ "$rc" = 0 ]; then
      rm -f "$f"
      echo -e "$load\t$heap\t$i\t$rc\t${ok}\tGOLD"
    else
      mv "$f" "$OUT/FAIL_${load}_${heap}_$i.txt" 2>/dev/null || true
      local cls
      cls=$(classify_file "$rc" "$OUT/FAIL_${load}_${heap}_$i.txt")
      echo -e "$load\t$heap\t$i\t$rc\t${ok}\t$cls"
    fi
  }
  export -f run_one classify_file parse_metrics
  export OUT NSLOT CORES_LO CORES_HI
  local specs=()
  local i
  for i in $(seq 1 "$n"); do
    specs+=("$arm,$so,$load,$heap,$i,$name,$bin,$gold,$timeout_s")
  done
  printf '%s\n' "${specs[@]}" | xargs -P "$NSLOT" -I{} bash -c 'run_one "$@"' _ {} > "$OUT/mega.tsv"
  summarize_cell "$OUT" "$name"
  touch "$OUT/DONE"
  echo "RUN_OK $name $(date -Is)" | tee -a "$ROOT/logs/driver.log"
}

# Pair A/B in the same window: launch both arms' specs mixed, 15-wide.
run_paired() {
  local load=$1 heap=$2 n=$3 bin=$4 gold=$5 timeout_s=$6
  local name="pair_${load}_${heap}"
  local OUT=$ROOT/mega/$name
  mkdir -p "$OUT"
  if [ -f "$OUT/DONE" ]; then
    echo "RUN_SKIP $name"
    cat "$OUT/summary.txt" 2>/dev/null || true
    ls "$OUT"/summary_*.txt 2>/dev/null
    return 0
  fi
  echo "PAIR_START $name N=$n $(date -Is)" | tee -a "$ROOT/logs/driver.log"
  rm -f "$OUT"/o_*.txt "$OUT"/FAIL_*.txt "$OUT"/met_*.tsv "$OUT"/mega.tsv
  run_one_pair() {
    local spec=$1
    IFS=, read -r arm so load heap i nme bin gold timeout_s <<< "$spec"
    local slice=$(( (i - 1) % NSLOT ))
    local c0=$(( CORES_LO + slice * 2 ))
    local c1=$(( c0 + 1 ))
    [ "$c1" -gt "$CORES_HI" ] && c1=$CORES_HI
    local tag="${arm}_${load}_${heap}_$i"
    local f=$OUT/o_${tag}.txt
    local t0 t1 wall rc
    t0=$(date +%s.%N)
    LD_LIBRARY_PATH=$so cjHeapSize=$heap CANGJIE_CJHEAP_SIZE=$heap \
      MRT_GC_LOG=1 MRT_LOG_LEVEL=i \
      taskset -c $c0-$c1 timeout "$timeout_s" "$bin" >"$f" 2>&1
    rc=$?
    t1=$(date +%s.%N)
    wall=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.3f", b-a}')
    local out
    out=$(grep -oE "_OK checksum=[0-9]+" "$f" | tail -1)
    local ok=n
    [ "$out" = "_OK checksum=$gold" ] && ok=y
    parse_metrics "$f" "$OUT/met_${tag}.tsv" "$wall" "$rc"
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
  export -f run_one_pair classify_file parse_metrics
  export OUT NSLOT CORES_LO CORES_HI
  local specs=()
  local i
  for i in $(seq 1 "$n"); do
    if [ $((i % 2)) -eq 1 ]; then
      specs+=("A,$A,$load,$heap,$i,$name,$bin,$gold,$timeout_s")
      specs+=("B,$B,$load,$heap,$i,$name,$bin,$gold,$timeout_s")
    else
      specs+=("B,$B,$load,$heap,$i,$name,$bin,$gold,$timeout_s")
      specs+=("A,$A,$load,$heap,$i,$name,$bin,$gold,$timeout_s")
    fi
  done
  printf '%s\n' "${specs[@]}" | xargs -P "$NSLOT" -I{} bash -c 'run_one_pair "$@"' _ {} > "$OUT/mega.tsv"
  python3 - "$OUT" "$name" <<'PY'
import sys, collections, os, re, statistics
out, name = sys.argv[1], sys.argv[2]
rows=[]
with open(os.path.join(out,"mega.tsv")) as f:
    for line in f:
        p=line.rstrip("\n").split("\t")
        if len(p)>=7:
            rows.append(p)
def dump(arm):
    cls=collections.Counter(); rc=collections.Counter(); si=collections.Counter(); abort=collections.Counter()
    ok=0; n=0
    walls=[]; young_med=[]; young_p99=[]; young_sum=[]; young_n=[]; minor=[]; stw2=[]
    for p in rows:
        if p[0]!=arm: continue
        n+=1
        rci=p[4]; okf=p[5]; c=p[6]
        rc[rci]+=1
        if okf=="y":
            ok+=1; cls["GOLD"]+=1
        else:
            cls[c]+=1
        fn=os.path.join(out, f"FAIL_{arm}_{p[1]}_{p[2]}_{p[3]}.txt")
        t=""
        if os.path.exists(fn):
            t=open(fn, errors="replace").read()
        if rci=="139":
            m=re.findall(r"si_code=\S+|si_addr=\S+", t)
            si[" ".join(m[:4]) if m else "no_si"]+=1
        if rci=="134":
            if "IsLockedState" in t: abort["IsLockedState"]+=1
            elif "PREFORWARD" in t: abort["phase"]+=1
            elif "toRegion2Idx" in t: abort["toRegion2Idx"]+=1
            else: abort["other134"]+=1
        if rci=="1":
            if "Cannot allocate" in t or "Out of memory" in t: abort["oom"]+=1
            else: abort["rc1_other"]+=1
        met=os.path.join(out, f"met_{arm}_{p[1]}_{p[2]}_{p[3]}.tsv")
        if os.path.exists(met):
            rec={}
            for tok in open(met).read().split():
                if "=" in tok:
                    k,v=tok.split("=",1); rec[k]=v
            def f(k):
                try: return float(rec.get(k,0))
                except: return 0.0
            walls.append(f("wall")); young_med.append(f("young_med_ns")); young_p99.append(f("young_p99_ns"))
            young_sum.append(f("young_sum_ns")); young_n.append(f("young_n")); minor.append(f("minor_cycles")); stw2.append(f("stw2"))
    def med(xs):
        return statistics.median(xs) if xs else float("nan")
    line=(
        f"ARM {arm} n={n} golden={ok} GOLD={cls['GOLD']} "
        f"A_locked={cls['A_locked']} A2_phase={cls['A2_phase']} A3_toreg={cls['A3_toreg']} "
        f"A4_other134={cls['A4_other134']} B_segv={cls['B_segv']} C_drift={cls['C_drift']} "
        f"D_rc1={cls['D_rc1']} TO={cls['TO']} "
        f"other={sum(v for k,v in cls.items() if k not in ('GOLD','A_locked','A2_phase','A3_toreg','A4_other134','B_segv','C_drift','D_rc1','TO'))} "
        f"rc={dict(rc)} abort={dict(abort)} si_top={si.most_common(5)}"
    )
    econ=(
        f"ECON {arm} n={n} wall_med={med(walls):.3f} young_med_ms={med(young_med)/1e6:.3f} "
        f"young_p99_ms={med(young_p99)/1e6:.3f} young_sum_s={med(young_sum)/1e9:.3f} "
        f"young_n_med={med(young_n)} minor_cyc_med={med(minor)} stw2_med={med(stw2)} "
        f"stw2_pos={sum(1 for x in stw2 if x>0)}/{len(stw2)}"
    )
    open(os.path.join(out,f"summary_{arm}.txt"),"w").write(line+"\n"+econ+"\n")
    print(line)
    print(econ)
dump("A"); dump("B")
PY
  touch "$OUT/DONE"
  echo "PAIR_OK $name $(date -Is)" | tee -a "$ROOT/logs/driver.log"
}

id_block() {
  echo "===== ID $(date -Is) ====="
  echo "HEAD_SRC=$(cat $ROOT/HEAD 2>/dev/null || echo unknown)"
  for arm in so-econ so-product; do
    local so=$ROOT/$arm/libcangjie-runtime.so
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
      parse_metrics "$f" "$OUT/met_A_$i.tsv" "$wall" "$rc"
      echo "wave12 A_$i rc=$rc $cs wall=$wall"
    done
    echo "WAVE12_END $(date -Is)"
    ;;
  recstw)
    n=${2:-5}
    OUT=$ROOT/recstw
    mkdir -p "$OUT"
    soak=$(cat "$SOAK_PID_FILE")
    echo "RECSTW_STOP soak.pid=$soak pgid=$(ps -o pgid= -p $soak | tr -d ' ')"
    kill -STOP -- -"$soak" || true
    echo "RECSTW_START N=$n $(date -Is)"
    for i in $(seq 1 "$n"); do
      f=$OUT/A_$i.txt
      t0=$(date +%s.%N)
      LD_LIBRARY_PATH=$A cjHeapSize=1GB CANGJIE_CJHEAP_SIZE=1GB \
        MRT_GC_LOG=1 MRT_LOG_LEVEL=i \
        taskset -c $CORES_LO-$CORES_HI timeout 180 "$SD" >"$f" 2>&1
      rc=$?
      t1=$(date +%s.%N)
      wall=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.3f", b-a}')
      parse_metrics "$f" "$OUT/met_A_$i.tsv" "$wall" "$rc"
      python3 - "$f" "$OUT/held_A_$i.txt" <<'PY'
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
PY
      echo "recstw A_$i rc=$rc wall=$wall held_med=$(cat $OUT/held_A_$i.txt)"
    done
    echo "RECSTW_CONT soak.pid=$soak"
    kill -CONT -- -"$soak" || true
    python3 - "$OUT" <<'PY'
import pathlib, statistics
root=pathlib.Path(sys.argv[1])
xs=[]
for p in sorted(root.glob("held_A_*.txt")):
    xs.append(int(p.read_text().strip() or 0))
print("recstw held_meds", xs)
print("recstw cand_med", statistics.median(xs) if xs else "NA")
PY
    echo "RECSTW_END $(date -Is)"
    ;;
  *)
    echo "usage: $0 {id|pair-nw1g|pair-nw256|pair-sd256|pair-sd1g|wave12|recstw} [N]"
    exit 2
    ;;
esac
