#!/usr/bin/env python3
"""Structural workflow-to-consumer contract checks; no permission execution claim."""
from pathlib import Path
import unittest
import yaml

ROOT = Path(__file__).resolve().parents[4]


class Workflow(unittest.TestCase):
    def setUp(self):
        self.workflow = yaml.safe_load((ROOT / '.github/workflows/metadata-platform.yml').read_text())

    def test_same_repository_transport(self):
        self.assertEqual(self.workflow['permissions'], {'contents': 'read', 'actions': 'read'})
        fixture = self.workflow['jobs']['fixtures']
        download = next(s for s in fixture['steps'] if s.get('uses') == 'actions/download-artifact@v4')
        self.assertEqual(download['with']['repository'], '${{ github.repository }}')
        self.assertEqual(download['with']['github-token'], '${{ github.token }}')
        self.assertEqual(download['with']['artifact-ids'], '${{ steps.producer.outputs.artifact_id }}')
        self.assertEqual(download['with']['run-id'], '${{ inputs.fixture_tuple_run }}')
        steps = fixture['steps']
        verifier = next(s for s in steps if s.get('id') == 'producer')
        self.assertLess(steps.index(verifier), steps.index(download))
        self.assertIn('--verify-run', verifier['run'])
        for variable, expression in {
            'METADATA_TOOL_REPOSITORY': '${{ fromJSON(inputs.fixture_provenance).repository }}',
            'METADATA_TUPLE_ATTEMPT': '${{ fromJSON(inputs.fixture_provenance).attempt }}',
            'METADATA_TOOL_PRODUCER_SHA': '${{ fromJSON(inputs.fixture_provenance).producer_sha }}',
            'METADATA_TOOL_ARTIFACT_IDS': '${{ toJSON(fromJSON(inputs.fixture_provenance).artifact_ids) }}',
            'METADATA_TOOL_MANIFESTS': '${{ inputs.fixture_tool_manifests }}'}.items():
            self.assertEqual(fixture['env'][variable], expression)
        print('TARGET_EXECUTED workflow_to_same_repository_consumer')

    def test_producer_is_separate(self):
        jobs = self.workflow['jobs']
        for job in ('prepare', 'arms', 'pac-capability'):
            self.assertIn("inputs.a2_batch != 'tools'", jobs[job]['if'])
        self.assertIn("inputs.a2_batch == 'fixtures'", jobs['fixtures']['if'])
        producer = jobs['tools']
        self.assertIn("inputs.a2_batch == 'tools'", producer['if'])
        checkouts = [s for s in producer['steps'] if s.get('uses') == 'actions/checkout@v4']
        self.assertEqual(checkouts[1]['with'], {'repository': 'cjcj-dev/cjcj',
            'ref': 'bf788bfcb2f1b3172cebb395169b375d1656b91d', 'path': '_tool-producer', 'persist-credentials': False})
        commands = [s['run'] for s in producer['steps'] if 'run' in s]
        self.assertEqual(sum('prepare_tools.py _tool-producer' in command for command in commands), 2)
        self.assertFalse(any('build.py fixtures' in command for command in commands))
        self.assertEqual({row['runner'] for row in producer['strategy']['matrix']['include']},
                         {'ubuntu-24.04-arm', 'windows-2025', 'macos-15'})
        self.assertEqual(producer['env']['CMAKE_CXX_COMPILER_LAUNCHER'], 'sccache')
        print('TARGET_EXECUTED producer_only_mode')


if __name__ == '__main__':
    unittest.main(verbosity=2)
