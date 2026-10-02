# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
"""One frozen synthetic recipe per invocation. Never imports or starts GDB."""
import ast
import json
import os
from pathlib import Path
import sys
import a2_observer_contract as c
from a2_qualification_batch import check, CASES

RECIPES = ('maps-positive', 'required-deleted', 'required-missing', 'required-hash',
           'required-inode', 'maps-boundary', 'maps-malformed', 'location-multiple',
           'location-empty', 'location-external', 'location-stale', 'location-ambiguous',
           'callback-mutation', 'outer-lifecycle', 'exception', 'out-of-scope',
           'identity-loss', 'collector', 'watch-install', 'source-contract')


def rejected(fn, reason):
    try:
        fn()
    except (ValueError, OSError) as error:
        print('TARGET_REJECTION', reason, str(error), flush=True)
        return
    raise AssertionError('TARGET_NOT_REJECTED: ' + reason)


def run(name, root):
    root.mkdir(parents=True, exist_ok=False)
    paths = [root / n for n in ('elf', 'libcangjie-runtime.so', 'libboundscheck.so', 'fixture.so')]
    for p in paths:
        p.write_bytes(b'explicit synthetic file input\n')
    manifest = {str(p.resolve()): c.digest(p) for p in paths}
    raw = []
    for i, p in enumerate(paths):
        st = p.stat()
        raw.append('%x-%x r-xp 00000000 %x:%x %d %s' %
                   (0x1000 + i * 0x1000, 0x2000 + i * 0x1000,
                    os.major(st.st_dev), os.minor(st.st_dev), st.st_ino, p))
    raw.extend(['5000-6000 rw-p 00000000 00:00 0',
                '6000-7000 rw-s 00000000 00:01 99 /memfd:any_heap (deleted)',
                '7000-8000 r--p 00000000 00:00 0 [vvar]'])
    table = ('Num     Type           Disp Enb Address            What\n'
             '1       breakpoint     keep y   <MULTIPLE>\n'
             '1.1                         y   0x2000             in inline Build\n'
             '1.2                         y   0x2100             in Build\n')
    events = ['input', 'read', 'build', 'return', 'exit0']
    print(json.dumps({'synthetic': True, 'recipe': name, 'argv': sys.argv,
                      'raw_maps': raw, 'manifest': manifest, 'location_table': table,
                      'events': events, 'file_bytes': 'explicit synthetic file input\\n'}), flush=True)
    rows = c.parse_maps('\n'.join(raw))
    owner = lambda a: c.bind(c.contains(rows, a, executable=True)[5], rows, manifest)
    if name == 'maps-positive':
        assert len(c.required_inputs(str(paths[0]), rows, manifest)) == 3
        assert c.bind(str(paths[3]), rows, manifest)['inode'] == paths[3].stat().st_ino
        assert [r[6] for r in rows[-3:]] == ['anonymous', 'memfd', 'bracket']
        assert c.contains(rows, 0x6010, 4)[4] == 99
    elif name.startswith('required-'):
        changed = list(rows)
        if name == 'required-deleted':
            r = list(changed[1]); r[5] += ' (deleted)'; changed[1] = tuple(r)
        elif name == 'required-missing':
            paths[1].unlink()
        elif name == 'required-hash':
            manifest[str(paths[1])] = '0' * 64
        else:
            r = list(changed[1]); r[4] += 1; changed[1] = tuple(r)
        print('ACTUAL_MUTATED_INPUT', json.dumps({'rows': changed, 'manifest': manifest,
                                                'file_exists': paths[1].exists()}), flush=True)
        rejected(lambda: c.required_inputs(str(paths[0]), changed, manifest), name)
    elif name == 'maps-boundary':
        assert c.contains(rows, 0x5ffc, 4)[6] == 'anonymous'
        rejected(lambda: c.contains(rows, 0x5ffd, 4), 'cross-map read')
        rejected(lambda: c.contains(rows, 0x6010, executable=True), 'heap execute')
    elif name == 'maps-malformed':
        rejected(lambda: c.parse_maps('1000-1000 rw-p 0 00:00 0'), 'empty range')
        rejected(lambda: c.parse_maps('not maps'), 'raw parse')
    elif name == 'location-multiple':
        addresses, identities = c.locate(table, 1, 0, 0, owner)
        assert addresses == [0x2000, 0x2100] and len(identities) == 2
    elif name == 'location-empty':
        rejected(lambda: c.cli_locations(table.split('1.1')[0], 1), name)
    elif name == 'location-external':
        rejected(lambda: c.locate(table.replace('0x2100', '0x4000'), 1, 0, 0, owner), name)
    elif name == 'location-stale':
        rejected(lambda: c.locate(table, 1, 0, 1, owner), name)
        c.validate_symbols([True], ['runtime'], ['runtime'])
        rejected(lambda: c.validate_symbols([False], ['runtime'], ['runtime']), 'old objfile invalid')
        rejected(lambda: c.validate_symbols([True], ['runtime'], []), 'SO removed')
    elif name == 'location-ambiguous':
        rejected(lambda: c.cli_locations(table + 'unknown annotation\n', 1), name)
        rejected(lambda: c.cli_locations(table.replace('0x2100', '0x2000'), 1), 'duplicate')
        assert c.cli_locations(table.splitlines()[0] + '\n1 breakpoint keep y 0x2000 Build\n', 1) == [0x2000]
    elif name == 'callback-mutation':
        life = c.Lifecycle(); life.phase = 'callback'
        for action in ('install', 'disable', 'continue', 'delete'):
            rejected(lambda: life.mutation(action), action)
        life.phase = 'outer'
        assert life.mutation('install') == 'install'
        print('TARGET_OUTER_MUTATION_ACCEPTED')
    elif name in ('outer-lifecycle', 'exception', 'out-of-scope', 'identity-loss'):
        life = c.Lifecycle(); identity = ((1, 2, 3), 0x100, 0x200)
        def event(e, ident=identity):
            life.phase = 'callback'; life.capture(e, ident); life.phase = 'outer'
            action = life.dispatch(); print('TARGET_DISPATCH', e, action, flush=True); return action
        assert event('input') == 'install'
        if name == 'identity-loss':
            life.phase = 'callback'
            rejected(lambda: life.capture('read', ((9, 2, 3), 0x100, 0x200)), name)
            life.phase = 'outer'; life.error = 'identity loss'; assert life.dispatch() == 'cleanup'
            assert not life.exit(0)
        elif name != 'outer-lifecycle':
            assert event('exception' if name == 'exception' else 'out_of_scope') == 'cleanup'
            assert not life.exit(0)
        else:
            assert event('read') == event('build') == 'resume'
            assert event('return') == 'disable'
            assert life.exit(0)
            rejected(lambda: event('input'), 'duplicate after return')
    elif name == 'collector':
        files = []
        for case in CASES:
            p = root / (case + '.json')
            r = {'case': case, 'status': 'OBSERVED', 'target': True, 'errors': [], 'inferior_rc': 0,
                 'identities': [], 'prefix_reads': [1] if case in CASES[:2] else [],
                 'build_entries': [1] if case == 'roots-zero' else [],
                 'map_reads': [1] if case == 'roots-zero' else [], 'build_locations': [1]}
            p.write_text(json.dumps(r)); files.append(str(p))
        assert check(files)['status'] == 'QUALIFIED_INPUTS'
        print('TARGET_COLLECTOR_SYNTHETIC_POSITIVE')
        r = json.loads(Path(files[0]).read_text()); r['status'] = 'INVALID'
        Path(files[0]).write_text(json.dumps(r))
        rejected(lambda: check(files), 'collector INVALID')
    elif name == 'watch-install':
        t = 'Num Type Disp Enb Address What\n2 read watchpoint keep y *(unsigned int*)0x6000\n'
        assert c.verify_watch(t, 2, '*(unsigned int*)0x6000')
        rejected(lambda: c.verify_watch(t.replace('read watchpoint', 'hw watchpoint'), 2, '*(unsigned int*)0x6000'), 'write watch')
    elif name == 'source-contract':
        source = Path(__file__).with_name('a2_read_boundaries_gdb.py').read_text()
        tree = ast.parse(source)
        for node in ast.walk(tree):
            if isinstance(node, ast.FunctionDef) and node.name in ('stop', 'out_of_scope'):
                for child in ast.walk(node):
                    if isinstance(child, ast.Call):
                        assert not (isinstance(child.func, ast.Attribute) and child.func.attr in ('execute', 'delete'))
                        assert not (isinstance(child.func, ast.Name) and child.func.id in ('ReadWatch', 'ConsumerReturn', 'mutate'))
                    if isinstance(child, ast.Attribute) and isinstance(child.ctx, ast.Store):
                        assert child.attr != 'enabled'
        assert '.locations' not in source
        for filename in ('a2_observer_contract.py', 'a2_observer_offline.py'):
            ast.parse(Path(__file__).with_name(filename).read_text())
    else:
        raise ValueError('unknown recipe')
    print('TARGET_REACHED PASS', name, flush=True)


if __name__ == '__main__':
    name, directory = sys.argv[1:]
    if name not in RECIPES:
        raise ValueError('unfrozen recipe')
    run(name, Path(directory).resolve())
