#!/usr/bin/env python3
"""Check the real x86_64 OHOS Release Inspector inputs and linked heap data.

Consumes the product LTO object, archive and final SO; does not execute an
OHOS service or certify its runtime statistics. All assertions are reported,
so a missing heap input cannot hide the serializer control assertions.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


SERIALIZE = '_ZN12MapleRuntime11CjAllocData14SerializeStatsEv'
RECORD = '_ZN12MapleRuntime11CjAllocData16RecordAllocNodesEPKNS_8TypeInfoEj'
GET_HEAP = '_ZN12MapleRuntime4Heap7GetHeapEv'
GET_SIZE = '_ZNK12MapleRuntime4Heap16GetAllocatedSizeEv'
HEAP_ROOT = '_ZN12MapleRuntime14ZCollectedHeap15_collected_heapE'


def run(argv, cwd=None):
    return subprocess.run([str(a) for a in argv], cwd=cwd,
                          check=True, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE).stdout


def function(ir, symbol):
    match = re.search(r'^define [^\n]*@' + re.escape(symbol)
                      + r'\([^\n]*\n.*?^}', ir, re.M | re.S)
    return match.group() if match else ''


def heap_value_reaches_writer(body):
    heap = re.search(r'(%[\w.]+) = [^\n]*call [^\n]*@' + GET_HEAP + r'\(', body)
    size = re.search(r'(%[\w.]+) = [^\n]*call [^\n]*@' + GET_SIZE
                     + r'\([^\n]*? (%[\w.]+)\)', body)
    if not heap or not size or size[2] != heap[1]:
        return False
    # Release inlines WriteNumber into CString(int) then WriteString.
    value = re.search(r'(%[\w.]+) = trunc i64 ' + re.escape(size[1]) + r' to i32', body)
    if not value:
        return False
    ctor = re.search(r'call void @_ZN12MapleRuntime7CStringC[12]Ei\('
                     r'[^\n]*? (%[\w.]+), i32 [^\n]*?'
                     + re.escape(value[1]) + r'\)', body)
    return bool(ctor and re.search(
        r'call void @_ZN12MapleRuntime12StreamWriter11WriteStringERKNS_7CStringE'
        r'\([^\n]*? ' + re.escape(ctor[1]) + r'\)', body[ctor.end():]))


def linked_heap_value(serializer, getter, size_getter):
    # Derive offsets from the linked getters, rather than assume a Heap layout.
    base = re.search(r'addq\s+\$(\d+), %rax', getter)
    offset = re.search(r'movq\s+(\d+)\(%rdi\), %rax', size_getter)
    if not base or not offset or HEAP_ROOT not in getter:
        return False
    total = int(base[1]) + int(offset[1])
    # This supported recipe inlines both getters. The loaded result is the
    # integer argument in %rsi to the real CString constructor (SysV x86_64).
    return bool(re.search(
        re.escape(HEAP_ROOT) + r'>[^\n]*\n[^\n]*movq\s+\(%rax\), %rax\n'
        r'[^\n]*movq\s+' + str(total) + r'\(%rax\), %rsi\n'
        r'(?:(?!\b(?:callq|%rsi)\b)[^\n]*\n){0,8}'
        r'[^\n]*callq[^\n]*<_ZN12MapleRuntime7CStringC[12]Ei>', serializer))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--sdk', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    build, sdk = args.build.resolve(), args.sdk.resolve()
    tools = sdk / 'llvm/bin'
    obj = build / 'src/Inspector/CMakeFiles/Inspector.dir/CjAllocData.cpp.o'
    archive = build / 'runtime-staging/ar/x86_64_Release/libInspector.a'
    so = build / 'runtime-staging/lib/x86_64_Release/libcangjie-runtime.so'
    checks, artifacts = [], []

    def check(name, passed, detail=''):
        checks.append({'name': name, 'pass': bool(passed), 'detail': detail})
        print(('PASS ' if passed else 'FAIL ') + name, flush=True)

    try:
        for path in (obj, archive, so):
            artifacts.append({'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
        commands = json.loads((build / 'compile_commands.json').read_text())
        command = next(c for c in commands if c['file'].endswith('/Inspector/CjAllocData.cpp'))
        check('real_ohos_target', '--target=x86_64-linux-ohos' in command['command']
              and '-D__OHOS__' in command['command'] and '-flto' in command['command']
              and str(sdk / 'sysroot') in command['command'], command)
        archived = run([tools / 'llvm-ar', 'p', archive, 'CjAllocData.cpp.o'])
        check('archive_actual_object', archived == obj.read_bytes())
        link = (build / 'src/CMakeFiles/cangjie-runtime.dir/link.txt').read_text()
        inspector_link = (build / 'src/Inspector/CMakeFiles/Inspector.dir/link.txt').read_text()
        check('product_link_inputs', 'libInspector.a' in link and 'libHeap.a' in link
              and 'CjAllocData.cpp.o' in inspector_link and 'ProfilerAgentImpl.cpp.o' in inspector_link,
              {'runtime': link, 'inspector': inspector_link})
        ir = run([tools / 'llvm-dis', obj, '-o', '-']).decode()
        body = function(ir, SERIALIZE)
        record = function(ir, RECORD)
        check('serializer_controls', bool(body) and 'StreamWriter11WriteString' in body
              and 'SetContext' in body)
        check('record_to_serializer', bool(record) and re.search(r'call[^\n]*@' + SERIALIZE + r'\(', record))
        api_obj = build / 'src/CMakeFiles/runtime.dir/CangjieRuntimeApi.cpp.o'
        profiler_obj = build / 'src/Inspector/CMakeFiles/Inspector.dir/ProfilerAgentImpl.cpp.o'
        allocation_obj = build / 'src/ObjectModel/CMakeFiles/ObjectModel.dir/MObject.cpp.o'
        api_ir, profiler_ir, allocation_ir = [
            run([tools / 'llvm-dis', p, '-o', '-']).decode()
            for p in (api_obj, profiler_obj, allocation_obj)]
        profiler = re.search(r'^define [^\n]*@([^\s(]*ProfilerAgentImpl[^\s(]*)\(', profiler_ir, re.M)
        profiler_body = function(profiler_ir, profiler[1]) if profiler else ''
        check('public_tracking_entry', profiler and re.search(
            r'call[^\n]*@' + re.escape(profiler[1]) + r'\(', function(api_ir, 'ProfilerAgent'))
            and re.search(r'call[^\n]*@_ZN12MapleRuntime11CjAllocData14SetCjAllocDataEv\(', profiler_body),
            {'public': function(api_ir, 'ProfilerAgent'), 'dispatch': profiler_body})
        allocators = re.findall(r'^define [^\n]*@([^\s(]*MObject[^\s(]*)\(', allocation_ir, re.M)
        allocation_calls = {s: function(allocation_ir, s) for s in allocators
                            if re.search(r'call[^\n]*@' + RECORD + r'\(', function(allocation_ir, s))}
        check('allocation_record_entries', any('NewObject' in s for s in allocation_calls)
              and any('NewFinalizer' in s for s in allocation_calls), allocation_calls)
        symbols = run(['nm', '--defined-only', so]).decode()
        check('linked_real_service', all(re.search(r'\b' + re.escape(s) + r'$', symbols, re.M)
              for s in (SERIALIZE, RECORD, GET_HEAP, GET_SIZE, 'ProfilerAgent')))

        def disassemble(symbol):
            return run([tools / 'llvm-objdump', '-d', '--disassemble-symbols=' + symbol, so]).decode()

        assembly = disassemble(SERIALIZE)
        getter, size_getter = disassemble(GET_HEAP), disassemble(GET_SIZE)
        check('heap_input_to_serialized_number', heap_value_reaches_writer(body)
              and linked_heap_value(assembly, getter, size_getter),
              {'ir': body, 'linked_serializer': assembly,
               'linked_heap': getter, 'linked_size': size_getter})
    except (OSError, ValueError, StopIteration, subprocess.CalledProcessError) as error:
        check('artifact_read', False, str(error))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps({'checks': checks, 'artifacts': artifacts}, indent=2) + '\n')
    return 0 if checks and all(c['pass'] for c in checks) else 1


if __name__ == '__main__':
    raise SystemExit(main())
