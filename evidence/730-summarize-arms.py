from pathlib import Path
import json,re,collections,hashlib
root=Path('/root/sym_cangjie_runtime_730_implement_r5746939673-evidence')
rows=[]
for line in (root/'results.tsv').read_text().splitlines():
    arm,config,test,n,rc,wall=line.split('\t')
    p=root/f'{arm}-{config}'/('pinned.gdb.log' if test=='PinnedPublication.GdbWindow' else f'{test}.{n}.log')
    targets=[s for s in p.read_text(errors='replace').splitlines() if any(t in s for t in ['CPU_MIGRATION_TARGET','CPU_SLOT_ZERO_TARGET','STALL_PRODUCT_LATE_TARGET','PINNED_PUBLICATION_TARGET','PINNED_RETENTION_TARGET','expected','ASSERT'])]
    rows.append(dict(arm=arm,config=config,test=test,n=int(n),rc=int(rc),wall=int(wall),log=str(p),targets=targets))
identities={}
for p in root.glob('*/identity.sha256'):
    lines=[s.split(maxsplit=1) for s in p.read_text().splitlines()]
    identities[p.parent.name]={Path(path).name:sha for sha,path in lines}
counts={}
for row in rows:
    key=f"{row['arm']}-{row['config']}"
    c=counts.setdefault(key,dict(total=0,rc0=0,rc1=0,other=[])); c['total']+=1
    if row['rc']==0:c['rc0']+=1
    elif row['rc']==1:c['rc1']+=1
    else:c['other'].append(row)
identity_checks={}
for config in ['default','testable']:
    green=identities[f'green-{config}']; restored=identities[f'restored-{config}']
    identity_checks[config]={'green_equals_restored':green==restored}
    for arm in ['cpu','pinned','late']:
        x=identities[f'{arm}-{config}']
        identity_checks[config][arm]={'test_equal':x['cj_gc_unit']==green['cj_gc_unit'], 'gdb_equal':x['pinned_publication_window']==green['pinned_publication_window'],'bounds_equal':x['libboundscheck.so']==green['libboundscheck.so'],'so_different':x['libcangjie-runtime.so']!=green['libcangjie-runtime.so']}
summary=dict(counts=counts,identities=identities,identity_checks=identity_checks,rows=rows,
             uptime_before=(root/'uptime-before.txt').read_text(),uptime_after=(root/'uptime-after.txt').read_text(),cores='16-31')
(root/'summary.json').write_text(json.dumps(summary,indent=2))
print(json.dumps(dict(counts=counts,identity_checks=identity_checks),indent=2))
