#!/bin/bash
# ⭐ kkk2 差分服务（0916 23:5x）：给定候选 sha，在 kkk2 自动两构型构建 + 三臂（default/filler/testable）并行跑 gc_unit，
#   与其 merge-base 主线（或指定 --base）的基线臂做 FAILED/INCOMPLETE 差集，产出 JSON + 摘要。基线按 sha 缓存在 kkk2:/root/diff_<sha12>/。
# 第四臂 managed（#708）：候选 vs 基线各跑 kkk2_managed.sh 两宿主（H48 + stained staging），CAND-ONLY 语义同前三臂。
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

run_arm() { # run_arm <sha> ：若 kkk2 无缓存则建+三臂；rc 0=可用
  local sha=$1; local s12=${sha:0:12}; local lane="diff_$s12"; local wt="$SCR/diff_$s12"
  if [ "$FORCE" = 0 ] && bash "$B" kkk2 "test -s /root/$lane/unit-default/run.rc -a -s /root/$lane/unit-filler/run.rc -a -s /root/$lane/unit-testable/run.rc -a -s /root/$lane/managed-runs/kkk2_managed.json" >/dev/null 2>&1; then
    echo "# $lane: 命中缓存"; return 0; fi
  rm -rf "$wt"; git -C "$REPO" worktree add -q --detach "$wt" "$sha" || { echo "⛔ worktree add 失败 $sha"; return 1; }
  echo "# $lane: 构建两构型…"; bash "$WF" build "$wt" "$lane" 2>&1 | /usr/bin/grep -E "^== |⛔|first errors" | head -6
  local brc; brc=$(bash "$B" kkk2 "cat /root/$lane/default-build.rc /root/$lane/testable-build.rc 2>/dev/null | tr '\n' ' '")
  case "$brc" in "0 0 "*) ;; *) echo "⛔ $lane 构建 rc=[$brc]"; git -C "$REPO" worktree remove --force "$wt"; return 1;; esac
  echo "# $lane: 三臂+managed 并行…"
  bash "$WF" unit "$lane" default  > "$SCR/$lane.unit-default.log"  2>&1 &
  bash "$WF" unit "$lane" filler   > "$SCR/$lane.unit-filler.log"   2>&1 &
  bash "$WF" unit "$lane" testable > "$SCR/$lane.unit-testable.log" 2>&1 &
  bash "$WF" sh "$lane" "ulimit -c 0; export LANE=/root/$lane SRCROOT=/root/$lane/default OUT=/root/$lane/managed-runs N=3 CANGJIE_HOME=/root/sdkdepot/945fe3e8f023-fa13e8d5c17b; bash /root/$lane/default/runtime/tests/gc_unit/kkk2_managed.sh $sha" > "$SCR/$lane.managed.log" 2>&1 &
  wait
  # ssh 会话可能先于远端跑完就断（ControlMaster 冲突时尤甚）⇒ 轮询 run.rc 最多 40 分钟，再对仍缺的臂串行补跑一次
  local a t=0
  while [ $t -lt 2400 ]; do
    if bash "$B" kkk2 "test -s /root/$lane/unit-default/run.rc -a -s /root/$lane/unit-filler/run.rc -a -s /root/$lane/unit-testable/run.rc -a -s /root/$lane/managed-runs/kkk2_managed.json" >/dev/null 2>&1; then break; fi
    sleep 30; t=$((t+30))
  done
  for a in default filler testable; do
    if ! bash "$B" kkk2 "test -s /root/$lane/unit-$a/run.rc" >/dev/null 2>&1; then
      echo "# $lane: 臂 $a 40 分钟仍无 run.rc ⇒ 串行补跑一次"; bash "$WF" unit "$lane" "$a" > "$SCR/$lane.unit-$a.retry.log" 2>&1
    fi
  done
  git -C "$REPO" worktree remove --force "$wt" 2>/dev/null
  # 缓存只留结果：删构建树与 unit 中间物（保留 run.log/run.rc/*.sha256/*.txt），每个 sha 约省 3G
  bash "$B" kkk2 "cd /root/$lane && rm -rf default/build testable/build default/runtime testable/runtime source.tar.gz unit-*/build_standalone unit-*/objects unit-*/test-logs unit-*/ohos_host_runroot 2>/dev/null; du -sh /root/$lane | cut -f1"
  bash "$B" kkk2 "for a in default filler testable; do printf '%s rc=%s ' \$a \$(cat /root/$lane/unit-\$a/run.rc 2>/dev/null || echo NA); done; echo"
}
run_arm "$BS"; run_arm "$CS"

# 拉回三臂 FAILED/INCOMPLETE 名单与身份
mkdir -p "$SCR/diffout"; OUT="$SCR/diffout/$C12-vs-$B12"; rm -rf "$OUT"; mkdir -p "$OUT"
for who in cand base; do sha=$CS; [ $who = base ] && sha=$BS; lane="diff_${sha:0:12}"
  bash "$B" kkk2 "cd /root/$lane && for a in default filler testable; do echo \"== \$a rc=\$(cat unit-\$a/run.rc 2>/dev/null)\"; grep -E '^\\[ *(FAILED|INCOMPLETE) *\\] [A-Za-z]' unit-\$a/run.log 2>/dev/null | sed -E 's/^\\[ *(FAILED|INCOMPLETE) *\\] +//; s/ .*//' | sort -u; echo \"== total \$(grep -E '^\[==========\] [0-9]+ tests' unit-\$a/run.log | tail -1)\"; done; python3 -c 'import json,pathlib; p=pathlib.Path(\"managed-runs/kkk2_managed.json\");
