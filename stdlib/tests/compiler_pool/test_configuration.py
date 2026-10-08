#!/usr/bin/env python3
"""Check generated production custom-command pools without invoking a compiler."""
import argparse
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    source = Path(__file__).resolve().parent
    args.output.mkdir(parents=True, exist_ok=True)
    for name, generator, value, expected_rc in [
        ('default', 'Ninja', None, 0),
        ('bounded', 'Ninja', '2', 0),
        ('zero', 'Ninja', '0', 1),
        ('negative', 'Ninja', '-2', 1),
        ('text', 'Ninja', '2x', 1),
        ('unsupported', 'Unix Makefiles', '2', 1),
    ]:
        build = args.output / name
        command = ['cmake', '-S', str(source), '-B', str(build), '-G', generator]
        if value is not None:
            command.append('-DCANGJIE_MAX_COMPILER_PROCESSES=' + value)
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (args.output / (name + '.log')).write_bytes(result.stdout)
        (args.output / (name + '.rc')).write_text(str(result.returncode) + '\n')
        assert (result.returncode == 0) == (expected_rc == 0), (name, result.returncode)
        if expected_rc:
            assert b'CANGJIE_MAX_COMPILER_PROCESSES' in result.stdout, name
            print(name + ': explicit request rejected')
            continue
        ninja = (build / 'build.ninja').read_text()
        rules = (build / 'CMakeFiles/rules.ninja').read_text()
        edges = [section for section in ninja.split('\n\n')
                 if ': CUSTOM_COMMAND' in section and 'COMMENT' not in section
                 and '  COMMAND = ' in section and ' cjc ' in section]
        assert len(edges) == 6, (name, len(edges))
        if value is None:
            assert 'pool cj_compile' not in rules
            assert all('pool = cj_compile' not in edge for edge in edges)
        else:
            assert rules.count('pool cj_compile\n') == 1
            assert 'pool cj_compile\n  depth = 2' in rules
            assert all('  pool = cj_compile' in edge for edge in edges)
        print(name + ': six real static/BC edges, shared pool assertion executed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
