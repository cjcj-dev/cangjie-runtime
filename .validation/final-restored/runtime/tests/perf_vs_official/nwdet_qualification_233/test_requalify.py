"""Script controls only: fixtures do not qualify a compiler, std install or NW."""

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ENTRY = Path(__file__).with_name("requalify.py")
OFFICIAL = "9619dc3e2b3ab335a340d9d7b2e6e86c8e0103704a152b953a20a56bf5eab9cb"


class LinkIdentityControls(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.elf = self.root / "fixture-elf"
        self.elf.write_bytes(b"fixture, not a workload ELF")
        self.receipt = self.root / "fixture-receipt"
        self.receipt.write_bytes(b"fixture, not a qualified std receipt")
        self.marker = self.root / "gate-entered"
        self.gate = self.root / "fixture-gate.py"
        self.gate.write_text(
            "import os\nfrom pathlib import Path\n"
            f"Path({str(self.marker)!r}).write_text(os.environ['BIN'])\n"
            "raise SystemExit(17)\n"
        )
        self.value = {"linkage": {
            "elf_sha256": hashlib.sha256(self.elf.read_bytes()).hexdigest(),
            "static_std_archives": [{"path": "/fixture/core.a", "sha256": "a" * 64}],
            "std_origin": "stage1-coloured",
            "std_build_receipt": {
                "path": str(self.receipt),
                "sha256": hashlib.sha256(self.receipt.read_bytes()).hexdigest(),
            },
        }}

    def invoke(self):
        manifest = self.root / "manifest.json"
        manifest.write_text(json.dumps(self.value))
        return subprocess.run(
            [sys.executable, str(ENTRY), "--manifest", str(manifest),
             "--elf", str(self.elf), "--gate", str(self.gate)],
            env=dict(os.environ, BIN="/wrong/inherited/binary"),
            capture_output=True, text=True,
        )

    def rejected(self, reason):
        result = self.invoke()
        self.assertEqual(result.returncode, 3, result.stdout + result.stderr)
        self.assertIn("reason=" + reason, result.stdout)
        self.assertFalse(self.marker.exists(), "rejected input reached gate")

    def test_official_digest_rejected_even_if_renamed_and_origin_claims_coloured(self):
        for digest in (OFFICIAL, OFFICIAL.upper()):
            with self.subTest(digest=digest):
                self.value["linkage"]["static_std_archives"][0] = {
                    "path": "/renamed/source-built-core.a", "sha256": digest,
                }
                self.rejected("负载链接了无色官方 std")

    def test_elf_must_match_link_record(self):
        self.elf.write_bytes(b"different fixture")
        self.rejected("linkage-elf-sha256-mismatch")

    def test_missing_archives_rejected(self):
        self.value["linkage"]["static_std_archives"] = []
        self.rejected("static-std-linkage-missing")

    def test_uncoloured_origin_rejected(self):
        self.value["linkage"]["std_origin"] = "stage0"
        self.rejected("stage1-coloured-std-required")

    def test_receipt_identity_required(self):
        self.receipt.write_bytes(b"changed receipt")
        self.rejected("std-build-receipt-sha256-mismatch")

    def test_positive_fixture_reaches_gate_with_bound_binary_and_preserves_rc(self):
        result = self.invoke()
        self.assertEqual(result.returncode, 17, result.stdout + result.stderr)
        self.assertEqual(self.marker.read_text(), str(self.elf.resolve()))


if __name__ == "__main__":
    unittest.main(verbosity=2)
