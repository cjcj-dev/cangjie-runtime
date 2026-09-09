#!/usr/bin/env python3
from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path


HERE = Path(__file__).resolve().parent
RUNNER = HERE / "run_mutualwait_manifest.py"


class MutualWaitAstOnceTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="mutualwait-once-test-")
        self.root = Path(self.temporary.name)
        self.source = self.root / "source.cpp"
        self.manifest = self.root / "manifest.tsv"
        self.product = self.root / "product"
        self.include = self.root / "include"
        self.analyzer = self.root / "analyzer.py"
        self.compiler = self.root / "compiler"
        self.receipt = self.root / "receipt.json"
        self.counter = self.root / "counter"
        self.source.write_text("int source_marker;\n", encoding="utf-8")
        self.manifest.write_text("manifest-marker\n", encoding="utf-8")
        self.product.mkdir()
        self.include.mkdir()
        (self.product / "consumer.cpp").write_text("int product_marker;\n", encoding="utf-8")
        (self.include / "header.h").write_text("#define HEADER_MARKER 1\n", encoding="utf-8")
        self.compiler.write_text("#!/usr/bin/env bash\necho fake-compiler-v1\n", encoding="utf-8")
        self.compiler.chmod(0o755)
        self.analyzer.write_text(textwrap.dedent("""\
            #!/usr/bin/env python3
            import os
            from pathlib import Path
            counter = Path(os.environ["MUTUALWAIT_FAKE_COUNTER"])
            with counter.open("a", encoding="utf-8") as output:
                output.write("run\\n")
            for index in range(7):
                print(f"GATE_MUTUALWAIT_PRODUCT_MANIFEST_ROW_OK row={index}")
            raise SystemExit(int(os.environ.get("MUTUALWAIT_FAKE_RC", "0")))
        """), encoding="utf-8")
        self.analyzer.chmod(0o755)

    def tearDown(self) -> None:
        self.temporary.cleanup()

    def arguments(self, *extra: str) -> list[str]:
        return [
            "--source", str(self.source),
            "--manifest", str(self.manifest),
            "--product-root", str(self.product),
            "--compiler", str(self.compiler),
            f"--compile-arg=-I{self.include}",
            *extra,
        ]

    def run_arm(self, arm: str, *extra: str, analyzer_rc: int = 0) -> subprocess.CompletedProcess[str]:
        env = os.environ.copy()
        env["MUTUALWAIT_FAKE_COUNTER"] = str(self.counter)
        env["MUTUALWAIT_FAKE_RC"] = str(analyzer_rc)
        return subprocess.run(
            [sys.executable, str(RUNNER), "--arm", arm, "--receipt", str(self.receipt),
             "--analyzer", str(self.analyzer), "--", *self.arguments(*extra)],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env, check=False,
        )

    def invocation_count(self) -> int:
        if not self.counter.exists():
            return 0
        return len(self.counter.read_text(encoding="utf-8").splitlines())

    def test_default_filler_share_exactly_one_execution(self) -> None:
        default = self.run_arm("default")
        filler = self.run_arm("filler")
        self.assertEqual((default.returncode, filler.returncode), (0, 0))
        self.assertEqual(self.invocation_count(), 1)
        self.assertEqual(default.stdout.count("GATE_MUTUALWAIT_PRODUCT_MANIFEST_ROW_OK"), 7)
        self.assertIn("GATE_MUTUALWAIT_AST_RECEIPT_CONSUMED", filler.stdout)

    def test_default_always_executes_and_filler_only_executes(self) -> None:
        self.assertEqual(self.run_arm("default").returncode, 0)
        self.assertEqual(self.run_arm("default").returncode, 0)
        self.receipt.unlink()
        self.assertEqual(self.run_arm("filler").returncode, 0)
        self.assertEqual(self.invocation_count(), 3)

    def test_changed_source_manifest_and_compile_args_each_miss(self) -> None:
        mutations = (
            lambda: self.source.write_text("int changed_source;\n", encoding="utf-8"),
            lambda: self.manifest.write_text("changed-manifest\n", encoding="utf-8"),
            None,
        )
        for index, mutation in enumerate(mutations):
            with self.subTest(index=index):
                self.counter.unlink(missing_ok=True)
                self.receipt.unlink(missing_ok=True)
                self.assertEqual(self.run_arm("default").returncode, 0)
                if mutation is not None:
                    mutation()
                    filler = self.run_arm("filler")
                else:
                    filler = self.run_arm("filler", "--compile-arg=-DCHANGED=1")
                self.assertEqual(filler.returncode, 0)
                self.assertEqual(self.invocation_count(), 2)
                self.assertIn("GATE_MUTUALWAIT_AST_RECEIPT_MISS", filler.stdout)

    def test_product_header_and_compiler_identity_changes_miss(self) -> None:
        changes = (
            lambda: (self.product / "consumer.cpp").write_text("int changed_product;\n", encoding="utf-8"),
            lambda: (self.include / "header.h").write_text("#define HEADER_MARKER 2\n", encoding="utf-8"),
            lambda: self.compiler.write_text("#!/usr/bin/env bash\necho fake-compiler-v2\n", encoding="utf-8"),
        )
        for index, change in enumerate(changes):
            with self.subTest(index=index):
                self.counter.unlink(missing_ok=True)
                self.receipt.unlink(missing_ok=True)
                self.assertEqual(self.run_arm("default").returncode, 0)
                change()
                self.compiler.chmod(0o755)
                self.assertEqual(self.run_arm("filler").returncode, 0)
                self.assertEqual(self.invocation_count(), 2)

    def test_default_failure_is_reused_before_filler_behavior(self) -> None:
        default = self.run_arm("default", analyzer_rc=5)
        filler = self.run_arm("filler")
        self.assertEqual((default.returncode, filler.returncode), (5, 5))
        self.assertEqual(self.invocation_count(), 1)
        self.assertIn("GC_UNIT_MUTUALWAIT_AST_RECEIPT_FAILURE", filler.stderr)


if __name__ == "__main__":
    unittest.main()
