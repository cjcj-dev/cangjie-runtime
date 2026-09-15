#!/usr/bin/env python3
"""Compile changed P04 TU and relink against immutable P01 archive inputs."""
import concurrent.futures, hashlib, json, os, pathlib, shlex, shutil, subprocess, sys, time
root = pathlib.Path(sys.argv[1]).resolve()
base = pathlib.Path('/root/sym_cangjie_runtime_608_implement_r5683164869/abi-final-restored')
canonical = '/root/sym_cangjie_runtime_608_implement_r5683164869/abi-canonical'
build = base / 'llvm-build'
expected = '384006c43e210d7cc667996f82b6c5b1689a04583b3b1407f0ae9b2e4d530764'
sha = lambda p: hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()
assert sha(base/'llvm-src/llvm/lib/CodeGen/CJBarrierLowering.cpp') == expected
assert (base/'evidence/build.rc').read_text().strip() == '0'
root.mkdir(parents=True, exist_ok=True)
subprocess.run(['df','-h','/root'],stdout=(root/'df-before.txt').open('w'),check=True)
subprocess.run(['uptime'],stdout=(root/'uptime-before.txt').open('w'),check=True)
start = time.monotonic()
records=[]
def command(target):
    p=subprocess.run(['ninja','-t','commands',target],cwd=build,text=True,capture_output=True,check=True)
    text=p.stdout.splitlines()[-1].replace(canonical,str(base))
    args=shlex.split(text)
    if args[:2]==[':', '&&']:args=args[2:]
    if args[-2:]==['&&', ':']:args=args[:-2]
    assert '&&' not in args and ';' not in args
    return args

def compile_obj(target, source=None):
    args=command(target)
    output=root/pathlib.Path(target).name
    for flag,value in [('-o',str(output)),('-MF',str(output)+'.d'),('-MT',str(output))]:
        if flag in args:args[args.index(flag)+1]=value
    if source:args[args.index('-c')+1]=str(source)
    for kind in ['file','debug','macro']:
        args.insert(1,'-f'+kind+'-prefix-map='+str(root)+'=/usr/src/cangjie-llvm/llvm/lib/CodeGen') if args[0]!='ccache' else None
    relative_root=os.path.relpath(root,build)
    # Keep ccache as the launcher; add content-based mappings as compiler args.
    if args[0]=='ccache':
        args[2:2]=['-f'+kind+'-prefix-map='+prefix+'=/usr/src/cangjie-llvm/llvm/lib/CodeGen' for prefix in [str(root),relative_root] for kind in ['file','debug','macro']]
    env=os.environ.copy();env.update(CCACHE_DIR='/root/.ccache',CCACHE_BASEDIR=str(root),CCACHE_NOHASHDIR='1')
    with (root/(output.name+'.log')).open('w') as log:
        p=subprocess.run(args,cwd=build,env=env,stdout=log,stderr=subprocess.STDOUT)
    record={'target':target,'command':args,'rc':p.returncode,'output':str(output)}
    if output.exists():record['sha256']=sha(output)
    dependency=pathlib.Path(str(output)+'.d')
    if dependency.exists():
        content=dependency.read_text().replace('\\\n',' ')
        paths=shlex.split(content.split(':',1)[1])
        inputs={}
        for name in paths:
            header=pathlib.Path(name)
            if not header.is_absolute():header=build/header
            header=header.resolve()
            if header.is_file() and str(header).startswith(str(base)):
                inputs[str(header)]={'sha256':sha(header),'mtime_ns':header.stat().st_mtime_ns}
        record['actual_dependencies']=inputs
        record['dependencies_captured_at']=time.time()

    records.append(record)
    assert p.returncode==0, record
    return output

try:
    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        changed=pool.submit(compile_obj,'lib/CodeGen/CMakeFiles/LLVMCodeGen.dir/CJBarrierLowering.cpp.o',root/'CJBarrierLowering.cpp')
        driver=pool.submit(compile_obj,'tools/llc/CMakeFiles/llc.dir/llc.cpp.o')
        obj=changed.result();llc_obj=driver.result()
    archive=root/'libLLVMCodeGen.a'
    shutil.copy2(build/'lib/libLLVMCodeGen.a',archive)
    subprocess.run(['ar','rD',str(archive),str(obj)],check=True)
    subprocess.run(['ranlib',str(archive)],check=True)
    args=command('bin/llc')
    for i,a in enumerate(args):
        if a=='lib/libLLVMCodeGen.a':args[i]=str(archive)
        elif a=='tools/llc/CMakeFiles/llc.dir/llc.cpp.o':args[i]=str(llc_obj)
    args[args.index('-o')+1]=str(root/'llc')
    link_inputs={a:sha(build/a) for a in args if a.endswith('.a') and not pathlib.Path(a).is_absolute()}
    with (root/'link.log').open('w') as log:
        p=subprocess.run(args,cwd=build,stdout=log,stderr=subprocess.STDOUT)
    records.append({'target':'llc','command':args,'rc':p.returncode,'readonly_archive_inputs':link_inputs})
    assert p.returncode==0
    print('LLVM_OVERLAY_BUILD_RC=0 sha256='+sha(root/'llc'))
    (root/'llc.sha256').write_text(sha(root/'llc')+'  '+str(root/'llc')+'\n')
    rc=0
except Exception as e:
    print('LLVM_OVERLAY_BUILD_FAILURE',str(e)[:1000]);rc=1
finally:
    (root/'build.rc').write_text(str(rc)+'\n')
    (root/'build.wall').write_text(str(time.monotonic()-start)+'\n')
    (root/'build-record.json').write_text(json.dumps({'base':'1a01451912f160219665abdc497274e574338bad','expected_base_source':expected,'candidate_source':sha(root/'CJBarrierLowering.cpp'),'records':records},indent=2)+'\n')
    subprocess.run(['uptime'],stdout=(root/'uptime-after.txt').open('w'))
sys.exit(rc)
