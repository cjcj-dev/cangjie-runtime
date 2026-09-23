import json,re,subprocess
from pathlib import Path
root=Path('/root/diff_40969cb83b54')
data={'candidate':'40969cb83b5425915bca21022019b62429c0495a','base':'ed74b464efc257cc884efd94d5ee923a970b6eab','cpuset':'96-111','arms':{}}
for arm in ['green','producer','consumer','ceiling','preclean','geometry','restored']:
    path=root/('tenuring-'+arm)
    cases={}
    for rc in sorted(path.glob('*.rc')):
        if rc.name=='run.rc': continue
        log=rc.with_suffix('.log').read_text(errors='replace')
        cases[rc.stem]={'rc':int(rc.read_text()),'targets':[line for line in log.splitlines() if 'TENURING_' in line or 'EXPECT failed:' in line], 'loaded_product_paths':sorted(set(re.findall(r'calling init: (\S*libcangjie-runtime\.so)',log)))}
    data['arms'][arm]={'rc':int((path/'run.rc').read_text()),'summary':(path/'summary.txt').read_text().strip(),'identity':(path/'identity.sha256').read_text().splitlines(),'uptime_before':(path/'uptime-before.txt').read_text().strip(),'uptime_after':(path/'uptime-after.txt').read_text().strip(),'cases':cases}
elf=root/'unit-default/cj_gc_unit'
so=root/'default/build/runtime-staging/lib/x86_64_Release/libcangjie-runtime.so'
needles=['MapleRuntime::ZArguments::initialize()','MapleRuntime::ZGenerationYoung::SelectTenuringThreshold','MapleRuntime::HeapManager::Init','MapleRuntime::ZHeuristics::max_heap_size()']
data['symbol_identity']={}
for label,file in [('elf',elf),('so',so)]:
    p=subprocess.run(['nm','-C','--defined-only',str(file)],text=True,capture_output=True)
    data['symbol_identity'][label]={'command':['nm','-C','--defined-only',str(file)],'rc':p.returncode,'matches':{n:[l for l in p.stdout.splitlines() if n in l] for n in needles},'main_positive_control':[l for l in p.stdout.splitlines() if l.endswith(' main')]}
for arm in ['default','filler','testable']:
    path=root/('unit-'+arm)
    data.setdefault('suite',{})[arm]={'rc':(path/'run.rc').read_text().strip(),'elf_identity':(path/'elf.sha256').read_text().splitlines(),'failures':[l for l in (path/'run.log').read_text(errors='replace').splitlines() if l.startswith('[  FAILED  ]')]}
(root/'tenuring-evidence.json').write_text(json.dumps(data,indent=2)+'\n')
print(json.dumps({a:{'rc':v['rc'],'summary':v['summary'],'so':v['identity'][1]} for a,v in data['arms'].items()},indent=2))
