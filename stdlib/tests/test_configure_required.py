#!/usr/bin/env python3
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from build import requested_target_lib, stdlib_configure_required


class ConfigureRequiredTest(unittest.TestCase):
    def test_missing_cache_configures(self):
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / "CMakeCache.txt"
            self.assertTrue(stdlib_configure_required(str(cache), "/runtime"))

    def test_same_target_lib_reuses_the_cache(self):
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / "CMakeCache.txt"
            cache.write_text("CANGJIE_TARGET_LIB:STRING=/runtime/output\n", encoding="utf-8")
            self.assertFalse(stdlib_configure_required(str(cache), "/runtime/output"))

    def test_changed_target_lib_reconfigures(self):
        with tempfile.TemporaryDirectory() as temporary:
            cache = Path(temporary) / "CMakeCache.txt"
            cache.write_text("CANGJIE_TARGET_LIB:STRING=/old/output\n", encoding="utf-8")
            self.assertTrue(stdlib_configure_required(str(cache), "/new/output"))
            self.assertEqual(requested_target_lib(["/new/output", "/openssl"]), "/new/output;/openssl")


if __name__ == "__main__":
    unittest.main()
