#!/usr/bin/env python3
"""Actual prepare_tools CLI, with only external runner/download/process I/O mocked.

30 frozen cases per arm: three approved download boundaries, 24 exact-input
rejections, and three real downloaded-byte hash rejections. No native production.
"""
import copy
import io
import json
import os
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import prepare_tools

SOURCE = Path(sys.argv[1]).resolve()
sys.argv = [sys.argv[0], *sys.argv[2:]]
REAL_CHECK_OUTPUT = subprocess.check_output
REAL_RUN = subprocess.run
APPROVED = prepare_tools.approved_sdk_inputs(SOURCE)


class DownloadBoundary(Exception):
    pass


class ProductionBoundary(Exception):
    pass


def cli(selected, sdk, downloaded=False):
    downloads, commands = [], []
    systems = {'linux-arm64': ('Linux', 'aarch64'), 'windows-x64': ('Windows', 'AMD64'),
               'macos-arm64': ('Darwin', 'arm64')}

    def external_read(argv, **kwargs):
        # Isolated fixed-source module export has no checkout metadata. Simulate
        # only git's external identity reads; provider code and hashes run intact.
        if argv[0] == 'git':
            if argv[-2:] == ['rev-parse', 'HEAD']:
                return prepare_tools.TOOL_SOURCE + '\n'
            if argv[-2:] == ['status', '--porcelain']:
                return ''
            raise AssertionError('unexpected git I/O: ' + repr(argv))
        return REAL_CHECK_OUTPUT(argv, **kwargs)

    def download(url):
        downloads.append(url)
        if not downloaded:
            raise DownloadBoundary()
        return io.BytesIO(b'offline bytes deliberately unlike the approved SDK archive')

    def production(argv, **kwargs):
        if argv[0] == 'node':
            return REAL_RUN(argv, **kwargs)
        commands.append([str(x) for x in argv])
        raise ProductionBoundary()

    with tempfile.TemporaryDirectory() as root:
        environment = dict(METADATA_PLATFORM=selected, GITHUB_REPOSITORY='cjcj-dev/cangjie-runtime',
                           TOOL_PREPARE_INPUTS=json.dumps({'base_sdk': {selected: sdk}}),
                           TUPLE_ROOT=root, TUPLE_PLATFORM=prepare_tools.inputs.PLATFORMS[selected])
        with patch.dict(os.environ, environment), \
                patch.object(sys, 'argv', [str(HERE / 'prepare_tools.py'), str(SOURCE)]), \
                patch('platform.system', return_value=systems[selected][0]), \
                patch('platform.machine', return_value=systems[selected][1]), \
                patch('subprocess.check_output', side_effect=external_read), \
                patch('subprocess.run', side_effect=production), \
                patch('urllib.request.urlopen', side_effect=download), \
                patch('shutil.which', return_value='/offline/sccache'):
            error = None
            try:
                runpy.run_path(str(HERE / 'prepare_tools.py'), run_name='__main__')
            except (ValueError, DownloadBoundary, ProductionBoundary) as caught:
                error = caught
        return error, downloads, commands


class SdkCli(unittest.TestCase):
    pass


def case(selected, kind):
    def test(self):
        sdk = copy.deepcopy(APPROVED['base_sdk'][selected])
        url = sdk['url']
        mutations = {
            'protocol': url.replace('https://', 'http://'),
            'host': url.replace('gitcode.com/', 'gitcode.com.evil/'),
            'provider': url.replace('/Cangjie/', '/Other/'),
            'path': url.replace('/nightly_build/', '/another_repo/'),
            'version': url.replace('20260925001050', '20260925001051'),
            'platform': APPROVED['base_sdk'][next(p for p in APPROVED['base_sdk'] if p != selected)]['url'],
            'archive': url + '.unexpected',
        }
        if kind in mutations:
            sdk['url'] = mutations[kind]
        if kind == 'digest':
            sdk['sha256'] = '0' * 64
        error, downloads, commands = cli(selected, sdk, downloaded=kind == 'bytes')
        if kind == 'approved':
            observed = isinstance(error, DownloadBoundary) and downloads == [url] and not commands
        elif kind == 'bytes':
            observed = (isinstance(error, ValueError) and
                        str(error) == 'base SDK archive digest differs from approved input' and
                        downloads == [url] and not commands)
        else:
            observed = (isinstance(error, ValueError) and
                        str(error) == 'base SDK input differs from fixed approved provider URL and digest' and
                        not downloads and not commands)
        print('TARGET', selected, kind, 'observed=' + str(observed),
              'error=' + type(error).__name__, 'downloads=' + str(len(downloads)),
              'production_commands=' + str(len(commands)), flush=True)
        self.assertTrue(observed, 'target exact CLI outcome / I/O boundary')
    return test


for selected in APPROVED['base_sdk']:
    for kind in ('approved', 'protocol', 'host', 'provider', 'path', 'version', 'platform', 'archive', 'digest', 'bytes'):
        setattr(SdkCli, 'test_' + selected.replace('-', '_') + '_' + kind, case(selected, kind))

if __name__ == '__main__':
    if sys.argv[1:] == ['--remaining']:
        names = [name for name in unittest.defaultTestLoader.getTestCaseNames(SdkCli)
                 if name != 'test_linux_arm64_approved']
        result = unittest.TextTestRunner(verbosity=2).run(
            unittest.TestSuite(SdkCli(name) for name in names))
        sys.exit(not result.wasSuccessful())
    unittest.main(verbosity=2)
