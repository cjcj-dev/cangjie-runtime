"""One candidate, one consumer-call cut, one restore batch; stop on mismatch."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parent
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)
consumer = root / 'platform_inputs.py'
original = consumer.read_bytes()
needle = b'    receipt = tool_receipt(artifact, platform, hashes, environment)\n'
assert original.count(needle) == 1, 'unique real consumer call required'
cut = original.replace(needle, b'    receipt = None  # transient consumer-call cut\n')
plan = [('candidate', original, 0), ('cut', cut, 1), ('restored', original, 0)]
(out / 'plan.json').write_text(json.dumps(dict(batches=3, cases_per_batch=3,
    cases=['legal', 'bad-source', 'bad-pair'], retries=0), indent=2))
results = []
try:
    for name, source, expected in plan:
        consumer.write_bytes(source)
        (out / (name + '.sha256')).write_text('\n'.join(
            hashlib.sha256(p.read_bytes()).hexdigest() + '  ' + str(p)
            for p in (consumer, root / 'test_receipt_consumer.py', Path(__file__))) + '\n')
        with (out / (name + '.log')).open('w') as log:
            result = subprocess.run([sys.executable, '-B', str(root / 'test_receipt_consumer.py')],
                                    cwd=out, stdout=log, stderr=subprocess.STDOUT)
        (out / (name + '.rc')).write_text(str(result.returncode) + '\n')
        text = (out / (name + '.log')).read_text()
        targets = text.count('TARGET_RECEIPT_CONSUMER name=')
        failures = text.count('FAIL: test_receipt_rejection')
        valid = result.returncode == expected and targets == 3 and failures == (2 if name == 'cut' else 0)
        results.append(dict(arm=name, rc=result.returncode, targets=targets, failures=failures, valid=valid))
        print(json.dumps(results[-1]), flush=True)
        if not valid:
            break
finally:
    consumer.write_bytes(original)
    (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
sys.exit(0 if len(results) == 3 and all(r['valid'] for r in results) else 1)
