from pathlib import Path
import hashlib,json,shutil
source=Path('/root/diff_40969cb83b54')
root=Path('/root/sym_cangjie_runtime_906_implement_r5785382414-delivery')
root.mkdir(exist_ok=True)
manifest={}
def preserve(src,dst):
    dst.parent.mkdir(parents=True,exist_ok=True)
    shutil.copy2(src,dst,follow_symlinks=True)
    digest=hashlib.sha256(dst.read_bytes()).hexdigest()
    assert digest==hashlib.sha256(src.read_bytes()).hexdigest()
    manifest[str(dst)]={'source':str(src),'sha256':digest,'source_mtime_ns':src.stat().st_mtime_ns}
for arm in ['green','producer','consumer','ceiling','preclean','geometry','restored']:
    base=source if arm=='green' else Path('/root/sym_cangjie_runtime_906_closed_'+arm)
    for name in ['libcangjie-runtime.so','libboundscheck.so']:
        preserve(base/'default/build/runtime-staging/lib/x86_64_Release'/name,root/arm/name)
    shutil.copytree(source/('tenuring-'+arm),root/arm/'results',dirs_exist_ok=True)
for name in ['cj_gc_unit','cj_gc_forwarding_publication_unit']:
    preserve(source/'unit-default'/name,root/'tests'/name)
preserve(source/'tenuring-evidence.json',root/'tenuring-evidence.json')
ohos=Path('/root/sym_cangjie_runtime_906_closed_restored')
shutil.copytree(ohos/'ohos-product-unit',root/'ohos-results',dirs_exist_ok=True)
for name in ['libcangjie-runtime.so','libboundscheck.so']:
    preserve(ohos/'ohos-build/runtime-staging/lib/x86_64_Release'/name,root/'ohos'/name)
(root/'artifact-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print('preserved',len(manifest),'identity-checked files at',root)
