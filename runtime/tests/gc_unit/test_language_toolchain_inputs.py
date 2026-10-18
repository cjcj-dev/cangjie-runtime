#!/usr/bin/env python3
"""Real ELF/archive fixtures exercise the shipped reader CLI, not a GC workload."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class LanguageInputs(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='language-inputs-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.sdk = self.root / 'sdk'
        self.tuple = 'linux_x86_64_cjnative'
        self.target = self.sdk / 'runtime/lib' / self.tuple
        self.host = self.sdk / '.compiler-exec/runtime/lib' / self.tuple
        self.plain = self.root / 'plain.so'
        self.reader = self.root / 'reader/language_toolchain.py'
        self.reader.parent.mkdir()
        source = Path(os.environ.get('LANGUAGE_READER_OVERRIDE', Path(__file__).with_name('language_toolchain.py')))
        shutil.copyfile(source, self.reader)
        shutil.copyfile(Path(__file__).with_name('std_runtime_colour.py'), self.reader.with_name('std_runtime_colour.py'))
        for directory in (self.target, self.host, self.sdk / 'bin', self.sdk / 'libexec',
                          self.sdk / 'lib' / self.tuple, self.sdk / 'modules' / self.tuple,
                          self.sdk / 'third_party/llvm/bin', self.sdk / 'third_party/llvm/lib'):
            directory.mkdir(parents=True, exist_ok=True)
        def library(file, text):
            c = self.root / 'input.c'
            c.write_text(text)
            subprocess.run(['cc', '-shared', '-fPIC', str(c), '-o', str(file)], check=True, capture_output=True)
        library(self.plain, 'void plain(void) {}')
        for directory in (self.target, self.host):
            library(directory / 'libcangjie-runtime.so', 'void CJ_MCC_ReadRefField(void) {}')
            library(directory / 'libboundscheck.so', 'void bounds(void) {}')
        self.llvm = self.sdk / 'third_party/llvm/lib/libLLVM-15.so'
        library(self.llvm, 'void llvm(void) {}')
        c = self.root / 'core.c'; c.write_text('extern void CJ_MCC_ReadRefField(void); void core(void) { CJ_MCC_ReadRefField(); }')
        subprocess.run(['cc', '-c', str(c), '-o', str(self.root / 'core.o')], check=True)
        subprocess.run(['ar', 'rcs', str(self.sdk / 'lib' / self.tuple / 'libcangjie-std-core.a'), str(self.root / 'core.o')], check=True)
        (self.sdk / 'modules' / self.tuple / 'std.core.cjo').write_bytes(b'fixture module bytes')
        for rel in ('bin/cjc', 'libexec/cjc', 'third_party/llvm/bin/llc', 'third_party/llvm/bin/opt'):
            shutil.copyfile('/usr/bin/true', self.sdk / rel)
            (self.sdk / rel).chmod(0o755)
        digest = hashlib.sha256((self.sdk / 'libexec/cjc').read_bytes()).hexdigest()
        (self.sdk / 'compiler-inputs.json').write_text(json.dumps({'commit':'a'*40, 'compilerSha256':digest}))
        self.status = self.target / 'gc_unit_gate.status'
        self.configure(False)

    def configure(self, same):
        host = self.target if same else self.host
        relative = str(host.relative_to(self.sdk))
        (self.sdk / 'bin/cjc.entry').write_text('cjcj-sdk-entry-v1\n1\nlibexec/cjc\nlinux\n\n'+relative+'\nthird_party/llvm/lib\n')
        self.env = {**os.environ, 'GC_UNIT_LANGUAGE_SDK':str(self.sdk), 'GC_UNIT_CJC_RUNTIME_LIB_DIR':str(host),
                    'GCV2_RUNTIME_LIB_DIR':str(self.target), 'GC_UNIT_COLOUR_HOST_RUNTIME':str(self.plain),
                    'GC_UNIT_RUNTIME_TUPLE':self.tuple, 'CJC':str(self.sdk / 'bin/cjc')}

    def call(self, *args):
        return subprocess.run(['python3', str(self.reader), *args], env=self.env, capture_output=True, text=True)

    def record(self):
        self.status.write_text('GATE=STARTED\n')
        result = self.call(); self.assertEqual(result.returncode, 0, result.stderr)
        self.status.write_text('GATE=PASS\nLANGUAGE_TESTS=LANGUAGE_DONE\nTOOLCHAIN_IDENTITY='+result.stdout.strip()+'\n')

    def verify(self, expected):
        result = self.call('--verify-status', str(self.status))
        print('READER_TARGET_ASSERTION', self.id(), 'actual', result.returncode, 'expected', expected)
        self.assertEqual(result.returncode, expected, result.stderr)
        if expected: self.assertIn('LANGUAGE_CACHE_INPUT_CHANGED', result.stderr)

    def test_same_host_first_and_repeat(self):
        self.configure(True); self.record(); self.verify(0); self.verify(0)
        (self.target / 'gate.diagnostic').write_text('new output')
        self.verify(0)

    def test_separate_host_first_and_repeat(self):
        self.record(); self.verify(0); self.verify(0)

    def test_llvm_shared_input_reject_and_restore(self):
        self.record(); original = self.llvm.read_bytes()
        self.llvm.write_bytes(original+b'changed shared dependency'); self.verify(2)
        self.llvm.write_bytes(original); self.verify(0)

    def test_same_host_library_reject_and_restore(self):
        self.configure(True); self.record()
        file = self.target / 'libboundscheck.so'; original=file.read_bytes()
        file.write_bytes(original+b'changed'); self.verify(2)
        file.write_bytes(original); self.verify(0)

    def test_module_independent_control(self):
        self.record(); file=self.sdk / 'modules' / self.tuple / 'std.core.cjo'
        original=file.read_bytes(); file.write_bytes(original+b'changed'); self.verify(2)
        file.write_bytes(original); self.verify(0)

    def test_recipe_selected_extra_library(self):
        directory=self.sdk / 'extra-loader'; directory.mkdir()
        file=directory / 'libextra.so'; shutil.copyfile(self.llvm,file)
        with (self.sdk / 'bin/cjc.entry').open('a') as stream: stream.write('extra-loader\n')
        self.record(); original=file.read_bytes();file.write_bytes(original+b'changed'); self.verify(2)
        file.write_bytes(original);self.verify(0)


if __name__ == '__main__':
    unittest.main(verbosity=2)
