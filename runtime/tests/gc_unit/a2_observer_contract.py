# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# Licensed under Apache-2.0 with Runtime Library Exception.
"""Shared read-only observer decisions; no debugger import or inferior access."""
import hashlib
import os
import re


def digest(path):
    with open(path, 'rb') as stream:
        h = hashlib.sha256()
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(chunk)
        return h.hexdigest()


def parse_maps(text):
    rows = []
    for line in text.splitlines():
        f = line.split(None, 5)
        if len(f) < 5 or not re.fullmatch(r'[r-][w-][x-][ps]', f[1]):
            raise ValueError('malformed raw mapping')
        start, end = (int(v, 16) for v in f[0].split('-'))
        if start >= end or (rows and start < rows[-1][1]):
            raise ValueError('overlapping or empty raw mapping')
        path = f[5] if len(f) == 6 else ''
        kind = ('memfd' if path.startswith(('/memfd:', 'memfd:')) else
                'bracket' if path.startswith('[') else
                'file' if path.startswith('/') else 'anonymous')
        rows.append((start, end, f[1], f[3], int(f[4]), path, kind, int(f[2], 16)))
    if not rows:
        raise ValueError('empty raw mappings')
    return rows


def contains(rows, address, size=1, executable=False):
    selected = [r for r in rows if r[0] <= address and address + size <= r[1]
                and ('x' in r[2] if executable else 'r' in r[2])]
    if size <= 0 or len(selected) != 1:
        raise ValueError('address lacks a unique mapped boundary')
    return selected[0]


def bind(path, rows, manifest):
    path = os.path.realpath(path)
    wanted = manifest.get(path)
    if wanted is None or digest(path) != wanted:
        raise ValueError('unbound input hash: ' + path)
    st = os.stat(path)
    selected = [r for r in rows if r[6] == 'file' and
                os.path.realpath(r[5].removesuffix(' (deleted)')) == path]
    if not selected or any(r[5].endswith(' (deleted)') or r[4] != st.st_ino or
            tuple(int(v, 16) for v in r[3].split(':')) !=
            (os.major(st.st_dev), os.minor(st.st_dev)) for r in selected):
        raise ValueError('selected mapped input deleted/device/inode mismatch: ' + path)
    return {'path': path, 'sha256': wanted, 'device': st.st_dev,
            'inode': st.st_ino, 'maps': [list(r[:5]) + [r[7]] for r in selected]}


def read_boundary(rows, address, size, owner=None, manifest=None):
    """Cover a read without gaps; file reads retain their bound input owner.

    Anonymous ROOTS reads use the original single-row boundary instead.
    """
    limit = 1 << 64
    if size <= 0 or address < 0 or address >= limit or size > limit - address:
        raise ValueError('invalid read extent')
    if owner is None:
        return [contains(rows, address, size)]
    if bind(owner['path'], rows, manifest) != owner:
        raise ValueError('read owner changed')
    end = address + size
    selected = sorted((r for r in rows if r[0] < end and address < r[1]),
                      key=lambda r: r[0])
    cursor = address
    for row in selected:
        if row[0] > cursor or (cursor != address and row[0] != cursor):
            raise ValueError('read gap or overlapping ownership')
        if row[6] != 'file' or 'r' not in row[2] or row[5].endswith(' (deleted)') or                 os.path.realpath(row[5]) != owner['path'] or row[4] != owner['inode'] or                 tuple(int(v, 16) for v in row[3].split(':')) !=                 (os.major(owner['device']), os.minor(owner['device'])):
            raise ValueError('read outside bound readable owner')
        if row[1] <= cursor:
            raise ValueError('overlapping read ownership')
        cursor = min(end, row[1])
    if cursor != end:
        raise ValueError('incomplete read coverage')
    return selected


def required_inputs(executable, rows, manifest):
    paths = [os.path.realpath(executable)]
    for name in ('libcangjie-runtime.so', 'libboundscheck.so'):
        candidates = [p for p in manifest if os.path.basename(p) == name]
        if len(candidates) != 1:
            raise ValueError('required manifest dependency ambiguous/missing: ' + name)
        paths.extend(candidates)
    return [bind(p, rows, manifest) for p in paths]


