from pathlib import Path
import hashlib,json
base=Path('/root');prefix='sym_cangjie_runtime_607_implement_r5684610492-'
roots=[p for p in base.iterdir() if p.is_dir() and p.name.startswith(prefix)]
kept={};removed=[]
for root in roots:
 for arm in ('default','testable'):
  b=root/arm/'build'
  if not b.is_dir(): continue
  for p in b.rglob('*.so'):
   if p.is_file(): kept[str(p)]=hashlib.sha256(p.read_bytes()).hexdigest()
  for p in b.rglob('*'):
   if p.is_file() and p.suffix in ('.o','.a'):
    removed.append({'path':str(p),'bytes':p.stat().st_size});p.unlink()
after={p:hashlib.sha256(Path(p).read_bytes()).hexdigest() for p in kept}
result={'removed':removed,'removed_bytes':sum(p['bytes'] for p in removed),'retained_so_before':kept,'retained_so_after':after,'so_unchanged':kept==after}
path=base/(prefix+'build7')/'pruned-object-files.json';path.write_text(json.dumps(result,indent=2)+'\n');print({'files':len(removed),'bytes':result['removed_bytes'],'retained_so':len(kept),'so_unchanged':result['so_unchanged']})
