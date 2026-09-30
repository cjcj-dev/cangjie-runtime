#!/usr/bin/env python3
import argparse
import re
import subprocess
import sys
from pathlib import Path


def targets(root):
    hooks = {}
    for source in sorted(root.rglob('*.cpp')):
        relative = source.relative_to(root)
        guarded = []
        for number, line in enumerate(source.read_text().splitlines(), 1):
            directive = re.match(r'\s*#\s*(if|ifdef|ifndef|else|elif|endif)\b(.*)', line)
            if directive:
                kind, expression = directive.groups()
                if kind in ('if', 'ifdef', 'ifndef'):
                    guarded.append(kind != 'ifndef' and 'MRT_TESTABLE_INTERNALS' in expression)
                elif kind in ('else', 'elif'):
                    guarded[-1] = False
                else:
                    guarded.pop()
                continue
            if not any(guarded):
                continue
            function = re.match(r'^[\w:*&<> ]+\s+(\w+::\w+)\(', line)
            variable = re.match(r'^size_t\s+(\w+)\s*=', line)
            if function or variable:
                name = (function or variable).group(1)
                hooks[name] = f'{relative}:{number}'
    if len(hooks) < 5:
        raise ValueError(f'target set shrank: expected at least 5, found {len(hooks)}')
    return hooks


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path, required=True)
    parser.add_argument('--so', type=Path)
    arguments = parser.parse_args()
    try:
        hooks = targets(arguments.source)
        print(f'TESTABLE_PRODUCT_HOOK_TARGETS={len(hooks)}', flush=True)
        for name, anchor in sorted(hooks.items()):
            print(f'TESTABLE_PRODUCT_HOOK_TARGET={name} source={anchor}', flush=True)
        if arguments.so is None:
            return 0
        symbols = subprocess.run(['nm', '--defined-only', str(arguments.so)],
                                 check=True, text=True, capture_output=True).stdout
        symbols = subprocess.run(['c++filt'], input=symbols, check=True,
                                 text=True, capture_output=True).stdout
        missing = [name for name in hooks
                   if not re.search(r'\bMapleRuntime::' + re.escape(name) +
                                    (r'\(' if '::' in name else r'(?:@@?\S+)?$'),
                                    symbols, re.MULTILINE)]
        for name in missing:
            print(f'TESTABLE_PRODUCT_HOOK_MISSING={name}', file=sys.stderr)
        return 2 if missing else 0
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'TESTABLE_PRODUCT_HOOK_FAIL={error}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
