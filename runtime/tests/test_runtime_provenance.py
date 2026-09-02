#!/usr/bin/env python3
# Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
# This source file is part of the Cangjie project, licensed under Apache-2.0
# with Runtime Library Exception.
#
# See https://cangjie-lang.cn/pages/LICENSE for license information.

import os
import pathlib
import re
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
        self.source = self.repo / "runtime"
        self.source.mkdir(parents=True)
        self.output = self.root / "generated" / "RuntimeProvenance.cpp"
        self.run_command("git", "init", "-q", cwd=self.repo)
        self.run_command("git", "config", "user.name", "Runtime Test", cwd=self.repo)
        self.run_command("git", "config", "user.email", "runtime-test@example.invalid", cwd=self.repo)
        (self.source / "tracked.cpp").write_text("clean\n", encoding="utf-8")
        self.run_command("git", "add", "runtime/tracked.cpp", cwd=self.repo)
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

    def generate(self, repository=None, source=None, override=None, env_commit=None):
        repository = repository or self.repo
        source = source or pathlib.Path(repository) / "runtime"
        command = [
            "cmake",
            f"-DREPOSITORY_DIR={repository}",
            f"-DSOURCE_DIR={source}",
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

    @staticmethod
    def stamp(generated):
        return re.search(r"CJRT-COMMIT:([^\\]+)", generated).group(1)

    def test_tree_state_is_sampled_each_run_without_rewriting_stable_output(self):
        clean = self.generate()
        self.assertIn(f"CJRT-COMMIT:{self.commit}", clean)
        clean_mtime = self.output.stat().st_mtime_ns

        self.assertEqual(clean, self.generate())
        self.assertEqual(clean_mtime, self.output.stat().st_mtime_ns)

        (self.source / "tracked.cpp").write_text("changed\n", encoding="utf-8")
        dirty = self.generate()
        self.assertIn(f"CJRT-COMMIT:{self.commit}-dirty", dirty)
        self.assertNotEqual(clean, dirty)

    def test_head_is_sampled_each_run(self):
        before = self.generate()
        self.assertIn(f"CJRT-COMMIT:{self.commit}", before)

        (self.source / "tracked.cpp").write_text("committed change\n", encoding="utf-8")
        self.run_command("git", "add", "runtime/tracked.cpp", cwd=self.repo)
        self.run_command("git", "commit", "-q", "-m", "advance fixture head", cwd=self.repo)
        new_commit = self.run_command("git", "rev-parse", "HEAD", cwd=self.repo).stdout.strip()

        after = self.generate()
        self.assertNotEqual(self.commit, new_commit)
        self.assertNotEqual(before, after)
        self.assertIn(f"CJRT-COMMIT:{new_commit}", after)
        self.assertNotIn(f"CJRT-COMMIT:{new_commit}-dirty", after)

    def test_declaration_precedence_does_not_override_git_identity(self):
        generated = self.generate(override="configured", env_commit="environment")
        self.assertIn(f"CJRT-COMMIT:{self.commit}", generated)
        self.assertIn("CJRT-DECLARED:configured", generated)

        generated = self.generate(env_commit="environment")
        self.assertIn(f"CJRT-COMMIT:{self.commit}", generated)
        self.assertIn("CJRT-DECLARED:environment", generated)

    def test_repository_free_identity_ignores_declarations_and_tracks_source_content(self):
        non_repo = self.root / "not-a-repository"
        source = non_repo / "runtime"
        (source / "src").mkdir(parents=True)
        source_file = source / "src" / "source.cpp"
        source_file.write_text("source-a\n", encoding="utf-8")

        env_a = self.generate(repository=non_repo, source=source, env_commit="source-a")
        env_a_stamp = self.stamp(env_a)
        self.assertRegex(env_a_stamp, r"^src-[0-9a-f]{64}$")
        self.assertIn("CJRT-DECLARED:source-a", env_a)

        generated_build = source / "tests" / "gc_unit" / "build_standalone"
        generated_build.mkdir(parents=True)
        (generated_build / "generated.txt").write_text("build output\n", encoding="utf-8")
        env_b = self.generate(repository=non_repo, source=source, env_commit="source-b")
        self.assertEqual(env_a_stamp, self.stamp(env_b))
        self.assertIn("CJRT-DECLARED:source-b", env_b)

        source_file.write_text("source-b\n", encoding="utf-8")
        changed = self.generate(repository=non_repo, source=source, env_commit="source-a")
        self.assertNotEqual(env_a_stamp, self.stamp(changed))


if __name__ == "__main__":
    unittest.main()
