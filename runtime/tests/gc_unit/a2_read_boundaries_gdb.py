# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
# External Linux x86_64 observer. No inferior calls, return overrides, register
# writes or product state changes. All instruction addresses come from this SO.
import json
import os
import re
import sys
import functools
import gdb

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from a2_observer_contract import (digest, parse_maps, bind, required_inputs,
                                  contains, read_boundary, prefix_input, locate, Lifecycle, verify_watch, validate_symbols)

case = os.environ.get('A2_CASE', '')
out = os.environ.get('A2_OBSERVER_OUT', '')
record = {'case': case, 'status': 'INVALID', 'errors': [], 'identities': [],
          'prefix_reads': [], 'map_reads': [], 'build_entries': [], 'consumers': []}
manifest = {}
armed = False
watches = []
build_probe = None
returns = []
life = Lifecycle()
input_frame = None
input_item = None
input_watch = None
symbol_generation = 0
snapshot_generation = None
exited = False
symbol_objects = []
symbol_paths = []


def mappings():
    with open('/proc/%d/maps' % gdb.selected_inferior().pid) as stream:
        rows = parse_maps(stream.read())
    record['raw_mappings'] = [list(r) for r in rows]
    return rows


def bound(path, rows):
    identity = bind(path, rows, manifest)
    if identity not in record['identities']:
        record['identities'].append(identity)
    return identity


def bind_inputs():
    rows = mappings()
    for identity in required_inputs(gdb.current_progspace().filename, rows, manifest):
        if identity not in record['identities']:
            record['identities'].append(identity)
    return rows


def consumer_identity(returning=False):
    if life.identity is None:
        raise RuntimeError('consumer identity absent')
    thread, sp, caller = life.identity
    if tuple(gdb.selected_thread().ptid) != thread:
        raise RuntimeError('consumer thread changed')
    frame = gdb.newest_frame()
    wanted = caller if returning else sp
    while frame is not None:
        if int(frame.read_register('sp')) == wanted:
            return life.identity
        frame = frame.older()
    raise RuntimeError('consumer frame identity lost')


def captured(event):
    def decorate(method):
        @functools.wraps(method)
        def stop(self):
            life.phase = 'callback'
            try:
                if life.state == 'armed':
                    if case.startswith('prefix-') and (not record.get('prefix_watch_verified') or
                            len(watches) != 1 or not watches[0].is_valid() or not watches[0].enabled):
                        raise RuntimeError('prefix observation window absent or invalidated')
                    check_symbols()
                identity = method(self)
                life.capture(event, identity)
            except Exception as error:
                fail(error)
            return True  # outer gdb.execute('continue') returns before mutations
        return stop
    return decorate


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
    rows = mappings()
    row = contains(rows, pc, executable=True)
    if row[6] != 'file' or os.path.realpath(row[5]) != os.path.realpath(path):
        raise RuntimeError('instruction mapping and debug SO ownership disagree')
    identity = bound(path, rows)
    return symbol, identity


def fail(error):
    record['errors'].append(str(error))
    life.error = str(error)


class ReadWatch(gdb.Breakpoint):
    def __init__(self, address, kind, owner):
        life.mutation('construct read watch')
        super().__init__('*(unsigned int*)0x%x' % address, type=gdb.BP_WATCHPOINT,
                         wp_class=gdb.WP_READ, internal=False)
        self.address, self.kind, self.owner = address, kind, owner
        self.hits = 0

    @captured('read')
    def stop(self):
        try:
            consumer_identity()
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
            read_boundary(mappings(), self.address, 4, self.owner, manifest)
            contains(mappings(), instruction['addr'], instruction['length'], executable=True)
            self.hits += 1
            record[self.kind + '_reads'].append({'watched_address': self.address,
                'instruction': instruction, 'stop_pc': after, 'so': identity,
                'bytes': bytes(gdb.selected_inferior().read_memory(instruction['addr'], instruction['length'])).hex()})
        except Exception as error:
            fail(error)
        return consumer_identity()


