from pathlib import Path
import hashlib, json, sys
root = Path(sys.argv[1])
results = {}
for arm in sorted(p for p in root.iterdir() if p.is_dir()):
    rc = {p.stem: int(p.read_text()) for p in sorted(arm.glob('*.rc'))}
    so = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in (arm/'keep/lib').glob('*.so')}
    # Exit status alone does not qualify a negative arm: signals/timeouts are
    # execution failures, not assertion evidence. Keep the original rc map.
    test_rc = {k: v for k, v in rc.items() if k not in ('build', 'configure') and not k.startswith('gdb-')}
    abnormal = [k for k, v in test_rc.items() if v not in (0, 1)]
    results[arm.name] = {'rc': rc, 'red': [k for k,v in rc.items() if v],
                         'abnormal_exits': abnormal,
                         'test_n': sum(k not in ('build','configure') and not k.startswith('gdb-') for k in rc),
                         'observer_n': sum(k.startswith('gdb-') for k in rc), 'so': so,
                         'elf': (arm/'elf.sha256').read_text().split()[0] if (arm/'elf.sha256').exists() else None}
    print(arm.name, json.dumps(results[arm.name]['red']), 'n=', len(rc))
(root/'results.json').write_text(json.dumps(results, indent=2)+'\n')
