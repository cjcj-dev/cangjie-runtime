import subprocess,json,re
from pathlib import Path
base='403916767341fd0610fdc7a1201f1a0333e0da38'
def run(args):
 p=subprocess.run(args,text=True,capture_output=True);return {'command':args,'rc':p.returncode,'stdout':p.stdout,'stderr':p.stderr}
old=['RunRemapWindowTestHook','MRT_SetRemapWindowTestHook','g_remapWindowTestHook','RemapWindowTestHook']
out={'base':base,'head':run(['git','rev-parse','HEAD']), 'deleted':{},'main_content':{},'tests':{}}
for symbol in old:
 out['deleted'][symbol]={'positive':run(['git','grep','-n',symbol,base,'--','runtime/src']),'candidate':run(['git','grep','-n',symbol,'HEAD','--','runtime/src'])}
for symbol in ['RunYoungCollection','SelectTenuringThreshold','GenerationCycle::','DriverUnlocker','ShouldPrecleanYoung','GetYoungDriverPort']:
 out['main_content'][symbol]={'main':run(['git','grep','-c',symbol,base,'--','runtime/src']),'candidate':run(['git','grep','-c',symbol,'HEAD','--','runtime/src'])}
for ref in [base,'HEAD']:
 p=subprocess.run(['git','grep','-h','-E',r'GC_(OTHER_VM_)?TEST\(',ref,'--','runtime/tests/gc_unit'],capture_output=True,text=True)
 out['tests'][ref]={'rc':p.returncode,'names':sorted(set(re.findall(r'GC_(?:OTHER_VM_)?TEST\(\s*(\w+)\s*,\s*(\w+)\s*\)',p.stdout)))}
a=set(map(tuple,out['tests'][base]['names']));b=set(map(tuple,out['tests']['HEAD']['names']))
out['tests']['added']=sorted(b-a);out['tests']['deleted']=sorted(a-b)
out['switches']=run(['git','diff',base,'HEAD','--','runtime/src'])
out['switches']['stdout']='\n'.join(l for l in out['switches']['stdout'].splitlines() if l.startswith('+') and 'MRT_GCV2_' in l)
Path('evidence/a15/source-evidence.json').write_text(json.dumps(out,indent=2)+'\n')