class ConsumerReturn(gdb.FinishBreakpoint):
    def __init__(self, frame, item):
        life.mutation('construct Finish')
        super().__init__(frame, internal=False)
        self.item = item

    @captured('return')
    def stop(self):
        identity = consumer_identity(returning=True)
        self.item['returned'] = True
        if case.startswith('prefix-'):
            if self.return_value is None:
                raise RuntimeError('prefix consumer return unavailable')
            self.item['accepted'] = int(self.return_value)
        return identity

    def out_of_scope(self):
        life.phase = 'callback'
        try:
            life.capture('out_of_scope')
            fail('real consumer did not return normally')
        except Exception as error:
            fail(error)
        finally:
            life.phase = 'outer'



class BuildEntry(gdb.Breakpoint):
    @captured('build')
    def stop(self):
        consumer_identity()
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
        return consumer_identity()


class Input(gdb.Breakpoint):
    @captured('input')
    def stop(self):
        global input_frame, input_item, input_watch
        frame = gdb.newest_frame()
        rows = bind_inputs()
        pc = int(frame.read_var('pc'))
        owner = contains(rows, pc, executable=True)
        owner_identity = bound(owner[5], rows)
        if case.startswith('prefix-') and not os.path.basename(owner[5]).startswith('libcj_metadata'):
            raise RuntimeError('prefix PC not in real fixture DSO')
        caller = frame.older()
        if caller is None:
            raise RuntimeError('consumer caller unavailable')
        identity = (tuple(gdb.selected_thread().ptid), int(frame.read_register('sp')),
                    int(caller.read_register('sp')))
        input_frame = frame
        input_item = {'pc': pc, 'mapped_owner': owner[5], 'returned': False,
                      'thread_frame': identity}
        record['consumers'].append(input_item)
        input_watch = None
        if case.startswith('prefix-'):
            admission = prefix_input(rows, pc, owner_identity, manifest)
            input_item['prefix_input'] = admission
            if admission['expected_accepted']:
                input_watch = (pc - 4, 'prefix', owner_identity)
        else:
            address = int(frame.read_var('map'))
            input_item['map'] = address
            if case == 'roots-zero':
                read_boundary(rows, address, 4)
                input_watch = (address, 'map', None)
        return identity


def mutate(action, operation):
    life.mutation(action)
    return operation()


def cleanup():
    for bp in watches + returns + ([build_probe] if build_probe else []):
        if bp.is_valid():
            mutate('disable', lambda bp=bp: setattr(bp, 'enabled', False))


def dispatch():
    global armed, build_probe, snapshot_generation, symbol_objects, symbol_paths
    action = life.dispatch()
    record.setdefault('dispatch', []).append(action)
    if action == 'install':
        if case.startswith('prefix-') and not input_item['prefix_input']['expected_accepted']:
            # Input admission is valid. Observation is a separate obligation:
            # GDB12's Python WP_READ evaluates the dereference (no -location
            # option). Do not invoke it on an unreadable/foreign prefix, and
            # do not resume without a verified observation window.
            record['observation_missing'] = {
                'address': input_item['prefix_input']['address'], 'size': 4,
                'reason': 'GNU12.1 safe no-read window unavailable',
                'installed': False, 'positive_control': False}
            raise RuntimeError('valid rejection input; safe read observation missing')
        symbol_objects = list(gdb.objfiles())
        symbol_paths = [o.filename for o in symbol_objects]
        snapshot_generation = symbol_generation
        install_build()
        if input_watch:
            watches.append(mutate('install read watch', lambda: ReadWatch(*input_watch)))
        for watch in watches:
            text = gdb.execute('info breakpoints %d' % watch.number, to_string=True)
            record['watch_installation'] = text
            verify_watch(text, watch.number, '*(unsigned int*)0x%x' % watch.address)
            if watch.kind == 'prefix':
                record['prefix_watch_verified'] = True
        returns.append(mutate('install Finish', lambda: ConsumerReturn(input_frame, input_item)))
        if build_probe:
            mutate('enable Build', lambda: setattr(build_probe, 'enabled', True))
        armed = True
    elif action in ('disable', 'cleanup'):
        armed = False
        cleanup()
    if life.error:
        raise RuntimeError('observer lifecycle invalid: ' + life.error)


