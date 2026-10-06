"""Real subprocess/git qualification of the production checkout guard (Linux host)."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

import platform_inputs as inputs


class CheckoutIdentityTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.root = self.base / 'tool'
        self.logs = self.base / 'logs'
        self.root.mkdir()
        self.git('init', '-q')
        self.git('config', 'user.name', 'Zxilly')
        self.git('config', 'user.email', 'zxilly@outlook.com')
        self.file = self.root / 'tracked file.txt'
        self.file.write_bytes(b'original\n')
        self.git('add', '.')
        self.git('commit', '-qm', 'fixture')
        self.head = self.git('rev-parse', 'HEAD').decode().strip()

    def git(self, *args):
        return subprocess.check_output(['git', '-C', str(self.root), *args], stderr=subprocess.STDOUT)

    def check(self, expected=None):
        return inputs.checkout_identity(self.root, expected or self.head, role='tool', logs=self.logs)

    def test_clean(self):
        self.assertEqual(self.check(), self.head)

    def test_dirty_rejection_and_log(self):
        self.file.write_bytes(b'changed\n')
        with self.assertRaisesRegex(ValueError, '^tool checkout must be clean$'):
            self.check()
        # The identity rejection is checked independently before the target log assertion.
        print('IDENTITY_REJECTION_OBSERVED; TARGET_LOG_ASSERTION_EXECUTED', flush=True)
        self.assertTrue((self.logs / 'tool-checkout.json').is_file(), 'TARGET_CHECKOUT_LOG_MISSING')
        record = json.loads((self.logs / 'tool-checkout.json').read_text())
        self.assertEqual(record['role'], 'tool')
        self.assertEqual(record['root'], str(self.root.resolve()))
        self.assertEqual(record['expected_sha'], self.head)
        self.assertEqual(record['actual_sha'], self.head)
        self.assertEqual(record['git_executable'], str(Path(shutil.which('git')).resolve()))
        self.assertIn('git version', record['git_version'])
        self.assertEqual(record['tracked_paths'], ['tracked file.txt'])
        self.assertEqual((self.logs / 'tool-checkout-porcelain.stdout').read_bytes(),
                         self.git('status', '--porcelain=v1', '-z'))
        self.assertIn(b'+changed', (self.logs / 'tool-checkout-diff.stdout').read_bytes())
        self.assertIn(b'tracked file.txt', (self.logs / 'tool-checkout-eol.stdout').read_bytes())
        self.assertTrue((self.logs / 'tool-checkout-attributes.stdout').exists())
        self.assertTrue((self.logs / 'tool-checkout-config.stdout').exists())

    def test_restore_same_file(self):
        self.file.write_bytes(b'changed\n')
        with self.assertRaisesRegex(ValueError, 'tool checkout must be clean'):
            self.check()
        self.file.write_bytes(b'original\n')
        self.assertEqual(self.check(), self.head)

    def test_wrong_head(self):
        with self.assertRaisesRegex(ValueError, '^tool checkout does not match expected source SHA$'):
            self.check('0' * 40)

    def test_adjacent_runtime_is_not_tool(self):
        runtime = self.base / 'runtime'
        subprocess.run(['git', 'clone', '-q', str(self.root), str(runtime)], check=True)
        (runtime / self.file.name).write_bytes(b'changed adjacent runtime\n')
        self.assertTrue(subprocess.check_output(['git', '-C', str(runtime), 'status', '--porcelain']))
        self.assertEqual(self.check(), self.head)


if __name__ == '__main__':
    unittest.main(verbosity=2)
