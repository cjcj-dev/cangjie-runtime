#!/usr/bin/env python3
"""Offline input/argv tests. Synthetic tool headers are never platform results."""
import gzip
import json
import os
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

import platform_inputs as inputs


class Wiring(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.environment = dict(GITHUB_SHA='a' * 40, METADATA_RUNTIME_SOURCE_SHA='a' * 40,
                                METADATA_CANDIDATE_SHA='a' * 40, GITHUB_EVENT_NAME='workflow_dispatch',
                                GITHUB_REF_NAME='sym/1496-fixtures-1006', METADATA_TUPLE_RUN='123', GITHUB_REPOSITORY='cjcj-dev/cangjie-runtime',
                                METADATA_TOOL_REPOSITORY='cjcj-dev/cangjie-runtime', METADATA_TUPLE_ATTEMPT='1',
                                METADATA_TOOL_PRODUCER_SHA='a' * 40,
                                METADATA_TOOL_ARTIFACT_IDS=json.dumps({name: '456' for name in inputs.PLATFORMS}),
                                METADATA_TOOL_MANIFESTS=json.dumps({name: {'tuple': 'b' * 64, 'reader': 'c' * 64}
                                                                  for name in inputs.PLATFORMS}))

    def test_bound_ref(self):
        source, hashes = inputs.dispatch(self.environment)
        self.assertEqual(source, self.environment['GITHUB_SHA'])
        self.assertEqual(set(hashes), set(inputs.PLATFORMS))
        self.environment['GITHUB_REF_NAME'] = 'sym/1478-other'
        with self.assertRaisesRegex(ValueError, 'bind'):
            inputs.dispatch(self.environment)

    def test_source_binding(self):
        self.environment['METADATA_RUNTIME_SOURCE_SHA'] = 'd' * 40
        with self.assertRaisesRegex(ValueError, 'bind'):
            inputs.dispatch(self.environment)

    def test_resume_isolation(self):
        self.environment['METADATA_RESUME_RUN'] = '123'
        with self.assertRaisesRegex(ValueError, 'resume'):
            inputs.dispatch(self.environment)

    def test_checkout_consumption(self):
        with patch.object(inputs.subprocess, 'check_output', side_effect=['a' * 40 + '\n', '']):
            self.assertEqual(inputs.checkout_identity(self.root, 'a' * 40), 'a' * 40)
        with patch.object(inputs.subprocess, 'check_output', return_value='d' * 40 + '\n'):
            with self.assertRaisesRegex(ValueError, 'checkout'):
                inputs.checkout_identity(self.root, 'a' * 40)

    def test_config_consumption(self):
        recipe = self.root / 'compile_commands.json'
        recipe.write_text(json.dumps([{'file': '/runtime/src/Heap/z/zGeneration.cpp',
                                     'arguments': ['clang++', '-DMRT_TESTABLE_INTERNALS=1', '-DMRT_GC_UNIT_TESTS=1']}]))
        self.assertEqual(inputs.configuration(self.root), ['MRT_TESTABLE_INTERNALS', 'MRT_GC_UNIT_TESTS'])
        recipe.write_text(json.dumps([{'file': '/runtime/src/Heap/z/zGeneration.cpp',
                                     'arguments': ['clang++', '-DNDEBUG']}]))
        self.assertEqual(inputs.configuration(self.root), ['NDEBUG'])

    def test_build_parameter_consumption(self):
        command = inputs.metadata_command(Path('/source/runtime'), Path('/product/build'),
                                          Path('/product/lib'), Path('/test/build'), '/tools/ld64.lld')
        self.assertIn('-DMANAGED_METADATA_LINKER=/tools/ld64.lld', command)
        self.assertIn('-DPRODUCT_BUILD=/product/build', command)
        self.assertIn('-DGCV2_RUNTIME_LIB_DIR=/product/lib', command)
        self.assertEqual(command[2], '/source/runtime/tests/gc_unit/metadata')

    def test_original_build_entry(self):
        # Execute the existing build.py fixture configure branch up to its first
        # checked subprocess. Capture argv; never create or pretend to run DSOs.
        import ast
        from types import SimpleNamespace
        self.root.joinpath('compile_commands.json').write_text(json.dumps([{
            'file': '/runtime/src/Heap/z/zGeneration.cpp', 'arguments': ['clang++', '-DNDEBUG']}]))
        tree = ast.parse(Path(__file__).with_name('build.py').read_text())
        branch = next(node for node in ast.walk(tree) if isinstance(node, ast.If)
                      and ast.unparse(node.test) == "args.mode == 'fixtures'"
                      and any(isinstance(child, ast.Assign) and any(isinstance(t, ast.Name) and t.id == 'testbuild'
                              for t in child.targets) for child in node.body))
        code = compile(ast.fix_missing_locations(ast.Module(body=[branch], type_ignores=[])), '<original-build-entry>', 'exec')
        commands = []
        class CommandCaptured(Exception):
            pass
        def capture(command, label):
            commands.append((command, label))
            raise CommandCaptured()
        record = {}
        scope = dict(args=SimpleNamespace(mode='fixtures'), out=Path('/output'), inputs=inputs,
                     build=self.root, tree=Path('/source/runtime'), libdir=Path('/product/lib'),
                     tool_identity={'linker': '/tools/ld64.lld'}, os=os, record=record, checked=capture)
        with self.assertRaises(CommandCaptured):
            exec(code, scope)
        command, label = commands[0]
        self.assertEqual(label, 'fixture-test-configure')
        self.assertIn('-DMANAGED_METADATA_LINKER=/tools/ld64.lld', command)
        self.assertIn('-DCMAKE_SHARED_LINKER_FLAGS=--ld-path=/tools/ld64.lld', command)
        self.assertIn('-DGCV2_RUNTIME_LIB_DIR=/product/lib', command)
        self.assertEqual(record['product_macros'], ['NDEBUG'])

    def test_platform_link_commands(self):
        # Execute the existing builder's actual link branch with a capture-only
        # run function. No object, DSO or platform result is manufactured.
        import ast
        from types import SimpleNamespace
        tree = ast.parse(Path(__file__).with_name('build_managed_fixture.py').read_text())
        branch = next(node for node in ast.walk(tree) if isinstance(node, ast.If)
                      and ast.unparse(node.test) == "system == 'windows'"
                      and any(isinstance(child, ast.Assign) and any(isinstance(t, ast.Name) and t.id == 'artifact'
                              for t in child.targets) for child in node.body))
        code = compile(ast.fix_missing_locations(ast.Module(body=[branch], type_ignores=[])), '<builder-link-branch>', 'exec')
        for system, name in [('macos', 'ld64.lld'), ('windows', 'ld.lld'), ('windows', 'lld-link')]:
            commands = []
            scope = dict(system=system, linker_name=name, flags=['--target=fixture-only'],
                         a=SimpleNamespace(cc='host-clang', linker='/tools/' + name, output=Path('/output')),
                         run=lambda command: commands.append([str(x) for x in command]))
            exec(code, scope)
            command = commands[0]
            if system == 'macos':
                self.assertIn('--ld-path=/tools/ld64.lld', command)
                self.assertIn('-dynamiclib', command)
            else:
                self.assertEqual(command[0], '/tools/' + name)
                self.assertEqual('-flavor' in command, name == 'ld.lld')
                self.assertIn('/noentry', command)

    def tool_input(self):
        # Deliberately inert bytes: only validation and command generation run.
        data = bytearray(64)
        data[:6] = b'\x7fELF\x02\x01'
        struct.pack_into('<H', data, 18, 183)
        for name in ('ld.lld', 'llvm-readobj'):
            with gzip.open(self.root / (name + '.gz'), 'wb') as stream:
                stream.write(data)
        import hashlib
        digest = hashlib.sha256(data).hexdigest()
        (self.root / 'llvm-tools.manifest').write_text('\n'.join([
            'PLATFORM=linux_aarch64', 'LLVM_SHA=' + inputs.PRODUCER,
            'LLD_SOURCE=tuple:' + inputs.PRODUCER, 'LLD_TOOL=ld.lld',
            'LLD_SHA256=' + digest, 'LLD_VERSION=LLD fixture-version']))
        (self.root / 'llvm-tools.packaged.manifest').write_text('\n'.join([
            'SCHEMA=packaged-v1', 'LLVM_SHA=' + inputs.PRODUCER, 'BASE_SDK_SHA256=' + '0' * 64,
            'tool\tpresent\tsource\tversion\tsha256',
            'llvm-readobj\tyes\ttuple:' + inputs.PRODUCER + '\tLLVM fixture-version\t' + digest]))
        return {'tuple': inputs.sha(self.root / 'llvm-tools.manifest'),
                'reader': inputs.sha(self.root / 'llvm-tools.packaged.manifest')}

    def test_tool_selection(self):
        hashes = self.tool_input()
        with patch.object(inputs.subprocess, 'check_output', side_effect=['LLD fixture-version', 'LLVM fixture-version']):
            selected = inputs.tools(self.root, self.root / 'selected', 'linux_aarch64', hashes)
        self.assertEqual(selected['linker'], str(self.root / 'selected/ld.lld'))
        self.assertEqual(selected['reader'], str(self.root / 'selected/llvm-readobj'))
        self.assertEqual(selected['producer'], inputs.PRODUCER)

    def test_payload_rejection(self):
        hashes = self.tool_input()
        # Preserve native header and manifest; corrupt only the transported body.
        with gzip.open(self.root / 'ld.lld.gz', 'rb') as stream:
            data = stream.read() + b'changed'
        with gzip.open(self.root / 'ld.lld.gz', 'wb') as stream:
            stream.write(data)
        with patch.object(inputs.subprocess, 'check_output', side_effect=['LLD fixture-version', 'LLVM fixture-version']):
            with self.assertRaisesRegex(ValueError, 'payload digest'):
                inputs.tools(self.root, self.root / 'selected', 'linux_aarch64', hashes)

    def test_manifest_rejection(self):
        hashes = self.tool_input()
        hashes['tuple'] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'manifest differs'):
            inputs.tools(self.root, self.root / 'selected', 'linux_aarch64', hashes)

    def test_native_rejection(self):
        hashes = self.tool_input()
        with self.assertRaisesRegex(ValueError, 'not native windows'):
            inputs.native(self.root / 'llvm-tools.manifest', 'windows_x86_64')


if __name__ == '__main__':
    unittest.main(verbosity=2)