def install_build():
    global build_probe, snapshot_generation
    if case.startswith('roots-'):
        # Source is a locator, not evidence of execution. Inline locations must
        # belong to this product SO and expose the actual ROOTS Build chain.
        build_probe = mutate('install Build', lambda: BuildEntry('StackMap.h:277', internal=False))
        snapshot_generation = symbol_generation
        text = gdb.execute('info breakpoints %d' % build_probe.number, to_string=True)
        record['build_location_table'] = text
        addresses, identities = locate(text, build_probe.number, snapshot_generation,
                                       symbol_generation, lambda pc: product_at(pc)[1])
        record['build_locations'] = sorted([{'so_sha256': identity['sha256'],
            'relative_pc': pc - min(row[0] - row[5] for row in identity['maps'])}
            for pc, identity in zip(addresses, identities)], key=lambda r: r['relative_pc'])

def check_symbols():
    validate_symbols([o.is_valid() for o in symbol_objects], symbol_paths,
                     [o.filename for o in gdb.objfiles()])
    if snapshot_generation != symbol_generation:
        raise RuntimeError('symbol generation invalidated')


def symbols_changed(event):
    global symbol_generation
    symbol_generation += 1
    if life.state == 'armed':
        fail('symbol snapshot invalidated by DSO load/unload')


def finish(event):
    global exited
    exited = True
    record['inferior_rc'] = getattr(event, 'exit_code', None)


def finalize():
    inputs = record['consumers']
    valid = life.exit(record['inferior_rc']) and not record['errors'] and len(inputs) == 1 and inputs[0]['returned']
    if case.startswith('prefix-'):
        positive = inputs[0]['prefix_input']['expected_accepted'] if inputs else None
        target = inputs and inputs[0].get('accepted') == int(positive) and bool(record['prefix_reads']) == positive
    else:
        positive = case == 'roots-zero'
        target = bool(record['build_entries']) == positive and bool(record['map_reads']) == positive
    valid = valid and not record.get('observation_missing')
    if case.startswith('prefix-'):
        valid = valid and record.get('prefix_watch_verified', False)
    record['status'] = 'OBSERVED' if valid and target else 'INVALID'
    record['target'] = bool(valid and target)
    with open(out, 'w') as stream:
        json.dump(record, stream, indent=2)
    gdb.write('A2_BOUNDARY_RESULT ' + json.dumps(record) + '\n')



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
    mutate('install Input', lambda: Input('A2ObservePrefix' if case.startswith('prefix-') else 'A2ObserveRoots', internal=False))
    gdb.events.new_objfile.connect(symbols_changed)
    gdb.events.clear_objfiles.connect(symbols_changed)
    gdb.events.exited.connect(finish)
    while not exited:
        if life.error:
            raise RuntimeError(life.error)
        if life.state == 'armed':
            check_symbols()
        mutate('continue', lambda: gdb.execute('continue'))
        life.phase = 'outer'
        if exited:
            break
        dispatch()  # stopped, outside all Breakpoint.stop callbacks
    cleanup()
    finalize()
    gdb.execute('quit %d' % (0 if record['target'] else 2))

except Exception as error:
    life.phase = 'outer'
    fail(error)
    record['status'] = 'INVALID'
    record['target'] = False
    try:
        cleanup()
    except Exception as cleanup_error:
        fail(cleanup_error)
    if out:
        with open(out, 'w') as stream:
            json.dump(record, stream, indent=2)
    gdb.write('A2_BOUNDARY_INVALID ' + json.dumps(record) + '\n')
    gdb.execute('quit 2')
