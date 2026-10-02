# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
# Read existing five-case records only. This does not execute any target.
import json
import sys

CASES = ('prefix-ordinary', 'prefix-continuous', 'prefix-invalid', 'roots-missing', 'roots-zero')


def check(paths):
    if len(paths) != len(CASES):
        raise ValueError('exactly five independent case records required')
    records = [json.load(open(path)) for path in paths]
    by_case = {r['case']: r for r in records}
    if len(by_case) != 5 or set(by_case) != set(CASES):
        raise ValueError('duplicate or missing qualification case')
    identities = []
    for case in CASES:
        r = by_case[case]
        if r['status'] != 'OBSERVED' or not r['target'] or r['errors'] or r['inferior_rc'] != 0:
            raise ValueError('case is INVALID: ' + case)
        # All cases use one ELF and one real product/dependency set. Fixtures
        # differ by design; compare path+hash, not an absolute mapping address.
        identities.append({i['path']: i['sha256'] for i in r['identities']
                           if 'libcj_metadata' not in i['path']})
    if any(i != identities[0] for i in identities):
        raise ValueError('ELF/product/dependency identities differ across cases')
    for case in ('prefix-ordinary', 'prefix-continuous'):
        if not by_case[case]['prefix_reads']:
            raise ValueError('prefix instrument has no legitimate positive')
    if by_case['prefix-invalid']['prefix_reads']:
        raise ValueError('invalid prefix was actually read')
    if by_case['roots-missing']['build_entries'] or by_case['roots-missing']['map_reads']:
        raise ValueError('missing descriptor entered Build/map read')
    if not by_case['roots-zero']['build_entries'] or not by_case['roots-zero']['map_reads']:
        raise ValueError('ROOTS instrument has no Build/map positive')
    if by_case['roots-missing']['build_locations'] != by_case['roots-zero']['build_locations']:
        # Addresses are normalized to the verified mapped product base.
        raise ValueError('Build probe locations differ; input instrument is not identical')
    return {'status': 'QUALIFIED_INPUTS', 'cases': list(CASES), 'n_per_case': 1,
            'scope': 'green input/instrument qualification only; no cut or product acceptance'}


if __name__ == '__main__':
    try:
        print(json.dumps(check(sys.argv[1:]), indent=2))
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(json.dumps({'status': 'INVALID', 'reason': str(error)}))
        sys.exit(2)
