#!/usr/bin/env python3
# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

import os
import pathlib
import subprocess
import tempfile
import unittest


RUNTIME_DIR = pathlib.Path(__file__).resolve().parents[1]
GENERATOR = RUNTIME_DIR / "build" / "cmake" / "GenerateRuntimeProvenance.cmake"


class RuntimeProvenanceGeneratorTest(unittest.TestCase):
    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tempdir.name)
        self.repo = self.root / "repo"
        self.repo.mkdir()
        self.output = self.root / "generated" / "RuntimeProvenance.cpp"
        self.run_command("git", "init", "-q", cwd=self.repo)
        self.run_command("git", "config", "user.name", "Runtime Test", cwd=self.repo)
        self.run_command("git", "config", "user.email", "runtime-test@example.invalid", cwd=self.repo)
        (self.repo / "tracked.txt").write_text("clean\n", encoding="utf-8")
        self.run_command("git", "add", "tracked.txt", cwd=self.repo)
        self.run_command("git", "commit", "-q", "-m", "fixture", cwd=self.repo)
        self.commit = self.run_command("git", "rev-parse", "HEAD", cwd=self.repo).stdout.strip()

    def tearDown(self):
        self.tempdir.cleanup()

    def run_command(self, *args, cwd=None, env=None):
        return subprocess.run(
            args,
            cwd=cwd,
            env=env,
            check=True,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

    def generate(self, repository=None, override=None, env_commit=None):
        command = [
            "cmake",
            f"-DREPOSITORY_DIR={repository or self.repo}",
            f"-DOUTPUT_FILE={self.output}",
        ]
        if override is not None:
            command.append(f"-DCOMMIT_OVERRIDE={override}")
        command.extend(("-P", str(GENERATOR)))
        env = os.environ.copy()
        env.pop("CJ_RUNTIME_COMMIT", None)
        if env_commit is not None:
            env["CJ_RUNTIME_COMMIT"] = env_commit
        self.run_command(*command, env=env)
        return self.output.read_text(encoding="utf-8")

    def test_tree_state_is_sampled_each_run_without_rewriting_stable_output(self):
        clean = self.generate()
        self.assertIn(f"CJRT-COMMIT:{self.commit}", clean)
        clean_mtime = self.output.stat().st_mtime_ns

        self.assertEqual(clean, self.generate())
        self.assertEqual(clean_mtime, self.output.stat().st_mtime_ns)

        (self.repo / "tracked.txt").write_text("changed\n", encoding="utf-8")
        dirty = self.generate()
        self.assertIn(f"CJRT-COMMIT:{self.commit}-dirty", dirty)
        self.assertNotEqual(clean, dirty)

    def test_override_precedence_and_repository_free_fallback(self):
        generated = self.generate(override="configured", env_commit="environment")
        self.assertIn("CJRT-COMMIT:configured", generated)

        generated = self.generate(env_commit="environment")
        self.assertIn("CJRT-COMMIT:environment", generated)

        non_repo = self.root / "not-a-repository"
        non_repo.mkdir()
        generated = self.generate(repository=non_repo)
        self.assertIn("CJRT-COMMIT:unknown", generated)


if __name__ == "__main__":
    unittest.main()
