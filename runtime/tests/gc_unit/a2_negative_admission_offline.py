# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
"""Synthetic callback integration, never GDB/API or product qualification.

Execute the observer definitions themselves against explicit fake events and
raw maps. Installation below is a simulated CLI receipt, not hardware proof.
"""
import ast
import json
import os
from pathlib import Path
import sys
from types import SimpleNamespace
import a2_observer_contract as c

RECIPES = ('ordinary', 'continuous', 'negative-unreadable', 'negative-gap',
           'partial', 'inode', 'hash', 'pc', 'window-absent',
           'window-invalidated', 'forbidden-read')


def run(recipe, root):
    assert recipe in RECIPES
    root.mkdir(parents=True, exist_ok=False)
    path = root / 'libcj_metadata_synthetic.so'
    path.write_bytes(b'SYNTHETIC; not an ELF, SO, or product fixture')
    st = path.stat()
    dev = '%x:%x' % (os.major(st.st_dev), os.minor(st.st_dev))
    def raw(start, end, perm='r--p', inode=st.st_ino):
        return '%x-%x %s 00000000 %s %d %s' % (start, end, perm, dev, inode, path)
    lines = [raw(0x1000, 0x1004), raw(0x1004, 0x2000, 'r-xp')]
    if recipe == 'continuous':
        lines = [raw(0x1000, 0x1002), raw(0x1002, 0x2000, 'r-xp')]
    elif recipe in ('negative-unreadable', 'forbidden-read'):
        lines[0] = raw(0x1000, 0x1004, '---p')
    elif recipe == 'negative-gap':
        lines = lines[1:]
    elif recipe == 'partial':
        lines = [raw(0x1000, 0x1002, '---p'), raw(0x1002, 0x2000, 'r-xp')]
    elif recipe == 'inode':
        lines[1] = raw(0x1004, 0x2000, 'r-xp', st.st_ino + 1)
    elif recipe == 'pc':
        lines[1] = raw(0x1004, 0x2000, 'r--p')
    rows = c.parse_maps('\n'.join(lines))
    manifest = {str(path): c.digest(path)}
    if recipe == 'hash':
        path.write_bytes(b'CHANGED synthetic identity')
    cli, writes, reads = [], [], []
    class BP:
        serial = 0
        def __init__(self, spec=None, **kwargs):
            BP.serial += 1
            self.number, self.enabled, self.valid = BP.serial, True, True
            self.return_value = 1
            cli.append({'construct': str(spec), 'kwargs': kwargs})
        def is_valid(self):
            return self.valid
    class Frame:
        def read_var(self, name):
            assert name == 'pc'
            return 0x1004
        def read_register(self, name):
            return {'sp': 0x3000, 'pc': 0x1013}[name]
        def older(self):
            return SimpleNamespace(read_register=lambda name: 0x3010, older=lambda: None)
        def architecture(self):
            return SimpleNamespace(disassemble=lambda *a, **k:
                [{'addr': 0x1010, 'length': 3, 'asm': 'movsxd rax,DWORD PTR [r15]'}])
    frame = Frame()
    def execute(command, **kwargs):
        cli.append({'command': command})
        if command.startswith('info breakpoints '):
            return ('Num Type Disp Enb Address What\n%d read watchpoint keep y '
                    '*(unsigned int*)0x1000\n') % int(command.split()[-1])
        raise AssertionError('unexpected synthetic command: ' + command)
    def read_memory(address, size):
        reads.append((address, size))
        assert address == 0x1010 and size == 3, 'observer read prefix bytes'
        return b'\x49\x63\x07'
    obj = SimpleNamespace(filename=str(path), is_valid=lambda: True)
    gdb = SimpleNamespace(Breakpoint=BP, FinishBreakpoint=BP, BP_WATCHPOINT=2,
        WP_READ=1, selected_thread=lambda: SimpleNamespace(ptid=(10, 10, 0)),
        newest_frame=lambda: frame, execute=execute, objfiles=lambda: [obj],
        block_for_pc=lambda pc: SimpleNamespace(function=True, start=0x1010),
        selected_inferior=lambda: SimpleNamespace(read_memory=read_memory),
        write=lambda text: writes.append(text))
    sys.modules['gdb'] = gdb
    source = Path(__file__).with_name('a2_read_boundaries_gdb.py')
    tree = ast.parse(source.read_text())
    assert isinstance(tree.body[-1], ast.Try)
    # Only omit the actual debugger loader/runner. All callback definitions,
    # decorators, dispatch and finalize below are the production source.
    tree.body = tree.body[:-1]
    ns = {'__file__': str(source), '__name__': 'synthetic_observer'}
    exec(compile(tree, str(source), 'exec'), ns)
    ns.update(case='prefix-ordinary', manifest=manifest, out=str(root / 'record.json'),
              bind_inputs=lambda: rows, mappings=lambda: rows)
    marker = ns['Input']('SYNTHETIC Input')
    marker.stop()
    if recipe in ('partial', 'inode', 'hash', 'pc'):
        assert ns['life'].error and not ns['input_watch'], 'TARGET invalid identity/admission accepted'
        assert not reads
        print('TARGET_REJECT invalid input', recipe, ns['life'].error)
        return
    admission = ns['input_item']['prefix_input']
    negative = recipe in ('negative-unreadable', 'negative-gap', 'forbidden-read')
    assert admission['expected_accepted'] == (not negative), 'TARGET mapping facts misclassified'
    assert admission['readable_owner_bytes'] == (0 if negative else 4)
    ns['life'].phase = 'outer'
    if negative:
        try:
            ns['dispatch']()
        except RuntimeError as error:
            assert str(error) == 'valid rejection input; safe read observation missing'
        else:
            raise AssertionError('TARGET negative resumed without safe window')
        assert len(cli) == 1 and not reads, 'TARGET negative installed/evaluated a watch'
        assert ns['record']['observation_missing']['installed'] is False
        if recipe == 'forbidden-read':
            # Counterexample only: synthetic read event is rejected by actual
            # callback/window gate; it is not a genuine hardware read receipt.
            ns['ReadWatch'](0x1000, 'prefix', admission['owner']).stop()
            assert ns['life'].error == 'prefix observation window absent or invalidated'
            print('TARGET_REJECT forbidden synthetic read event')
        ns['input_item'].update(returned=True, accepted=0)
        ns['life'].phase, ns['life'].state = 'outer', 'returned'
        ns['record']['inferior_rc'] = 0
        ns['finalize']()
        assert ns['record']['status'] == 'INVALID' and not ns['record']['target'], \
            'TARGET missing no-read window became OBSERVED'
        print('TARGET_REACHED legitimate negative admitted; observation MISSING', recipe)
        return
    ns['dispatch']()
    assert ns['record']['prefix_watch_verified'] and len(ns['watches']) == 1
    watch = ns['watches'][0]
    if recipe == 'window-absent':
        ns['record']['prefix_watch_verified'] = False
    elif recipe == 'window-invalidated':
        watch.valid = False
    # Real callback body with synthetic product identity and instruction input.
    ns['product_at'] = lambda pc: ('SYNTHETIC symbol', admission['owner'])
    watch.stop()
    if recipe.startswith('window-'):
        assert ns['life'].error == 'prefix observation window absent or invalidated', \
            'TARGET missing/invalidated window accepted'
        assert not reads and not ns['record']['prefix_reads']
        print('TARGET_REJECT observation window', recipe)
        return
    assert ns['record']['prefix_reads'] and reads == [(0x1010, 3)], 'TARGET actual ReadWatch body not reached'
    ns['life'].phase = 'outer'
    ns['dispatch']()
    # Input frame is still identifiable at normal Return in this synthetic
    # stack; replace newest frame with its caller for consumer_identity(True).
    gdb.newest_frame = lambda: frame.older()
    ns['returns'][0].stop()
    ns['life'].phase = 'outer'
    ns['dispatch']()
    ns['record']['inferior_rc'] = 0
    ns['finalize']()
    assert ns['record']['status'] == 'OBSERVED' and ns['record']['target'], 'TARGET positive wiring changed'
    print('TARGET_REACHED synthetic positive Input/ReadWatch/Finish/finalize', recipe)


if __name__ == '__main__':
    run(sys.argv[1], Path(sys.argv[2]).resolve())
