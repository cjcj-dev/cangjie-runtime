import hashlib,json,pathlib,re,subprocess
root=pathlib.Path('/root/sym_cangjie_runtime_608_implement_r5683164869')
summary={}
for arm in ('green','cut','restored'):
    out=root/f'consumer-final-{arm}'
    abi=root/f'abi-final-{arm}'
    samples={}
    for sample in ('1','2','3','hole','heap_dyn','heap_null','global'):
        text=(out/f'run-{sample}.log').read_text()
        rows=[dict(name=m[0],actual=m[1],expected=m[2],passed=m[3]=='1')
              for m in re.findall(r'SLOT_DOMAIN_ASSERT name=(\S+) actual=(\S+) expected=(\S+) pass=([01])',text)]
        samples[sample]=dict(rc=int((out/f'run-{sample}.rc').read_text()),assertions=rows)
    def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
    maps=(out/'process.maps').read_text()
    loaded=sorted(set(line.split()[-1] for line in maps.splitlines() if 'libcangjie-' in line or 'libboundscheck' in line))
    paths=[abi/'llvm-build/bin/llc',abi/'target/bin/cjcj-stage1',out/'slot_domain_driver',
           abi/'std-install/runtime/lib/linux_x86_64_cjnative/libcangjie-std-core.so',
           pathlib.Path(str(root)+'-finalrt/default/build/runtime-staging/lib/x86_64_Release/libcangjie-runtime.so'),
           pathlib.Path(str(root)+'-finalrt/default/build/runtime-staging/lib/x86_64_Release/libboundscheck.so')]
    summary[arm]=dict(build_rc=int((abi/'evidence/build.rc').read_text()),samples=samples,
                      hashes={p.name:sha(p) for p in paths},loaded=loaded)
    for path in (out/'slot_domain_consumer.o',out/'slot_domain_driver'):
        r=subprocess.run(['nm','--defined-only',str(path)],capture_output=True,text=True)
        (out/(path.name+'.defined.txt')).write_text(r.stdout)
    r=subprocess.run(['objdump','-dr',str(out/'slot_domain_consumer.o')],capture_output=True,text=True)
    (out/'consumer.disassembly.txt').write_text(r.stdout)
(root/'final-summary.json').write_text(json.dumps(summary,indent=2)+'\n')
for arm,data in summary.items():
    print(arm,'build_rc='+str(data['build_rc']))
    for sample,data2 in data['samples'].items():
        print(' ',sample,'rc='+str(data2['rc']), 'assertions='+str(len(data2['assertions'])),
              'failed='+','.join(r['name'] for r in data2['assertions'] if not r['passed']))
print('green/restored loaded product hashes equal:',summary['green']['hashes']==summary['restored']['hashes'])
