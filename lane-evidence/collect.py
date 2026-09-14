import pathlib,json,re,subprocess,tarfile
root=pathlib.Path('/root/sym_cangjie_runtime_581_implement_r5668277053-final3')
summary={}
for arm in ['unit-default','unit-filler','run-producer','run-promotion','run-phase','run-consumer','run-entry','run-restored','unit-ohos']:
 log=root/(arm+'.log');rc=root/(arm+'.rc')
 if not rc.exists():raise SystemExit('INCOMPLETE '+arm)
 s=log.read_text(errors='replace')
 summary[arm]={'rc':rc.read_text().strip(),'totals':re.findall(r'\[========\] (\d+) tests: (\d+) passed, (\d+) failed',s)[-1:], 'failed':re.findall(r'^\[  FAILED  \] ([A-Za-z]\S+)',s,re.M),'assertions':[x for x in s.splitlines() if 'RAW_ACQUIRE_ASSERT' in x or 'EXPECT failed:' in x or 'EXPECT_EQ failed:' in x or 'GC_UNIT_OHOS_HOST_FILTER' in x]}
(root/'results.json').write_text(json.dumps(summary,indent=2)+'\n')
lib=root/'default/build/runtime-staging/lib/x86_64_Release'
(root/'green-run-so.sha256').write_text(subprocess.check_output(['sha256sum',*map(str,lib.glob('*.so'))],text=True))
(root/'green-stamp.txt').write_text('\n'.join(x for x in subprocess.check_output(['strings',str(lib/'libcangjie-runtime.so')],text=True).splitlines() if 'CJRT-COMMIT:' in x)+'\n')
(root/'source-archive.sha256').write_text(subprocess.check_output(['sha256sum',str(root/'source.tar.gz')],text=True))
with (root/'symbols.txt').open('w') as f:
 for name in ['test.full-defined.txt','test.undefined.txt','restored-product.full-defined.txt']:
  lines=(root/name).read_text().splitlines();f.write(name+'\n')
  for sym in [' main','MCC_AcquireRawData','MCC_ReleaseRawData','WCollector::PinRawPointerObject','RegionManager::CompactRegion']:
   matches=[x for x in lines if sym in x];f.write(sym+' count='+str(len(matches))+'\n'+'\n'.join(matches)+'\n')
with tarfile.open(root/'evidence.tar.gz','w:gz') as tar:
 for p in root.iterdir():
  if p.is_file() and not p.name.endswith('.tar.gz'):tar.add(p,arcname=p.name)
 for name in ['unit-default','unit-filler','unit-ohos','run-producer','run-promotion','run-phase','run-consumer','run-entry','run-restored']:
  for p in (root/name).rglob('*'):
   if p.is_file() and (p.suffix in ['.log','.json','.txt','.receipt','.sha256'] or p.name.endswith('.rc')):tar.add(p,arcname=str(p.relative_to(root)))
 for arm in ['producer','promotion','phase','consumer','entry','restored','ohos']:
  child=pathlib.Path(str(root)+'-'+arm)
  for p in child.iterdir():
   if p.is_file() and p.suffix in ['.log','.rc','.txt','.sha256','.sh']:tar.add(p,arcname='build-'+arm+'/'+p.name)
print(json.dumps({k:{'rc':v['rc'],'totals':v['totals'],'failed':v['failed']} for k,v in summary.items()},indent=2))
print('ARCHIVE_BYTES', (root/'evidence.tar.gz').stat().st_size)
