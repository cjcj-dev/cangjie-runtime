import pathlib, shutil, hashlib, json, sys
root=pathlib.Path(sys.argv[1])
assert root.parent==pathlib.Path('/root') and root.name.startswith('sym_cangjie_runtime_610_implement_r5687426297-')
rows=[]
for arm in ['default','testable']:
    assert (root/(arm+'-build.rc')).read_text().strip()=='0'
    build=root/arm/'build';stage=build/'runtime-staging';kept=root/arm/'staging'
    if kept.exists():continue
    assert (stage/'lib/x86_64_Release/libcangjie-runtime.so').is_file()
    before={str(p.relative_to(stage)):hashlib.sha256(p.read_bytes()).hexdigest() for p in stage.rglob('*') if p.is_file()}
    stage.rename(kept)
    shutil.rmtree(build)
    build.mkdir();(build/'runtime-staging').symlink_to('../staging')
    after={str(p.relative_to(kept)):hashlib.sha256(p.read_bytes()).hexdigest() for p in kept.rglob('*') if p.is_file()}
    assert before==after
    rows.append({'arm':arm,'preserved_staging':str(kept),'hashes':after})
(root/'build-prune.json').write_text(json.dumps(rows,indent=2)+'\n')
print('Preserved staging and removed own reconstructible build objects:',root)
