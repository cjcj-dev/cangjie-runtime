import subprocess, os, json, time, hashlib
from pathlib import Path
lane='sym_cangjie_runtime_606_implement_r5673376405'
accept=Path('/root')/(lane+'-accept-delivery')
out=accept/'control-arms'
out.mkdir(exist_ok=True)
( out/'uptime-before.txt').write_bytes(subprocess.check_output(['uptime']))
main=accept/'unit-testable/cj_gc_unit'
publication=accept/'unit-testable/cj_gc_forwarding_publication_unit'
runner=accept/'testable/runtime/tests/gc_unit/run_parallel_tests.sh'
children=[]
for arm in ['cut-alloc','cut-retire','cut-consumer','cut-pinned','cut-shared','cut-inplace','restored']:
    product=Path('/root')/(lane+'-'+('accept' if arm=='restored' else arm)+'-delivery')/'testable/build/runtime-staging/lib/x86_64_Release'
    dest=out/arm; dest.mkdir(exist_ok=True)
    files=[main,publication,product/'libcangjie-runtime.so',product/'libboundscheck.so']
    identity={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in files}
    identity['stamp']=subprocess.check_output(['strings',str(product/'libcangjie-runtime.so')]).decode(errors='replace').split('CJRT-COMMIT:')[-1].split('\n')[0]
    (dest/'identity.json').write_text(json.dumps(identity,indent=2))
    env=os.environ.copy();env['GC_UNIT_TALLY_FILE']=str(dest/'tally.json');env['MRT_TESTABLE_INTERNALS']='1'
    log=(dest/'suite.log').open('wb');start=time.monotonic()
    proc=subprocess.Popen(['bash',str(runner),str(main),str(publication),str(dest),str(product)],env=env,stdout=log,stderr=subprocess.STDOUT)
    children.append((arm,proc,log,start,dest))
for arm,proc,log,start,dest in children:
    rc=proc.wait();log.close();wall=time.monotonic()-start
    (dest/'rc.txt').write_text(str(rc)+'\n');(dest/'wall.txt').write_text(str(wall)+'\n')
    print(arm,'rc=',rc,'wall=',round(wall,2),flush=True)
(out/'uptime-after.txt').write_bytes(subprocess.check_output(['uptime']))
