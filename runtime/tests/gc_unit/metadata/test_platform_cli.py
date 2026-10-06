#!/usr/bin/env python3
"""Full CLI subprocess qualification with synthetic repositories and external tools.
No native product/tool qualification: platform and version commands are mocks.
"""
import copy
import gzip
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

import platform_inputs as inputs

HERE = Path(__file__).resolve().parent


class CLI(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.driver = self.root / 'driver'
        self.entry = self.driver / 'runtime/tests/gc_unit/metadata'
        self.entry.mkdir(parents=True)
        for name in ('build.py', 'run.py', 'platform_inputs.py'):
            shutil.copy2(HERE / name, self.entry / name)
        self.git(self.driver, 'init', '-q')
        self.git(self.driver, 'add', '.')
        self.git(self.driver, '-c', 'user.name=Zxilly', '-c', 'user.email=zxilly@outlook.com', 'commit', '-qm', 'synthetic CLI input')
        self.head = self.git(self.driver, 'rev-parse', 'HEAD').strip()
        self.source = self.root / 'source'
        subprocess.run(['git', 'clone', '-q', str(self.driver), str(self.source)], check=True)
        self.artifact = self.root / 'artifact'
        self.artifact.mkdir()
        self.marker = self.root / 'native-starts'
        self.mock = self.root / 'external-mocks'
        self.mock.mkdir()
        (self.mock / 'sitecustomize.py').write_text('''import os, platform, subprocess, pathlib, json, io, urllib.request
platform.system = lambda: os.environ.get('MOCK_SYSTEM', 'Linux')
platform.machine = lambda: os.environ.get('MOCK_MACHINE', 'aarch64')
original = subprocess.check_output
def output(argv, *args, **kwargs):
    if str(argv[0]) == 'git':
        return original(argv, *args, **kwargs)
    with open(os.environ['MOCK_NATIVE_MARKER'], 'a') as f:
        f.write(json.dumps([str(x) for x in argv]) + '\\n')
    return 'fixture-version'
subprocess.check_output = output
def urlopen(request):
    key = request.full_url.split('https://api.github.com', 1)[1]
    return io.BytesIO(json.dumps(json.loads(os.environ['MOCK_API'])[key]).encode())
urllib.request.urlopen = urlopen
''')
        clang = self.mock / 'clang'
        clang.write_text('#!/bin/sh\nprintf "compiler\\n" >> "$MOCK_NATIVE_MARKER"\nexit 86\n')
        clang.chmod(0o755)
        self.data = bytearray(64)
        self.data[:6] = b'\x7fELF\x02\x01'
        struct.pack_into('<H', self.data, 18, 183)
        self.write_tools()
        self.env = dict(os.environ, PYTHONPATH=str(self.mock), PATH=str(self.mock) + os.pathsep + os.environ['PATH'],
                        MOCK_NATIVE_MARKER=str(self.marker), RUNNER_TEMP=str(self.root / 'out'),
                        GITHUB_SHA=self.head, GITHUB_EVENT_NAME='workflow_dispatch',
                        GITHUB_REF_NAME='sym/1496-fixtures-1006', GITHUB_REPOSITORY='cjcj-dev/cangjie-runtime',
                        GITHUB_REPOSITORY_VISIBILITY='public', METADATA_RUNTIME_SOURCE_SHA=self.head,
                        METADATA_CANDIDATE_SHA=self.head, METADATA_RUNTIME_CHECKOUT=str(self.source),
                        METADATA_TOOL_REPOSITORY='cjcj-dev/cangjie-runtime', METADATA_TUPLE_RUN='123',
                        METADATA_TUPLE_ATTEMPT='1', METADATA_TOOL_PRODUCER_SHA=self.head,
                        METADATA_TOOL_ARTIFACT_IDS=json.dumps({p: '456' for p in inputs.PLATFORMS}),
                        METADATA_TOOL_MANIFESTS=json.dumps({p: self.hashes for p in inputs.PLATFORMS}),
                        METADATA_TUPLE_ARTIFACT=str(self.artifact), METADATA_PLATFORM='linux-arm64',
                        GITHUB_OUTPUT=str(self.root / 'github-output'), GITHUB_TOKEN='synthetic-never-sent')
        self.api = {
            '/repos/cjcj-dev/cangjie-runtime/actions/runs/123/attempts/1/jobs': {'jobs': [dict(name='metadata-tools-linux_aarch64', conclusion='success', started_at='2026-10-06T01:00:00Z', completed_at='2026-10-06T03:00:00Z')]},
            '/repos/cjcj-dev/cangjie-runtime/actions/runs/123': dict(id=123, run_attempt=1,
                repository={'full_name': 'cjcj-dev/cangjie-runtime'}, head_repository={'full_name': 'cjcj-dev/cangjie-runtime'},
                head_sha=self.head, head_branch='sym/1496-fixtures-1006', event='workflow_dispatch',
                path='.github/workflows/metadata-platform.yml', conclusion='success', run_started_at='2026-10-06T01:00:00Z'),
            '/repos/cjcj-dev/cangjie-runtime/actions/artifacts/456': dict(id=456, expired=False,
                name='fixed-llvm-tools-linux_aarch64', workflow_run={'id': 123, 'head_sha': self.head},
                created_at='2026-10-06T02:00:00Z')}

    def git(self, root, *args):
        return subprocess.check_output(['git', '-C', str(root), *args], text=True)

    def write_tools(self):
        digest = hashlib.sha256(self.data).hexdigest()
        for name in ('ld.lld', 'llvm-readobj'):
            with gzip.open(self.artifact / (name + '.gz'), 'wb') as output:
                output.write(self.data)
        (self.artifact / 'llvm-tools.manifest').write_text('\n'.join([
            'PLATFORM=linux_aarch64', 'LLVM_SHA=' + inputs.PRODUCER, 'LLD_SOURCE=tuple:' + inputs.PRODUCER,
            'LLD_TOOL=ld.lld', 'LLD_SHA256=' + digest, 'LLD_VERSION=fixture-version']))
        (self.artifact / 'llvm-tools.packaged.manifest').write_text('\n'.join([
            'SCHEMA=packaged-v1', 'LLVM_SHA=' + inputs.PRODUCER, 'BASE_SDK_SHA256=' + '0' * 64,
            'tool\tpresent\tsource\tversion\tsha256',
            'llvm-readobj\tyes\ttuple:' + inputs.PRODUCER + '\tfixture-version\t' + digest]))
        self.hashes = {'tuple': inputs.sha(self.artifact / 'llvm-tools.manifest'),
                       'reader': inputs.sha(self.artifact / 'llvm-tools.packaged.manifest')}

    def approve_changed_manifests(self):
        self.hashes = {k: inputs.sha(self.artifact / name) for k, name in
                      [('tuple', 'llvm-tools.manifest'), ('reader', 'llvm-tools.packaged.manifest')]}
        self.env['METADATA_TOOL_MANIFESTS'] = json.dumps({p: self.hashes for p in inputs.PLATFORMS})

    def invoke(self, build=True, extra=()):
        self.env['MOCK_API'] = json.dumps(self.api)
        argv = [sys.executable, str(self.entry / ('build.py' if build else 'platform_inputs.py'))]
        argv += ['fixtures', '--config', 'default'] if build else list(extra)
        result = subprocess.run(argv, env=self.env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        print('CLI_RESULT', self.id().split('.')[-1], 'rc=' + str(result.returncode), result.stdout.strip(), flush=True)
        return result

    def reject(self, text, build=True, extra=()):
        result = self.invoke(build, extra)
        self.assertNotEqual(result.returncode, 0, 'TARGET rejection rc')
        self.assertIn(text, result.stdout, 'TARGET exact input rejection')
        self.assertFalse(self.marker.exists(), 'TARGET no native tool starts before input rejection')
        print('TARGET_EXECUTED', self.id().split('.')[-1], flush=True)

    def test_dispatch_control(self):
        self.assertEqual(self.invoke(False).returncode, 0)

    def test_configuration_control(self):
        recipe = self.root / 'recipe'
        recipe.mkdir()
        (recipe / 'compile_commands.json').write_text(json.dumps([{'file': '/src/Heap/z/zGeneration.cpp', 'arguments': ['clang++', '-DNDEBUG']}]))
        result = self.invoke(False, ['--configuration', str(recipe)])
        self.assertEqual((result.returncode, result.stdout.strip()), (0, 'NDEBUG'))

    def test_complete_cli_control(self):
        result = self.invoke()
        self.assertEqual(result.returncode, 86)
        starts = self.marker.read_text().splitlines()
        self.assertEqual(len(starts), 3)  # linker, reader, then mock compiler refusal
        record = json.loads((self.root / 'out/metadata/build-result.json').read_text())
        self.assertEqual(record['workflow_sha'], self.head)
        self.assertEqual(record['runtime_source_sha'], self.head)
        self.assertEqual(record['tool_identity']['producer'], inputs.PRODUCER)
        self.assertEqual(record['compiler']['rc'], 86)
        self.assertNotIn('configure', record)

    def test_source_head(self):
        self.git(self.source, '-c', 'user.name=Zxilly', '-c', 'user.email=zxilly@outlook.com', 'commit', '--allow-empty', '-qm', 'other')
        self.reject('runtime checkout does not match')

    def test_source_dirty(self):
        (self.source / 'untracked').write_text('dirty')
        self.reject('runtime checkout must be clean')

    def test_driver_head(self):
        self.env['GITHUB_SHA'] = self.env['METADATA_RUNTIME_SOURCE_SHA'] = self.env['METADATA_CANDIDATE_SHA'] = self.git(self.source, 'rev-parse', 'HEAD').strip()
        self.git(self.driver, '-c', 'user.name=Zxilly', '-c', 'user.email=zxilly@outlook.com', 'commit', '--allow-empty', '-qm', 'other driver')
        self.reject('workflow driver checkout differs')

    def test_host(self):
        self.env['MOCK_MACHINE'] = 'x86_64'
        self.reject('requested tuple differs from native runner')

    def test_repository(self):
        self.env['METADATA_TOOL_REPOSITORY'] = 'cjcj-dev/cjcj'
        self.reject('current runtime repository')

    def test_attempt_input(self):
        self.env['METADATA_TUPLE_ATTEMPT'] = '0'
        self.reject('approved producer run attempt')

    def test_producer_input(self):
        self.env['METADATA_TOOL_PRODUCER_SHA'] = 'bad'
        self.reject('approved runtime tool producer checkout')

    def test_run_input(self):
        self.env['METADATA_TUPLE_RUN'] = '0'
        self.reject('same-repository tuple run')

    def test_manifest_digest(self):
        self.env['METADATA_TOOL_MANIFESTS'] = json.dumps({p: {'tuple': '0' * 64, 'reader': self.hashes['reader']} for p in inputs.PLATFORMS})
        self.reject('tool manifest differs')

    def test_manifest_path(self):
        manifest = self.artifact / 'llvm-tools.manifest'
        escaped = self.root / 'escaped'
        manifest.rename(escaped)
        manifest.symlink_to(escaped)
        self.reject('manifest path escapes')

    def test_tool_producer(self):
        manifest = self.artifact / 'llvm-tools.manifest'
        manifest.write_text(manifest.read_text().replace(inputs.PRODUCER, 'f' * 40))
        self.approve_changed_manifests()
        self.reject('tuple platform/linker producer mismatch')

    def test_payload_digest(self):
        with gzip.open(self.artifact / 'ld.lld.gz', 'wb') as output:
            output.write(self.data + b'corrupt')
        self.reject('tool payload digest mismatch')

    def invalid_native(self, name):
        with gzip.open(self.artifact / (name + '.gz'), 'wb') as output:
            output.write(b'not-native' * 8)
        manifest = self.artifact / ('llvm-tools.manifest' if name == 'ld.lld' else 'llvm-tools.packaged.manifest')
        manifest.write_text(manifest.read_text().replace(hashlib.sha256(self.data).hexdigest(), hashlib.sha256(b'not-native' * 8).hexdigest()))
        self.approve_changed_manifests()
        self.reject('tool is not native linux_aarch64')

    def test_native_linker(self): self.invalid_native('ld.lld')
    def test_native_reader(self): self.invalid_native('llvm-readobj')

    def test_service_control(self):
        self.assertEqual(self.invoke(False, ['--verify-run']).returncode, 0)
        self.assertEqual((self.root / 'github-output').read_text(), 'artifact_id=456\n')

    def service_bad(self, key, value, artifact=False):
        entity = self.api['/repos/cjcj-dev/cangjie-runtime/actions/' + ('artifacts/456' if artifact else 'runs/123')]
        entity[key] = value
        self.reject('unapproved producer artifact' if artifact else 'unapproved producer repository/run/attempt/checkout/workflow', False, ['--verify-run'])

    def test_service_job(self):
        self.api['/repos/cjcj-dev/cangjie-runtime/actions/runs/123/attempts/1/jobs']['jobs'][0]['conclusion'] = 'failure'
        self.reject('successful same-repository tools producer job', False, ['--verify-run'])

    def test_service_run(self): self.service_bad('id', 999)
    def test_service_attempt(self): self.service_bad('run_attempt', 2)
    def test_service_repository(self): self.service_bad('repository', {'full_name': 'cjcj-dev/cjcj'})
    def test_service_producer(self): self.service_bad('head_sha', 'f' * 40)
    def test_service_artifact(self): self.service_bad('workflow_run', {'id': 999, 'head_sha': self.head}, True)
    def test_service_old_attempt(self): self.service_bad('created_at', '2026-10-05T00:00:00Z', True)


if __name__ == '__main__':
    unittest.main(verbosity=2)
