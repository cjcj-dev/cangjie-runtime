from pathlib import Path
import shutil,hashlib,json
head='9de9226b8d58ed655534b195df2f6938be789062'
r=Path('/root/sym_cangjie_runtime_606_implement_r5674249495-final2')
root=Path('/root/sodepot')/head
rows=[]
for arm in ['default','testable']:
 source=r/arm/'build/runtime-staging/lib/x86_64_Release'
 dest=root/arm;dest.mkdir(parents=True,exist_ok=True)
 for name in ['libcangjie-runtime.so','libboundscheck.so','libcangjie-trace.so']:
  src=source/name;dst=dest/name
  digest=hashlib.sha256(src.read_bytes()).hexdigest()
  if dst.exists():
   if hashlib.sha256(dst.read_bytes()).hexdigest()!=digest:raise RuntimeError('refusing mismatched existing '+str(dst))
  else:shutil.copy2(src,dst)
  rows.append({'source':str(src),'destination':str(dst),'sha256':digest})
(r/'sodepot-manifest.json').write_text(json.dumps(rows,indent=2))
# The default runner captured these immediately after linking; filler used
# these same paths and did not build a second ELF.
(r/'default-filler-identity.txt').write_text((r/'unit-default/test-artifacts.sha256').read_text())
print(str(root))
