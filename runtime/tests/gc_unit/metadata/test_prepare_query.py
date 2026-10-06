"""Real CLI and fixed provider; no tuple, native tool production or downloads."""
import json
import os
from pathlib import Path
import subprocess
import sys
import unittest

TOOL, APPROVED, WRONG, WORK = map(lambda x: Path(x).resolve(), sys.argv[1:5])
sys.argv = sys.argv[:1]
ENTRY = Path(__file__).with_name('prepare_tools.py').resolve()

class Query(unittest.TestCase):
    def query(self, repo, runner=None):
        env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1')
        env.pop('TUPLE_ROOT', None)
        env.pop('RUNNER_TEMP', None)
        if runner is not None:
            env['RUNNER_TEMP'] = str(runner)
        return subprocess.run([sys.executable, str(ENTRY), str(repo), '--sdk-inputs'],
                              env=env, capture_output=True, text=True)

    def success(self, runner):
        result = self.query(TOOL, runner)
        print('TARGET_QUERY_REACHED rc=' + str(result.returncode), flush=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        value = json.loads(result.stdout)
        self.assertEqual(value, json.loads(APPROVED.read_text()), 'TARGET_FIXED_PROVIDER_JSON')
        logs = (runner or TOOL.parent) / 'metadata-tool-sources/logs'
        record = json.loads((logs / 'tool-checkout.json').read_text())
        self.assertEqual(record['role'], 'tool')
        self.assertEqual(record['root'], str(TOOL))
        self.assertEqual(record['actual_sha'], 'cab2fea8a3f66d30933508d96f78b08f9c99f571')
        (WORK / ('query-runner.json' if runner else 'query-fallback.json')).write_text(result.stdout)
        print('TARGET_QUERY_PASS logs=' + str(logs), flush=True)

    def test_runner_temp(self):
        self.success(WORK / 'query-runner')

    def test_fallback(self):
        self.success(None)

    def test_wrong_head(self):
        result = self.query(WRONG, WORK / 'query-wrong')
        print('TARGET_QUERY_WRONG_HEAD_REACHED rc=' + str(result.returncode), flush=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('tool checkout does not match expected source SHA', result.stderr)
        self.assertEqual(result.stdout, '', 'TARGET_PROVIDER_NOT_ENTERED')
        (WORK / 'query-wrong.stderr').write_text(result.stderr)
        record = json.loads((WORK / 'query-wrong/metadata-tool-sources/logs/tool-checkout.json').read_text())
        self.assertEqual(record['role'], 'tool')
        self.assertEqual(record['root'], str(WRONG))

if __name__ == '__main__':
    unittest.main(verbosity=2)
