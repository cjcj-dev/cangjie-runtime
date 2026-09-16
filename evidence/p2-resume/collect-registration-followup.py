from pathlib import Path
import json,re
r=Path('/root/sym_cangjie_runtime_607_implement_r5684610492-build9')
g=r/'registration4'; lines=(g/'run.log').read_text().splitlines()
ident=[line for line in lines if re.match(r'^[0-9a-f]{64}  ',line)]
assert len(ident)==4, ident
d={'green':{'path':str(g),'rc':int((g/'run.rc').read_text()),'assertions':[line for line in lines if line.startswith('P2_ASSERT ')], 'result':[line for line in lines if line.startswith('P2_REGISTRATION_RESULT')], 'identity':ident,'before':(g/'before.txt').read_text(),'after':(g/'after.txt').read_text(),'wall':(g/'wall.txt').read_text()},'fault_source_base':'f473079dd357fd60f927282d05c197f7d27b4f75'}
for arm in ('final9-producer','restored'):
 p=r/'qualified-registration4'/arm;d[arm]={'path':str(p),'result':json.loads((p/'result.json').read_text()),'identity':(p/'identity.sha256').read_text().splitlines(),'lineage':(p/'lineage.txt').read_text(),'before':(p/'before.txt').read_text(),'after':(p/'after.txt').read_text(),'wall':(p/'wall.txt').read_text(),'cores':(p/'config.txt').read_text()}
h=lambda arm:[line.split()[0] for line in d[arm]['identity']]
a,b,c=h('green'),h('final9-producer'),h('restored')
d['identity_comparison']={'test_elf_equal':a[0]==b[0]==c[0],'test_dso_equal':a[1]==b[1]==c[1],'bounds_equal':a[3]==b[3]==c[3],'green_restored_runtime_equal':a[2]==c[2],'cut_runtime_different':a[2]!=b[2]}
(r/'registration-followup-evidence.json').write_text(json.dumps(d,indent=2)+'\n')
print(json.dumps({'identity':d['identity_comparison'],'rc':[d['green']['rc'],d['final9-producer']['result']['rc'],d['restored']['result']['rc']]},indent=2))
