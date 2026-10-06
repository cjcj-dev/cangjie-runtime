"""Receipt consumption only; the first manifest read is a bounded observer.

No native payload or successful tools return is fabricated. Legal input must
reach the successor; rejected input must produce the receipt-specific error
from the actual tools entry before reaching that successor.
"""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import platform_inputs as inputs


class ManifestBoundary(Exception):
    pass


def exercise_receipt_consumer(test, artifact):
    artifact.mkdir(parents=True, exist_ok=True)
    manifest = artifact / 'llvm-tools.manifest'
    manifest.write_text('receipt-only observation boundary; not a native manifest\n')
    hashes = dict(tuple=inputs.sha(manifest), reader='b' * 64)
    record = dict(tools_source_sha=inputs.TOOL_SOURCE, llvm_sha=inputs.PRODUCER,
                  paired_runtime_sha=inputs.PAIRED_RUNTIME, paired_runtime_mode='private',
                  paired_runtime_url=inputs.RUNTIME_URL,
                  artifact='fixed-llvm-tools-linux_x86_64',
                  tuple_manifest_sha256=hashes['tuple'], reader_manifest_sha256=hashes['reader'])
    original_sha = inputs.sha
    for name, key in [('legal', None), ('bad-source', 'tools_source_sha'),
                      ('bad-pair', 'paired_runtime_sha')]:
        with test.subTest(receipt=name):
            value = dict(record)
            if key:
                value[key] = '0' * 40
            (artifact / 'producer.json').write_text(json.dumps(value))
            observed = []

            def boundary(path):
                # Read the real successor input, then stop before manifest
                # qualification. Never return a fake digest or tools success.
                digest = original_sha(path)
                observed.append((str(path), digest))
                raise ManifestBoundary(str(path))

            error = None
            with patch.object(inputs, 'sha', side_effect=boundary):
                try:
                    inputs.tools(artifact, artifact / 'never-produced', 'linux_x86_64', hashes)
                except Exception as caught:
                    error = caught
            expected = ('successor', str(manifest.resolve()), hashes['tuple']) if key is None else (
                'receipt-rejected', 'tool producer receipt differs from approved source/pair/manifests')
            if isinstance(error, ManifestBoundary) and len(observed) == 1:
                actual = ('successor', *observed[0])
            elif type(error) is ValueError and not observed:
                actual = ('receipt-rejected', str(error))
            else:
                actual = ('unexpected', type(error).__name__, str(error), observed)
            print('TARGET_RECEIPT_CONSUMER name=' + name + ' actual=' + repr(actual), flush=True)
            test.assertEqual(actual, expected, 'TARGET_RECEIPT_CONSUMER ' + name)
    (artifact / 'producer.json').write_text(json.dumps(record))


class ReceiptConsumer(unittest.TestCase):
    def test_receipt_rejection(self):
        with tempfile.TemporaryDirectory(prefix='receipt-consumer-') as root:
            exercise_receipt_consumer(self, Path(root))


if __name__ == '__main__':
    unittest.main(verbosity=2)
