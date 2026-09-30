from pathlib import Path
import hashlib, json, sys
root = Path(sys.argv[1])
results = {}
for arm in sorted(p for p in root.iterdir() if p.is_dir()):
    rc = {p.stem: int(p.read_text()) for p in sorted(arm.glob('*.rc'))}
    so = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in (arm/'keep/lib').glob('*.so')}
    results[arm.name] = {'rc': rc, 'red': [k for k,v in rc.items() if v], 'so': so,
                         'elf': (arm/'elf.sha256').read_text().split()[0] if (arm/'elf.sha256').exists() else None}
    print(arm.name, json.dumps(results[arm.name]['red']), 'n=', len(rc))
(root/'results.json').write_text(json.dumps(results, indent=2)+'\n')
