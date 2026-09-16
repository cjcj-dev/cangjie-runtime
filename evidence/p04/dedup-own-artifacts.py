import hashlib,json,pathlib,os
prefix='sym_cangjie_runtime_610_implement_r5687426297-'
roots=[p for p in pathlib.Path('/root').glob(prefix+'*') if p.is_dir() and p.name!=prefix+'llvm']
seen={};rows=[];saved=0
for root in sorted(roots):
    for config in ['default','testable']:
        # Only completed, immutable arms. Never share mutable compiler objects.
        if not (root/(config+'-build.rc')).is_file() or (root/(config+'-build.rc')).read_text().strip()!='0':continue
        for directory in [root/config/'staging',root/config/'runtime/output']:
            for p in sorted(directory.rglob('*')):
                if p.is_symlink() or not p.is_file() or p.suffix not in ['.a','.so']:continue
                digest=hashlib.sha256(p.read_bytes()).hexdigest();key=(p.stat().st_size,digest)
                if key not in seen:seen[key]=p;continue
                original=seen[key]
                if os.path.samefile(p,original):continue
                old_size=p.stat().st_size
                tmp=p.with_name(p.name+'.dedup-link')
                os.link(original,tmp);os.replace(tmp,p)
                assert hashlib.sha256(p.read_bytes()).hexdigest()==digest
                rows.append({'path':str(p),'same_bytes_as':str(original),'sha256':digest,'bytes':old_size});saved+=old_size
out=pathlib.Path('/root/'+prefix+'llvm/own-artifact-dedup.json')
out.write_text(json.dumps({'preserved_paths_and_bytes':rows,'duplicate_bytes':saved},indent=2)+'\n')
print('Own immutable artifact duplicate bytes removed:',saved,'paths preserved:',len(rows))
