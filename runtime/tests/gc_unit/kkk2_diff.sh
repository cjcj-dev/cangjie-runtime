#!/bin/bash
# ⭐ kkk2 差分服务（0916 23:5x）：给定候选 sha，在 kkk2 自动两构型构建 + 三臂（default/filler/testable）并行跑 gc_unit，
#   与其 merge-base 主线（或指定 --base）的基线臂做 FAILED/INCOMPLETE 差集，产出 JSON + 摘要。基线按 sha 缓存在 kkk2:/root/diff_<sha12>/。
# 第四臂 managed（#708）：候选 vs 基线各跑 kkk2_managed.sh 染色目标（编译器宿主固定 H48），CAND-ONLY 语义同前三臂。
# 用法：ops/bin/kkk2_diff.sh <candidate-ref> [--base <ref>] [--repo /root/cj_build/cangjie_runtime] [--force]
# 产物：reports/DIFF-<cand12>-vs-<base12>.json / .md；kkk2:/root/diff_<sha12>/{default,testable}/ + unit-{default,filler,testable}/ + managed-runs/
set -uo pipefail
REPO=/root/cj_build/cangjie_runtime; BASE=""; FORCE=0; CAND=""
while [ $# -gt 0 ]; do case "$1" in --base) BASE=$2; shift 2;; --repo) REPO=$2; shift 2;; --force) FORCE=1; shift;; *) CAND=$1; shift;; esac; done
[ -n "$CAND" ] || { echo "用法: $0 <candidate-ref> [--base <ref>] [--force]"; exit 2; }
B=/root/cj_build/tools/box.sh; WF=/root/cj_build/ops/bin/wf_kkk2.sh; SCR=/root/cj_build/agent_scratch
git -C "$REPO" fetch -q cjcjdev 2>/dev/null || true
CS=$(git -C "$REPO" rev-parse --verify -q "$CAND^{commit}") || { echo "⛔ 候选解析不了: $CAND"; exit 2; }
if [ -z "$BASE" ]; then BS=$(git -C "$REPO" merge-base "$CS" cjcjdev/main); else BS=$(git -C "$REPO" rev-parse --verify -q "$BASE^{commit}") || { echo "⛔ base 解析不了"; exit 2; }; fi
C12=${CS:0:12}; B12=${BS:0:12}
echo "# kkk2_diff cand=$CS base=$BS $(date -Iseconds)"

# #708：两侧必须用【候选树】同一份染色目标 kkk2_managed.sh，禁止跑基线 sha 自带的旧目标脚本（否则 failed 形态差会假 CAND-ONLY）
# 0919 20:3x：harness 路径按 runner sha 分目录——共享 /root/diff_harness_708/ 曾被并行跑的另一条 kkk2_diff 覆盖（#723 棒的改版 runner 混进主线健康差分）
HTMP=$(mktemp -d "$SCR/diff_harness_708.XXXXXX")
git -C "$REPO" show "$CS:runtime/tests/gc_unit/kkk2_managed.sh" > "$HTMP/kkk2_managed.sh" || { echo "⛔ 候选无 kkk2_managed.sh"; exit 2; }
/usr/bin/grep -q "Target arm: stained" "$HTMP/kkk2_managed.sh" || { echo "⛔ 抽出的 kkk2_managed.sh 不是染色目标形态"; head -5 "$HTMP/kkk2_managed.sh"; exit 2; }
git -C "$REPO" show "$CS:tools/zstat_pillars.py" > "$HTMP/zstat_pillars.py" || { echo "⛔ 候选无 zstat_pillars.py"; exit 2; }
RUNNER_SHA256=$(sha256sum "$HTMP/kkk2_managed.sh" | awk '{print $1}')
HKEY=${RUNNER_SHA256:0:12}
HARNET=/root/diff_harness_708/$HKEY
mkdir -p "$SCR/diff_harness_708/$HKEY"; mv -f "$HTMP/kkk2_managed.sh" "$HTMP/zstat_pillars.py" "$SCR/diff_harness_708/$HKEY/"; rmdir "$HTMP"
chmod +x "$SCR/diff_harness_708/$HKEY/kkk2_managed.sh"
bash "$B" kkk2 "mkdir -p $HARNET"
bash "$B" kkk2 --put "$SCR/diff_harness_708/$HKEY/kkk2_managed.sh" $HARNET/kkk2_managed.sh
bash "$B" kkk2 --put "$SCR/diff_harness_708/$HKEY/zstat_pillars.py" $HARNET/zstat_pillars.py
echo "# pinned runner sha256=$RUNNER_SHA256 path=kkk2:$HARNET/kkk2_managed.sh"

managed_json_ok() { # 新形态：必须有 arms 键；旧目标 JSON 当缺失
  local lane=$1
  bash "$B" kkk2 "python3 -c 'import json,pathlib,sys; p=pathlib.Path(\"/root/$lane/managed-runs/kkk2_managed.json\");
d=json.loads(p.read_text()) if p.is_file() else {};
n=d.get(\"n\",0); arms=d.get(\"arms\",{});
ok=isinstance(arms,dict) and set(arms)=={\"stained\"} and isinstance(d.get(\"failed\"),list) and isinstance(n,int) and n>=3 and isinstance(d.get(\"build_fail\"),list);
ok=ok and d.get(\"runner_sha256\")==\"$RUNNER_SHA256\";
ok=ok and all(isinstance(arms.get(a,{}).get(\"runs\",{}).get(t),list) and len(arms[a][\"runs\"][t])+sum(f.get(\"arm\")==a and f.get(\"name\")==t for f in d.get(\"build_fail\",[]))==n and all(isinstance(rc,int) and rc not in (126,127,-1) for rc in arms[a][\"runs\"][t]) for a in (\"stained\",) for t in (\"finalizer\",\"segmented\",\"phase\"));
sys.exit(0 if ok else 1)'" >/dev/null 2>&1
}

run_managed() {
  local sha=$1; local lane=$2
  # Unit-cache cleanup can remove managed inputs. Restore this side's source,
  # while keeping the candidate runner pinned for both sides.
  local inputs="$SCR/$lane.managed-inputs.tar"
  git -C "$REPO" archive "$sha" runtime tools > "$inputs" || return 1
  bash "$B" kkk2 --put "$inputs" "/root/$lane/managed-inputs.tar" || return 1
  rm -f "$inputs"
  bash "$B" kkk2 "tar -xf /root/$lane/managed-inputs.tar -C /root/$lane/default && rm /root/$lane/managed-inputs.tar" || return 1
  bash "$WF" sh "$lane" "ulimit -c 0; mkdir -p /root/$lane/harness /root/$lane/managed-runs /root/$lane/tools-bundle /root/$lane/default/tools; cp -a $HARNET/kkk2_managed.sh /root/$lane/harness/kkk2_managed.sh; cp -a $HARNET/zstat_pillars.py /root/$lane/tools-bundle/zstat_pillars.py; cp -a $HARNET/zstat_pillars.py /root/$lane/default/tools/zstat_pillars.py; /usr/bin/grep -q 'Target arm: stained' /root/$lane/harness/kkk2_managed.sh || { echo '⛔ lane harness not stained-target'; exit 2; }; export LANE=/root/$lane SRCROOT=/root/$lane/default OUT=/root/$lane/managed-runs N=3; bash /root/$lane/harness/kkk2_managed.sh $sha"
}

run_arm() { # run_arm <sha> ：若 kkk2 无缓存则建+三臂；rc 0=可用
  local sha=$1; local s12=${sha:0:12}; local lane="diff_$s12"; local wt="$SCR/diff_$s12"
  local unit_ok=0 managed_ok=0
  if [ "$FORCE" = 0 ] && bash "$B" kkk2 "test -s /root/$lane/unit-default/run.rc -a -s /root/$lane/unit-filler/run.rc -a -s /root/$lane/unit-testable/run.rc -a -s /root/$lane/default/build/runtime-staging/lib/x86_64_Release/libcangjie-runtime.so -a -s /root/$lane/default/build/runtime-staging/lib/x86_64_Release/libboundscheck.so" >/dev/null 2>&1; then unit_ok=1; fi
  if [ "$FORCE" = 0 ] && managed_json_ok "$lane"; then managed_ok=1; fi
  if [ "$unit_ok" = 1 ] && [ "$managed_ok" = 1 ]; then echo "# $lane: 命中缓存"; return 0; fi
  if [ "$unit_ok" = 0 ]; then
    rm -rf "$wt"; git -C "$REPO" worktree add -q --detach "$wt" "$sha" || { echo "⛔ worktree add 失败 $sha"; return 1; }
    echo "# $lane: 构建两构型…"; bash "$WF" build "$wt" "$lane" 2>&1 | /usr/bin/grep -E "^== |⛔|first errors" | head -6
    local brc; brc=$(bash "$B" kkk2 "cat /root/$lane/default-build.rc /root/$lane/testable-build.rc 2>/dev/null | tr '\n' ' '")
    case "$brc" in "0 0 "*) ;; *) echo "⛔ $lane 构建 rc=[$brc]"; git -C "$REPO" worktree remove --force "$wt"; return 1;; esac
    echo "# $lane: 三臂+managed 并行…"
    bash "$WF" unit "$lane" default  > "$SCR/$lane.unit-default.log"  2>&1 &
    bash "$WF" unit "$lane" filler   > "$SCR/$lane.unit-filler.log"   2>&1 &
    bash "$WF" unit "$lane" testable > "$SCR/$lane.unit-testable.log" 2>&1 &
    run_managed "$sha" "$lane" > "$SCR/$lane.managed.log" 2>&1 &
    wait
    git -C "$REPO" worktree remove --force "$wt" 2>/dev/null
  else
    echo "# $lane: unit 缓存命中，只补跑 managed（候选染色目标脚本）"
    run_managed "$sha" "$lane" > "$SCR/$lane.managed.log" 2>&1
  fi
  local a t=0
  if [ "$unit_ok" = 0 ]; then
    while [ $t -lt 2400 ]; do
      if bash "$B" kkk2 "test -s /root/$lane/unit-default/run.rc -a -s /root/$lane/unit-filler/run.rc -a -s /root/$lane/unit-testable/run.rc" >/dev/null 2>&1; then break; fi
      sleep 30; t=$((t+30))
    done
  fi
  for a in default filler testable; do
    if ! bash "$B" kkk2 "test -s /root/$lane/unit-$a/run.rc" >/dev/null 2>&1; then
      echo "# $lane: 臂 $a 40 分钟仍无 run.rc ⇒ 串行补跑一次"; bash "$WF" unit "$lane" "$a" > "$SCR/$lane.unit-$a.retry.log" 2>&1
    fi
  done
  if ! managed_json_ok "$lane"; then
    echo "# $lane: managed JSON 缺失或旧目标形态 ⇒ 用候选染色目标脚本串行补跑"
    run_managed "$sha" "$lane" > "$SCR/$lane.managed.retry.log" 2>&1
  fi
  # 保留 default/build/runtime-staging 与 gc_unit，供 managed 补跑/复核；只删大体积中间物
  bash "$B" kkk2 "cd /root/$lane && rm -rf testable/build default/runtime/src testable/runtime source.tar.gz unit-*/build_standalone unit-*/objects unit-*/test-logs unit-*/ohos_host_runroot 2>/dev/null; du -sh /root/$lane | cut -f1"
  bash "$B" kkk2 "for a in default filler testable; do printf '%s rc=%s ' \$a \$(cat /root/$lane/unit-\$a/run.rc 2>/dev/null || echo NA); done; echo"
}
run_arm "$BS"; run_arm "$CS"

# 拉回三臂 FAILED/INCOMPLETE 名单与身份
mkdir -p "$SCR/diffout"; OUT="$SCR/diffout/$C12-vs-$B12"; rm -rf "$OUT"; mkdir -p "$OUT"
for who in cand base; do sha=$CS; [ $who = base ] && sha=$BS; lane="diff_${sha:0:12}"
  bash "$B" kkk2 "cd /root/$lane && for a in default filler testable; do echo \"== \$a rc=\$(cat unit-\$a/run.rc 2>/dev/null)\"; grep -E '^\\[ *(FAILED|INCOMPLETE) *\\] [A-Za-z]' unit-\$a/run.log 2>/dev/null | sed -E 's/^\\[ *(FAILED|INCOMPLETE) *\\] +//; s/ .*//' | sort -u; echo \"== total \$(grep -E '^\[==========\] [0-9]+ tests' unit-\$a/run.log | tail -1)\"; done; python3 -c 'import json,pathlib; p=pathlib.Path(\"managed-runs/kkk2_managed.json\");
ok=p.exists();
d=json.loads(p.read_text()) if ok else {};
n=d.get(\"n\",0); arms=d.get(\"arms\",{});
shape=isinstance(arms,dict) and set(arms)=={\"stained\"} and isinstance(d.get(\"failed\"),list) and isinstance(n,int) and n>=3 and isinstance(d.get(\"build_fail\"),list);
shape=shape and d.get(\"runner_sha256\")==\"$RUNNER_SHA256\";
shape=shape and all(isinstance(arms.get(a,{}).get(\"runs\",{}).get(t),list) and len(arms[a][\"runs\"][t])+sum(f.get(\"arm\")==a and f.get(\"name\")==t for f in d.get(\"build_fail\",[]))==n and all(isinstance(rc,int) and rc not in (126,127,-1) for rc in arms[a][\"runs\"][t]) for a in (\"stained\",) for t in (\"finalizer\",\"segmented\",\"phase\"));
print(\"== managed rc=\" + (\"0\" if shape else \"NA\"));
print(\"== build_fail \" + json.dumps(d.get(\"build_fail\",[])));
fails=list(d.get(\"failed\") or []) if shape else [\"managed/SHAPE\"];
print(\"== total tests 3\");
[print(x) for x in fails];
print(\"== ident runner_sha256=\" + str(d.get(\"runner_sha256\",\"\")));
print(\"== ident cangjie_home=\" + str(d.get(\"cangjie_home\",\"\")));
print(\"== ident h48_rt=\" + str(d.get(\"h48_rt\",\"\")));
print(\"== ident stained_rt=\" + str(d.get(\"stained_rt\",\"\")))'; echo '== so'; cat default-so.sha256 testable-so.sha256 2>/dev/null | sed -E 's#/root/[^ ]*/build/#build/#'" > "$OUT/$who.txt" 2>/dev/null
done
python3 - "$OUT" "$CS" "$BS" "$RUNNER_SHA256" "$HARNET" <<'PY'
import sys,re,json,os
out,cs,bs,runner_sha,harnet=sys.argv[1:6]
def parse(p):
    arms={}; cur=None; so=[]; ident={}
    for line in open(p):
        line=line.rstrip('\n')
        m=re.match(r'^== (default|filler|testable|managed) rc=(\S*)',line)
        if m: cur=m.group(1); arms[cur]={'rc':m.group(2),'failed':set(),'total':'','build_fail':[]}; continue
        if line.startswith('== build_fail '): arms[cur]['build_fail']=json.loads(line[len('== build_fail '):]); continue
        if line.startswith('== total'): arms[cur]['total']=line[9:].strip(); continue
        mi=re.match(r'^== ident (runner_sha256|cangjie_home|h48_rt|stained_rt)=(.*)$',line)
        if mi: ident[mi.group(1)]=mi.group(2); continue
        if line=='== so': cur='so'; continue
        if cur=='so': so.append(line); continue
        if cur and line: arms[cur]['failed'].add(line)
    return arms,so,ident
c,cso,cident=parse(f'{out}/cand.txt'); b,bso,bident=parse(f'{out}/base.txt')
res={'candidate':cs,'base':bs,'arms':{},'positive_control':{},
     'managed_runner':{
         'sha256':runner_sha,
         'cand_runner_sha256':cident.get('runner_sha256',''),
         'base_runner_sha256':bident.get('runner_sha256',''),
         'path':'kkk2:'+harnet+'/kkk2_managed.sh',
         'cangjie_home':cident.get('cangjie_home') or bident.get('cangjie_home') or '',
         'h48_rt':cident.get('h48_rt') or bident.get('h48_rt') or '',
         'stained_rt':cident.get('stained_rt') or bident.get('stained_rt') or '',
     }}
for a in ('default','filler','testable','managed'):
    ca=c.get(a,{'rc':'NA','failed':set(),'total':''}); ba=b.get(a,{'rc':'NA','failed':set(),'total':''})
    res['arms'][a]={'cand_rc':ca['rc'],'base_rc':ba['rc'],'cand_total':ca['total'],'base_total':ba['total'],
        'cand_only':sorted(ca['failed']-ba['failed']),'base_only':sorted(ba['failed']-ca['failed']),'common':len(ca['failed']&ba['failed']),
        'cand_failed_n':len(ca['failed']),'base_failed_n':len(ba['failed']),
        'build_fail': {'cand': ca.get('build_fail',[]) or (['unit build rc=123'] if ca['rc']=='123' else []),
                       'base': ba.get('build_fail',[]) or (['unit build rc=123'] if ba['rc']=='123' else [])}}
    def broken(x):  # rc 不是 0/1（如 123=编译失败、124=超时）或跑了却没有 gtest 总数行 ⇒ 该臂不可判
        return x['rc'] not in ('0','1') or not x['total']
    if any(res['arms'][a]['build_fail'].values()) or ca['rc'] in ('','NA') or ba['rc'] in ('','NA'):
        res['arms'][a]['cand_only']=None; res['arms'][a]['base_only']=None; res['arms'][a]['status']='NOT_RUN'; res['positive_control'][a]='NOT_RUN（臂未跑，⛔ 不得据此宣称独红为 0）'
    elif broken(ca) or broken(ba):
        res['arms'][a]['status']=f"BROKEN(cand_rc={ca['rc']},base_rc={ba['rc']})"; res['positive_control'][a]='BROKEN（构建/运行未完成，failed=0 是假的，⛔ 不得据此宣称独红为 0）'
    else:
        res['arms'][a]['status']='ran'; res['positive_control'][a]='sets differ' if ca['failed']!=ba['failed'] else ('identical sets' if ca['failed'] else 'both empty')
res['cand_so']=cso; res['base_so']=bso
json.dump(res,open(f'{out}/DIFF.json','w'),ensure_ascii=False,indent=1)
mr=res['managed_runner']
md=[f"# DIFF {cs[:12]} vs {bs[:12]}","",
    f"runner_sha256={mr['sha256']} path={mr['path']}",
    f"cand_runner_sha256={mr['cand_runner_sha256']} base_runner_sha256={mr['base_runner_sha256']}",
    f"CANGJIE_HOME={mr['cangjie_home']}",
    f"H48={mr['h48_rt']}",
    f"stained={mr['stained_rt']}",""]
not_run=[a for a,v in res['arms'].items() if v.get('status')!='ran']
if not_run: md.append(f"⛔ 未跑/未完成的臂: {[(a,res['arms'][a]['status']) for a in not_run]} ⇒ 本 DIFF 对这些臂不可作为验收证据（0917 实撞：P09 testable 编译失败 rc=123 被读成 CAND-ONLY=0 放行）")
for a,v in res['arms'].items():
    md.append(f"## {a}: status={v['status']} · cand rc={v['cand_rc']} failed={v['cand_failed_n']} · base rc={v['base_rc']} failed={v['base_failed_n']} · common={v['common']} · CAND-ONLY={len(v['cand_only']) if v['cand_only'] is not None else 'NOT_COMPUTED'} BASE-ONLY={len(v['base_only']) if v['base_only'] is not None else 'NOT_COMPUTED'} · 阳性对照={res['positive_control'][a]}")
    for t in v['cand_only'] or []: md.append(f"- CAND-ONLY {t}")
    for t in (v['base_only'] or [])[:20]: md.append(f"- base-only {t}")
open(f'{out}/DIFF.md','w').write('\n'.join(md)+'\n')
print('\n'.join(md[:40]))
sys.exit(3 if not_run else 0)
PY
DRC=$?
cp "$OUT/DIFF.json" "/root/cj_build/reports/DIFF-$C12-vs-$B12.json"; cp "$OUT/DIFF.md" "/root/cj_build/reports/DIFF-$C12-vs-$B12.md"
echo "# 产物: /root/cj_build/reports/DIFF-$C12-vs-$B12.{json,md}  原始名单: $OUT/{cand,base}.txt  kkk2: /root/diff_$C12 /root/diff_$B12"
exit $DRC
