import pathlib,tarfile
root=pathlib.Path('/root/sym_cangjie_runtime_608_implement_r5683164869')
paths=[root/'final-summary.json',root/'final-summary.txt',root/'llvm-source-check.json']
for arm in ('green','cut','restored'):
 paths.extend((root/f'abi-final-{arm}'/'evidence').rglob('*'))
 paths.extend((root/f'consumer-final-{arm}').glob('*'))
 paths.extend(root.glob(f'abi-final-{arm}-launch.log'))
 paths.extend(root.glob(f'complete-{arm}.log'))
finalrt=pathlib.Path(str(root)+'-finalrt')
paths.extend(finalrt.glob('*.log')); paths.extend(finalrt.glob('*.rc')); paths.extend(finalrt.glob('*.txt')); paths.extend(finalrt.glob('*.sha256'))
for arm in ('default','filler','testable'):paths.extend((finalrt/f'unit-{arm}').glob('*'))
paths.extend((root/'ohos/evidence').glob('*'))
allowed={'.log','.rc','.json','.txt','.sha256','.wall','.maps','.before','.after'}
with tarfile.open(root/'delivery-records.tar.gz','w:gz') as tar:
 for p in sorted(set(paths)):
  if p.is_file() and p.suffix in allowed:
   tar.add(p,arcname=str(p.relative_to(root.parent)))
print((root/'delivery-records.tar.gz').stat().st_size)