ok=p.exists();
print(\"== managed rc=\" + (\"0\" if ok else \"NA\"));
d=json.loads(p.read_text()) if ok else {\"failed\":[\"managed/MISSING\"]};
fails=d.get(\"failed\") or [];
print(\"== total tests 6\");
[print(x) for x in fails]'; echo '== so'; cat default-so.sha256 testable-so.sha256 2>/dev/null | sed -E 's#/root/[^ ]*/build/#build/#'" > "$OUT/$who.txt" 2>/dev/null
done
python3 - "$OUT" "$CS" "$BS" <<'PY'
import sys,re,json,os
out,cs,bs=sys.argv[1:4]
def parse(p):
    arms={}; cur=None; so=[]
    for line in open(p):
        line=line.rstrip('\n')
        m=re.match(r'^== (default|filler|testable|managed) rc=(\S*)',line)
        if m: cur=m.group(1); arms[cur]={'rc':m.group(2),'failed':set(),'total':''}; continue
        if line.startswith('== total'): arms[cur]['total']=line[9:].strip(); continue
        if line=='== so': cur='so'; continue
        if cur=='so': so.append(line); continue
        if cur and line: arms[cur]['failed'].add(line)
    return arms,so
c,cso=parse(f'{out}/cand.txt'); b,bso=parse(f'{out}/base.txt')
res={'candidate':cs,'base':bs,'arms':{},'positive_control':{}}
for a in ('default','filler','testable','managed'):
    ca=c.get(a,{'rc':'NA','failed':set(),'total':''}); ba=b.get(a,{'rc':'NA','failed':set(),'total':''})
    res['arms'][a]={'cand_rc':ca['rc'],'base_rc':ba['rc'],'cand_total':ca['total'],'base_total':ba['total'],
        'cand_only':sorted(ca['failed']-ba['failed']),'base_only':sorted(ba['failed']-ca['failed']),'common':len(ca['failed']&ba['failed']),
        'cand_failed_n':len(ca['failed']),'base_failed_n':len(ba['failed'])}
    def broken(x):  # rc 不是 0/1（如 123=编译失败、124=超时）或跑了却没有 gtest 总数行 ⇒ 该臂不可判
        return x['rc'] not in ('0','1') or not x['total']
    if ca['rc'] in ('','NA') or ba['rc'] in ('','NA'):
        res['arms'][a]['status']='NOT_RUN'; res['positive_control'][a]='NOT_RUN（臂未跑，⛔ 不得据此宣称独红为 0）'
    elif broken(ca) or broken(ba):
        res['arms'][a]['status']=f"BROKEN(cand_rc={ca['rc']},base_rc={ba['rc']})"; res['positive_control'][a]='BROKEN（构建/运行未完成，failed=0 是假的，⛔ 不得据此宣称独红为 0）'
    else:
        res['arms'][a]['status']='ran'; res['positive_control'][a]='sets differ' if ca['failed']!=ba['failed'] else ('identical sets' if ca['failed'] else 'both empty')
res['cand_so']=cso; res['base_so']=bso
json.dump(res,open(f'{out}/DIFF.json','w'),ensure_ascii=False,indent=1)
md=[f"# DIFF {cs[:12]} vs {bs[:12]}",""]
not_run=[a for a,v in res['arms'].items() if v.get('status')!='ran']
if not_run: md.append(f"⛔ 未跑/未完成的臂: {[(a,res['arms'][a]['status']) for a in not_run]} ⇒ 本 DIFF 对这些臂不可作为验收证据（0917 实撞：P09 testable 编译失败 rc=123 被读成 CAND-ONLY=0 放行）")
for a,v in res['arms'].items():
    md.append(f"## {a}: status={v['status']} · cand rc={v['cand_rc']} failed={v['cand_failed_n']} · base rc={v['base_rc']} failed={v['base_failed_n']} · common={v['common']} · CAND-ONLY={len(v['cand_only'])} BASE-ONLY={len(v['base_only'])} · 阳性对照={res['positive_control'][a]}")
    for t in v['cand_only']: md.append(f"- CAND-ONLY {t}")
    for t in v['base_only'][:20]: md.append(f"- base-only {t}")
open(f'{out}/DIFF.md','w').write('\n'.join(md)+'\n')
print('\n'.join(md[:40]))
sys.exit(3 if not_run else 0)
PY
DRC=$?
cp "$OUT/DIFF.json" "/root/cj_build/reports/DIFF-$C12-vs-$B12.json"; cp "$OUT/DIFF.md" "/root/cj_build/reports/DIFF-$C12-vs-$B12.md"
echo "# 产物: /root/cj_build/reports/DIFF-$C12-vs-$B12.{json,md}  原始名单: $OUT/{cand,base}.txt  kkk2: /root/diff_$C12 /root/diff_$B12"
exit $DRC
