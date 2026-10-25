# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
"""Synthetic boundary batch. No debugger import, API calls or real fixtures."""
import ast
import json
import os
from pathlib import Path
import sys
from types import SimpleNamespace
import a2_observer_contract as c

RECIPES = ('continuous', 'single', 'gap', 'unreadable', 'other-owner',
           'overlap', 'extent', 'deleted', 'hash', 'inode', 'anonymous')


def run(name, root):
    root.mkdir(parents=True, exist_ok=True)
    path = root / 'synthetic-owner.bin'
    path.write_bytes(b'synthetic owner; not ELF or fixture')
    st = path.stat()
    dev = '%x:%x' % (os.major(st.st_dev), os.minor(st.st_dev))
    def row(a, b, perm='r--p', file=path, inode=st.st_ino):
        return (a, b, perm, dev, inode, str(file), 'file', 0)
    rows = [row(0x1000, 0x1002), row(0x1002, 0x2000, 'r-xp')]
    manifest = {str(path): c.digest(path)}
    owner = c.bind(str(path), rows, manifest)
    # Execute the actual consumer call expressions, extracted from their source.
    # This proves argument wiring only; it does not execute debugger callbacks.
    tree = ast.parse(Path(__file__).with_name('a2_read_boundaries_gdb.py').read_text())
    calls = {}
    for cls in tree.body:
        if isinstance(cls, ast.ClassDef) and cls.name in ('Input', 'ReadWatch'):
            matches = [n for n in ast.walk(cls) if isinstance(n, ast.Call) and
                       isinstance(n.func, ast.Name) and n.func.id == 'read_boundary']
            calls[cls.name] = next(n for n in matches if len(n.args) == 5)
    def consume():
        results = []
        for label, call in calls.items():
            env = dict(read_boundary=c.read_boundary, rows=rows, pc=0x1004,
                       owner_identity=owner, manifest=manifest,
                       mappings=lambda: rows,
                       self=SimpleNamespace(address=0x1000, owner=owner))
            results.append(eval(compile(ast.Expression(call), '<actual-'+label+'>', 'eval'), env))
        assert len(results) == 2 and all(results)
        return results
    def reject_then_restore(mutated):
        nonlocal rows, owner
        original, original_owner = rows, owner
        rows = mutated
        try:
            owner = c.bind(str(path), rows, manifest)
        except (ValueError, OSError):
            pass
        for label, call in calls.items():
            env = dict(read_boundary=c.read_boundary, rows=rows, pc=0x1004,
                       owner_identity=owner, manifest=manifest, mappings=lambda: rows,
                       self=SimpleNamespace(address=0x1000, owner=owner))
            try:
                eval(compile(ast.Expression(call), '<actual-'+label+'>', 'eval'), env)
            except (ValueError, OSError) as error:
                print('TARGET_REJECT', label, str(error))
            else:
                raise AssertionError(label+' accepted invalid read')
        rows, owner = original, original_owner
        if name == 'hash':
            path.write_bytes(b'synthetic owner; not ELF or fixture')
        consume()
        print('TARGET_RESTORED both consumers')
    print('SYNTHETIC_INPUT', json.dumps(dict(rows=rows, owner=owner, manifest=manifest)))
    consume()
    if name == 'single':
        rows = [row(0x1000, 0x2000, 'r-xp')]
        owner = c.bind(str(path), rows, manifest)
        consume()
    elif name == 'gap':
        reject_then_restore([row(0x1000, 0x1001), row(0x1002, 0x2000, 'r-xp')])
    elif name == 'unreadable':
        reject_then_restore([row(0x1000, 0x1002, '---p'), rows[1]])
    elif name == 'other-owner':
        reject_then_restore([row(0x1000, 0x1002, file=root/'other'), rows[1]])
    elif name == 'overlap':
        reject_then_restore([rows[0], row(0x1001, 0x2000, 'r-xp')])
    elif name == 'deleted':
        reject_then_restore([row(0x1000, 0x1002, file=str(path)+' (deleted)'), rows[1]])
    elif name == 'inode':
        reject_then_restore([row(0x1000, 0x1002, inode=st.st_ino+1), rows[1]])
    elif name == 'hash':
        path.write_bytes(b'changed')
        reject_then_restore(rows)
    elif name == 'extent':
        for address, size in ((0, 0), (-1, 4), ((1<<64)-2, 4)):
            try:
                c.read_boundary(rows, address, size, owner, manifest)
            except ValueError:
                print('TARGET_REJECT extent', address, size)
            else:
                raise AssertionError('invalid extent accepted')
        consume()
    elif name == 'anonymous':
        anon = [(0x3000, 0x4000, 'rw-p', '00:00', 0, '', 'anonymous', 0)]
        assert c.read_boundary(anon, 0x3000, 4)
        reject_then_restore([(0x1000, 0x1002, 'rw-p', '00:00', 0, '', 'anonymous', 0), rows[1]])
    print('TARGET_REACHED PASS', name)


if __name__ == '__main__':
    run(sys.argv[1], Path(sys.argv[2]).resolve())
