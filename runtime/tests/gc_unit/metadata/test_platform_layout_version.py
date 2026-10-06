#!/usr/bin/env python3
"""Offline fixed-action transport and real fixture CLI validation, not native qualification.
ACTION_JS is the unchanged upstream TS transpiled in lane scratch with external mocks.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import unittest
import yaml
from test_platform_cli import CLI, HERE


class LayoutVersion(CLI):
    # Only this class's explicitly selected cases are run by the bounded batch.
    def test_action_layout(self):
        workflow = yaml.safe_load((HERE.parents[3] / '.github/workflows/metadata-platform.yml').read_text())
        download = next(s for s in workflow['jobs']['fixtures']['steps']
                        if s.get('uses') == 'actions/download-artifact@v4')['with']
        verified = self.invoke(False, ['--verify-run'])
        self.assertEqual(verified.returncode, 0)
        artifact_id = (self.root / 'github-output').read_text().strip().split('=', 1)[1]
        self.assertEqual(artifact_id, '456')
        payload = self.root / 'payload'
        self.artifact.rename(payload)
        action_env = dict(os.environ, ACTION_INPUTS=json.dumps({
            'artifact-ids': artifact_id, 'path': str(self.artifact),
            'merge-multiple': bool(download.get('merge-multiple', False)),
            'repository': self.env['GITHUB_REPOSITORY'], 'run-id': '123',
            'github-token': 'offline-mock'}), ACTION_PAYLOAD=str(payload),
            ACTION_RESULT=str(self.root / 'action-result.json'))
        action = subprocess.run(['node', os.environ['ACTION_JS']], env=action_env,
                                text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        self.assertEqual(action.returncode, 0, action.stdout)
        observed = json.loads((self.root / 'action-result.json').read_text())
        result = self.invoke()
        # Read both actual outputs before the target assertion: a layout knife must
        # reach this assertion, not stop at a missing-manifest precondition.
        print('ACTION_CONSUMER_TARGET', json.dumps(observed), 'consumer_rc=', result.returncode,
              'consumer_root=', self.env['METADATA_TUPLE_ARTIFACT'], flush=True)
        self.assertEqual((observed['download'], observed['output'], result.returncode),
                         (str(self.artifact), str(self.artifact), 86),
                         'TARGET action extraction path reaches actual build.py tools consumer')
        self.assertEqual(len(self.marker.read_text().splitlines()), 3)

    def version_case(self, role, mismatch):
        name = 'llvm-tools.manifest' if role == 'linker' else 'llvm-tools.packaged.manifest'
        manifest = self.artifact / name
        manifest.write_text(manifest.read_text().replace('fixture-version',
                            'unmatched-version' if mismatch else '-'))
        self.approve_changed_manifests()
        result = self.invoke()
        starts = self.marker.read_text().splitlines() if self.marker.exists() else []
        diagnostic = ('tool version differs from manifest: ' + ('ld.lld' if role == 'linker' else 'llvm-readobj')
                      if mismatch else 'invalid tool digest/version in manifest')
        expected_probes = (1 if role == 'linker' else 2) if mismatch else 0
        print('VERSION_TARGET', role, 'mismatch=', mismatch, 'probes=', starts,
              'diagnostic=', diagnostic, 'rc=', result.returncode, flush=True)
        self.assertIn(diagnostic, result.stdout, 'TARGET real version rejection branch')
        self.assertEqual(result.returncode, 1)
        self.assertEqual(len(starts), expected_probes, 'TARGET allowed version probes only')
        self.assertTrue(all('--version' in line for line in starts), 'TARGET no compiler/native build')

    def test_linker_manifest_version(self): self.version_case('linker', False)
    def test_reader_manifest_version(self): self.version_case('reader', False)
    def test_linker_actual_version(self): self.version_case('linker', True)
    def test_reader_actual_version(self): self.version_case('reader', True)

    def test_single_id(self):
        values = json.loads(self.env['METADATA_TOOL_ARTIFACT_IDS'])
        values['linux-arm64'] = '456,457'
        self.env['METADATA_TOOL_ARTIFACT_IDS'] = json.dumps(values)
        self.reject('approved artifact IDs required', False, ['--verify-run'])

    def test_inconsistent_id(self):
        self.api['/repos/cjcj-dev/cangjie-runtime/actions/artifacts/456']['id'] = 457
        self.reject('unapproved producer artifact identity', False, ['--verify-run'])
