from pathlib import Path
import re,json
root=Path('/root/sym_cangjie_runtime_571_implement_r5668109509-preview-final')
rows=[]; summary=[]
for directory in sorted(p for p in root.iterdir() if p.is_dir()):
 s=(directory/'gdb.out').read_text()
 hits=[]
 for part in s.split('PREVIEW_REJECT ')[1:]:
  lines=part.splitlines()
  m=re.match(r'slot=(\S+) word=(\S+) entry=(.*)',lines[0]);assert m
  target=next(x for x in lines if x.startswith('PREVIEW_TARGET'))
  row=dict(run=directory.name,slot=m[1],word=m[2],entry=m[3],symbol=lines[1],target=target,static_root_table='StaticRootTable::VisitRoots' in part)
  hits.append(row);rows.append(row)
 summary.append(dict(run=directory.name,gdb_rc=(directory/'rc').read_text().strip(),inferior_sigabrt='SIGABRT' in s,rejected=len(hits),all_in_elf=all('section .bss of' in x['symbol'] for x in hits),static_root_frames=sum(x['static_root_table'] for x in hits),counts=re.findall(r'PREVIEW_COUNT (.*)',s),errors='PREVIEW_TARGET_UNAVAILABLE' in s or 'kind=error' in s))
(root/'rejected-roots.json').write_text(json.dumps(rows,indent=2))
(root/'summary.json').write_text(json.dumps(summary,indent=2))
md=['# 真实 NativeSlot 编码守卫预演逐槽清单','', '每项来自原 ELF 的 .bss 静态根；原始调用栈和对象类型在同目录 gdb.out。预演只读，负载在既有 SIGABRT 位点停止，gdb rc=0 不表示托管完成。','', '| 输入（第1发，其他两发同集合） | 槽 | 字 | ELF 符号 | 对象类型 |','|---|---|---|---|---|']
for row in rows:
 if row['run'].endswith('-1'):
  typ=row['target'].split('type=',1)[-1]
  md.append('| '+' | '.join([row['run'],row['slot'],row['word'],row['symbol'].split(' in section')[0],typ])+' |')
(root/'REJECTED_ROOTS.md').write_text('\n'.join(md)+'\n')
for row in summary: print(json.dumps(row))
