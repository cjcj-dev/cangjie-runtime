"""Actual git identity rejection has priority over physical log I/O failures."""
import contextlib
import io
from pathlib import Path
import unittest
from test_checkout_identity import CheckoutIdentityTest
import platform_inputs as inputs

class CheckoutIo(unittest.TestCase):
    setUp = CheckoutIdentityTest.setUp
    git = CheckoutIdentityTest.git

    def reject(self, expected, message):
        # A file at the directory boundary forces mkdir failure even as root.
        self.logs.write_bytes(b'not a directory')
        diagnostic = io.StringIO()
        error = None
        with contextlib.redirect_stderr(diagnostic):
            try:
                inputs.checkout_identity(self.root, expected, role='tool', logs=self.logs)
            except Exception as caught:
                error = caught
        print('TARGET_IO_PRIORITY_REACHED expected=' + message + ' actual=' + str(error), flush=True)
        self.assertIsInstance(error, ValueError, 'TARGET_IDENTITY_PRIORITY')
        self.assertEqual(str(error), message, 'TARGET_IDENTITY_PRIORITY')
        self.assertIn('checkout diagnostic write failed:', diagnostic.getvalue(), 'TARGET_IO_DIAGNOSTIC_VISIBLE')

    def test_wrong_head_io(self):
        self.reject('0' * 40, 'tool checkout does not match expected source SHA')

    def test_dirty_io(self):
        self.file.write_bytes(b'changed\n')
        self.reject(self.head, 'tool checkout must be clean')

    def test_clean_io(self):
        self.reject(self.head, 'tool checkout required diagnostics could not be retained')

    def test_normal_log_control(self):
        self.assertEqual(inputs.checkout_identity(self.root, self.head, role='tool', logs=self.logs), self.head)
        self.assertTrue((self.logs / 'tool-checkout.json').is_file())
        print('TARGET_NORMAL_LOG_CONTROL_PASS', flush=True)

    def test_write_io(self):
        self.logs.mkdir()
        (self.logs / 'tool-checkout-head.stdout').symlink_to('/dev/full')
        diagnostic = io.StringIO()
        error = None
        with contextlib.redirect_stderr(diagnostic):
            try:
                inputs.checkout_identity(self.root, '0' * 40, role='tool', logs=self.logs)
            except Exception as caught:
                error = caught
        print('TARGET_WRITE_IO_REACHED actual=' + str(error), flush=True)
        self.assertIsInstance(error, ValueError, 'TARGET_WRITE_IDENTITY_PRIORITY')
        self.assertEqual(str(error), 'tool checkout does not match expected source SHA', 'TARGET_WRITE_IDENTITY_PRIORITY')
        self.assertIn('checkout diagnostic write failed:', diagnostic.getvalue())

if __name__ == '__main__':
    unittest.main(verbosity=2, defaultTest='CheckoutIo')
