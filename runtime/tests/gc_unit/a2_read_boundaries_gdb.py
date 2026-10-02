# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
# External Linux x86_64 observer. No inferior calls, return overrides, register
# writes or product state changes. All instruction addresses come from this SO.
import hashlib
import json
import os
import re
import gdb

case = os.environ.get('A2_CASE', '')
out = os.environ.get('A2_OBSERVER_OUT', '')
record = {'case': case, 'status': 'INVALID', 'errors': [], 'identities': [],
          'prefix_reads': [], 'map_reads': [], 'build_entries': [], 'consumers': []}
manifest = {}
armed = False
watches = []
build_probe = None


def digest(path):
    with open(path, 'rb') as stream:
        hasher = hashlib.sha256()
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            hasher.update(chunk)
        return hasher.hexdigest()


def mappings():
    pid = gdb.selected_inferior().pid
    result = []
    with open('/proc/%d/maps' % pid) as stream:
        for line in stream:
            fields = line.split(None, 5)
            if len(fields) != 6 or not fields[5].startswith('/'):
                continue
            path = fields[5].strip()
            if path.endswith(' (deleted)'):
                raise RuntimeError('deleted mapped input: ' + path)
            start, end = (int(x, 16) for x in fields[0].split('-'))
            result.append((start, end, fields[1], fields[3], int(fields[4]), path))
    return result


def bound(path, rows):
    path = os.path.realpath(path)
    wanted = manifest.get(path)
    if wanted is None or digest(path) != wanted:
        raise RuntimeError('unbound input hash: ' + path)
    stat = os.stat(path)
    rows = [r for r in rows if os.path.realpath(r[5]) == path]
    if not rows or any(r[4] != stat.st_ino or
                       tuple(int(v, 16) for v in r[3].split(':')) !=
                       (os.major(stat.st_dev), os.minor(stat.st_dev)) for r in rows):
        raise RuntimeError('mapped device/inode mismatch: ' + path)
    identity = {'path': path, 'sha256': wanted, 'device': stat.st_dev,
                'inode': stat.st_ino, 'maps': [list(r[:5]) for r in rows]}
    if identity not in record['identities']:
        record['identities'].append(identity)
    return identity


def bind_inputs():
    rows = mappings()
    executable = os.path.realpath(gdb.current_progspace().filename)
    bound(executable, rows)
    for path in {r[5] for r in rows}:
        name = os.path.basename(path)
        if name.startswith(('libcangjie', 'libcj_metadata')):
            bound(path, rows)
    for name in ('libcangjie-runtime.so', 'libcangjie-boundscheck.so'):
        if not any(os.path.basename(r[5]) == name for r in rows):
            raise RuntimeError('required mapped product dependency missing: ' + name)
    return rows


def product_at(pc):
    block = gdb.block_for_pc(pc)
    while block is not None and block.function is None:
        block = block.superblock
    if block is None or block.function is None or block.function.symtab is None:
        raise RuntimeError('instruction debug ownership unavailable')
    symbol = block.function
    path = symbol.symtab.objfile.filename
    if os.path.basename(path) != 'libcangjie-runtime.so':
        raise RuntimeError('observed instruction outside product SO: ' + path)
    identity = bound(path, mappings())
    return symbol, identity


def fail(error):
    record['errors'].append(str(error))


class ReadWatch(gdb.Breakpoint):
    def __init__(self, address, kind):
        super().__init__('*(unsigned int*)0x%x' % address, type=gdb.BP_WATCHPOINT,
                         wp_class=gdb.WP_READ, internal=True)
        self.address, self.kind = address, kind
        self.hits = 0

    def stop(self):
        try:
            frame = gdb.newest_frame()
            after = int(frame.read_register('pc'))
            symbol, identity = product_at(after)
            # x86 hardware read traps after the actual access instruction.
            # Decode the preceding instruction from the containing symbol,
            # retaining bytes/text; do not guess a source-line load address.
            block = gdb.block_for_pc(after)
            while block is not None and block.function is None:
                block = block.superblock
            if block is None:
                raise RuntimeError('read instruction function block unavailable')
            start = block.start
            instructions = frame.architecture().disassemble(start, after)
            preceding = [i for i in instructions if i['addr'] < after and i['addr'] + i['length'] == after]
            if len(preceding) != 1:
                raise RuntimeError('actual read instruction boundary unavailable')
            instruction = preceding[0]
            asm = instruction['asm']
            # Intel load has a memory source; a store/lea is not read evidence.
            operands = asm.split(None, 1)
            if len(operands) != 2 or operands[0] not in ('mov', 'movsxd', 'movsx', 'movzx', 'cmp', 'test'):
                raise RuntimeError('unclassified hardware watch access: ' + asm)
            if operands[0].startswith('mov') and ('[' not in operands[1].split(',')[-1]):
                raise RuntimeError('hardware watch saw a store: ' + asm)
            self.hits += 1
            record[self.kind + '_reads'].append({'watched_address': self.address,
                'instruction': instruction, 'stop_pc': after, 'so': identity,
                'bytes': bytes(gdb.selected_inferior().read_memory(instruction['addr'], instruction['length'])).hex()})
        except Exception as error:
            fail(error)
        return False


class ConsumerReturn(gdb.FinishBreakpoint):
    def __init__(self, frame, item):
        super().__init__(frame, internal=True)
        self.item = item

    def stop(self):
        global armed
        armed = False
        self.item['returned'] = True
        if case.startswith('prefix-'):
            if self.return_value is None:
                fail('prefix consumer return unavailable')
            else:
                self.item['accepted'] = int(self.return_value)
        for watch in watches:
            watch.enabled = False
        return False

    def out_of_scope(self):
        fail('real consumer did not return normally')


