import json,re
from pathlib import Path
root=Path('/root/sym917_r2_green')
result={'root':str(root),'arms':{}}
for arm in ['green','guard','thread','phase','restored']:
 for config in ['default','testable']:
  key=f'{arm}-{config}'
  d=root/f'causal-{key}'
  if not d.exists(): continue
  ids={p.split(maxsplit=1)[1].strip():p.split()[0] for p in (d/'identity.sha256').read_text().splitlines()}
  data={'identities':ids,'tests':{},'uptime_before':(d/'uptime-before.txt').read_text().strip(),'uptime_after':(d/'uptime-after.txt').read_text().strip(),'wall':(d/'wall.txt').read_text().strip(),'cores':'16-31'}
  lib=next(p for p in ids if p.endswith('/libcangjie-runtime.so'))
  for p in sorted(d.glob('*.rc')):
   log=p.with_suffix('.log').read_text(errors='replace')
   data['tests'][p.stem]={'rc':int(p.read_text()),'target_matched':bool(re.search(r'VERIFY_SHARED_STACK_ASSERT_EXECUTED status=\d+ matched=1',log)),'target_missed':'missed expected abort' in log,'loaded_product':any('calling init:' in line and lib in line for line in log.splitlines()),'diagnostics':[line.strip() for line in log.splitlines() if 'Should be at safepoint' in line or 'Should be a mutator thread' in line or 'VERIFY_SHARED_STACK_ASSERT_EXECUTED' in line or 'TLAB_SHARED_TARGET' in line]}
  prod=(d/'product-symbols.txt').read_text();test=(d/'test-symbols.txt').read_text()
  names=['ZObjectAllocator::PerAge::retire_pages()','ZObjectAllocator::retire_pages(','ZObjectAllocator::fast_available(','ZGenerationYoung::mark_start()','ZGenerationOld::mark_start()','AllocBuffer::AllocateImpl(']
  data['product_definitions']=[line for line in prod.splitlines() if any(n in line for n in names)]
  data['test_definitions']=[line for line in test.splitlines() if any(n in line for n in names)]
  data['test_main_control']=[line for line in test.splitlines() if line.endswith(' main')]
  result['arms'][key]=data
for a in ['default','testable','filler']:
 p=root/f'unit-final-{a}.rc'
 if p.exists():
  log=(root/f'unit-final-{a}.log').read_text(errors='replace')
  result['unit_'+a]={'rc':int(p.read_text()),'summary':re.findall(r'^\[========\].*$',log,re.M)[-1:],'failed':re.findall(r'^\[  FAILED  \] ([A-Za-z].*)$',log,re.M)}
p=root/'ohos.rc'
if p.exists():result['ohos']={'rc':int(p.read_text()),'configure':(root/'ohos-configure.rc').read_text().strip(),'build':(root/'ohos-build.rc').read_text().strip()}
(root/'causal-summary.json').write_text(json.dumps(result,indent=2))
for k,v in result['arms'].items():
 bad=[n for n,t in v['tests'].items() if t['rc']!=0]
 print(k,'tests=',len(v['tests']),'failed=',bad,'loaded=',all(t['loaded_product'] for t in v['tests'].values()))
