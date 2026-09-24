#!/usr/bin/env python3
"""Compare the existing macro-gated declarations with the linked ELF registry."""
from pathlib import Path
import sys


def check(enabled, manifest, listing):
    expected = {line for line in manifest.read_text().splitlines()
                if line and not line.startswith('#')}
    registered = set()
    suite = ''
    for line in listing.read_text().splitlines():
        if line.endswith('.') and not line[0].isspace():
            suite = line[:-1]
        elif line.startswith('  ') and suite:
            registered.add(suite + '.' + line.strip())
    missing = expected - registered if enabled else set()
    unexpected = expected & registered if not enabled else set()
    for name in sorted(missing):
        print(f'GC_UNIT_MACRO_REGISTRATION_FAIL missing={name}')
    for name in sorted(unexpected):
        print(f'GC_UNIT_MACRO_REGISTRATION_FAIL unexpected={name}')
    if missing or unexpected:
        return 1
    print(f'GC_UNIT_MACRO_REGISTRATION_OK enabled={int(enabled)} '
          f'gated={len(expected & registered)} total={len(registered)}')
    return 0


if __name__ == '__main__':
    if len(sys.argv) != 4 or sys.argv[1] not in ('0', '1'):
        sys.exit('usage: check_macro_registration.py 0|1 MANIFEST LISTING')
    sys.exit(check(sys.argv[1] == '1', Path(sys.argv[2]), Path(sys.argv[3])))
