"""Real fixed-source tuple inputs observed at first CMake, never native outputs.

Auxiliary compiler/schema transport uses the retained #848 fixture. LLVM and
runtime are their actual approved Git commits. Receipt records here are input
observations, not tool artifacts or successful production receipts.
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import unittest
import prepare_tools as prepare
import platform_inputs as inputs

TOOL, LLVM, RUNTIME, AUX, WORK = map(lambda x: Path(x).resolve(), sys.argv[1:6])
sys.argv = sys.argv[:1]

class PrivateTuple(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        WORK.mkdir()
        cls.root = WORK / 'tuple'
        cls.env = prepare.tuple_environment(TOOL, dict(os.environ, TUPLE_ROOT=str(cls.root),
                TUPLE_PLATFORM='linux_x86_64', LLVM_TARGETS='X86', PYTHONDONTWRITEBYTECODE='1'))
        for key in ('RUNTIME_REF', 'RUNTIME_SRC_URL'):
            cls.env.pop(key, None)
        aux_sha = subprocess.check_output(['git', '-C', str(AUX), 'rev-parse', 'HEAD'], text=True).strip()
        cls.env.update(CANGJIE_COMPILER_URL='file://' + str(AUX), CANGJIE_COMPILER_SHA=aux_sha,
                       FLATBUFFERS_URL='file://' + str(AUX), FLATBUFFERS_SHA=aux_sha,
                       CJCJ_SRCBUILD_SOURCE_MIRRORS=inputs.RUNTIME_URL + '=file://' + str(RUNTIME) +
                       ';https://github.com/cjcj-dev/cjcj-llvm.git=file://' + str(LLVM) +
                       ';file://' + str(AUX) + '=file://' + str(AUX),
                       CJCJ_SRCBUILD_REQUIRE_MIRRORS='1')
        with (WORK / 'fetch.log').open('w') as log:
            result = subprocess.run(['bash', 'ci/platform_tuples/fetch_sources.sh'], cwd=TOOL,
                                    env=cls.env, stdout=log, stderr=subprocess.STDOUT)
        (WORK / 'fetch.rc').write_text(str(result.returncode) + '\n')
        assert result.returncode == 0, 'actual fixed-input fetch failed; stop dependent tests'

    def test_source_environment(self):
        expected = ('private', inputs.RUNTIME_URL, inputs.PAIRED_RUNTIME, inputs.PRODUCER)
        actual = tuple(self.env[key] for key in ('CJCJ_LLVM_RUNTIME_MODE', 'CJCJ_LLVM_RUNTIME_URL',
                                                'CJCJ_LLVM_RUNTIME_SHA', 'LLVM_SHA'))
        print('TARGET_SOURCE_ENV_REACHED actual=' + repr(actual), flush=True)
        self.assertEqual(actual, expected, 'TARGET_APPROVED_SOURCE_ENV')
        for name, head in [('paired-runtime', inputs.PAIRED_RUNTIME), ('llvm-project', inputs.PRODUCER)]:
            self.assertEqual(subprocess.check_output(['git', '-C', str(self.root / name), 'rev-parse', 'HEAD'], text=True).strip(), head)

    def test_cmake_source(self):
        bin_dir = WORK / 'bin'
        bin_dir.mkdir()
        capture = WORK / 'cmake.json'
        cmake = bin_dir / 'cmake'
        cmake.write_text('#!/usr/bin/env python3\nimport json,os,sys\nfrom pathlib import Path\nPath(os.environ["TUPLE_CMAKE_CAPTURE"]).write_text(json.dumps(sys.argv[1:]))\nsys.exit(86)\n')
        cmake.chmod(0o755)
        env = dict(self.env, PATH=str(bin_dir) + os.pathsep + self.env['PATH'], TUPLE_CMAKE_CAPTURE=str(capture))
        with (WORK / 'build-boundary.log').open('w') as log:
            result = subprocess.run(['bash', 'ci/platform_tuples/build_tuple.sh'], cwd=TOOL, env=env,
                                    stdout=log, stderr=subprocess.STDOUT)
        (WORK / 'build-boundary.rc').write_text(str(result.returncode) + '\n')
        self.assertEqual(result.returncode, 86, 'actual build must reach first CMake observation boundary')
        command = json.loads(capture.read_text())
        selected = [arg for arg in command if arg.startswith('-DCANGJIE_RUNTIME_SOURCE_DIR=')]
        print('TARGET_CMAKE_SOURCE_REACHED actual=' + repr(selected), flush=True)
        self.assertEqual(selected, ['-DCANGJIE_RUNTIME_SOURCE_DIR=' + str((self.root / 'paired-runtime').resolve())])
        self.assertIn(str(self.root / 'llvm-project/llvm'), command)
        pair = prepare.pair_receipt(self.root, self.env, WORK / 'receipt-logs')
        (WORK / 'pair-observation.json').write_text(json.dumps(pair, indent=2) + '\n')
        self.assertEqual(pair['paired_runtime_sha'], inputs.PAIRED_RUNTIME)

    def test_pair_rejection(self):
        print('TARGET_PAIR_REJECTION_REACHED', flush=True)
        with self.assertRaisesRegex(ValueError, 'unapproved LLVM/runtime pair'):
            prepare.tuple_environment(TOOL, dict(self.env, LLVM_SHA='0' * 40))
        with self.assertRaisesRegex(ValueError, 'unapproved private runtime source'):
            prepare.tuple_environment(TOOL, dict(self.env, CJCJ_LLVM_RUNTIME_URL='https://invalid.example/runtime.git'))

    def test_missing_source(self):
        result = subprocess.run(['bash', str(TOOL / 'ci/fetch-llvm-runtime.sh'), '--verify-checkout',
                                 str(WORK / 'missing-source')], env=self.env, capture_output=True, text=True)
        print('TARGET_MISSING_SOURCE_REACHED rc=' + str(result.returncode), flush=True)
        self.assertEqual(result.returncode, 1)
        self.assertIn('private runtime destination is not a Git worktree', result.stderr)

    def test_receipt_rejection(self):
        artifact = WORK / 'receipt-observation'
        artifact.mkdir()
        hashes = dict(tuple='a' * 64, reader='b' * 64)
        pair = prepare.pair_receipt(self.root, self.env, WORK / 'receipt-consumer-logs')
        record = dict(pair, tools_source_sha=inputs.TOOL_SOURCE, llvm_sha=inputs.PRODUCER,
                      artifact='fixed-llvm-tools-linux_x86_64', tuple_manifest_sha256=hashes['tuple'],
                      reader_manifest_sha256=hashes['reader'])
        path = artifact / 'producer.json'
        path.write_text(json.dumps(record))
        self.assertEqual(inputs.tool_receipt(artifact, 'linux_x86_64', hashes), record)
        for key in ('tools_source_sha', 'paired_runtime_sha'):
            bad = dict(record, **{key: '0' * 40})
            path.write_text(json.dumps(bad))
            error = None
            try:
                inputs.tools(artifact, WORK / 'never-produced', 'linux_x86_64', hashes)
            except Exception as caught:
                error = caught
            print('TARGET_RECEIPT_REJECTION_REACHED key=' + key + ' actual=' + str(error), flush=True)
            self.assertIsInstance(error, ValueError, 'TARGET_RECEIPT_REJECTION')
            self.assertIn('tool producer receipt differs', str(error), 'TARGET_RECEIPT_REJECTION')
        path.unlink()
        with self.assertRaises(FileNotFoundError):
            inputs.tools(artifact, WORK / 'never-produced', 'linux_x86_64', hashes)
        self.assertFalse((WORK / 'never-produced').exists())

if __name__ == '__main__':
    unittest.main(verbosity=2)
