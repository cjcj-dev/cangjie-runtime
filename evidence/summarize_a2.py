import json,pathlib,hashlib,subprocess
r=pathlib.Path('/root/sym_cangjie_runtime_596_implement_r5673746875')
summary={'a2':{},'finalizer':json.loads((r/'finalizer-qualified/results.json').read_text()),'chains':{},'units':{},'suites':{}}
for a in ['baseline','export','V','green','thread-cut','export-cut','restored']:
 summary['a2'][a]=json.loads((r/('a2-'+a)/'results.json').read_text())
for a in ['green','old-strong-cut','finalizer-cut','restored']:
 rows=[]
 for p in sorted((r/('finalizer-'+a+'-slot-proof')).glob('*/result.json')):
  result=json.loads(p.read_text()); chain=json.loads(p.with_name('chain.json').read_text()); result['path']=str(p)
  result['objects']=[]
  for original in chain['tracked']:
   current=chain['forwarded'].get(original,original)
   events=[x for x in chain['events'] if x['obj'] in [original,current]]
   result['objects'].append({'original':original,'current':current,'events':events})
  rows.append(result)
 summary['chains'][a]=rows
for a in ['default','testable','filler','ohos']:
 d=r/'units';log=(d/(a+'.log')).read_text()
 summary['units'][a]={'rc':int((d/(a+'.rc')).read_text()),'wall':(d/(a+'.wall')).read_text(),'summary':[x for x in log.splitlines() if x.startswith('GC_UNIT_')], 'before':(d/(a+'-before.txt')).read_text(),'after':(d/(a+'-after.txt')).read_text(),'elf':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in (d/a).glob('cj_gc*unit')},'path':str(d/(a+'.log'))}
for a in ['thread-cut','export-cut','restored']:
 d=r/('suite-'+a);log=(d/'run.log').read_text()
 summary['suites'][a]={'rc':int((d/'rc').read_text()),'wall':(d/'wall').read_text(),'summary':[x for x in log.splitlines() if x.startswith('GC_UNIT_')], 'failures':[x for x in log.splitlines() if '[ FAIL' in x or '[  FAIL' in x], 'before':(d/'uptime-before.txt').read_text(),'after':(d/'uptime-after.txt').read_text(),'path':str(d/'run.log')}
(r/'delivery-summary.json').write_text(json.dumps(summary,indent=2))
for a,v in summary['suites'].items():print(a,v['rc'],v['summary'],v['failures'])
for a,rows in summary['chains'].items():
 print(a,'chains',len(rows),'missing-slots',sum(x.get('slot')=='0x0' for v in rows for o in v['objects'] for x in o['events'] if x['kind']=='discover_predicate'))