class BuildEntry(gdb.Breakpoint):
    def stop(self):
        if not armed:
            return False
        try:
            pc = int(gdb.newest_frame().read_register('pc'))
            symbol, identity = product_at(pc)
            # Breakpoint locations at the first Build body statement, including
            # inline instances. Save the actual chain to prove ROOTS caller.
            names = []
            frame = gdb.newest_frame()
            while frame is not None:
                names.append(frame.name() or '')
                frame = frame.older()
            if not any('CheckRegisterRoots' in n for n in names) or not any('Build<' in n or '::Build' in n for n in names):
                raise RuntimeError('Build boundary is not the ROOTS consumer instance')
            record['build_entries'].append({'pc': pc, 'symbol': str(symbol), 'stack': names,
                'instruction': gdb.newest_frame().architecture().disassemble(pc, count=1), 'so': identity})
        except Exception as error:
            fail(error)
        return False


class Input(gdb.Breakpoint):
    def stop(self):
        global armed
        try:
            if armed:
                raise RuntimeError('more than one qualification input')
            frame = gdb.newest_frame()
            rows = bind_inputs()
            pc = int(frame.read_var('pc'))
            owner = [r for r in rows if r[0] <= pc < r[1] and 'x' in r[2]]
            if len(owner) != 1:
                raise RuntimeError('input PC has no unique mapped executable identity')
            bound(owner[0][5], rows)
            if case.startswith('prefix-') and not os.path.basename(owner[0][5]).startswith('libcj_metadata'):
                raise RuntimeError('prefix PC not in real fixture DSO')
            item = {'pc': pc, 'mapped_owner': owner[0][5], 'returned': False}
            record['consumers'].append(item)
            armed = True
            if case.startswith('prefix-'):
                watches.append(ReadWatch(pc - 4, 'prefix'))
            else:
                map_address = int(frame.read_var('map'))
                item['map'] = map_address
                if case == 'roots-zero':
                    if not map_address or not any(r[0] <= map_address and map_address + 4 <= r[1] for r in rows):
                        raise RuntimeError('zero-root map not in actual mapped input')
                    watches.append(ReadWatch(map_address, 'map'))
            # No watchpoint resource/unmapped-input failure is a green result.
            watch_text = gdb.execute('info breakpoints', to_string=True)
            record['watch_installation'] = watch_text
            if watches and 'hw read watchpoint' not in watch_text.lower():
                raise RuntimeError('hardware read watchpoint could not be verified')
            ConsumerReturn(frame, item)
        except Exception as error:
            fail(error)
        return False


def finish(event):
    record['inferior_rc'] = getattr(event, 'exit_code', None)
    inputs = record['consumers']
    valid = not record['errors'] and record['inferior_rc'] == 0 and len(inputs) == 1 and inputs[0]['returned']
    if case.startswith('prefix-'):
        positive = case != 'prefix-invalid'
        target = inputs and inputs[0].get('accepted') == int(positive) and bool(record['prefix_reads']) == positive
    else:
        positive = case == 'roots-zero'
        target = bool(record['build_entries']) == positive and bool(record['map_reads']) == positive
    record['status'] = 'OBSERVED' if valid and target else 'INVALID'
    record['target'] = bool(valid and target)
    with open(out, 'w') as stream:
        json.dump(record, stream, indent=2)
    gdb.write('A2_BOUNDARY_RESULT ' + json.dumps(record) + '\n')
    gdb.execute('quit %d' % (0 if record['target'] else 2))


try:
    if case not in ('prefix-ordinary', 'prefix-continuous', 'prefix-invalid', 'roots-missing', 'roots-zero') or not out:
        raise RuntimeError('case/output missing')
    with open(os.environ['A2_INPUT_MANIFEST']) as stream:
        manifest = {os.path.realpath(p): h for p, h in json.load(stream).items()}
    if any(not re.fullmatch('[0-9a-f]{64}', h) for h in manifest.values()):
        raise RuntimeError('manifest contains an invalid hash')
    gdb.execute('set pagination off')
    gdb.execute('set disassembly-flavor intel')
    gdb.execute('start')
    if gdb.newest_frame().architecture().name() != 'i386:x86-64':
        raise RuntimeError('observer is Linux x86_64 only')
    bind_inputs()
    gdb.execute('set breakpoint pending off')
    if case.startswith('roots-'):
        # Source is a locator, not evidence of execution. Inline locations must
        # belong to this product SO and expose the actual ROOTS Build chain.
        build_probe = BuildEntry('StackMap.h:278', internal=True)
        locations = [l for l in build_probe.locations if l.address is not None]
        product_locations = []
        for location in locations:
            try:
                product_at(location.address)
                product_locations.append(location.address)
            except RuntimeError:
                location.enabled = False
        if not product_locations:
            raise RuntimeError('real SO Build entry has no observable location')
        record['build_locations'] = product_locations
    Input('A2ObservePrefix' if case.startswith('prefix-') else 'A2ObserveRoots', internal=True)
    gdb.events.exited.connect(finish)
    gdb.execute('continue')
except Exception as error:
    fail(error)
    if out:
        with open(out, 'w') as stream:
            json.dump(record, stream, indent=2)
    gdb.write('A2_BOUNDARY_INVALID ' + json.dumps(record) + '\n')
    gdb.execute('quit 2')
