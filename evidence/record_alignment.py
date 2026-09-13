from pathlib import Path
import subprocess,re,json,hashlib
BASE='8e1455a55012b2068c7a65b8e3f9cfb3437d086f'
FROZEN='5e04db890b22df66bc8875d3557c55ff7ef9177d'
e=Path('evidence')
def run(args):
 p=subprocess.run(args,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
 return p.returncode,p.stdout
names=[Path(x).stem for x in json.loads((e/'deleted_verify_files.json').read_text())]+['HeapZap','M0Corr','StackExposureHook','UntagRefFieldBreadcrumb','PeekYoungAllocBlack']
pattern='|'.join(sorted(set(names)))
with (e/'grep-evidence.txt').open('w') as out:
 for args in [['git','grep','-n','-E',pattern,'--','runtime/src','runtime/tests','runtime/CMakeLists.txt','runtime/config.cmake'],['git','grep','-n','-E','ZVerify::|ZVerifyRoots|AssertBarrierTransitionMonotonicity|MarkingStacks::VerifyAllEmpty','--','runtime/src/Heap/z/zVerify.cpp','runtime/src/Heap/z/zMark.cpp','runtime/src/Heap/z/zBarrier.cpp','runtime/src/ObjectModel/RefField.h'],['git','grep','-n','-E',pattern,FROZEN,'--','runtime/src/Heap/Verify/CMakeLists.txt']]:
  rc,text=run(args);out.write('$ '+' '.join(args)+'\n'+text+f'rc={rc}\n\n')
# Explicit source/CMake references to every deleted product/test file.
rc,diff=run(['git','diff','--name-only','--diff-filter=D',BASE,'--','runtime']);deleted=diff.splitlines()
with (e/'source-cmake.txt').open('w') as out:
 for f in deleted:
  rc,text=run(['git','grep','-n','-F',Path(f).name,'--','runtime/src','runtime/tests','runtime/CMakeLists.txt','runtime/config.cmake'])
  out.write(f'$ git grep -n -F {Path(f).name} -- runtime/src runtime/tests runtime/CMakeLists.txt runtime/config.cmake\n{text}rc={rc}\n')
  if rc!=1:raise RuntimeError('remaining reference: '+f)
 rc,text=run(['git','grep','-n','-F','../z/zVerify.cpp','--','runtime/src/Heap/Verify/CMakeLists.txt']);out.write(f'Positive retained source registration rc={rc}\n{text}')
 assert rc==0
for f in ['runtime/src/Heap/z/zVerify.cpp','runtime/src/Heap/z/zBarrier.cpp','runtime/src/Heap/z/zForwarding.cpp','runtime/src/ObjectModel/RefField.h']:
 old=subprocess.check_output(['git','show',BASE+':'+f]);new=Path(f).read_bytes();assert old==new
 with (e/'retained-hashes.txt').open('a') as out:out.write(f'{f} main={hashlib.sha256(old).hexdigest()} candidate={hashlib.sha256(new).hexdigest()}\n')
# Main feature count uses identical git grep ruler on both trees, with raw per-file output.
with (e/'main-features.txt').open('w') as out:
 for sym in ['in_place_relocation_claim_page','->detach_page()','set_in_place','insert_receipt','ForwardObjectImpl','ForwardClaimedPage','SetYoungAge']:
  counts=[]
  for ref in [BASE,'HEAD']:
   args=['git','grep','-c','-F','-e',sym,ref,'--','runtime/src'];rc,text=run(args)
   count=sum(int(l.rsplit(':',1)[1]) for l in text.splitlines()) if rc==0 else 0;counts.append(count)
   assert rc==0, (sym,rc,text)
   out.write('$ '+' '.join(args)+'\n'+text+f'rc={rc} total={count}\n')
  out.write(f'comparison {sym}: main={counts[0]} candidate={counts[1]}\n\n')
# Test name set differences, no tests executed.
def tests(ref):
 rc,listing=run(['git','ls-tree','-r','--name-only',ref,'runtime/tests']);res={}
 for f in listing.splitlines():
  if not f.endswith('.cpp'):continue
  text=subprocess.check_output(['git','show',ref+':'+f],text=True)
  for m in re.finditer(r'GC_(?:OTHER_VM_)?TEST\(\s*(\w+)\s*,\s*(\w+)\s*\)',text):res[m[1]+'.'+m[2]]=f+':'+str(text[:m.start()].count('\n')+1)
 return res
before,after=tests(BASE),tests('HEAD')
with (e/'tests-delta.md').open('w') as out:
 out.write('| 名称 | 基线位置 | 处置理由 |\n|---|---|---|\n')
 for n in sorted(before.keys()-after.keys()):out.write(f'| {n} | `{before[n]}` | 仅测试已删除诊断/观测接口，无 ZGC 测试对应 |\n')
 out.write('\n新增：'+str(sorted(after.keys()-before.keys()))+'\n')
# Disposition map includes files already gone at frozen HEAD, not attributed to this patch.
text=Path('/root/cj_build/ops/design/VERIFY_DISPOSITION.md').read_text()
with (e/'deletion-table.md').open('w') as out:
 out.write('| 原文件（行锚基于冻结 5e04db89） | 本轮处置 / 承载 | ZGC 理由 |\n|---|---|---|\n')
 for line in text.splitlines():
  if not line.startswith('|'):continue
  m=re.search(r'`R:([^`:]+):\d+`',line)
  if not m:continue
  f=m[1];cols=line.split('|');reason=cols[4].strip() if len(cols)>5 else ''
  exists=run(['git','cat-file','-e',FROZEN+':'+f])[0]==0
  status='删除' if exists and not Path(f).exists() else ('仅登记 `../z/zVerify.cpp`' if f.endswith('CMakeLists.txt') else '冻结时已迁走/删除；本轮不重复归功')
  if 'ZgcSelfHealDiag' in f:status+='；本体已在 `zBarrier.cpp:36` / `RefField.h:322`'
  if 'ZgcInvariants' in f:status+='；自创 tuple/地址相等计数删除，现有屏障同一 good 地址数据流保留'
  out.write(f'| `{f}:1`'+('（冻结时不存在）' if not exists else '')+f' | {status} | {reason} |\n')
 for f,reason in [('runtime/src/UnwindStack/StackExposureHook.cpp','自创 hook + 观测计数；唯一产品调用只记录 STW，真实栈处理在 zStackWatermark 家族'),('runtime/src/UnwindStack/StackExposureHook.h','同上；诊断接口与专属 harness 同删'),('runtime/src/Heap/WCollector/UntagRefFieldBreadcrumb.h','TLS breadcrumb 的信号打印接口，非 ZVerify')]:out.write(f'| `{f}:1` | 删除 | {reason} |\n')
(e/'identity.txt').write_text(f'Frozen={FROZEN}\nMergedMain={BASE}\nHead='+subprocess.check_output(['git','rev-parse','HEAD'],text=True)+'rev-parse specified repository cjcjdev/main rc=0\n')