def cli_locations(text, number):
    """Strict CLI info breakpoints table, single or <MULTIPLE> with subrows.

    Trailing What is opaque display text, never address evidence. Unknown row
    formats, disabled/pending entries and mixed parent/subrow forms fail closed.
    The actual GDB12 table dialect remains a future real-API calibration item.
    """
    lines = text.splitlines()
    if not lines or not re.fullmatch(r'Num\s+Type\s+Disp\s+Enb\s+Address\s+What\s*', lines[0]):
        raise ValueError('unknown breakpoint table header')
    addresses, parent, multiple = [], False, False
    for line in lines[1:]:
        if not line.strip():
            continue
        m = re.fullmatch(r'\s*(\d+(?:\.\d+)?)\s+(?:(breakpoint)\s+(keep|del)\s+)?([yn])\s+(0x[0-9a-fA-F]+|<MULTIPLE>|<PENDING>)\s*(.*)', line)
        if not m:
            raise ValueError('ambiguous breakpoint table row')
        key, typ, disp, enabled, addr, what = m.groups()
        if key.split('.')[0] != str(number) or enabled != 'y' or addr == '<PENDING>':
            raise ValueError('wrong/disabled/unresolved breakpoint location')
        if '.' not in key:
            if parent or typ != 'breakpoint':
                raise ValueError('duplicate/malformed parent')
            parent = True
            multiple = addr == '<MULTIPLE>'
        elif not parent or not multiple or typ is not None or addr == '<MULTIPLE>':
            raise ValueError('orphan/malformed location')
        if addr != '<MULTIPLE>':
            addresses.append(int(addr, 16))
    if not parent or not addresses or len(set(addresses)) != len(addresses):
        raise ValueError('empty/duplicate breakpoint locations')
    return addresses


def locate(text, number, generation, current, owner):
    if generation != current:
        raise ValueError('symbol snapshot invalidated')
    addresses = cli_locations(text, number)
    identities = [owner(a) for a in addresses]
    if any(i['path'] != identities[0]['path'] or i['sha256'] != identities[0]['sha256']
           for i in identities):
        raise ValueError('locations span different SO identities')
    return addresses, identities


class Lifecycle:
    """Explicit outer dispatch gate used by every observer mutation."""
    def __init__(self):
        self.phase = 'outer'
        self.state = 'idle'
        self.identity = None
        self.pending = None
        self.error = None

    def mutation(self, action):
        if self.phase != 'outer':
            raise ValueError('callback mutation forbidden: ' + action)
        return action

    def capture(self, event, identity=None):
        if self.phase != 'callback' or self.pending is not None or self.error:
            raise ValueError('invalid capture phase/pending/error')
        if event == 'input':
            if self.state != 'idle' or identity is None:
                raise ValueError('duplicate or unidentified consumer')
        elif event in ('read', 'build', 'return'):
            if self.state != 'armed' or identity != self.identity:
                raise ValueError('consumer thread/frame identity lost')
        elif event not in ('exception', 'out_of_scope'):
            raise ValueError('unknown capture event')
        self.pending = (event, identity)

    def dispatch(self):
        self.mutation('dispatch')
        if self.error:
            self.state = 'invalid'
            self.pending = None
            return 'cleanup'
        if self.pending is None:
            raise ValueError('stop without observer decision')
        event, identity = self.pending
        self.pending = None
        if event == 'input':
            self.identity, self.state = identity, 'armed'
            return 'install'
        if event == 'return':
            self.state = 'returned'
            return 'disable'
        if event in ('exception', 'out_of_scope'):
            self.error, self.state = event, 'invalid'
            return 'cleanup'
        return 'resume'

    def exit(self, rc):
        self.mutation('exit cleanup')
        if rc != 0 or self.state != 'returned' or self.pending is not None or self.error:
            self.state = 'invalid'
        return self.state == 'returned'


def verify_watch(text, number, expression):
    lines = text.splitlines()
    if len(lines) != 2 or not re.fullmatch(r'Num\s+Type\s+Disp\s+Enb\s+Address\s+What\s*', lines[0]):
        raise ValueError('ambiguous watch installation table')
    m = re.fullmatch(r'\s*(\d+)\s+read watchpoint\s+keep\s+y\s+(.*)', lines[1])
    if not m or int(m[1]) != number or m[2].strip() != expression:
        raise ValueError('hardware read watchpoint installation unverified')
    return True


def validate_symbols(validity, saved_paths, current_paths):
    if not all(validity) or sorted(saved_paths) != sorted(current_paths):
        raise ValueError('symbol object snapshot invalidated')
